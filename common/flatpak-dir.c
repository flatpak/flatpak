/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/types.h>
#include <utime.h>
#include <glnx-console.h>

#include <glib/gi18n.h>

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include "libglnx/libglnx.h"
#include "lib/flatpak-error.h"

#include "flatpak-dir.h"
#include "flatpak-utils.h"
#include "flatpak-oci-registry.h"
#include "flatpak-run.h"

#include "errno.h"

#define NO_SYSTEM_HELPER ((FlatpakSystemHelper *) (gpointer) 1)

#define SUMMARY_CACHE_TIMEOUT_SEC 5*60

#define SYSCONF_INSTALLATIONS_DIR "installations.d"
#define SYSCONF_INSTALLATIONS_FILE_EXT ".conf"

#define SYSTEM_DIR_DEFAULT_ID "default"
#define SYSTEM_DIR_DEFAULT_DISPLAY_NAME "Default system directory"
#define SYSTEM_DIR_DEFAULT_STORAGE_TYPE FLATPAK_DIR_STORAGE_TYPE_DEFAULT
#define SYSTEM_DIR_DEFAULT_PRIORITY 0

static OstreeRepo * flatpak_dir_create_system_child_repo (FlatpakDir   *self,
                                                          GLnxLockFile *file_lock,
                                                          GError      **error);

static gboolean flatpak_dir_remote_fetch_summary (FlatpakDir   *self,
                                                  const char   *name,
                                                  GBytes      **out_summary,
                                                  GCancellable *cancellable,
                                                  GError      **error);

typedef struct
{
  GBytes *bytes;
  char *remote;
  char *url;
  guint64 time;
} CachedSummary;

typedef struct
{
  char                 *id;
  char                 *display_name;
  gint                  priority;
  FlatpakDirStorageType storage_type;
} DirExtraData;

struct FlatpakDir
{
  GObject              parent;

  gboolean             user;
  GFile               *basedir;
  DirExtraData        *extra_data;
  OstreeRepo          *repo;
  gboolean             no_system_helper;

  FlatpakSystemHelper *system_helper;

  GHashTable          *summary_cache;

  SoupSession         *soup_session;
};

typedef struct
{
  GObjectClass parent_class;
} FlatpakDirClass;

struct FlatpakDeploy
{
  GObject         parent;

  GFile          *dir;
  GKeyFile       *metadata;
  FlatpakContext *system_overrides;
  FlatpakContext *user_overrides;
};

typedef struct
{
  GObjectClass parent_class;
} FlatpakDeployClass;

G_DEFINE_TYPE (FlatpakDir, flatpak_dir, G_TYPE_OBJECT)
G_DEFINE_TYPE (FlatpakDeploy, flatpak_deploy, G_TYPE_OBJECT)

enum {
  PROP_0,

  PROP_USER,
  PROP_PATH
};

#define OSTREE_GIO_FAST_QUERYINFO ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target," \
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev")

static const char *
get_config_dir_location (void)
{
  static gsize path = 0;

  if (g_once_init_enter (&path))
    {
      gsize setup_value = 0;
      const char *config_dir = g_getenv ("FLATPAK_CONFIG_DIR");
      if (config_dir != NULL)
        setup_value = (gsize)config_dir;
      else
        setup_value = (gsize)FLATPAK_CONFIGDIR;
      g_once_init_leave (&path, setup_value);
     }

  return (const char *)path;
}

static DirExtraData *
dir_extra_data_new (const char           *id,
                    const char           *display_name,
                    gint                  priority,
                    FlatpakDirStorageType type)
{
  DirExtraData *dir_extra_data = g_new0 (DirExtraData, 1);
  dir_extra_data->id = g_strdup (id);
  dir_extra_data->display_name = g_strdup (display_name);
  dir_extra_data->priority = priority;
  dir_extra_data->storage_type = type;

  return dir_extra_data;
}

static DirExtraData *
dir_extra_data_clone (DirExtraData *extra_data)
{
  if (extra_data != NULL)
    return dir_extra_data_new (extra_data->id,
                               extra_data->display_name,
                               extra_data->priority,
                               extra_data->storage_type);
  return NULL;
}

static void
dir_extra_data_free (DirExtraData *dir_extra_data)
{
  g_free (dir_extra_data->id);
  g_free (dir_extra_data->display_name);
  g_free (dir_extra_data);
}

static GVariant *
variant_new_ay_bytes (GBytes *bytes)
{
  gsize size;
  gconstpointer data;
  data = g_bytes_get_data (bytes, &size);
  g_bytes_ref (bytes);
  return g_variant_ref_sink (g_variant_new_from_data (G_VARIANT_TYPE ("ay"), data, size,
                                                      TRUE, (GDestroyNotify)g_bytes_unref, bytes));
}

static void
flatpak_deploy_finalize (GObject *object)
{
  FlatpakDeploy *self = FLATPAK_DEPLOY (object);

  g_clear_object (&self->dir);
  g_clear_pointer (&self->metadata, g_key_file_unref);
  g_clear_pointer (&self->system_overrides, flatpak_context_free);
  g_clear_pointer (&self->user_overrides, flatpak_context_free);

  G_OBJECT_CLASS (flatpak_deploy_parent_class)->finalize (object);
}

static void
flatpak_deploy_class_init (FlatpakDeployClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = flatpak_deploy_finalize;

}

static void
flatpak_deploy_init (FlatpakDeploy *self)
{
}

GFile *
flatpak_deploy_get_dir (FlatpakDeploy *deploy)
{
  return g_object_ref (deploy->dir);
}

GFile *
flatpak_deploy_get_files (FlatpakDeploy *deploy)
{
  return g_file_get_child (deploy->dir, "files");
}

FlatpakContext *
flatpak_deploy_get_overrides (FlatpakDeploy *deploy)
{
  FlatpakContext *overrides = flatpak_context_new ();

  if (deploy->system_overrides)
    flatpak_context_merge (overrides, deploy->system_overrides);

  if (deploy->user_overrides)
    flatpak_context_merge (overrides, deploy->user_overrides);

  return overrides;
}

GKeyFile *
flatpak_deploy_get_metadata (FlatpakDeploy *deploy)
{
  return g_key_file_ref (deploy->metadata);
}

static FlatpakDeploy *
flatpak_deploy_new (GFile *dir, GKeyFile *metadata)
{
  FlatpakDeploy *deploy;

  deploy = g_object_new (FLATPAK_TYPE_DEPLOY, NULL);
  deploy->dir = g_object_ref (dir);
  deploy->metadata = g_key_file_ref (metadata);

  return deploy;
}

GFile *
flatpak_get_system_default_base_dir_location (void)
{
  static gsize path = 0;

  if (g_once_init_enter (&path))
    {
      gsize setup_value = 0;
      const char *system_dir = g_getenv ("FLATPAK_SYSTEM_DIR");
      if (system_dir != NULL)
        setup_value = (gsize)system_dir;
      else
        setup_value = (gsize)FLATPAK_SYSTEMDIR;
      g_once_init_leave (&path, setup_value);
     }

  return g_file_new_for_path ((char *)path);
}

static FlatpakDirStorageType
parse_storage_type (const char *type_string)
{
  if (type_string != NULL)
    {
      g_autofree char *type_low = NULL;

      type_low = g_ascii_strdown (type_string, -1);
      if (g_strcmp0 (type_low, "mmc") == 0)
        return FLATPAK_DIR_STORAGE_TYPE_MMC;

      if (g_strcmp0 (type_low, "sdcard") == 0)
        return FLATPAK_DIR_STORAGE_TYPE_SDCARD;

      if (g_strcmp0 (type_low, "hardisk") == 0)
        return FLATPAK_DIR_STORAGE_TYPE_HARD_DISK;
    }

  return FLATPAK_DIR_STORAGE_TYPE_DEFAULT;
}

static gboolean
has_system_location (GPtrArray  *locations,
                     const char *id)
{
  int i;

  for (i = 0; i < locations->len; i++)
    {
      GFile *path = g_ptr_array_index (locations, i);
      DirExtraData *extra_data = g_object_get_data (G_OBJECT (path), "extra-data");
      if (extra_data != NULL && g_strcmp0 (extra_data->id, id) == 0)
        return TRUE;
    }

  return FALSE;
}

static void
append_new_system_location (GPtrArray            *locations,
                            GFile                *location,
                            const char           *id,
                            const char           *display_name,
                            FlatpakDirStorageType storage_type,
                            gint                  priority)
{
  DirExtraData *extra_data = NULL;

  extra_data = dir_extra_data_new (id, display_name, priority, storage_type);
  g_object_set_data_full (G_OBJECT (location), "extra-data", extra_data,
                          (GDestroyNotify)dir_extra_data_free);

  g_ptr_array_add (locations, location);
}

static gboolean
append_locations_from_config_file (GPtrArray    *locations,
                                   const char   *file_path,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  g_autoptr(GKeyFile) keyfile = NULL;
  g_auto(GStrv) groups = NULL;
  g_autoptr(GError) my_error = NULL;
  gboolean ret = FALSE;
  gsize n_groups;
  int i;

  keyfile = g_key_file_new ();

  g_key_file_load_from_file (keyfile, file_path, G_KEY_FILE_NONE, &my_error);
  if (my_error != NULL)
    {
      g_debug ("Could not get list of system installations: %s\n", my_error->message);
      g_propagate_error (error, g_steal_pointer (&my_error));
      goto out;
    }

  /* One configuration file might define more than one installation */
  groups = g_key_file_get_groups (keyfile, &n_groups);
  for (i = 0; i < n_groups; i++)
    {
      g_autofree char *id = NULL;
      g_autofree char *path = NULL;
      size_t len;

      if (!g_str_has_prefix (groups[i], "Installation \""))
        continue;

      id = g_strdup (&groups[i][14]);
      if (!g_str_has_suffix (id, "\""))
        continue;

      len = strlen (id);
      if (len > 0)
        id[len - 1] = '\0';

      if (has_system_location (locations, id))
        {
          g_warning ("Found duplicate flatpak installation (Id: %s). Ignoring", id);
          continue;
        }

      path = g_key_file_get_string (keyfile, groups[i], "Path", &my_error);
      if (path == NULL)
        {
          g_debug ("Unable to get path for installation '%s': %s\n", id, my_error->message);
          g_propagate_error (error, g_steal_pointer (&my_error));
          goto out;
        }
      else
        {
          GFile *location = NULL;
          g_autofree char *display_name = NULL;
          g_autofree char *priority = NULL;
          g_autofree char *storage_type = NULL;
          gint priority_val = 0;

          display_name = g_key_file_get_string (keyfile, groups[i], "DisplayName", NULL);
          priority = g_key_file_get_string (keyfile, groups[i], "Priority", NULL);
          storage_type = g_key_file_get_string (keyfile, groups[i], "StorageType", NULL);

          if (priority != NULL)
            priority_val = g_ascii_strtoll (priority, NULL, 10);

          location = g_file_new_for_path (path);
          append_new_system_location (locations, location, id, display_name,
                                      parse_storage_type (storage_type),
                                      priority_val);
        }
    }

  ret = TRUE;

 out:
  return ret;
}

static gint
system_locations_compare_func (gconstpointer location_a, gconstpointer location_b)
{
  const GFile *location_object_a = *(const GFile **)location_a;
  const GFile *location_object_b = *(const GFile **)location_b;
  DirExtraData *extra_data_a = NULL;
  DirExtraData *extra_data_b = NULL;
  gint prio_a = 0;
  gint prio_b = 0;

  extra_data_a = g_object_get_data (G_OBJECT (location_object_a), "extra-data");
  prio_a = (extra_data_a != NULL) ? extra_data_a->priority : 0;

  extra_data_b = g_object_get_data (G_OBJECT (location_object_b), "extra-data");
  prio_b = (extra_data_b != NULL) ? extra_data_b->priority : 0;

  return prio_b - prio_a;
}

static GPtrArray *
system_locations_from_configuration (GCancellable *cancellable,
                                     GError      **error)
{
  g_autoptr(GPtrArray) locations = NULL;
  g_autoptr(GFile) conf_dir = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GError) my_error = NULL;
  g_autofree char *config_dir = NULL;

  locations = g_ptr_array_new_with_free_func (g_object_unref);
  config_dir = g_strdup_printf ("%s/%s",
                                get_config_dir_location (),
                                SYSCONF_INSTALLATIONS_DIR);

  if (!g_file_test (config_dir, G_FILE_TEST_IS_DIR))
    {
      g_debug ("No installations directory in %s. Skipping", config_dir);
      goto out;
    }

  conf_dir = g_file_new_for_path (config_dir);
  dir_enum = g_file_enumerate_children (conf_dir,
                                        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                        G_FILE_QUERY_INFO_NONE,
                                        cancellable, &my_error);
  if (my_error != NULL)
    {
      g_debug ("Unexpected error retrieving extra installations in %s: %s",
               config_dir, my_error->message);
      g_propagate_error (error, g_steal_pointer (&my_error));
      goto out;
    }

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *path;
      const char *name;
      guint32 type;

      if (!g_file_enumerator_iterate (dir_enum, &file_info, &path,
                                      cancellable, &my_error))
        {
          g_debug ("Unexpected error reading file in %s: %s",
                   config_dir, my_error->message);
          g_propagate_error (error, g_steal_pointer (&my_error));
          goto out;
        }

      if (file_info == NULL)
        break;

      name = g_file_info_get_attribute_byte_string (file_info, "standard::name");
      type = g_file_info_get_attribute_uint32 (file_info, "standard::type");

      if (type == G_FILE_TYPE_REGULAR && g_str_has_suffix (name, SYSCONF_INSTALLATIONS_FILE_EXT))
        {
          g_autofree char *path_str = g_file_get_path (path);
          if (!append_locations_from_config_file (locations, path_str, cancellable, error))
            goto out;
        }
    }

 out:
  return g_steal_pointer (&locations);
}

static GPtrArray *
get_system_locations (GCancellable *cancellable,
                      GError      **error)
{
  g_autoptr(GPtrArray) locations = NULL;

  /* This will always return a GPtrArray, being an empty one
   * if no additional system installations have been configured.
   */
  locations = system_locations_from_configuration (cancellable, error);

  /* Only fill the details of the default directory if not overriden. */
  if (!has_system_location (locations, SYSTEM_DIR_DEFAULT_ID))
    {
      append_new_system_location (locations,
                                  flatpak_get_system_default_base_dir_location (),
                                  SYSTEM_DIR_DEFAULT_ID,
                                  SYSTEM_DIR_DEFAULT_DISPLAY_NAME,
                                  SYSTEM_DIR_DEFAULT_STORAGE_TYPE,
                                  SYSTEM_DIR_DEFAULT_PRIORITY);
    }

  /* Store the list of system locations sorted according to priorities */
  g_ptr_array_sort (locations, system_locations_compare_func);

  return g_steal_pointer (&locations);
}

GPtrArray *
flatpak_get_system_base_dir_locations (GCancellable *cancellable,
                                       GError      **error)
{
  static gsize array = 0;

  if (g_once_init_enter (&array))
    {
      gsize setup_value = 0;
      setup_value = (gsize) get_system_locations (cancellable, error);
      g_once_init_leave (&array, setup_value);
     }

  return (GPtrArray *) array;
}

GFile *
flatpak_get_user_base_dir_location (void)
{
  static gsize file = 0;

  if (g_once_init_enter (&file))
    {
      gsize setup_value = 0;
      const char *path;
      g_autofree char *free_me = NULL;
      const char *user_dir = g_getenv ("FLATPAK_USER_DIR");
      if (user_dir != NULL && *user_dir != 0)
        path = user_dir;
      else
        path = free_me = g_build_filename (g_get_user_data_dir (), "flatpak", NULL);

      setup_value = (gsize) g_file_new_for_path (path);

      g_once_init_leave (&file, setup_value);
    }

  return g_object_ref ((GFile *)file);
}

GFile *
flatpak_get_user_cache_dir_location (void)
{
  g_autoptr(GFile) base_dir = NULL;

  base_dir = flatpak_get_user_base_dir_location ();
  return g_file_get_child (base_dir, "system-cache");
}

GFile *
flatpak_ensure_user_cache_dir_location (GError **error)
{
  g_autoptr(GFile) cache_dir = NULL;
  g_autofree char *cache_path = NULL;

  cache_dir = flatpak_get_user_cache_dir_location ();
  cache_path = g_file_get_path (cache_dir);

  if (g_mkdir_with_parents (cache_path, 0755) != 0)
    {
      glnx_set_error_from_errno (error);
      return NULL;
    }

  return g_steal_pointer (&cache_dir);
}

static FlatpakSystemHelper *
flatpak_dir_get_system_helper (FlatpakDir *self)
{
  g_autoptr(GError) error = NULL;

  if (g_once_init_enter (&self->system_helper))
    {
      FlatpakSystemHelper *system_helper;
      const char *on_session = g_getenv ("FLATPAK_SYSTEM_HELPER_ON_SESSION");

      /* To ensure reverse mapping */
      flatpak_error_quark ();

      system_helper =
        flatpak_system_helper_proxy_new_for_bus_sync (on_session != NULL ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM,
                                                      G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                                                      G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                                      "org.freedesktop.Flatpak.SystemHelper",
                                                      "/org/freedesktop/Flatpak/SystemHelper",
                                                      NULL, &error);
      if (error != NULL)
        {
          g_warning ("Can't find org.freedesktop.Flatpak.SystemHelper: %s\n", error->message);
          system_helper = NO_SYSTEM_HELPER;
        }
      else
        {
          g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (system_helper), G_MAXINT);
        }
      g_once_init_leave (&self->system_helper, system_helper);
    }

  if (self->system_helper != NO_SYSTEM_HELPER)
    return self->system_helper;
  return NULL;
}

gboolean
flatpak_dir_use_system_helper (FlatpakDir *self)
{
  FlatpakSystemHelper *system_helper;

#ifndef USE_SYSTEM_HELPER
  if (TRUE)
    return FALSE;
#endif

  if (self->no_system_helper || self->user || getuid () == 0)
    return FALSE;

  system_helper = flatpak_dir_get_system_helper (self);

  return system_helper != NULL;
}

static OstreeRepo *
system_ostree_repo_new (GFile *repodir)
{
  g_autofree char *config_dir = NULL;

  config_dir = g_strdup_printf ("%s/%s",
                                get_config_dir_location (),
                                "/remotes.d");

  return g_object_new (OSTREE_TYPE_REPO, "path", repodir,
                       "remotes-config-dir",
                       config_dir,
                       NULL);
}

static void
flatpak_dir_finalize (GObject *object)
{
  FlatpakDir *self = FLATPAK_DIR (object);

  g_clear_object (&self->repo);
  g_clear_object (&self->basedir);
  g_clear_pointer (&self->extra_data, dir_extra_data_free);

  if (self->system_helper != NO_SYSTEM_HELPER)
    g_clear_object (&self->system_helper);

  g_clear_object (&self->soup_session);
  g_clear_pointer (&self->summary_cache, g_hash_table_unref);

  G_OBJECT_CLASS (flatpak_dir_parent_class)->finalize (object);
}

static void
flatpak_dir_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  FlatpakDir *self = FLATPAK_DIR (object);

  switch (prop_id)
    {
    case PROP_PATH:
      /* Canonicalize */
      self->basedir = g_file_new_for_path (flatpak_file_get_path_cached (g_value_get_object (value)));
      break;

    case PROP_USER:
      self->user = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_dir_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  FlatpakDir *self = FLATPAK_DIR (object);

  switch (prop_id)
    {
    case PROP_PATH:
      g_value_set_object (value, self->basedir);
      break;

    case PROP_USER:
      g_value_set_boolean (value, self->user);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_dir_class_init (FlatpakDirClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = flatpak_dir_get_property;
  object_class->set_property = flatpak_dir_set_property;
  object_class->finalize = flatpak_dir_finalize;

  g_object_class_install_property (object_class,
                                   PROP_USER,
                                   g_param_spec_boolean ("user",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class,
                                   PROP_PATH,
                                   g_param_spec_object ("path",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
flatpak_dir_init (FlatpakDir *self)
{
  /* Work around possible deadlock due to: https://bugzilla.gnome.org/show_bug.cgi?id=674885 */
  g_type_ensure (G_TYPE_UNIX_SOCKET_ADDRESS);

  /* Optional data that needs initialization */
  self->extra_data = NULL;
}

gboolean
flatpak_dir_is_user (FlatpakDir *self)
{
  return self->user;
}

void
flatpak_dir_set_no_system_helper (FlatpakDir *self,
                                  gboolean    no_system_helper)
{
  self->no_system_helper = no_system_helper;
}

GFile *
flatpak_dir_get_path (FlatpakDir *self)
{
  return self->basedir;
}

GFile *
flatpak_dir_get_changed_path (FlatpakDir *self)
{
  return g_file_get_child (self->basedir, ".changed");
}

const char *
flatpak_dir_get_id (FlatpakDir *self)
{
  if (self->extra_data != NULL)
    return self->extra_data->id;

  return NULL;
}

char *
flatpak_dir_get_name (FlatpakDir *self)
{
  const char *id = NULL;

  if (self->user)
    return g_strdup ("user");

  id = flatpak_dir_get_id (self);
  if (id != NULL && g_strcmp0 (id, "default") != 0)
    return g_strdup_printf ("system (%s)", id);

  return g_strdup ("system");
}

const char *
flatpak_dir_get_display_name (FlatpakDir *self)
{
  if (self->extra_data != NULL)
    return self->extra_data->display_name;

  return NULL;
}

gint
flatpak_dir_get_priority (FlatpakDir *self)
{
  if (self->extra_data != NULL)
    return self->extra_data->priority;

  return 0;
}

FlatpakDirStorageType
flatpak_dir_get_storage_type (FlatpakDir *self)
{
  if (self->extra_data != NULL)
    return self->extra_data->storage_type;

  return FLATPAK_DIR_STORAGE_TYPE_DEFAULT;
}

char *
flatpak_dir_load_override (FlatpakDir *self,
                           const char *app_id,
                           gsize      *length,
                           GError    **error)
{
  g_autoptr(GFile) override_dir = NULL;
  g_autoptr(GFile) file = NULL;
  char *metadata_contents;

  override_dir = g_file_get_child (self->basedir, "overrides");
  file = g_file_get_child (override_dir, app_id);

  if (!g_file_load_contents (file, NULL,
                             &metadata_contents, length, NULL, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   _("No overrides found for %s"), app_id);
      return NULL;
    }

  return metadata_contents;
}

GKeyFile *
flatpak_load_override_keyfile (const char *app_id, gboolean user, GError **error)
{
  g_autofree char *metadata_contents = NULL;
  gsize metadata_size;

  g_autoptr(GKeyFile) metakey = g_key_file_new ();
  g_autoptr(FlatpakDir) dir = NULL;

  dir = user ? flatpak_dir_get_user () : flatpak_dir_get_system_default ();

  metadata_contents = flatpak_dir_load_override (dir, app_id, &metadata_size, error);
  if (metadata_contents == NULL)
    return NULL;

  if (!g_key_file_load_from_data (metakey,
                                  metadata_contents,
                                  metadata_size,
                                  0, error))
    return NULL;

  return g_steal_pointer (&metakey);
}

FlatpakContext *
flatpak_load_override_file (const char *app_id, gboolean user, GError **error)
{
  g_autoptr(FlatpakContext) overrides = flatpak_context_new ();

  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GError) my_error = NULL;

  metakey = flatpak_load_override_keyfile (app_id, user, &my_error);
  if (metakey == NULL)
    {
      if (!g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return NULL;
        }
    }
  else
    {
      if (!flatpak_context_load_metadata (overrides, metakey, error))
        return NULL;
    }

  return g_steal_pointer (&overrides);
}

gboolean
flatpak_save_override_keyfile (GKeyFile   *metakey,
                               const char *app_id,
                               gboolean    user,
                               GError    **error)
{
  g_autoptr(GFile) base_dir = NULL;
  g_autoptr(GFile) override_dir = NULL;
  g_autoptr(GFile) file = NULL;
  g_autofree char *filename = NULL;
  g_autofree char *parent = NULL;

  if (user)
    base_dir = flatpak_get_user_base_dir_location ();
  else
    base_dir = flatpak_get_system_default_base_dir_location ();

  override_dir = g_file_get_child (base_dir, "overrides");
  file = g_file_get_child (override_dir, app_id);

  filename = g_file_get_path (file);
  parent = g_path_get_dirname (filename);
  if (g_mkdir_with_parents (parent, 0755))
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  return g_key_file_save_to_file (metakey, filename, error);
}

FlatpakDeploy *
flatpak_dir_load_deployed (FlatpakDir   *self,
                           const char   *ref,
                           const char   *checksum,
                           GCancellable *cancellable,
                           GError      **error)
{
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_auto(GStrv) ref_parts = NULL;
  g_autofree char *metadata_contents = NULL;
  FlatpakDeploy *deploy;
  gsize metadata_size;

  deploy_dir = flatpak_dir_get_if_deployed (self, ref, checksum, cancellable);
  if (deploy_dir == NULL)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   _("%s not installed"), ref);
      return NULL;
    }

  metadata = g_file_get_child (deploy_dir, "metadata");
  if (!g_file_load_contents (metadata, cancellable, &metadata_contents, &metadata_size, NULL, error))
    return NULL;

  metakey = g_key_file_new ();
  if (!g_key_file_load_from_data (metakey, metadata_contents, metadata_size, 0, error))
    return NULL;

  deploy = flatpak_deploy_new (deploy_dir, metakey);

  ref_parts = g_strsplit (ref, "/", -1);
  g_assert (g_strv_length (ref_parts) == 4);

  /* Only apps have overrides */
  if (strcmp (ref_parts[0], "app") == 0)
    {
      /* Only load system overrides for system installed apps */
      if (!self->user)
        {
          deploy->system_overrides = flatpak_load_override_file (ref_parts[1], FALSE, error);
          if (deploy->system_overrides == NULL)
            return NULL;
        }

      /* Always load user overrides */
      deploy->user_overrides = flatpak_load_override_file (ref_parts[1], TRUE, error);
      if (deploy->user_overrides == NULL)
        return NULL;
    }

  return deploy;
}

GFile *
flatpak_dir_get_deploy_dir (FlatpakDir *self,
                            const char *ref)
{
  return g_file_resolve_relative_path (self->basedir, ref);
}

GFile *
flatpak_dir_get_unmaintained_extension_dir (FlatpakDir *self,
                                            const char *name,
                                            const char *arch,
                                            const char *branch)
{
  const char *unmaintained_ref;

  unmaintained_ref = g_build_filename ("extension", name, arch, branch, NULL);
  return g_file_resolve_relative_path (self->basedir, unmaintained_ref);
}

GFile *
flatpak_dir_get_exports_dir (FlatpakDir *self)
{
  return g_file_get_child (self->basedir, "exports");
}

GFile *
flatpak_dir_get_removed_dir (FlatpakDir *self)
{
  return g_file_get_child (self->basedir, ".removed");
}

OstreeRepo *
flatpak_dir_get_repo (FlatpakDir *self)
{
  return self->repo;
}


/* This is an exclusive per flatpak installation file lock that is taken
 * whenever any config in the directory outside the repo is to be changed. For
 * instance deployments, overrides or active commit changes.
 *
 * For concurrency protection of the actual repository we rely on ostree
 * to do the right thing.
 */
gboolean
flatpak_dir_lock (FlatpakDir   *self,
                  GLnxLockFile *lockfile,
                  GCancellable *cancellable,
                  GError      **error)
{
  g_autoptr(GFile) lock_file = g_file_get_child (flatpak_dir_get_path (self), "lock");
  g_autofree char *lock_path = g_file_get_path (lock_file);

  return glnx_make_lock_file (AT_FDCWD, lock_path, LOCK_EX, lockfile, error);
}

const char *
flatpak_deploy_data_get_origin (GVariant *deploy_data)
{
  const char *origin;

  g_variant_get_child (deploy_data, 0, "&s", &origin);
  return origin;
}

const char *
flatpak_deploy_data_get_commit (GVariant *deploy_data)
{
  const char *commit;

  g_variant_get_child (deploy_data, 1, "&s", &commit);
  return commit;
}

const char *
flatpak_deploy_data_get_alt_id (GVariant *deploy_data)
{
  g_autoptr(GVariant) metadata = g_variant_get_child_value (deploy_data, 4);
  const char *alt_id = NULL;

  g_variant_lookup (metadata, "alt-id", "&s", &alt_id);

  return alt_id;
}

/**
 * flatpak_deploy_data_get_subpaths:
 *
 * Returns: (array length=length zero-terminated=1) (transfer container): an array of constant strings
 **/
const char **
flatpak_deploy_data_get_subpaths (GVariant *deploy_data)
{
  const char **subpaths;

  g_variant_get_child (deploy_data, 2, "^a&s", &subpaths);
  return subpaths;
}

guint64
flatpak_deploy_data_get_installed_size (GVariant *deploy_data)
{
  guint64 size;

  g_variant_get_child (deploy_data, 3, "t", &size);
  return GUINT64_FROM_BE (size);
}

static GVariant *
flatpak_dir_new_deploy_data (const char *origin,
                             const char *commit,
                             char      **subpaths,
                             guint64     installed_size,
                             GVariant   *metadata)
{
  char *empty_subpaths[] = {NULL};
  GVariantBuilder builder;

  if (metadata == NULL)
    {
      g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
      metadata = g_variant_builder_end (&builder);
    }

  return g_variant_ref_sink (g_variant_new ("(ss^ast@a{sv})",
                                            origin,
                                            commit,
                                            subpaths ? subpaths : empty_subpaths,
                                            GUINT64_TO_BE (installed_size),
                                            metadata));
}

static char **
get_old_subpaths (GFile        *deploy_base,
                  GCancellable *cancellable,
                  GError      **error)
{
  g_autoptr(GFile) file = NULL;
  g_autofree char *data = NULL;
  g_autoptr(GError) my_error = NULL;
  g_autoptr(GPtrArray) subpaths = NULL;
  g_auto(GStrv) lines = NULL;
  int i;

  file = g_file_get_child (deploy_base, "subpaths");
  if (!g_file_load_contents (file, cancellable, &data, NULL, NULL, &my_error))
    {
      if (g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          data = g_strdup ("");
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return NULL;
        }
    }

  lines = g_strsplit (data, "\n", 0);

  subpaths = g_ptr_array_new ();
  for (i = 0; lines[i] != NULL; i++)
    {
      lines[i] = g_strstrip (lines[i]);
      if (lines[i][0] == '/')
        g_ptr_array_add (subpaths, g_strdup (lines[i]));
    }

  g_ptr_array_add (subpaths, NULL);
  return (char **) g_ptr_array_free (subpaths, FALSE);
}

static GVariant *
flatpak_create_deploy_data_from_old (FlatpakDir   *self,
                                     GFile        *deploy_dir,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  g_autoptr(GFile) deploy_base = NULL;
  g_autofree char *old_origin = NULL;
  g_autofree char *commit = NULL;
  g_auto(GStrv) old_subpaths = NULL;
  g_autoptr(GFile) origin = NULL;
  guint64 installed_size;

  deploy_base = g_file_get_parent (deploy_dir);
  commit = g_file_get_basename (deploy_dir);

  origin = g_file_get_child (deploy_base, "origin");
  if (!g_file_load_contents (origin, cancellable, &old_origin, NULL, NULL, error))
    return NULL;

  old_subpaths = get_old_subpaths (deploy_base, cancellable, error);
  if (old_subpaths == NULL)
    return NULL;

  /* For backwards compat we return a 0 installed size, its to slow to regenerate */
  installed_size = 0;

  return flatpak_dir_new_deploy_data (old_origin, commit, old_subpaths,
                                      installed_size, NULL);
}

GVariant *
flatpak_dir_get_deploy_data (FlatpakDir   *self,
                             const char   *ref,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(GFile) data_file = NULL;
  g_autoptr(GError) my_error = NULL;
  char *data = NULL;
  gsize data_size;

  deploy_dir = flatpak_dir_get_if_deployed (self, ref, NULL, cancellable);
  if (deploy_dir == NULL)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   _("%s not installed"), ref);
      return NULL;
    }

  data_file = g_file_get_child (deploy_dir, "deploy");
  if (!g_file_load_contents (data_file, cancellable, &data, &data_size, NULL, &my_error))
    {
      if (!g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return NULL;
        }

      return flatpak_create_deploy_data_from_old (self, deploy_dir,
                                                  cancellable, error);
    }

  return g_variant_ref_sink (g_variant_new_from_data (FLATPAK_DEPLOY_DATA_GVARIANT_FORMAT,
                                                      data, data_size,
                                                      FALSE, g_free, data));
}


char *
flatpak_dir_get_origin (FlatpakDir   *self,
                        const char   *ref,
                        GCancellable *cancellable,
                        GError      **error)
{
  g_autoptr(GVariant) deploy_data = NULL;

  deploy_data = flatpak_dir_get_deploy_data (self, ref,
                                             cancellable, error);
  if (deploy_data == NULL)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   _("%s not installed"), ref);
      return NULL;
    }

  return g_strdup (flatpak_deploy_data_get_origin (deploy_data));
}

char **
flatpak_dir_get_subpaths (FlatpakDir   *self,
                          const char   *ref,
                          GCancellable *cancellable,
                          GError      **error)
{
  g_autoptr(GVariant) deploy_data = NULL;
  char **subpaths;
  int i;

  deploy_data = flatpak_dir_get_deploy_data (self, ref,
                                             cancellable, error);
  if (deploy_data == NULL)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   _("%s not installed"), ref);
      return NULL;
    }

  subpaths = (char **) flatpak_deploy_data_get_subpaths (deploy_data);
  for (i = 0; subpaths[i] != NULL; i++)
    subpaths[i] = g_strdup (subpaths[i]);

  return subpaths;
}

gboolean
flatpak_dir_ensure_path (FlatpakDir   *self,
                         GCancellable *cancellable,
                         GError      **error)
{
  return flatpak_mkdir_p (self->basedir, cancellable, error);
}

/* Warning: This is not threadsafe, don't use in libflatpak */
gboolean
flatpak_dir_recreate_repo (FlatpakDir   *self,
                            GCancellable *cancellable,
                            GError      **error)
{
  gboolean res;
  OstreeRepo *old_repo = g_steal_pointer (&self->repo);
  res = flatpak_dir_ensure_repo (self, cancellable, error);
  g_clear_object (&old_repo);
  return res;
}

gboolean
flatpak_dir_ensure_repo (FlatpakDir   *self,
                         GCancellable *cancellable,
                         GError      **error)
{
  gboolean ret = FALSE;

  g_autoptr(GFile) repodir = NULL;
  g_autoptr(OstreeRepo) repo = NULL;

  if (self->repo == NULL)
    {
      if (!flatpak_dir_ensure_path (self, cancellable, error))
        goto out;

      repodir = g_file_get_child (self->basedir, "repo");
      if (self->no_system_helper || self->user || getuid () == 0)
        {
          repo = ostree_repo_new (repodir);
        }
      else
        {
          g_autoptr(GFile) cache_dir = NULL;
          g_autofree char *cache_path = NULL;

          repo = system_ostree_repo_new (repodir);

          cache_dir = flatpak_ensure_user_cache_dir_location (error);
          if (cache_dir == NULL)
            goto out;

          cache_path = g_file_get_path (cache_dir);
          if (!ostree_repo_set_cache_dir (repo,
                                          AT_FDCWD, cache_path,
                                          cancellable, error))
            goto out;
        }

      if (!g_file_query_exists (repodir, cancellable))
        {
          if (!ostree_repo_create (repo,
                                   OSTREE_REPO_MODE_BARE_USER,
                                   cancellable, error))
            {
              flatpak_rm_rf (repodir, cancellable, NULL);
              goto out;
            }

          /* Create .changes file early to avoid polling non-existing file in monitor */
          flatpak_dir_mark_changed (self, NULL);
        }
      else
        {
          if (!ostree_repo_open (repo, cancellable, error))
            {
              g_autofree char *repopath = NULL;

              repopath = g_file_get_path (repodir);
              g_prefix_error (error, _("While opening repository %s: "), repopath);
              goto out;
            }
        }

      /* Make sure we didn't reenter weirdly */
      g_assert (self->repo == NULL);
      self->repo = g_object_ref (repo);
    }

  ret = TRUE;
out:
  return ret;
}

gboolean
flatpak_dir_mark_changed (FlatpakDir *self,
                          GError    **error)
{
  g_autoptr(GFile) changed_file = NULL;

  changed_file = flatpak_dir_get_changed_path (self);
  if (!g_file_replace_contents (changed_file, "", 0, NULL, FALSE,
                                G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_dir_remove_appstream (FlatpakDir   *self,
                              const char   *remote,
                              GCancellable *cancellable,
                              GError      **error)
{
  g_autoptr(GFile) appstream_dir = NULL;
  g_autoptr(GFile) remote_dir = NULL;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  appstream_dir = g_file_get_child (flatpak_dir_get_path (self), "appstream");
  remote_dir = g_file_get_child (appstream_dir, remote);

  if (g_file_query_exists (remote_dir, cancellable) &&
      !flatpak_rm_rf (remote_dir, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_dir_deploy_appstream (FlatpakDir          *self,
                              const char          *remote,
                              const char          *arch,
                              gboolean            *out_changed,
                              GCancellable        *cancellable,
                              GError             **error)
{
  g_autoptr(GFile) appstream_dir = NULL;
  g_autoptr(GFile) remote_dir = NULL;
  g_autoptr(GFile) arch_dir = NULL;
  g_autoptr(GFile) checkout_dir = NULL;
  g_autoptr(GFile) real_checkout_dir = NULL;
  g_autoptr(GFile) timestamp_file = NULL;
  g_autofree char *arch_path = NULL;
  gboolean checkout_exists;
  g_autofree char *remote_and_branch = NULL;
  const char *old_checksum = NULL;
  g_autofree char *new_checksum = NULL;
  g_autoptr(GFile) active_link = NULL;
  g_autofree char *branch = NULL;
  g_autoptr(GFile) old_checkout_dir = NULL;
  g_autoptr(GFile) active_tmp_link = NULL;
  g_autoptr(GError) tmp_error = NULL;
  g_autofree char *checkout_dir_path = NULL;
  OstreeRepoCheckoutAtOptions options = { 0, };
  glnx_fd_close int dfd = -1;
  g_autoptr(GFileInfo) file_info = NULL;
  g_autofree char *tmpname = g_strdup (".active-XXXXXX");

  appstream_dir = g_file_get_child (flatpak_dir_get_path (self), "appstream");
  remote_dir = g_file_get_child (appstream_dir, remote);
  arch_dir = g_file_get_child (remote_dir, arch);
  active_link = g_file_get_child (arch_dir, "active");
  timestamp_file = g_file_get_child (arch_dir, ".timestamp");

  arch_path = g_file_get_path (arch_dir);
  if (g_mkdir_with_parents (arch_path, 0755) != 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  if (!glnx_opendirat (AT_FDCWD, arch_path, TRUE, &dfd, error))
    return FALSE;

  old_checksum = NULL;
  file_info = g_file_query_info (active_link, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, NULL);
  if (file_info != NULL)
    old_checksum =  g_file_info_get_symlink_target (file_info);

  branch = g_strdup_printf ("appstream/%s", arch);
  remote_and_branch = g_strdup_printf ("%s:%s", remote, branch);
  if (!ostree_repo_resolve_rev (self->repo, remote_and_branch, TRUE, &new_checksum, error))
    return FALSE;

  real_checkout_dir = g_file_get_child (arch_dir, new_checksum);
  checkout_exists = g_file_query_exists (real_checkout_dir, NULL);

  if (old_checksum != NULL && new_checksum != NULL &&
      strcmp (old_checksum, new_checksum) == 0 &&
      checkout_exists)
    {
      if (!g_file_replace_contents (timestamp_file, "", 0, NULL, FALSE,
                                    G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, error))
        return FALSE;

      if (out_changed)
        *out_changed = FALSE;

      return TRUE; /* No changes, don't checkout */
    }

  {
    g_autofree char *template = g_strdup_printf (".%s-XXXXXX", new_checksum);
    g_autoptr(GFile) tmp_dir_template = g_file_get_child (arch_dir, template);
    checkout_dir_path = g_file_get_path (tmp_dir_template);
    if (g_mkdtemp_full (checkout_dir_path, 0755) == NULL)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     _("Can't create deploy directory"));
        return FALSE;
      }
  }
  checkout_dir = g_file_new_for_path (checkout_dir_path);

  options.mode = OSTREE_REPO_CHECKOUT_MODE_USER;
  options.overwrite_mode = OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES;
  options.enable_fsync = FALSE; /* We checkout to a temp dir and sync before moving it in place */

  if (!ostree_repo_checkout_at (self->repo, &options,
                                AT_FDCWD, checkout_dir_path, new_checksum,
                                cancellable, error))
    return FALSE;

  glnx_gen_temp_name (tmpname);
  active_tmp_link = g_file_get_child (arch_dir, tmpname);

  if (!g_file_make_symbolic_link (active_tmp_link, new_checksum, cancellable, error))
    return FALSE;

  if (syncfs (dfd) != 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  /* By now the checkout to the temporary directory is on disk, as is the temporary
     symlink pointing to the final target. */

  if (!g_file_move (checkout_dir, real_checkout_dir, G_FILE_COPY_NO_FALLBACK_FOR_MOVE,
                    cancellable, NULL, NULL, error))
    return FALSE;

  if (syncfs (dfd) != 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  if (!flatpak_file_rename (active_tmp_link,
                            active_link,
                            cancellable, error))
    return FALSE;

  if (old_checksum != NULL &&
      g_strcmp0 (old_checksum, new_checksum) != 0)
    {
      old_checkout_dir = g_file_get_child (arch_dir, old_checksum);
      if (!flatpak_rm_rf (old_checkout_dir, cancellable, &tmp_error))
        g_warning ("Unable to remove old appstream checkout: %s\n", tmp_error->message);
    }

  if (!g_file_replace_contents (timestamp_file, "", 0, NULL, FALSE,
                                G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, error))
    return FALSE;

  /* If we added a new checkout, touch the toplevel dir to tell people that they need
     to re-scan */
  if (!checkout_exists)
    {
      g_autofree char *appstream_dir_path = g_file_get_path (appstream_dir);
      utime (appstream_dir_path, NULL);
    }

  if (out_changed)
    *out_changed = TRUE;

  return TRUE;
}

gboolean
flatpak_dir_update_appstream (FlatpakDir          *self,
                              const char          *remote,
                              const char          *arch,
                              gboolean            *out_changed,
                              OstreeAsyncProgress *progress,
                              GCancellable        *cancellable,
                              GError             **error)
{
  g_autofree char *branch = NULL;
  g_autofree char *remote_and_branch = NULL;
  g_autofree char *new_checksum = NULL;

  if (out_changed)
    *out_changed = FALSE;

  if (arch == NULL)
    arch = flatpak_get_arch ();

  branch = g_strdup_printf ("appstream/%s", arch);

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  if (flatpak_dir_use_system_helper (self))
    {
      g_autoptr(OstreeRepo) child_repo = NULL;
      g_auto(GLnxLockFile) child_repo_lock = GLNX_LOCK_FILE_INIT;
      FlatpakSystemHelper *system_helper;

      child_repo = flatpak_dir_create_system_child_repo (self, &child_repo_lock, error);
      if (child_repo == NULL)
        return FALSE;

      system_helper = flatpak_dir_get_system_helper (self);

      g_assert (system_helper != NULL);

      if (!flatpak_dir_pull (self, remote, branch, NULL, NULL,
                             child_repo, FLATPAK_PULL_FLAGS_NONE, OSTREE_REPO_PULL_FLAGS_MIRROR,
                             progress, cancellable, error))
        return FALSE;

      if (!ostree_repo_resolve_rev (child_repo, branch, TRUE, &new_checksum, error))
        return FALSE;

      if (new_checksum == NULL)
        {
          g_warning ("No appstream branch in remote %s\n", remote);
        }
      else
        {
          const char *installation = flatpak_dir_get_id (self);

          g_debug ("Calling system helper: DeployAppstream");
          if (!flatpak_system_helper_call_deploy_appstream_sync (system_helper,
                                                                 flatpak_file_get_path_cached (ostree_repo_get_path (child_repo)),
                                                                 remote,
                                                                 arch,
                                                                 installation ? installation : "",
                                                                 cancellable,
                                                                 error))
            return FALSE;
        }

      (void) flatpak_rm_rf (ostree_repo_get_path (child_repo),
                            NULL, NULL);

      return TRUE;
    }

  if (!flatpak_dir_pull (self, remote, branch, NULL, NULL, NULL, FLATPAK_PULL_FLAGS_NONE, OSTREE_REPO_PULL_FLAGS_NONE, progress,
                         cancellable, error))
    return FALSE;

  remote_and_branch = g_strdup_printf ("%s:%s", remote, branch);

  if (!ostree_repo_resolve_rev (self->repo, remote_and_branch, TRUE, &new_checksum, error))
    return FALSE;

  if (new_checksum == NULL)
    {
      g_warning ("No appstream branch in remote %s\n", remote);
      return TRUE;
    }

  return flatpak_dir_deploy_appstream (self,
                                       remote,
                                       arch,
                                       out_changed,
                                       cancellable,
                                       error);
}

static void
default_progress_changed (OstreeAsyncProgress *progress,
                          gpointer             user_data)
{
  guint outstanding_extra_data;
  guint64 transferred_extra_data_bytes;
  guint64 total_extra_data_bytes;

  outstanding_extra_data = ostree_async_progress_get_uint (progress, "outstanding-extra-data");
  total_extra_data_bytes = ostree_async_progress_get_uint64 (progress, "total-extra-data-bytes");
  transferred_extra_data_bytes = ostree_async_progress_get_uint64 (progress, "transferred-extra-data-bytes");

  if (outstanding_extra_data > 0)
    {
      g_autofree char *transferred = g_format_size (transferred_extra_data_bytes);
      g_autofree char *total = g_format_size (total_extra_data_bytes);
      g_autofree char *line = g_strdup_printf ("Downloading extra data %s/%s", transferred, total);
      glnx_console_text (line);
    }
  else
    ostree_repo_pull_default_console_progress_changed (progress, user_data);
}

/* This is a copy of ostree_repo_pull_one_dir that always disables
   static deltas if subdir is used */
static gboolean
repo_pull_one_dir (OstreeRepo          *self,
                   const char          *remote_name,
                   const char         **dirs_to_pull,
                   const char          *ref_to_fetch,
                   const char          *rev_to_fetch,
                   OstreeRepoPullFlags  flags,
                   OstreeAsyncProgress *progress,
                   GCancellable        *cancellable,
                   GError             **error)
{
  GVariantBuilder builder;
  gboolean force_disable_deltas = FALSE;
  g_autoptr(GVariant) options = NULL;
  const char *refs_to_fetch[2];
  const char *revs_to_fetch[2];
  gboolean res;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

  if (dirs_to_pull)
    {
      g_variant_builder_add (&builder, "{s@v}", "subdirs",
                             g_variant_new_variant (g_variant_new_strv ((const char * const *)dirs_to_pull, -1)));
      force_disable_deltas = TRUE;
    }

  if (force_disable_deltas)
    g_variant_builder_add (&builder, "{s@v}", "disable-static-deltas",
                           g_variant_new_variant (g_variant_new_boolean (TRUE)));

  g_variant_builder_add (&builder, "{s@v}", "inherit-transaction",
                         g_variant_new_variant (g_variant_new_boolean (TRUE)));

  g_variant_builder_add (&builder, "{s@v}", "flags",
                         g_variant_new_variant (g_variant_new_int32 (flags)));

  refs_to_fetch[0] = ref_to_fetch;
  refs_to_fetch[1] = NULL;
  g_variant_builder_add (&builder, "{s@v}", "refs",
                         g_variant_new_variant (g_variant_new_strv ((const char * const *) refs_to_fetch, -1)));

  revs_to_fetch[0] = rev_to_fetch;
  revs_to_fetch[1] = NULL;
  g_variant_builder_add (&builder, "{s@v}", "override-commit-ids",
                         g_variant_new_variant (g_variant_new_strv ((const char * const *) revs_to_fetch, -1)));

  options = g_variant_ref_sink (g_variant_builder_end (&builder));
  res = ostree_repo_pull_with_options (self, remote_name, options,
                                       progress, cancellable, error);

  return res;
}

static void
ensure_soup_session (FlatpakDir *self)
{
  if (g_once_init_enter (&self->soup_session))
    {
      SoupSession *soup_session;

      soup_session = flatpak_create_soup_session ("ostree");

      if (g_getenv ("OSTREE_DEBUG_HTTP"))
        soup_session_add_feature (soup_session, (SoupSessionFeature *) soup_logger_new (SOUP_LOGGER_LOG_BODY, 500));

      g_once_init_leave (&self->soup_session, soup_session);
    }
}

typedef struct {
  OstreeAsyncProgress *progress;
  guint64 previous_dl;
} ExtraDataProgress;

static void
extra_data_progress_report (guint64 downloaded_bytes,
                            gpointer user_data)
{
  ExtraDataProgress *extra_progress = user_data;

  if (extra_progress->progress)
    ostree_async_progress_set_uint64 (extra_progress->progress, "transferred-extra-data-bytes",
                                      extra_progress->previous_dl + downloaded_bytes);

}

static gboolean
flatpak_dir_pull_extra_data (FlatpakDir          *self,
                             OstreeRepo          *repo,
                             const char          *repository,
                             const char          *ref,
                             const char          *rev,
                             FlatpakPullFlags     flatpak_flags,
                             OstreeAsyncProgress *progress,
                             GCancellable        *cancellable,
                             GError             **error)
{
  g_autoptr(GVariant) extra_data_sources = NULL;
  g_autoptr(GVariant) detached_metadata = NULL;
  g_auto(GVariantDict) new_metadata_dict = FLATPAK_VARIANT_DICT_INITIALIZER;
  g_autoptr(GVariantBuilder) extra_data_builder = NULL;
  g_autoptr(GVariant) new_detached_metadata = NULL;
  g_autoptr(GVariant) extra_data = NULL;
  int i;
  gsize n_extra_data;
  guint64 total_download_size;
  ExtraDataProgress extra_data_progress = { NULL };

  extra_data_sources = flatpak_repo_get_extra_data_sources (repo, rev, cancellable, NULL);
  if (extra_data_sources == NULL)
    return TRUE;

  n_extra_data = g_variant_n_children (extra_data_sources);
  if (n_extra_data == 0)
    return TRUE;

  if ((flatpak_flags & FLATPAK_PULL_FLAGS_DOWNLOAD_EXTRA_DATA) == 0)
    return flatpak_fail (error, "extra data not supported for non-gpg-verified local system installs");

  extra_data_builder = g_variant_builder_new (G_VARIANT_TYPE ("a(ayay)"));

  total_download_size = 0;
  for (i = 0; i < n_extra_data; i++)
    {
      guint64 download_size;

      flatpak_repo_parse_extra_data_sources (extra_data_sources, i,
                                             NULL,
                                             &download_size,
                                             NULL,
                                             NULL,
                                             NULL);

      total_download_size += download_size;
    }

  if (progress)
    {
      ostree_async_progress_set_uint64 (progress, "start-time-extra-data", g_get_monotonic_time ());
      ostree_async_progress_set_uint (progress, "outstanding-extra-data", n_extra_data);
      ostree_async_progress_set_uint (progress, "total-extra-data", n_extra_data);
      ostree_async_progress_set_uint64 (progress, "total-extra-data-bytes", total_download_size);
      ostree_async_progress_set_uint64 (progress, "transferred-extra-data-bytes", 0);
    }

  extra_data_progress.progress = progress;

  for (i = 0; i < n_extra_data; i++)
    {
      const char *extra_data_uri = NULL;
      g_autofree char *extra_data_sha256 = NULL;
      const char *extra_data_name = NULL;
      guint64 download_size;
      guint64 installed_size;
      g_autofree char *sha256 = NULL;
      const guchar *sha256_bytes;
      g_autoptr(GBytes) bytes = NULL;

      flatpak_repo_parse_extra_data_sources (extra_data_sources, i,
                                             &extra_data_name,
                                             &download_size,
                                             &installed_size,
                                             &sha256_bytes,
                                             &extra_data_uri);

      if (sha256_bytes == NULL)
        return flatpak_fail (error, _("Invalid sha256 for extra data uri %s"), extra_data_uri);

      extra_data_sha256 = ostree_checksum_from_bytes (sha256_bytes);

      if (*extra_data_name == 0)
        return flatpak_fail (error, _("Empty name for extra data uri %s"), extra_data_uri);

      /* Don't allow file uris here as that could read local files based on remote data */
      if (!g_str_has_prefix (extra_data_uri, "http:") &&
          !g_str_has_prefix (extra_data_uri, "https:"))
        return flatpak_fail (error, _("Unsupported extra data uri %s"), extra_data_uri);

      /* TODO: Download to disk to support resumed downloads on error */

      ensure_soup_session (self);
      bytes = flatpak_load_http_uri (self->soup_session, extra_data_uri,
                                     extra_data_progress_report, &extra_data_progress,
                                     cancellable, error);
      if (bytes == NULL)
        {
          g_prefix_error (error, _("While downloading %s: "), extra_data_uri);
          return FALSE;
        }

      if (g_bytes_get_size (bytes) != download_size)
        return flatpak_fail (error, _("Wrong size for extra data %s"), extra_data_uri);

      extra_data_progress.previous_dl += download_size;
      if (progress)
        ostree_async_progress_set_uint (progress, "outstanding-extra-data", n_extra_data - i - 1);

      sha256 = g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, bytes);
      if (strcmp (sha256, extra_data_sha256) != 0)
        return flatpak_fail (error, _("Invalid checksum for extra data %s"), extra_data_uri);

      g_variant_builder_add (extra_data_builder,
                             "(^ay@ay)",
                             extra_data_name,
                             g_variant_new_from_bytes (G_VARIANT_TYPE ("ay"), bytes, TRUE));
    }

  extra_data = g_variant_ref_sink (g_variant_builder_end (extra_data_builder));

  if (!ostree_repo_read_commit_detached_metadata (repo, rev, &detached_metadata,
                                                  cancellable, error))
    return FALSE;

  g_variant_dict_init (&new_metadata_dict, detached_metadata);
  g_variant_dict_insert_value (&new_metadata_dict, "xa.extra-data", extra_data);
  new_detached_metadata = g_variant_dict_end (&new_metadata_dict);

  /* There is a commitmeta size limit when pulling, so we have to side-load it
     when installing in the system repo */
  if (flatpak_flags & FLATPAK_PULL_FLAGS_SIDELOAD_EXTRA_DATA)
    {
      int dfd =  ostree_repo_get_dfd (repo);
      g_autoptr(GVariant) normalized = g_variant_get_normal_form (new_detached_metadata);
      gsize normalized_size = g_variant_get_size (normalized);
      const guint8 *data = g_variant_get_data (normalized);
      g_autofree char *filename = NULL;

      filename = g_strconcat (rev, ".commitmeta", NULL);
      if (!glnx_file_replace_contents_at (dfd, filename,
                                          data, normalized_size,
                                          0, cancellable, error))
        {
          g_prefix_error (error, "Unable to write sideloaded detached metadata: ");
          return FALSE;
        }
    }
  else
    {
      if (!ostree_repo_write_commit_detached_metadata (repo, rev, new_detached_metadata,
                                                       cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
flatpak_dir_pull_oci (FlatpakDir          *self,
                      const char          *remote,
                      const char          *ref,
                      OstreeRepo          *repo,
                      FlatpakPullFlags     flatpak_flags,
                      OstreeRepoPullFlags  flags,
                      OstreeAsyncProgress *progress,
                      GCancellable        *cancellable,
                      GError             **error)
{
  GHashTable *annotations;
  g_autoptr(FlatpakOciManifest) manifest = NULL;
  g_autoptr(FlatpakOciRegistry) registry = NULL;
  g_autofree char *oci_ref = NULL;
  g_autofree char *oci_uri = NULL;
  g_autofree char *oci_tag = NULL;
  g_autofree char *oci_digest = NULL;
  g_autofree char *full_ref = NULL;
  g_autofree char *checksum = NULL;

  oci_uri = flatpak_dir_get_remote_oci_uri (self, remote);
  g_assert (oci_uri != NULL);

  oci_tag = flatpak_dir_get_remote_oci_tag (self, remote);
  if (oci_tag == NULL)
    oci_tag = g_strdup ("latest");

  registry = flatpak_oci_registry_new (oci_uri, FALSE, -1, NULL, error);
  if (registry == NULL)
    return FALSE;

  manifest = flatpak_oci_registry_chose_image (registry, oci_tag, &oci_digest,
                                               NULL, error);
  if (manifest == NULL)
    return FALSE;

  annotations = flatpak_oci_manifest_get_annotations (manifest);
  if (annotations)
    flatpak_oci_parse_commit_annotations (annotations, NULL,
                                          NULL, NULL,
                                          &oci_ref, NULL, NULL,
                                          NULL);

  if (oci_ref == NULL)
    return flatpak_fail (error, _("OCI image is not a flatpak (missing ref)"));

  if (strcmp (ref, oci_ref) != 0)
    return flatpak_fail (error, _("OCI image specifies the wrong app id"));

  full_ref = g_strdup_printf ("%s:%s", remote, ref);

  if (repo == NULL)
    repo = self->repo;

  checksum = flatpak_pull_from_oci (repo, registry, oci_digest, manifest, full_ref, cancellable, error);
  if (checksum == NULL)
    return FALSE;

  g_debug ("Imported OCI image as checksum %s\n", checksum);

  return TRUE;
}

gboolean
flatpak_dir_pull (FlatpakDir          *self,
                  const char          *repository,
                  const char          *ref,
                  const char          *opt_rev,
                  const char         **subpaths,
                  OstreeRepo          *repo,
                  FlatpakPullFlags     flatpak_flags,
                  OstreeRepoPullFlags  flags,
                  OstreeAsyncProgress *progress,
                  GCancellable        *cancellable,
                  GError             **error)
{
  gboolean ret = FALSE;
  const char *rev;
  g_autofree char *url = NULL;
  g_autoptr(GBytes) summary_bytes = NULL;
  g_autofree char *latest_rev = NULL;
  g_autofree char *oci_uri = NULL;
  g_auto(GLnxConsoleRef) console = { 0, };
  g_autoptr(OstreeAsyncProgress) console_progress = NULL;
  g_autoptr(GPtrArray) subdirs_arg = NULL;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  oci_uri = flatpak_dir_get_remote_oci_uri (self, repository);
  if (oci_uri != NULL)
    return flatpak_dir_pull_oci (self, repository, ref, repo, flatpak_flags,
                                 flags, progress, cancellable, error);

  if (!ostree_repo_remote_get_url (self->repo,
                                   repository,
                                   &url,
                                   error))
    return FALSE;

  if (*url == 0)
    return TRUE; /* Empty url, silently disables updates */

  /* We get the rev ahead of time so that we know it for looking up e.g. extra-data
     and to make sure we're atomically using a single rev if we happen to do multiple
     pulls (e.g. with subpaths) */
  if (opt_rev != NULL)
    rev = opt_rev;
  else
    {
      g_autoptr(GVariant) summary = NULL;

      if (!flatpak_dir_remote_fetch_summary (self, repository,
                                             &summary_bytes,
                                             cancellable, error))
        return FALSE;

      summary = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                                              summary_bytes, FALSE));
      if (!flatpak_summary_lookup_ref (summary,
                                       ref,
                                       &latest_rev))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "No such branch '%s' in repository summary",
                       ref);
          return FALSE;
        }

      rev = latest_rev;
    }

  if (repo == NULL)
    repo = self->repo;

  if (progress == NULL)
    {
      glnx_console_lock (&console);
      if (console.is_tty)
        {
          console_progress = ostree_async_progress_new_and_connect (default_progress_changed, &console);
          progress = console_progress;
        }
    }

  /* Past this we must use goto out, so we clean up console and
     abort the transaction on error */

  if (subpaths != NULL && subpaths[0] != NULL)
    {
      subdirs_arg = g_ptr_array_new_with_free_func (g_free);
      int i;
      g_ptr_array_add (subdirs_arg, g_strdup ("/metadata"));
      for (i = 0; subpaths[i] != NULL; i++)
        g_ptr_array_add (subdirs_arg,
                         g_build_filename ("/files", subpaths[i], NULL));
      g_ptr_array_add (subdirs_arg, NULL);
    }

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  if (!repo_pull_one_dir (repo, repository,
                          subdirs_arg ? (const char **)subdirs_arg->pdata : NULL,
                          ref, rev, flags,
                          progress,
                          cancellable, error))
    {
      g_prefix_error (error, _("While pulling %s from remote %s: "), ref, repository);
      goto out;
    }

  if (!flatpak_dir_pull_extra_data (self, repo,
                                    repository,
                                    ref, rev,
                                    flatpak_flags,
                                    progress,
                                    cancellable,
                                    error))
    goto out;


  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    goto out;

  ret = TRUE;

out:
  if (!ret)
    ostree_repo_abort_transaction (repo, cancellable, NULL);

  if (progress)
    ostree_async_progress_finish (progress);

  return ret;
}

static gboolean
repo_pull_one_untrusted (OstreeRepo          *self,
                         const char          *remote_name,
                         const char          *url,
                         const char         **dirs_to_pull,
                         const char          *ref,
                         const char          *checksum,
                         OstreeAsyncProgress *progress,
                         GCancellable        *cancellable,
                         GError             **error)
{
  OstreeRepoPullFlags flags = OSTREE_REPO_PULL_FLAGS_UNTRUSTED;
  GVariantBuilder builder;
  g_auto(GLnxConsoleRef) console = { 0, };
  g_autoptr(OstreeAsyncProgress) console_progress = NULL;
  gboolean res;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  const char *refs[2] = { NULL, NULL };
  const char *commits[2] = { NULL, NULL };

  if (progress == NULL)
    {
      glnx_console_lock (&console);
      if (console.is_tty)
        {
          console_progress = ostree_async_progress_new_and_connect (default_progress_changed, &console);
          progress = console_progress;
        }
    }

  refs[0] = ref;
  commits[0] = checksum;

  g_variant_builder_add (&builder, "{s@v}", "flags",
                         g_variant_new_variant (g_variant_new_int32 (flags)));
  g_variant_builder_add (&builder, "{s@v}", "refs",
                         g_variant_new_variant (g_variant_new_strv ((const char * const *) refs, -1)));
  g_variant_builder_add (&builder, "{s@v}", "override-commit-ids",
                         g_variant_new_variant (g_variant_new_strv ((const char * const *) commits, -1)));
  g_variant_builder_add (&builder, "{s@v}", "override-remote-name",
                         g_variant_new_variant (g_variant_new_string (remote_name)));
  g_variant_builder_add (&builder, "{s@v}", "gpg-verify",
                         g_variant_new_variant (g_variant_new_boolean (TRUE)));
  g_variant_builder_add (&builder, "{s@v}", "gpg-verify-summary",
                         g_variant_new_variant (g_variant_new_boolean (TRUE)));
  g_variant_builder_add (&builder, "{s@v}", "inherit-transaction",
                         g_variant_new_variant (g_variant_new_boolean (TRUE)));

  if (dirs_to_pull)
    {
      g_variant_builder_add (&builder, "{s@v}", "subdirs",
                             g_variant_new_variant (g_variant_new_strv ((const char * const *)dirs_to_pull, -1)));
      g_variant_builder_add (&builder, "{s@v}", "disable-static-deltas",
                             g_variant_new_variant (g_variant_new_boolean (TRUE)));
    }

  res = ostree_repo_pull_with_options (self, url, g_variant_builder_end (&builder),
                                       progress, cancellable, error);

  if (progress)
    ostree_async_progress_finish (progress);

  return res;
}

gboolean
flatpak_dir_pull_untrusted_local (FlatpakDir          *self,
                                  const char          *src_path,
                                  const char          *remote_name,
                                  const char          *ref,
                                  const char         **subpaths,
                                  OstreeAsyncProgress *progress,
                                  GCancellable        *cancellable,
                                  GError             **error)
{
  g_autoptr(GFile) path_file = g_file_new_for_path (src_path);
  g_autoptr(GFile) summary_file = g_file_get_child (path_file, "summary");
  g_autoptr(GFile) summary_sig_file = g_file_get_child (path_file, "summary.sig");
  g_autofree char *url = g_file_get_uri (path_file);
  g_autofree char *checksum = NULL;
  gboolean gpg_verify_summary;
  gboolean gpg_verify;
  char *summary_data = NULL;
  char *summary_sig_data = NULL;
  gsize summary_data_size, summary_sig_data_size;
  g_autoptr(GBytes) summary_bytes = NULL;
  g_autoptr(GBytes) summary_sig_bytes = NULL;
  g_autoptr(OstreeGpgVerifyResult) gpg_result = NULL;
  g_autoptr(GVariant) summary = NULL;
  g_autoptr(GVariant) old_commit = NULL;
  g_autoptr(OstreeRepo) src_repo = NULL;
  g_autoptr(GVariant) new_commit = NULL;
  g_autoptr(GVariant) extra_data_sources = NULL;
  g_autoptr(GPtrArray) subdirs_arg = NULL;
  gboolean ret = FALSE;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  if (!ostree_repo_remote_get_gpg_verify_summary (self->repo, remote_name,
                                                  &gpg_verify_summary, error))
    return FALSE;

  if (!ostree_repo_remote_get_gpg_verify (self->repo, remote_name,
                                          &gpg_verify, error))
    return FALSE;

  /* This was verified in the client, but lets do it here too */
  if (!gpg_verify_summary || !gpg_verify)
    return flatpak_fail (error, "Can't pull from untrusted non-gpg verified remote");

  /* We verify the summary manually before anything else to make sure
     we've got something right before looking too hard at the repo and
     so we can check for a downgrade before pulling and updating the
     ref */

  if (!g_file_load_contents (summary_sig_file, cancellable,
                             &summary_sig_data, &summary_sig_data_size, NULL, NULL))
    return flatpak_fail (error, "GPG verification enabled, but no summary signatures found");

  summary_sig_bytes = g_bytes_new_take (summary_sig_data, summary_sig_data_size);

  if (!g_file_load_contents (summary_file, cancellable,
                             &summary_data, &summary_data_size, NULL, NULL))
    return flatpak_fail (error, "No summary found");
  summary_bytes = g_bytes_new_take (summary_data, summary_data_size);

  gpg_result = ostree_repo_verify_summary (self->repo,
                                           remote_name,
                                           summary_bytes,
                                           summary_sig_bytes,
                                           cancellable, error);
  if (gpg_result == NULL)
    return FALSE;

  if (ostree_gpg_verify_result_count_valid (gpg_result) == 0)
    return flatpak_fail (error, "GPG signatures found, but none are in trusted keyring");

  summary = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, summary_bytes, FALSE));
  if (!flatpak_summary_lookup_ref (summary,
                                   ref,
                                   &checksum))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   _("Can't find %s in remote %s"), ref, remote_name);
      return FALSE;
    }

  (void) ostree_repo_load_commit (self->repo, checksum, &old_commit, NULL, NULL);

  src_repo = ostree_repo_new (path_file);
  if (!ostree_repo_open (src_repo, cancellable, error))
    return FALSE;

  if (!ostree_repo_load_commit (src_repo, checksum, &new_commit, NULL, error))
    return FALSE;

  if (old_commit)
    {
      guint64 old_timestamp;
      guint64 new_timestamp;

      old_timestamp = ostree_commit_get_timestamp (old_commit);
      new_timestamp = ostree_commit_get_timestamp (new_commit);

      if (new_timestamp < old_timestamp)
        return flatpak_fail (error, "Not allowed to downgrade %s", ref);
    }

  if (subpaths != NULL && subpaths[0] != NULL)
    {
      subdirs_arg = g_ptr_array_new_with_free_func (g_free);
      int i;
      g_ptr_array_add (subdirs_arg, g_strdup ("/metadata"));
      for (i = 0; subpaths[i] != NULL; i++)
        g_ptr_array_add (subdirs_arg,
                         g_build_filename ("/files", subpaths[i], NULL));
      g_ptr_array_add (subdirs_arg, NULL);
    }

  if (!ostree_repo_prepare_transaction (self->repo, NULL, cancellable, error))
    goto out;

  /* Past this we must use goto out, so we abort the transaction on error */

  if (!repo_pull_one_untrusted (self->repo, remote_name, url,
                                subdirs_arg ? (const char **)subdirs_arg->pdata : NULL,
                                ref, checksum, progress,
                                cancellable, error))
    {
      g_prefix_error (error, _("While pulling %s from remote %s: "), ref, remote_name);
      goto out;
    }

  /* Get the out of bands extra-data required due to an ostree pull
     commitmeta size limit */
  extra_data_sources = flatpak_commit_get_extra_data_sources (new_commit, NULL);
  if (extra_data_sources)
    {
      GFile *dir = ostree_repo_get_path (src_repo);
      g_autoptr(GFile) file = NULL;
      g_autofree char *filename = NULL;
      g_autofree char *commitmeta = NULL;
      gsize commitmeta_size;
      g_autoptr(GVariant) new_metadata = NULL;

      filename = g_strconcat (checksum, ".commitmeta", NULL);
      file = g_file_get_child (dir, filename);
      if (!g_file_load_contents (file, cancellable,
                                 &commitmeta, &commitmeta_size,
                                 NULL, error))
        goto out;

      new_metadata = g_variant_ref_sink (g_variant_new_from_data (G_VARIANT_TYPE ("a{sv}"),
                                                                  commitmeta, commitmeta_size,
                                                                  FALSE,
                                                                  g_free, commitmeta));
      g_steal_pointer (&commitmeta); /* steal into the variant */

      if (!ostree_repo_write_commit_detached_metadata (self->repo, checksum, new_metadata, cancellable, error))
        goto out;
    }

  if (!ostree_repo_commit_transaction (self->repo, NULL, cancellable, error))
    goto out;

  ret = TRUE;

out:
  if (!ret)
    ostree_repo_abort_transaction (self->repo, cancellable, NULL);

  if (progress)
    ostree_async_progress_finish (progress);

  return ret;
}

char *
flatpak_dir_current_ref (FlatpakDir   *self,
                         const char   *name,
                         GCancellable *cancellable)
{
  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) dir = NULL;
  g_autoptr(GFile) current_link = NULL;
  g_autoptr(GFileInfo) file_info = NULL;

  base = g_file_get_child (flatpak_dir_get_path (self), "app");
  dir = g_file_get_child (base, name);

  current_link = g_file_get_child (dir, "current");

  file_info = g_file_query_info (current_link, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, NULL);
  if (file_info == NULL)
    return NULL;

  return g_strconcat ("app/", name, "/", g_file_info_get_symlink_target (file_info), NULL);
}

gboolean
flatpak_dir_drop_current_ref (FlatpakDir   *self,
                              const char   *name,
                              GCancellable *cancellable,
                              GError      **error)
{
  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) dir = NULL;
  g_autoptr(GFile) current_link = NULL;
  g_auto(GStrv) refs = NULL;
  g_autofree char *current_ref = NULL;
  const char *other_ref = NULL;

  base = g_file_get_child (flatpak_dir_get_path (self), "app");
  dir = g_file_get_child (base, name);

  current_ref = flatpak_dir_current_ref (self, name, cancellable);

  if (flatpak_dir_list_refs_for_name (self, "app", name, &refs, cancellable, NULL))
    {
      int i;
      for (i = 0; refs[i] != NULL; i++)
        {
          if (g_strcmp0 (refs[i], current_ref) != 0)
            {
              other_ref = refs[i];
              break;
            }
        }
    }

  current_link = g_file_get_child (dir, "current");

  if (!g_file_delete (current_link, cancellable, error))
    return FALSE;

  if (other_ref)
    {
      if (!flatpak_dir_make_current_ref (self, other_ref, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_dir_make_current_ref (FlatpakDir   *self,
                              const char   *ref,
                              GCancellable *cancellable,
                              GError      **error)
{
  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) dir = NULL;
  g_autoptr(GFile) current_link = NULL;
  g_auto(GStrv) ref_parts = NULL;
  g_autofree char *rest = NULL;
  gboolean ret = FALSE;

  ref_parts = g_strsplit (ref, "/", -1);

  g_assert (g_strv_length (ref_parts) == 4);
  g_assert (strcmp (ref_parts[0], "app") == 0);

  base = g_file_get_child (flatpak_dir_get_path (self), ref_parts[0]);
  dir = g_file_get_child (base, ref_parts[1]);

  current_link = g_file_get_child (dir, "current");

  g_file_delete (current_link, cancellable, NULL);

  if (*ref_parts[3] != 0)
    {
      rest = g_strdup_printf ("%s/%s", ref_parts[2], ref_parts[3]);
      if (!g_file_make_symbolic_link (current_link, rest, cancellable, error))
        goto out;
    }

  ret = TRUE;

out:
  return ret;
}

gboolean
flatpak_dir_list_refs_for_name (FlatpakDir   *self,
                                const char   *kind,
                                const char   *name,
                                char       ***refs_out,
                                GCancellable *cancellable,
                                GError      **error)
{
  gboolean ret = FALSE;

  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) dir = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  GError *temp_error = NULL;
  g_autoptr(GPtrArray) refs = NULL;

  base = g_file_get_child (flatpak_dir_get_path (self), kind);
  dir = g_file_get_child (base, name);

  refs = g_ptr_array_new ();

  if (!g_file_query_exists (dir, cancellable))
    {
      ret = TRUE;
      goto out;
    }

  dir_enum = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)))
    {
      g_autoptr(GFile) child = NULL;
      g_autoptr(GFileEnumerator) dir_enum2 = NULL;
      g_autoptr(GFileInfo) child_info2 = NULL;
      const char *arch;

      arch = g_file_info_get_name (child_info);

      if (g_file_info_get_file_type (child_info) != G_FILE_TYPE_DIRECTORY ||
          strcmp (arch, "data") == 0 /* There used to be a data dir here, lets ignore it */)
        {
          g_clear_object (&child_info);
          continue;
        }

      child = g_file_get_child (dir, arch);
      g_clear_object (&dir_enum2);
      dir_enum2 = g_file_enumerate_children (child, OSTREE_GIO_FAST_QUERYINFO,
                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                             cancellable, error);
      if (!dir_enum2)
        goto out;

      while ((child_info2 = g_file_enumerator_next_file (dir_enum2, cancellable, &temp_error)))
        {
          const char *branch;

          if (g_file_info_get_file_type (child_info2) == G_FILE_TYPE_DIRECTORY)
            {
              branch = g_file_info_get_name (child_info2);
              g_ptr_array_add (refs,
                               g_strdup_printf ("%s/%s/%s/%s", kind, name, arch, branch));
            }

          g_clear_object (&child_info2);
        }


      if (temp_error != NULL)
        goto out;

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    goto out;

  g_ptr_array_sort (refs, flatpak_strcmp0_ptr);

  ret = TRUE;

out:
  if (ret)
    {
      g_ptr_array_add (refs, NULL);
      *refs_out = (char **) g_ptr_array_free (refs, FALSE);
      refs = NULL;
    }

  if (temp_error != NULL)
    g_propagate_error (error, temp_error);

  return ret;
}

gboolean
flatpak_dir_list_refs (FlatpakDir   *self,
                       const char   *kind,
                       char       ***refs_out,
                       GCancellable *cancellable,
                       GError      **error)
{
  gboolean ret = FALSE;

  g_autoptr(GFile) base = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  GError *temp_error = NULL;
  g_autoptr(GPtrArray) refs = NULL;

  refs = g_ptr_array_new ();

  base = g_file_get_child (flatpak_dir_get_path (self), kind);

  if (!g_file_query_exists (base, cancellable))
    {
      ret = TRUE;
      goto out;
    }

  dir_enum = g_file_enumerate_children (base, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)))
    {
      gchar **sub_refs = NULL;
      const char *name;
      int i;

      if (g_file_info_get_file_type (child_info) != G_FILE_TYPE_DIRECTORY)
        {
          g_clear_object (&child_info);
          continue;
        }

      name = g_file_info_get_name (child_info);

      if (!flatpak_dir_list_refs_for_name (self, kind, name, &sub_refs, cancellable, error))
        goto out;

      for (i = 0; sub_refs[i] != NULL; i++)
        g_ptr_array_add (refs, sub_refs[i]);
      g_free (sub_refs);

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    goto out;

  ret = TRUE;

  g_ptr_array_sort (refs, flatpak_strcmp0_ptr);

out:
  if (ret)
    {
      g_ptr_array_add (refs, NULL);
      *refs_out = (char **) g_ptr_array_free (refs, FALSE);
      refs = NULL;
    }

  if (temp_error != NULL)
    g_propagate_error (error, temp_error);

  return ret;
}

char *
flatpak_dir_read_latest (FlatpakDir   *self,
                         const char   *remote,
                         const char   *ref,
                         char        **out_alt_id,
                         GCancellable *cancellable,
                         GError      **error)
{
  g_autofree char *remote_and_ref = NULL;
  g_autofree char *alt_id = NULL;
  char *res = NULL;

  /* There may be several remotes with the same branch (if we for
   * instance changed the origin, so prepend the current origin to
   * make sure we get the right one */

  if (remote)
    remote_and_ref = g_strdup_printf ("%s:%s", remote, ref);
  else
    remote_and_ref = g_strdup (ref);

  if (!ostree_repo_resolve_rev (self->repo, remote_and_ref, FALSE, &res, error))
    return NULL;

  if (out_alt_id)
    {
      g_autoptr(GVariant) commit_data = NULL;
      g_autoptr(GVariant) commit_metadata = NULL;

      if (!ostree_repo_load_commit (self->repo, res, &commit_data, NULL, error))
        return FALSE;

      commit_metadata = g_variant_get_child_value (commit_data, 0);
      g_variant_lookup (commit_metadata, "xa.alt-id", "s", &alt_id);

      *out_alt_id = g_steal_pointer (&alt_id);
     }

  return res;
}


char *
flatpak_dir_read_active (FlatpakDir   *self,
                         const char   *ref,
                         GCancellable *cancellable)
{
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) active_link = NULL;
  g_autoptr(GFileInfo) file_info = NULL;

  deploy_base = flatpak_dir_get_deploy_dir (self, ref);
  active_link = g_file_get_child (deploy_base, "active");

  file_info = g_file_query_info (active_link, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, NULL);
  if (file_info == NULL)
    return NULL;

  return g_strdup (g_file_info_get_symlink_target (file_info));
}

gboolean
flatpak_dir_set_active (FlatpakDir   *self,
                        const char   *ref,
                        const char   *checksum,
                        GCancellable *cancellable,
                        GError      **error)
{
  gboolean ret = FALSE;

  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) active_tmp_link = NULL;
  g_autoptr(GFile) active_link = NULL;
  g_autoptr(GError) my_error = NULL;
  g_autofree char *tmpname = g_strdup (".active-XXXXXX");

  deploy_base = flatpak_dir_get_deploy_dir (self, ref);
  active_link = g_file_get_child (deploy_base, "active");

  if (checksum != NULL)
    {
      glnx_gen_temp_name (tmpname);
      active_tmp_link = g_file_get_child (deploy_base, tmpname);
      if (!g_file_make_symbolic_link (active_tmp_link, checksum, cancellable, error))
        goto out;

      if (!flatpak_file_rename (active_tmp_link,
                                active_link,
                                cancellable, error))
        goto out;
    }
  else
    {
      if (!g_file_delete (active_link, cancellable, &my_error) &&
          !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_propagate_error (error, my_error);
          my_error = NULL;
          goto out;
        }
    }

  ret = TRUE;
out:
  return ret;
}


gboolean
flatpak_dir_run_triggers (FlatpakDir   *self,
                          GCancellable *cancellable,
                          GError      **error)
{
  gboolean ret = FALSE;

  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  g_autoptr(GFile) triggersdir = NULL;
  GError *temp_error = NULL;
  const char *triggerspath;

  triggerspath = g_getenv ("FLATPAK_TRIGGERSDIR");
  if (triggerspath == NULL)
    triggerspath = FLATPAK_TRIGGERDIR;

  g_debug ("running triggers from %s", triggerspath);

  triggersdir = g_file_new_for_path (triggerspath);

  dir_enum = g_file_enumerate_children (triggersdir, "standard::type,standard::name",
                                        0, cancellable, error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      g_autoptr(GFile) child = NULL;
      const char *name;
      GError *trigger_error = NULL;

      name = g_file_info_get_name (child_info);

      child = g_file_get_child (triggersdir, name);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_REGULAR &&
          g_str_has_suffix (name, ".trigger"))
        {
          g_autoptr(GPtrArray) argv_array = NULL;
          /* We need to canonicalize the basedir, because if has a symlink
             somewhere the bind mount will be on the target of that, not
             at that exact path. */
          g_autofree char *basedir_orig = g_file_get_path (self->basedir);
          g_autofree char *basedir = canonicalize_file_name (basedir_orig);

          g_debug ("running trigger %s", name);

          argv_array = g_ptr_array_new_with_free_func (g_free);
#ifdef DISABLE_SANDBOXED_TRIGGERS
          g_ptr_array_add (argv_array, g_file_get_path (child));
          g_ptr_array_add (argv_array, g_strdup (basedir));
#else
          g_ptr_array_add (argv_array, g_strdup (flatpak_get_bwrap ()));
          g_ptr_array_add (argv_array, g_strdup ("--unshare-ipc"));
          g_ptr_array_add (argv_array, g_strdup ("--unshare-net"));
          g_ptr_array_add (argv_array, g_strdup ("--unshare-pid"));
          g_ptr_array_add (argv_array, g_strdup ("--ro-bind"));
          g_ptr_array_add (argv_array, g_strdup ("/"));
          g_ptr_array_add (argv_array, g_strdup ("/"));
          g_ptr_array_add (argv_array, g_strdup ("--proc"));
          g_ptr_array_add (argv_array, g_strdup ("/proc"));
          g_ptr_array_add (argv_array, g_strdup ("--dev"));
          g_ptr_array_add (argv_array, g_strdup ("/dev"));
          g_ptr_array_add (argv_array, g_strdup ("--bind"));
          g_ptr_array_add (argv_array, g_strdup (basedir));
          g_ptr_array_add (argv_array, g_strdup (basedir));
#endif
          g_ptr_array_add (argv_array, g_file_get_path (child));
          g_ptr_array_add (argv_array, g_strdup (basedir));
          g_ptr_array_add (argv_array, NULL);

          if (!g_spawn_sync ("/",
                             (char **) argv_array->pdata,
                             NULL,
                             G_SPAWN_SEARCH_PATH,
                             NULL, NULL,
                             NULL, NULL,
                             NULL, &trigger_error))
            {
              g_warning ("Error running trigger %s: %s", name, trigger_error->message);
              g_clear_error (&trigger_error);
            }
        }

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
out:
  return ret;
}

static gboolean
read_fd (int          fd,
         struct stat *stat_buf,
         gchar      **contents,
         gsize       *length,
         GError     **error)
{
  gchar *buf;
  gsize bytes_read;
  gsize size;
  gsize alloc_size;

  size = stat_buf->st_size;

  alloc_size = size + 1;
  buf = g_try_malloc (alloc_size);

  if (buf == NULL)
    {
      g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_NOMEM,
                           _("Not enough memory"));
      return FALSE;
    }

  bytes_read = 0;
  while (bytes_read < size)
    {
      gssize rc;

      rc = read (fd, buf + bytes_read, size - bytes_read);

      if (rc < 0)
        {
          if (errno != EINTR)
            {
              int save_errno = errno;

              g_free (buf);
              g_set_error_literal (error, G_FILE_ERROR, g_file_error_from_errno (save_errno),
                                   _("Failed to read from exported file"));
              return FALSE;
            }
        }
      else if (rc == 0)
        {
          break;
        }
      else
        {
          bytes_read += rc;
        }
    }

  buf[bytes_read] = '\0';

  if (length)
    *length = bytes_read;

  *contents = buf;

  return TRUE;
}

/* This is conservative, but lets us avoid escaping most
   regular Exec= lines, which is nice as that can sometimes
   cause problems for apps launching desktop files. */
static gboolean
need_quotes (const char *str)
{
  const char *p;

  for (p = str; *p; p++)
    {
      if (!g_ascii_isalnum (*p) &&
          strchr ("-_%.=:/@", *p) == NULL)
        return TRUE;
    }

  return FALSE;
}

static char *
maybe_quote (const char *str)
{
  if (need_quotes (str))
    return g_shell_quote (str);
  return g_strdup (str);
}

static gboolean
export_desktop_file (const char   *app,
                     const char   *branch,
                     const char   *arch,
                     GKeyFile     *metadata,
                     int           parent_fd,
                     const char   *name,
                     struct stat  *stat_buf,
                     char        **target,
                     GCancellable *cancellable,
                     GError      **error)
{
  gboolean ret = FALSE;
  glnx_fd_close int desktop_fd = -1;
  g_autofree char *tmpfile_name = g_strdup_printf ("export-desktop-XXXXXX");
  g_autoptr(GOutputStream) out_stream = NULL;
  g_autofree gchar *data = NULL;
  gsize data_len;
  g_autofree gchar *new_data = NULL;
  gsize new_data_len;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree gchar *old_exec = NULL;
  gint old_argc;
  g_auto(GStrv) old_argv = NULL;
  g_auto(GStrv) groups = NULL;
  GString *new_exec = NULL;
  g_autofree char *escaped_app = maybe_quote (app);
  g_autofree char *escaped_branch = maybe_quote (branch);
  g_autofree char *escaped_arch = maybe_quote (arch);
  int i;

  if (!flatpak_openat_noatime (parent_fd, name, &desktop_fd, cancellable, error))
    goto out;

  if (!read_fd (desktop_fd, stat_buf, &data, &data_len, error))
    goto out;

  keyfile = g_key_file_new ();
  if (!g_key_file_load_from_data (keyfile, data, data_len, G_KEY_FILE_KEEP_TRANSLATIONS, error))
    goto out;

  if (g_str_has_suffix (name, ".service"))
    {
      g_autofree gchar *dbus_name = NULL;
      g_autofree gchar *expected_dbus_name = g_strndup (name, strlen (name) - strlen (".service"));

      dbus_name = g_key_file_get_string (keyfile, "D-BUS Service", "Name", NULL);

      if (dbus_name == NULL || strcmp (dbus_name, expected_dbus_name) != 0)
        {
          flatpak_fail (error, "dbus service file %s has wrong name", name);
          return FALSE;
        }
    }

  if (g_str_has_suffix (name, ".desktop"))
    {
      gsize length;
      g_auto(GStrv) tags = g_key_file_get_string_list (metadata,
                                                       "Application",
                                                       "tags", &length,
                                                       NULL);

      if (tags != NULL)
        {
          g_key_file_set_string_list (keyfile,
                                      "Desktop Entry",
                                      "X-Flatpak-Tags",
                                      (const char * const *) tags, length);
        }
    }

  groups = g_key_file_get_groups (keyfile, NULL);

  for (i = 0; groups[i] != NULL; i++)
    {
      g_key_file_remove_key (keyfile, groups[i], "TryExec", NULL);

      /* Remove this to make sure nothing tries to execute it outside the sandbox*/
      g_key_file_remove_key (keyfile, groups[i], "X-GNOME-Bugzilla-ExtraInfoScript", NULL);

      new_exec = g_string_new ("");
      g_string_append_printf (new_exec, FLATPAK_BINDIR "/flatpak run --branch=%s --arch=%s", escaped_branch, escaped_arch);

      old_exec = g_key_file_get_string (keyfile, groups[i], "Exec", NULL);
      if (old_exec && g_shell_parse_argv (old_exec, &old_argc, &old_argv, NULL) && old_argc >= 1)
        {
          int i;
          g_autofree char *command = maybe_quote (old_argv[0]);

          g_string_append_printf (new_exec, " --command=%s", command);

          g_string_append (new_exec, " ");
          g_string_append (new_exec, escaped_app);

          for (i = 1; i < old_argc; i++)
            {
              g_autofree char *arg = maybe_quote (old_argv[i]);
              g_string_append (new_exec, " ");
              g_string_append (new_exec, arg);
            }
        }
      else
        {
          g_string_append (new_exec, " ");
          g_string_append (new_exec, escaped_app);
        }

      g_key_file_set_string (keyfile, groups[i], G_KEY_FILE_DESKTOP_KEY_EXEC, new_exec->str);
    }

  new_data = g_key_file_to_data (keyfile, &new_data_len, error);
  if (new_data == NULL)
    goto out;

  if (!flatpak_open_in_tmpdir_at (parent_fd, 0755, tmpfile_name, &out_stream, cancellable, error))
    goto out;

  if (!g_output_stream_write_all (out_stream, new_data, new_data_len, NULL, cancellable, error))
    goto out;

  if (!g_output_stream_close (out_stream, cancellable, error))
    goto out;

  if (target)
    *target = g_steal_pointer (&tmpfile_name);

  ret = TRUE;
out:

  if (new_exec != NULL)
    g_string_free (new_exec, TRUE);

  return ret;
}

static gboolean
rewrite_export_dir (const char   *app,
                    const char   *branch,
                    const char   *arch,
                    GKeyFile     *metadata,
                    int           source_parent_fd,
                    const char   *source_name,
                    GCancellable *cancellable,
                    GError      **error)
{
  gboolean ret = FALSE;

  g_auto(GLnxDirFdIterator) source_iter = {0};
  g_autoptr(GHashTable) visited_children = NULL;
  struct dirent *dent;

  if (!glnx_dirfd_iterator_init_at (source_parent_fd, source_name, FALSE, &source_iter, error))
    goto out;

  visited_children = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  while (TRUE)
    {
      struct stat stbuf;

      if (!glnx_dirfd_iterator_next_dent (&source_iter, &dent, cancellable, error))
        goto out;

      if (dent == NULL)
        break;

      if (g_hash_table_contains (visited_children, dent->d_name))
        continue;

      /* Avoid processing the same file again if it was re-created during an export */
      g_hash_table_insert (visited_children, g_strdup (dent->d_name), GINT_TO_POINTER (1));

      if (fstatat (source_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
        {
          if (errno == ENOENT)
            {
              continue;
            }
          else
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }

      if (S_ISDIR (stbuf.st_mode))
        {
          if (!rewrite_export_dir (app, branch, arch, metadata,
                                   source_iter.fd, dent->d_name,
                                   cancellable, error))
            goto out;
        }
      else if (S_ISREG (stbuf.st_mode))
        {
          if (!flatpak_has_name_prefix (dent->d_name, app))
            {
              g_warning ("Non-prefixed filename %s in app %s, removing.", dent->d_name, app);
              if (unlinkat (source_iter.fd, dent->d_name, 0) != 0 && errno != ENOENT)
                {
                  glnx_set_error_from_errno (error);
                  goto out;
                }
            }

          if (g_str_has_suffix (dent->d_name, ".desktop") ||
              g_str_has_suffix (dent->d_name, ".service"))
            {
              g_autofree gchar *new_name = NULL;

              if (!export_desktop_file (app, branch, arch, metadata,
                                        source_iter.fd, dent->d_name, &stbuf, &new_name, cancellable, error))
                goto out;

              g_hash_table_insert (visited_children, g_strdup (new_name), GINT_TO_POINTER (1));

              if (renameat (source_iter.fd, new_name, source_iter.fd, dent->d_name) != 0)
                {
                  glnx_set_error_from_errno (error);
                  goto out;
                }
            }
        }
      else
        {
          g_warning ("Not exporting file %s of unsupported type.", dent->d_name);
          if (unlinkat (source_iter.fd, dent->d_name, 0) != 0 && errno != ENOENT)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }
    }

  ret = TRUE;
out:

  return ret;
}

gboolean
flatpak_rewrite_export_dir (const char   *app,
                            const char   *branch,
                            const char   *arch,
                            GKeyFile     *metadata,
                            GFile        *source,
                            GCancellable *cancellable,
                            GError      **error)
{
  gboolean ret = FALSE;

  /* The fds are closed by this call */
  if (!rewrite_export_dir (app, branch, arch, metadata,
                           AT_FDCWD, flatpak_file_get_path_cached (source),
                           cancellable, error))
    goto out;

  ret = TRUE;

out:
  return ret;
}


static gboolean
export_dir (int           source_parent_fd,
            const char   *source_name,
            const char   *source_symlink_prefix,
            const char   *source_relpath,
            int           destination_parent_fd,
            const char   *destination_name,
            GCancellable *cancellable,
            GError      **error)
{
  gboolean ret = FALSE;
  int res;

  g_auto(GLnxDirFdIterator) source_iter = {0};
  glnx_fd_close int destination_dfd = -1;
  struct dirent *dent;

  if (!glnx_dirfd_iterator_init_at (source_parent_fd, source_name, FALSE, &source_iter, error))
    goto out;

  do
    res = mkdirat (destination_parent_fd, destination_name, 0755);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      if (errno != EEXIST)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  if (!glnx_opendirat (destination_parent_fd, destination_name, TRUE,
                       &destination_dfd, error))
    goto out;

  while (TRUE)
    {
      struct stat stbuf;

      if (!glnx_dirfd_iterator_next_dent (&source_iter, &dent, cancellable, error))
        goto out;

      if (dent == NULL)
        break;

      if (fstatat (source_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
        {
          if (errno == ENOENT)
            {
              continue;
            }
          else
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }

      if (S_ISDIR (stbuf.st_mode))
        {
          g_autofree gchar *child_symlink_prefix = g_build_filename ("..", source_symlink_prefix, dent->d_name, NULL);
          g_autofree gchar *child_relpath = g_strconcat (source_relpath, dent->d_name, "/", NULL);

          if (!export_dir (source_iter.fd, dent->d_name, child_symlink_prefix, child_relpath, destination_dfd, dent->d_name,
                           cancellable, error))
            goto out;
        }
      else if (S_ISREG (stbuf.st_mode))
        {
          g_autofree gchar *target = NULL;

          target = g_build_filename (source_symlink_prefix, dent->d_name, NULL);

          if (unlinkat (destination_dfd, dent->d_name, 0) != 0 && errno != ENOENT)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }

          if (symlinkat (target, destination_dfd, dent->d_name) != 0)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }
    }

  ret = TRUE;
out:

  return ret;
}

gboolean
flatpak_export_dir (GFile        *source,
                    GFile        *destination,
                    const char   *symlink_prefix,
                    GCancellable *cancellable,
                    GError      **error)
{
  gboolean ret = FALSE;

  if (!flatpak_mkdir_p (destination, cancellable, error))
    goto out;

  /* The fds are closed by this call */
  if (!export_dir (AT_FDCWD, flatpak_file_get_path_cached (source), symlink_prefix, "",
                   AT_FDCWD, flatpak_file_get_path_cached (destination),
                   cancellable, error))
    goto out;

  ret = TRUE;

out:
  return ret;
}

gboolean
flatpak_dir_update_exports (FlatpakDir   *self,
                            const char   *changed_app,
                            GCancellable *cancellable,
                            GError      **error)
{
  gboolean ret = FALSE;

  g_autoptr(GFile) exports = NULL;
  g_autofree char *current_ref = NULL;
  g_autofree char *active_id = NULL;
  g_autofree char *symlink_prefix = NULL;

  exports = flatpak_dir_get_exports_dir (self);

  if (!flatpak_mkdir_p (exports, cancellable, error))
    goto out;

  if (changed_app &&
      (current_ref = flatpak_dir_current_ref (self, changed_app, cancellable)) &&
      (active_id = flatpak_dir_read_active (self, current_ref, cancellable)))
    {
      g_autoptr(GFile) deploy_base = NULL;
      g_autoptr(GFile) active = NULL;
      g_autoptr(GFile) export = NULL;

      deploy_base = flatpak_dir_get_deploy_dir (self, current_ref);
      active = g_file_get_child (deploy_base, active_id);
      export = g_file_get_child (active, "export");

      if (g_file_query_exists (export, cancellable))
        {
          symlink_prefix = g_build_filename ("..", "app", changed_app, "current", "active", "export", NULL);
          if (!flatpak_export_dir (export, exports,
                                   symlink_prefix,
                                   cancellable,
                                   error))
            goto out;
        }
    }

  if (!flatpak_remove_dangling_symlinks (exports, cancellable, error))
    goto out;

  if (!flatpak_dir_run_triggers (self, cancellable, error))
    goto out;

  ret = TRUE;

out:
  return ret;
}

static gboolean
extract_extra_data (FlatpakDir          *self,
                    const char          *checksum,
                    GFile               *extradir,
                    gboolean            *created_extra_data,
                    GCancellable        *cancellable,
                    GError             **error)
{
  g_autoptr(GVariant) detached_metadata = NULL;
  g_autoptr(GVariant) extra_data = NULL;
  g_autoptr(GVariant) extra_data_sources = NULL;
  g_autoptr(GError) local_error = NULL;
  gsize i, n_extra_data = 0;
  gsize n_extra_data_sources;

  extra_data_sources = flatpak_repo_get_extra_data_sources (self->repo, checksum,
                                                            cancellable, &local_error);
  if (extra_data_sources == NULL)
    {
      /* This should protect us against potential errors at the OSTree level
         (e.g. ostree_repo_load_variant), so that we don't report success. */
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }

      return TRUE;
    }

  n_extra_data_sources = g_variant_n_children (extra_data_sources);
  if (n_extra_data_sources == 0)
    return TRUE;

  g_debug ("extracting extra data to %s", g_file_get_path (extradir));

  if (!ostree_repo_read_commit_detached_metadata (self->repo, checksum, &detached_metadata,
                                                  cancellable, error))
    {
      g_prefix_error (error, _("While getting detached metadata: "));
      return FALSE;
    }

  if (detached_metadata == NULL)
    return flatpak_fail (error, "Extra data missing in detached metadata");

  extra_data = g_variant_lookup_value (detached_metadata, "xa.extra-data",
                                       G_VARIANT_TYPE ("a(ayay)"));
  if (extra_data == NULL)
    return flatpak_fail (error, "Extra data missing in detached metadata");

  n_extra_data = g_variant_n_children (extra_data);
  if (n_extra_data < n_extra_data_sources)
    return flatpak_fail (error, "Extra data missing in detached metadata");

  if (!flatpak_mkdir_p (extradir, cancellable, error))
    {
      g_prefix_error (error, _("While creating extradir: "));
      return FALSE;
    }

  for (i = 0; i < n_extra_data_sources; i++)
    {
      g_autofree char *extra_data_sha256 = NULL;
      const guchar *extra_data_sha256_bytes;
      const char *extra_data_source_name = NULL;
      guint64 download_size;
      gboolean found;
      int j;

      flatpak_repo_parse_extra_data_sources (extra_data_sources, i,
                                             &extra_data_source_name,
                                             &download_size,
                                             NULL,
                                             &extra_data_sha256_bytes,
                                             NULL);

      if (extra_data_sha256_bytes == NULL)
        return flatpak_fail (error, _("Invalid sha256 for extra data"));

      extra_data_sha256 = ostree_checksum_from_bytes (extra_data_sha256_bytes);

      /* We need to verify the data in the commitmeta again, because the only signed
         thing is the commit, which has the source info. We could have accidentally
         picked up some other commitmeta stuff from the remote, or via the untrusted
         local-pull of the system helper. */
      found = FALSE;
      for (j = 0; j < n_extra_data; j++)
        {
          g_autoptr(GVariant) content = NULL;
          g_autoptr(GFile) dest = NULL;
          g_autofree char *sha256 = NULL;
          const char *extra_data_name = NULL;
          const guchar  *data;
          gsize len;

          g_variant_get_child (extra_data, j, "(^ay@ay)",
                               &extra_data_name,
                               &content);

          if (strcmp (extra_data_source_name, extra_data_name) != 0)
            continue;

          data = g_variant_get_data (content);
          len = g_variant_get_size (content);

          if (len != download_size)
            return flatpak_fail (error, _("Wrong size for extra data"));

          sha256 = g_compute_checksum_for_data (G_CHECKSUM_SHA256, data, len);
          if (strcmp (sha256, extra_data_sha256) != 0)
            return flatpak_fail (error, _("Invalid checksum for extra data"));

          dest = g_file_get_child (extradir, extra_data_name);
          if (!g_file_replace_contents (dest,
                                        g_variant_get_data (content),
                                        g_variant_get_size (content),
                                        NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION,
                                        NULL, cancellable, error))
            {
              g_prefix_error (error, _("While writing extra data file '%s': "), extra_data_name);
              return FALSE;
            }
          found = TRUE;
        }

      if (!found)
        return flatpak_fail (error, "Extra data %s missing in detached metadata", extra_data_source_name);
    }

  *created_extra_data = TRUE;

  return TRUE;
}

static void
add_args (GPtrArray *argv_array, ...)
{
  va_list args;
  const gchar *arg;

  va_start (args, argv_array);
  while ((arg = va_arg (args, const gchar *)))
    g_ptr_array_add (argv_array, g_strdup (arg));
  va_end (args);
}

static void
clear_fd (gpointer data)
{
  int *fd_p = data;
  if (fd_p != NULL && *fd_p != -1)
    close (*fd_p);
}

static void
child_setup (gpointer user_data)
{
  GArray *fd_array = user_data;
  int i;

  /* If no fd_array was specified, don't care. */
  if (fd_array == NULL)
    return;

  /* Otherwise, mark not - close-on-exec all the fds in the array */
  for (i = 0; i < fd_array->len; i++)
    fcntl (g_array_index (fd_array, int, i), F_SETFD, 0);
}

static gboolean
apply_extra_data (FlatpakDir          *self,
                  GFile               *checkoutdir,
                  GCancellable        *cancellable,
                  GError             **error)
{
  g_autoptr(GFile) metadata = NULL;
  g_autofree char *metadata_contents = NULL;
  gsize metadata_size;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autofree char *id = NULL;
  g_autofree char *runtime = NULL;
  g_autofree char *runtime_ref = NULL;
  g_autoptr(FlatpakDeploy) runtime_deploy = NULL;
  g_autoptr(GFile) app_files = NULL;
  g_autoptr(GFile) apply_extra_file = NULL;
  g_autoptr(GFile) app_export_file = NULL;
  g_autoptr(GFile) extra_export_file = NULL;
  g_autoptr(GFile) extra_files = NULL;
  g_autoptr(GFile) runtime_files = NULL;
  g_autoptr(GPtrArray) argv_array = NULL;
  g_auto(GStrv) runtime_ref_parts = NULL;
  g_autoptr(FlatpakContext) app_context = NULL;
  g_autoptr(GArray) fd_array = NULL;
  g_auto(GStrv) envp = NULL;
  int exit_status;
  const char *group = "Application";
  g_autoptr(GError) local_error = NULL;

  apply_extra_file = g_file_resolve_relative_path (checkoutdir, "files/bin/apply_extra");
  if (!g_file_query_exists (apply_extra_file, cancellable))
    return TRUE;

  metadata = g_file_get_child (checkoutdir, "metadata");

  if (!g_file_load_contents (metadata, cancellable, &metadata_contents, &metadata_size, NULL, error))
    return FALSE;

  metakey = g_key_file_new ();
  if (!g_key_file_load_from_data (metakey, metadata_contents, metadata_size, 0, error))
    return FALSE;

  id = g_key_file_get_string (metakey, group, "name", &local_error);
  if (id == NULL)
    {
      group = "Runtime";
      id = g_key_file_get_string (metakey, group, "name", NULL);
      if (id == NULL)
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
      g_clear_error (&local_error);
    }

  runtime = g_key_file_get_string (metakey, group, "runtime", error);
  if (runtime == NULL)
    return FALSE;

  runtime_ref = g_build_filename ("runtime", runtime, NULL);

  runtime_ref_parts = flatpak_decompose_ref (runtime_ref, error);
  if (runtime_ref_parts == NULL)
    return FALSE;

  runtime_deploy = flatpak_find_deploy_for_ref (runtime_ref, cancellable, error);
  if (runtime_deploy == NULL)
    return FALSE;

  app_files = g_file_get_child (checkoutdir, "files");
  app_export_file = g_file_get_child (checkoutdir, "export");
  extra_files = g_file_get_child (app_files, "extra");
  extra_export_file = g_file_get_child (extra_files, "export");
  runtime_files = flatpak_deploy_get_files (runtime_deploy);

  argv_array = g_ptr_array_new_with_free_func (g_free);
  fd_array = g_array_new (FALSE, TRUE, sizeof (int));
  g_array_set_clear_func (fd_array, clear_fd);
  g_ptr_array_add (argv_array, g_strdup (flatpak_get_bwrap ()));

  add_args (argv_array,
            "--ro-bind", flatpak_file_get_path_cached (runtime_files), "/usr",
            "--lock-file", "/usr/.ref",
            "--ro-bind", flatpak_file_get_path_cached (app_files), "/app",
            "--bind", flatpak_file_get_path_cached (extra_files), "/app/extra",
            "--chdir", "/app/extra",
            NULL);

  if (!flatpak_run_setup_base_argv (argv_array, fd_array, runtime_files, NULL, runtime_ref_parts[2],
                                    FLATPAK_RUN_FLAG_NO_SESSION_HELPER,
                                    error))
    return FALSE;

  app_context = flatpak_context_new ();

  envp = flatpak_run_get_minimal_env (FALSE);
  flatpak_run_add_environment_args (argv_array, fd_array, &envp, NULL, NULL, id,
                                    app_context, NULL);

  g_ptr_array_add (argv_array, g_strdup ("/app/bin/apply_extra"));

  g_ptr_array_add (argv_array, NULL);

  g_debug ("Running /app/bin/apply_extra ");

  if (!g_spawn_sync (NULL,
                     (char **) argv_array->pdata,
                     envp,
                     G_SPAWN_SEARCH_PATH,
                     child_setup, fd_array,
                     NULL, NULL,
                     &exit_status,
                     error))
    return FALSE;

  if (exit_status != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("apply_extra script failed"));
      return FALSE;
    }

  if (g_file_query_exists (extra_export_file, cancellable))
    {
      if (!flatpak_mkdir_p (app_export_file, cancellable, error))
        return FALSE;
      if (!flatpak_cp_a (extra_export_file,
                         app_export_file,
                         FLATPAK_CP_FLAGS_MERGE,
                         cancellable, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_dir_deploy (FlatpakDir          *self,
                    const char          *origin,
                    const char          *ref,
                    const char          *checksum_or_latest,
                    const char * const * subpaths,
                    GVariant            *old_deploy_data,
                    GCancellable        *cancellable,
                    GError             **error)
{
  g_autofree char *resolved_ref = NULL;

  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) checkoutdir = NULL;
  g_autofree char *checkoutdirpath = NULL;
  g_autoptr(GFile) real_checkoutdir = NULL;
  g_autoptr(GFile) dotref = NULL;
  g_autoptr(GFile) files_etc = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_autoptr(GFile) deploy_data_file = NULL;
  g_autoptr(GVariant) deploy_data = NULL;
  g_autoptr(GFile) export = NULL;
  g_autoptr(GFile) extradir = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  guint64 installed_size = 0;
  OstreeRepoCheckoutAtOptions options = { 0, };
  const char *checksum;
  glnx_fd_close int checkoutdir_dfd = -1;
  g_autoptr(GFile) tmp_dir_template = NULL;
  g_autoptr(GVariant) commit_data = NULL;
  g_autofree char *tmp_dir_path = NULL;
  g_autofree char *alt_id = NULL;
  gboolean created_extra_data = FALSE;
  g_autoptr(GVariant) commit_metadata = NULL;
  GVariantBuilder metadata_builder;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  deploy_base = flatpak_dir_get_deploy_dir (self, ref);

  if (checksum_or_latest == NULL)
    {
      g_debug ("No checksum specified, getting tip of %s", ref);

      resolved_ref = flatpak_dir_read_latest (self, origin, ref, NULL, cancellable, error);
      if (resolved_ref == NULL)
        {
          g_prefix_error (error, _("While trying to resolve ref %s: "), ref);
          return FALSE;
        }

      checksum = resolved_ref;
      g_debug ("tip resolved to: %s", checksum);
    }
  else
    {
      g_autoptr(GFile) root = NULL;
      g_autofree char *commit = NULL;

      checksum = checksum_or_latest;
      g_debug ("Looking for checksum %s in local repo", checksum);
      if (!ostree_repo_read_commit (self->repo, checksum, &root, &commit, cancellable, NULL))
        return flatpak_fail (error, _("%s is not available"), ref);
    }

  if (!ostree_repo_load_commit (self->repo, checksum, &commit_data, NULL, error))
    return FALSE;

  commit_metadata = g_variant_get_child_value (commit_data, 0);
  g_variant_lookup (commit_metadata, "xa.alt-id", "s", &alt_id);

  real_checkoutdir = g_file_get_child (deploy_base, checksum);
  if (g_file_query_exists (real_checkoutdir, cancellable))
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                   _("%s branch %s already installed"), ref, checksum);
      return FALSE;
    }

  g_autofree char *template = g_strdup_printf (".%s-XXXXXX", checksum);
  tmp_dir_template = g_file_get_child (deploy_base, template);
  tmp_dir_path = g_file_get_path (tmp_dir_template);

  if (g_mkdtemp_full (tmp_dir_path, 0755) == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Can't create deploy directory"));
      return FALSE;
    }

  checkoutdir = g_file_new_for_path (tmp_dir_path);

  if (!ostree_repo_read_commit (self->repo, checksum, &root, NULL, cancellable, error))
    {
      g_prefix_error (error, _("Failed to read commit %s: "), checksum);
      return FALSE;
    }

  if (!flatpak_repo_collect_sizes (self->repo, root, &installed_size, NULL, cancellable, error))
    return FALSE;

  options.mode = OSTREE_REPO_CHECKOUT_MODE_USER;
  options.overwrite_mode = OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES;
  options.enable_fsync = FALSE; /* We checkout to a temp dir and sync before moving it in place */
  checkoutdirpath = g_file_get_path (checkoutdir);

  if (subpaths == NULL || *subpaths == NULL)
    {
      if (!ostree_repo_checkout_at (self->repo, &options,
                                    AT_FDCWD, checkoutdirpath,
                                    checksum,
                                    cancellable, error))
        {
          g_prefix_error (error, _("While trying to checkout %s into %s: "), checksum, checkoutdirpath);
          return FALSE;
        }
    }
  else
    {
      g_autofree char *checkoutdirpath = g_file_get_path (checkoutdir);
      g_autoptr(GFile) files = g_file_get_child (checkoutdir, "files");
      g_autoptr(GFile) root = NULL;
      g_autofree char *commit = NULL;
      int i;

      if (!g_file_make_directory_with_parents (files, cancellable, error))
        return FALSE;

      options.subpath = "/metadata";

      if (!ostree_repo_read_commit (self->repo, checksum, &root,  &commit, cancellable, error))
        return FALSE;

      if (!ostree_repo_checkout_at (self->repo, &options,
                                    AT_FDCWD, checkoutdirpath,
                                    checksum,
                                    cancellable, error))
        {
          g_prefix_error (error, _("While trying to checkout metadata subpath: "));
          return FALSE;
        }

      for (i = 0; subpaths[i] != NULL; i++)
        {
          g_autofree char *subpath = g_build_filename ("/files", subpaths[i], NULL);
          g_autofree char *dstpath = g_build_filename (checkoutdirpath, "/files", subpaths[i], NULL);
          g_autofree char *dstpath_parent = g_path_get_dirname (dstpath);
          g_autoptr(GFile) child = NULL;

          child = g_file_resolve_relative_path (root, subpath);

          if (!g_file_query_exists (child, cancellable))
            {
              g_debug ("subpath %s not in tree", subpaths[i]);
              continue;
            }

          if (g_mkdir_with_parents (dstpath_parent, 0755))
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }

          options.subpath = subpath;
          if (!ostree_repo_checkout_at (self->repo, &options,
                                        AT_FDCWD, dstpath,
                                        checksum,
                                        cancellable, error))
            {
              g_prefix_error (error, _("While trying to checkout metadata subpath: "));
              return FALSE;
            }
        }
    }

  /* Extract any extra data */
  extradir = g_file_resolve_relative_path (checkoutdir, "files/extra");
  if (!flatpak_rm_rf (extradir, cancellable, error))
    {
      g_prefix_error (error, _("While trying to remove existing extra dir: "));
      return FALSE;
    }

  if (!extract_extra_data (self, checksum, extradir, &created_extra_data, cancellable, error))
    return FALSE;

  if (created_extra_data)
    {
      if (!apply_extra_data (self, checkoutdir, cancellable, error))
        {
          g_prefix_error (error, _("While trying to apply extra data: "));
          return FALSE;
        }
    }

  dotref = g_file_resolve_relative_path (checkoutdir, "files/.ref");
  if (!g_file_replace_contents (dotref, "", 0, NULL, FALSE,
                                G_FILE_CREATE_REPLACE_DESTINATION, NULL, cancellable, error))
    return TRUE;

  /* Ensure that various files exists as regular files in /usr/etc, as we
     want to bind-mount over them */
  files_etc = g_file_resolve_relative_path (checkoutdir, "files/etc");
  if (g_file_query_exists (files_etc, cancellable))
    {
      char *etcfiles[] = {"passwd", "group", "machine-id" };
      g_autoptr(GFile) etc_resolve_conf = g_file_get_child (files_etc, "resolv.conf");
      int i;
      for (i = 0; i < G_N_ELEMENTS (etcfiles); i++)
        {
          g_autoptr(GFile) etc_file = g_file_get_child (files_etc, etcfiles[i]);
          GFileType type;

          type = g_file_query_file_type (etc_file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                         cancellable);
          if (type == G_FILE_TYPE_REGULAR)
            continue;

          if (type != G_FILE_TYPE_UNKNOWN)
            {
              /* Already exists, but not regular, probably symlink. Remove it */
              if (!g_file_delete (etc_file, cancellable, error))
                return FALSE;
            }

          if (!g_file_replace_contents (etc_file, "", 0, NULL, FALSE,
                                        G_FILE_CREATE_REPLACE_DESTINATION,
                                        NULL, cancellable, error))
            return FALSE;
        }

      if (g_file_query_exists (etc_resolve_conf, cancellable) &&
          !g_file_delete (etc_resolve_conf, cancellable, error))
        return TRUE;

      if (!g_file_make_symbolic_link (etc_resolve_conf,
                                      "/run/host/monitor/resolv.conf",
                                      cancellable, error))
        return FALSE;
    }

  keyfile = g_key_file_new ();
  metadata = g_file_get_child (checkoutdir, "metadata");
  if (g_file_query_exists (metadata, cancellable))
    {
      g_autofree char *path = g_file_get_path (metadata);

      if (!g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, error))
        return FALSE;
    }

  export = g_file_get_child (checkoutdir, "export");
  if (g_file_query_exists (export, cancellable))
    {
      g_auto(GStrv) ref_parts = NULL;

      ref_parts = g_strsplit (ref, "/", -1);

      if (!flatpak_rewrite_export_dir (ref_parts[1], ref_parts[3], ref_parts[2],
                                       keyfile, export,
                                       cancellable,
                                       error))
        return FALSE;
    }

  g_variant_builder_init (&metadata_builder, G_VARIANT_TYPE ("a{sv}"));
  if (alt_id)
    g_variant_builder_add (&metadata_builder, "{s@v}", "alt-id",
                           g_variant_new_variant (g_variant_new_string (alt_id)));

  deploy_data = flatpak_dir_new_deploy_data (origin,
                                             checksum,
                                             (char **) subpaths,
                                             installed_size,
                                             g_variant_builder_end (&metadata_builder));

  deploy_data_file = g_file_get_child (checkoutdir, "deploy");
  if (!flatpak_variant_save (deploy_data_file, deploy_data, cancellable, error))
    return FALSE;

  if (!glnx_opendirat (AT_FDCWD, checkoutdirpath, TRUE, &checkoutdir_dfd, error))
    return FALSE;

  if (syncfs (checkoutdir_dfd) != 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  if (!g_file_move (checkoutdir, real_checkoutdir, G_FILE_COPY_NO_FALLBACK_FOR_MOVE,
                    cancellable, NULL, NULL, error))
    return FALSE;

  if (!flatpak_dir_set_active (self, ref, checksum, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_dir_deploy_install (FlatpakDir   *self,
                            const char   *ref,
                            const char   *origin,
                            const char  **subpaths,
                            GCancellable *cancellable,
                            GError      **error)
{
  g_auto(GLnxLockFile) lock = GLNX_LOCK_FILE_INIT;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) old_deploy_dir = NULL;
  gboolean created_deploy_base = FALSE;
  gboolean ret = FALSE;
  g_autoptr(GError) local_error = NULL;
  g_auto(GStrv) ref_parts = g_strsplit (ref, "/", -1);

  if (!flatpak_dir_lock (self, &lock,
                         cancellable, error))
    goto out;

  old_deploy_dir = flatpak_dir_get_if_deployed (self, ref, NULL, cancellable);
  if (old_deploy_dir != NULL)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                   _("%s branch %s already installed"), ref_parts[1], ref_parts[3]);
      goto out;
    }

  deploy_base = flatpak_dir_get_deploy_dir (self, ref);
  if (!g_file_make_directory_with_parents (deploy_base, cancellable, &local_error))
    {
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          goto out;
        }
    }

  /* After we create the deploy base we must goto out on errors */
  created_deploy_base = TRUE;

  if (!flatpak_dir_deploy (self, origin, ref, NULL, (const char * const *) subpaths, NULL, cancellable, error))
    goto out;

  if (g_str_has_prefix (ref, "app/"))
    {

      if (!flatpak_dir_make_current_ref (self, ref, cancellable, error))
        goto out;

      if (!flatpak_dir_update_exports (self, ref_parts[1], cancellable, error))
        goto out;
    }

  /* Release lock before doing possibly slow prune */
  glnx_release_lock_file (&lock);

  flatpak_dir_cleanup_removed (self, cancellable, NULL);

  if (!flatpak_dir_mark_changed (self, error))
    goto out;

  ret = TRUE;

out:
  if (created_deploy_base && !ret)
    flatpak_rm_rf (deploy_base, cancellable, NULL);

  return ret;
}


gboolean
flatpak_dir_deploy_update (FlatpakDir   *self,
                           const char   *ref,
                           const char   *checksum_or_latest,
                           const char **opt_subpaths,
                           GCancellable *cancellable,
                           GError      **error)
{
  g_autoptr(GVariant) old_deploy_data = NULL;
  g_auto(GLnxLockFile) lock = GLNX_LOCK_FILE_INIT;
  g_autofree const char **old_subpaths = NULL;
  const char *old_active;
  const char *old_origin;

  if (!flatpak_dir_lock (self, &lock,
                         cancellable, error))
    return FALSE;

  old_deploy_data = flatpak_dir_get_deploy_data (self, ref,
                                                 cancellable, error);
  if (old_deploy_data == NULL)
    return FALSE;

  old_origin = flatpak_deploy_data_get_origin (old_deploy_data);
  old_active = flatpak_deploy_data_get_commit (old_deploy_data);
  old_subpaths = flatpak_deploy_data_get_subpaths (old_deploy_data);
  if (!flatpak_dir_deploy (self,
                           old_origin,
                           ref,
                           checksum_or_latest,
                           opt_subpaths ? opt_subpaths : old_subpaths,
                           old_deploy_data,
                           cancellable, error))
    return FALSE;

  if (!flatpak_dir_undeploy (self, ref, old_active,
                             TRUE, FALSE,
                             cancellable, error))
    return FALSE;

  if (g_str_has_prefix (ref, "app/"))
    {
      g_auto(GStrv) ref_parts = g_strsplit (ref, "/", -1);

      if (!flatpak_dir_update_exports (self, ref_parts[1], cancellable, error))
        return FALSE;
    }

  /* Release lock before doing possibly slow prune */
  glnx_release_lock_file (&lock);

  flatpak_dir_prune (self, cancellable, NULL);

  if (!flatpak_dir_mark_changed (self, error))
    return FALSE;

  flatpak_dir_cleanup_removed (self, cancellable, NULL);

  return TRUE;
}

static OstreeRepo *
flatpak_dir_create_system_child_repo (FlatpakDir   *self,
                                      GLnxLockFile *file_lock,
                                      GError      **error)
{
  g_autoptr(GFile) cache_dir = NULL;
  g_autoptr(GFile) repo_dir = NULL;
  g_autoptr(GFile) repo_dir_config = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  g_autofree char *tmpdir_name = NULL;
  g_autoptr(OstreeRepo) new_repo = NULL;
  g_autoptr(GKeyFile) config = NULL;

  g_assert (!self->user);

  if (!flatpak_dir_ensure_repo (self, NULL, error))
    return NULL;

  cache_dir = flatpak_ensure_user_cache_dir_location (error);
  if (cache_dir == NULL)
    return NULL;

  if (!flatpak_allocate_tmpdir (AT_FDCWD,
                                flatpak_file_get_path_cached (cache_dir),
                                "repo-", &tmpdir_name,
                                NULL,
                                file_lock,
                                NULL,
                                NULL, error))
    return NULL;

  repo_dir = g_file_get_child (cache_dir, tmpdir_name);

  new_repo = ostree_repo_new (repo_dir);

  repo_dir_config = g_file_get_child (repo_dir, "config");
  if (!g_file_query_exists (repo_dir_config, NULL))
    {
      if (!ostree_repo_create (new_repo,
                               OSTREE_REPO_MODE_BARE_USER,
                               NULL, error))
        return NULL;
    }
  else
    {
      if (!ostree_repo_open (new_repo, NULL, error))
        return NULL;
    }

  /* Ensure the config is updated */
  config = ostree_repo_copy_config (new_repo);
  g_key_file_set_string (config, "core", "parent",
                         flatpak_file_get_path_cached (ostree_repo_get_path (self->repo)));

  if (!ostree_repo_write_config (new_repo, config, error))
    return NULL;

  /* We need to reopen to apply the parent config */
  repo = system_ostree_repo_new (repo_dir);
  if (!ostree_repo_open (repo, NULL, error))
    return NULL;

  return g_steal_pointer (&repo);
}

gboolean
flatpak_dir_install (FlatpakDir          *self,
                     gboolean             no_pull,
                     gboolean             no_deploy,
                     const char          *ref,
                     const char          *remote_name,
                     const char         **opt_subpaths,
                     OstreeAsyncProgress *progress,
                     GCancellable        *cancellable,
                     GError             **error)
{
  if (flatpak_dir_use_system_helper (self))
    {
      g_autoptr(OstreeRepo) child_repo = NULL;
      g_auto(GLnxLockFile) child_repo_lock = GLNX_LOCK_FILE_INIT;
      const char *installation = flatpak_dir_get_id (self);
      const char *empty_subpaths[] = {NULL};
      const char **subpaths;
      g_autofree char *child_repo_path = NULL;
      FlatpakSystemHelper *system_helper;
      FlatpakHelperDeployFlags helper_flags = 0;
      g_autofree char *url = NULL;
      gboolean gpg_verify_summary;
      gboolean gpg_verify;

      system_helper = flatpak_dir_get_system_helper (self);
      g_assert (system_helper != NULL);

      if (opt_subpaths)
        subpaths = opt_subpaths;
      else
        subpaths = empty_subpaths;

      if (!flatpak_dir_ensure_repo (self, cancellable, error))
        return FALSE;

      if (!ostree_repo_remote_get_url (self->repo,
                                       remote_name,
                                       &url,
                                       error))
        return FALSE;

      if (!ostree_repo_remote_get_gpg_verify_summary (self->repo, remote_name,
                                                      &gpg_verify_summary, error))
        return FALSE;

      if (!ostree_repo_remote_get_gpg_verify (self->repo, remote_name,
                                              &gpg_verify, error))
        return FALSE;

      if (no_pull)
        {
          /* Do nothing */
        }
      else if (!gpg_verify_summary || !gpg_verify)
        {
          /* The remote is not gpg verified, so we don't want to allow installation via
             a download in the home directory, as there is no way to verify you're not
             injecting anything into the remote. However, in the case of a remote
             configured to a local filesystem we can just let the system helper do
             the installation, as it can then avoid network i/o and be certain the
             data comes from the right place. */
          if (g_str_has_prefix (url, "file:"))
            helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL;
          else
            return flatpak_fail (error, "Can't pull from untrusted non-gpg verified remote");
        }
      else
        {
          /* We're pulling from a remote source, we do the network mirroring pull as a
             user and hand back the resulting data to the system-helper, that trusts us
             due to the GPG signatures in the repo */

          child_repo = flatpak_dir_create_system_child_repo (self, &child_repo_lock, error);
          if (child_repo == NULL)
            return FALSE;

          if (!flatpak_dir_pull (self, remote_name, ref, NULL, subpaths,
                                 child_repo,
                                 FLATPAK_PULL_FLAGS_DOWNLOAD_EXTRA_DATA | FLATPAK_PULL_FLAGS_SIDELOAD_EXTRA_DATA,
                                 OSTREE_REPO_PULL_FLAGS_MIRROR,
                                 progress, cancellable, error))
            return FALSE;

          child_repo_path = g_file_get_path (ostree_repo_get_path (child_repo));
        }

      if (no_deploy)
        helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY;

      g_debug ("Calling system helper: Deploy");
      if (!flatpak_system_helper_call_deploy_sync (system_helper,
                                                   child_repo_path ? child_repo_path : "",
                                                   helper_flags, ref, remote_name,
                                                   (const char * const *) subpaths,
                                                   installation ? installation : "",
                                                   cancellable,
                                                   error))
        return FALSE;

      if (child_repo_path)
        (void) glnx_shutil_rm_rf_at (AT_FDCWD, child_repo_path, NULL, NULL);

      return TRUE;
    }

  if (!no_pull)
    {
      if (!flatpak_dir_pull (self, remote_name, ref, NULL, opt_subpaths, NULL,
                             FLATPAK_PULL_FLAGS_DOWNLOAD_EXTRA_DATA, OSTREE_REPO_PULL_FLAGS_NONE,
                             progress, cancellable, error))
        return FALSE;
    }

  if (!no_deploy)
    {
      if (!flatpak_dir_deploy_install (self, ref, remote_name, opt_subpaths,
                                       cancellable, error))
        return FALSE;
    }

  return TRUE;
}

char *
flatpak_dir_ensure_bundle_remote (FlatpakDir          *self,
                                  GFile               *file,
                                  GBytes              *extra_gpg_data,
                                  char               **out_ref,
                                  char               **out_metadata,
                                  gboolean            *out_created_remote,
                                  GCancellable        *cancellable,
                                  GError             **error)
{
  g_autofree char *ref = NULL;
  gboolean created_remote = FALSE;
  g_autoptr(GVariant) deploy_data = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *origin = NULL;
  g_autofree char *fp_metadata = NULL;
  g_auto(GStrv) parts = NULL;
  g_autofree char *basename = NULL;
  g_autoptr(GBytes) included_gpg_data = NULL;
  GBytes *gpg_data = NULL;
  g_autofree char *to_checksum = NULL;
  g_autofree char *remote = NULL;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return NULL;

  metadata = flatpak_bundle_load (file, &to_checksum,
                                  &ref,
                                  &origin,
                                  NULL, &fp_metadata, NULL,
                                  &included_gpg_data,
                                  error);
  if (metadata == NULL)
    return NULL;

  gpg_data = extra_gpg_data ? extra_gpg_data : included_gpg_data;

  parts = flatpak_decompose_ref (ref, error);
  if (parts == NULL)
    return NULL;

  deploy_data = flatpak_dir_get_deploy_data (self, ref, cancellable, NULL);
  if (deploy_data != NULL)
    {
      remote = g_strdup (flatpak_deploy_data_get_origin (deploy_data));

      /* We need to import any gpg keys because otherwise the pull will fail */
      if (gpg_data != NULL)
        {
          g_autoptr(GKeyFile) new_config = NULL;

          new_config = ostree_repo_copy_config (flatpak_dir_get_repo (self));

          if (!flatpak_dir_modify_remote (self, remote, new_config,
                                          gpg_data, cancellable, error))
            return NULL;
        }
    }
  else
    {
      /* Add a remote for later updates */
      basename = g_file_get_basename (file);
      remote = flatpak_dir_create_origin_remote (self,
                                                 origin,
                                                 parts[1],
                                                 basename,
                                                 ref,
                                                 NULL, NULL,
                                                 gpg_data,
                                                 cancellable,
                                                 error);
      if (remote == NULL)
        return NULL;

      /* From here we need to goto out on error, to clean up */
      created_remote = TRUE;
    }

  if (out_created_remote)
    *out_created_remote = created_remote;

  if (out_ref)
    *out_ref = g_steal_pointer (&ref);

  if (out_metadata)
    *out_metadata = g_steal_pointer (&fp_metadata);


  return g_steal_pointer (&remote);
}

gboolean
flatpak_dir_install_bundle (FlatpakDir          *self,
                            GFile               *file,
                            const char          *remote,
                            char               **out_ref,
                            GCancellable        *cancellable,
                            GError             **error)
{
  g_autofree char *ref = NULL;
  g_autoptr(GVariant) deploy_data = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *origin = NULL;
  g_auto(GStrv) parts = NULL;
  g_autofree char *to_checksum = NULL;
  gboolean gpg_verify;

  if (flatpak_dir_use_system_helper (self))
    {
      FlatpakSystemHelper *system_helper;
      const char *installation = flatpak_dir_get_id (self);

      system_helper = flatpak_dir_get_system_helper (self);
      g_assert (system_helper != NULL);

      g_debug ("Calling system helper: InstallBundle");
      if (!flatpak_system_helper_call_install_bundle_sync (system_helper,
                                                           flatpak_file_get_path_cached (file),
                                                           0, remote,
                                                           installation ? installation : "",
                                                           &ref,
                                                           cancellable,
                                                           error))
        return FALSE;

      if (out_ref)
        *out_ref = g_steal_pointer (&ref);

      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  metadata = flatpak_bundle_load (file, &to_checksum,
                                  &ref,
                                  &origin,
                                  NULL, NULL,
                                  NULL, NULL,
                                  error);
  if (metadata == NULL)
    return FALSE;

  parts = flatpak_decompose_ref (ref, error);
  if (parts == NULL)
    return FALSE;

  deploy_data = flatpak_dir_get_deploy_data (self, ref, cancellable, NULL);
  if (deploy_data != NULL)
    {
      if (strcmp (flatpak_deploy_data_get_commit (deploy_data), to_checksum) == 0)
        {
          g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                       _("This version of %s is already installed"), parts[1]);
          return FALSE;
        }

      if (strcmp (remote, flatpak_deploy_data_get_origin (deploy_data)) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       _("Can't change remote during bundle install"));
          return FALSE;
        }
    }

  if (!ostree_repo_remote_get_gpg_verify (self->repo, remote,
                                          &gpg_verify, error))
    return FALSE;

  if (!flatpak_pull_from_bundle (self->repo,
                                 file,
                                 remote,
                                 ref,
                                 gpg_verify,
                                 cancellable,
                                 error))
    return FALSE;

  if (deploy_data != NULL)
    {
      g_autofree char *group = g_strdup_printf ("remote \"%s\"", remote);
      g_autofree char *old_url = NULL;
      g_autoptr(GKeyFile) new_config = NULL;

      /* The pull succeeded, and this is an update. So, we need to update the repo config
         if anything changed */

      ostree_repo_remote_get_url (self->repo,
                                  remote,
                                  &old_url,
                                  NULL);
      if (origin != NULL &&
          (old_url == NULL || strcmp (old_url, origin) != 0))
        {
          if (new_config == NULL)
            new_config = ostree_repo_copy_config (self->repo);

          g_key_file_set_value (new_config, group, "url", origin);
        }

      if (new_config)
        {
          if (!ostree_repo_write_config (self->repo, new_config, error))
            return FALSE;
        }
    }

  if (deploy_data)
    {
      if (!flatpak_dir_deploy_update (self, ref, NULL, NULL, cancellable, error))
        return FALSE;
    }
  else
    {
      if (!flatpak_dir_deploy_install (self, ref, remote, NULL, cancellable, error))
        return FALSE;
    }

  if (out_ref)
    *out_ref = g_steal_pointer (&ref);

  return TRUE;
}

static gboolean
_g_strv_equal0 (gchar **a, gchar **b)
{
  gboolean ret = FALSE;
  guint n;
  if (a == NULL && b == NULL)
    {
      ret = TRUE;
      goto out;
    }
  if (a == NULL || b == NULL)
    goto out;
  if (g_strv_length (a) != g_strv_length (b))
    goto out;
  for (n = 0; a[n] != NULL; n++)
    if (g_strcmp0 (a[n], b[n]) != 0)
      goto out;
  ret = TRUE;
out:
  return ret;
}

gboolean
flatpak_dir_update (FlatpakDir          *self,
                    gboolean             no_pull,
                    gboolean             no_deploy,
                    const char          *ref,
                    const char          *remote_name,
                    const char          *checksum_or_latest,
                    const char         **opt_subpaths,
                    OstreeAsyncProgress *progress,
                    GCancellable        *cancellable,
                    GError             **error)
{
  g_autoptr(GVariant) deploy_data = NULL;
  g_autofree const char **old_subpaths = NULL;
  const char **subpaths;
  g_autoptr(GBytes) summary_bytes = NULL;
  g_autofree char *url = NULL;
  gboolean is_local;
  g_autofree char *latest_rev = NULL;
  const char *rev = NULL;
  g_autofree char *oci_uri = NULL;

  deploy_data = flatpak_dir_get_deploy_data (self, ref,
                                             cancellable, NULL);
  if (deploy_data != NULL)
    old_subpaths = flatpak_deploy_data_get_subpaths (deploy_data);
  else
    old_subpaths = g_new0 (const char *, 1); /* Empty strv == all subpatsh*/

  if (opt_subpaths)
    subpaths = opt_subpaths;
  else
    subpaths = old_subpaths;

  if (!ostree_repo_remote_get_url (self->repo, remote_name, &url, error))
    return FALSE;

  oci_uri = flatpak_dir_get_remote_oci_uri (self, remote_name);

  if (*url == 0 && oci_uri == NULL)
    return TRUE; /* Empty URL => disabled */

  rev = checksum_or_latest;

  is_local = g_str_has_prefix (url, "file:");

  /* Quick check to terminate early if nothing changed in cached summary
     (and subpaths didn't change) */
  if (!is_local && deploy_data != NULL &&
      _g_strv_equal0 ((char **)subpaths, (char **)old_subpaths))
    {
      const char *installed_commit = flatpak_deploy_data_get_commit (deploy_data);
      const char *installed_alt_id = flatpak_deploy_data_get_alt_id (deploy_data);

      if (checksum_or_latest != NULL)
        {
          if (strcmp (checksum_or_latest, installed_commit) == 0)
            {
              g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                           _("%s branch %s already installed"), ref, installed_commit);
              return FALSE;
            }
        }
      else if (flatpak_dir_remote_fetch_summary (self, remote_name,
                                                 &summary_bytes,
                                                 cancellable, NULL))
        {
          g_autoptr(GVariant) summary =
            g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                                          summary_bytes, FALSE));

          if (flatpak_summary_lookup_ref (summary,
                                          ref,
                                          &latest_rev))
            {
              if (g_strcmp0 (latest_rev, installed_commit) == 0 ||
                  g_strcmp0 (latest_rev, installed_alt_id) == 0)
                {
                  g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                               _("%s branch %s already installed"), ref, installed_commit);
                  return FALSE;
                }
              rev = latest_rev;
            }
        }
    }

  if (flatpak_dir_use_system_helper (self))
    {
      g_autoptr(OstreeRepo) child_repo = NULL;
      g_auto(GLnxLockFile) child_repo_lock = GLNX_LOCK_FILE_INIT;
      g_autofree char *latest_checksum = NULL;
      g_autofree char *active_checksum = NULL;
      FlatpakSystemHelper *system_helper;
      g_autofree char *child_repo_path = NULL;
      FlatpakHelperDeployFlags helper_flags = 0;
      g_autofree char *url = NULL;
      gboolean gpg_verify_summary;
      gboolean gpg_verify;

      if (checksum_or_latest != NULL)
        return flatpak_fail (error, "Can't update to a specific commit without root permissions");

      system_helper = flatpak_dir_get_system_helper (self);
      g_assert (system_helper != NULL);

      if (!flatpak_dir_ensure_repo (self, cancellable, error))
        return FALSE;

      if (!ostree_repo_remote_get_url (self->repo,
                                       remote_name,
                                       &url,
                                       error))
        return FALSE;

      helper_flags = FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE;

      if (!ostree_repo_remote_get_gpg_verify_summary (self->repo, remote_name,
                                                      &gpg_verify_summary, error))
        return FALSE;

      if (!ostree_repo_remote_get_gpg_verify (self->repo, remote_name,
                                              &gpg_verify, error))
        return FALSE;

      if (no_pull)
        {
          if (!ostree_repo_resolve_rev (self->repo, ref, FALSE, &latest_checksum, error))
            return FALSE;
        }
      else if (!gpg_verify_summary || !gpg_verify)
        {
          /* The remote is not gpg verified, so we don't want to allow installation via
             a download in the home directory, as there is no way to verify you're not
             injecting anything into the remote. However, in the case of a remote
             configured to a local filesystem we can just let the system helper do
             the installation, as it can then avoid network i/o and be certain the
             data comes from the right place. */
          if (g_str_has_prefix (url, "file:"))
            helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL;
          else
            return flatpak_fail (error, "Can't pull from untrusted non-gpg verified remote");
        }
      else
        {
          /* We're pulling from a remote source, we do the network mirroring pull as a
             user and hand back the resulting data to the system-helper, that trusts us
             due to the GPG signatures in the repo */

          child_repo = flatpak_dir_create_system_child_repo (self, &child_repo_lock, error);
          if (child_repo == NULL)
            return FALSE;

          if (!flatpak_dir_pull (self, remote_name, ref, rev, subpaths,
                                 child_repo,
                                 FLATPAK_PULL_FLAGS_DOWNLOAD_EXTRA_DATA | FLATPAK_PULL_FLAGS_SIDELOAD_EXTRA_DATA,
                                 OSTREE_REPO_PULL_FLAGS_MIRROR,
                                 progress, cancellable, error))
            return FALSE;

          if (!ostree_repo_resolve_rev (child_repo, ref, FALSE, &latest_checksum, error))
            return FALSE;

          child_repo_path = g_file_get_path (ostree_repo_get_path (child_repo));
        }

      if (no_deploy)
        helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY;

      active_checksum = flatpak_dir_read_active (self, ref, NULL);
      if (g_strcmp0 (active_checksum, latest_checksum) != 0)
        {
          const char *installation = flatpak_dir_get_id (self);

          g_debug ("Calling system helper: Deploy");
          if (!flatpak_system_helper_call_deploy_sync (system_helper,
                                                       child_repo_path ? child_repo_path : "",
                                                       helper_flags, ref, remote_name,
                                                       subpaths,
                                                       installation ? installation : "",
                                                       cancellable,
                                                       error))
            return FALSE;
        }

      if (child_repo_path)
        (void) glnx_shutil_rm_rf_at (AT_FDCWD, child_repo_path, NULL, NULL);

      return TRUE;
    }

  if (!no_pull)
    {
      if (!flatpak_dir_pull (self, remote_name, ref, rev, subpaths,
                             NULL, FLATPAK_PULL_FLAGS_DOWNLOAD_EXTRA_DATA, OSTREE_REPO_PULL_FLAGS_NONE,
                             progress, cancellable, error))
        return FALSE;
    }

  if (!no_deploy)
    {
      if (!flatpak_dir_deploy_update (self, ref, checksum_or_latest, subpaths,
                                      cancellable, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_dir_install_or_update (FlatpakDir          *self,
                               gboolean             no_pull,
                               gboolean             no_deploy,
                               const char          *ref,
                               const char          *remote_name,
                               const char         **opt_subpaths,
                               OstreeAsyncProgress *progress,
                               GCancellable        *cancellable,
                               GError             **error)
{
  g_autoptr(GFile) deploy_dir = NULL;

  deploy_dir = flatpak_dir_get_if_deployed (self, ref, NULL, cancellable);
  if (deploy_dir)
    return flatpak_dir_update (self,
                               no_pull,
                               no_deploy,
                               ref,
                               remote_name,
                               NULL,
                               opt_subpaths,
                               progress,
                               cancellable,
                               error);
  else
    return flatpak_dir_install (self,
                                no_pull,
                                no_deploy,
                                ref,
                                remote_name,
                                opt_subpaths,
                                progress,
                                cancellable,
                                error);
}

gboolean
flatpak_dir_uninstall (FlatpakDir          *self,
                       const char          *ref,
                       FlatpakHelperUninstallFlags flags,
                       GCancellable        *cancellable,
                       GError             **error)
{
  const char *repository;
  g_autofree char *current_ref = NULL;
  gboolean was_deployed;
  gboolean is_app;
  const char *name;
  g_auto(GStrv) parts = NULL;
  g_auto(GLnxLockFile) lock = GLNX_LOCK_FILE_INIT;
  g_autoptr(GVariant) deploy_data = NULL;
  gboolean keep_ref = flags & FLATPAK_HELPER_UNINSTALL_FLAGS_KEEP_REF;
  gboolean force_remove = flags & FLATPAK_HELPER_UNINSTALL_FLAGS_FORCE_REMOVE;

  parts = flatpak_decompose_ref (ref, error);
  if (parts == NULL)
    return FALSE;
  name = parts[1];

  if (flatpak_dir_use_system_helper (self))
    {
      FlatpakSystemHelper *system_helper;
      const char *installation = flatpak_dir_get_id (self);

      system_helper = flatpak_dir_get_system_helper (self);
      g_assert (system_helper != NULL);

      g_debug ("Calling system helper: Uninstall");
      if (!flatpak_system_helper_call_uninstall_sync (system_helper,
                                                      flags, ref,
                                                      installation ? installation : "",
                                                      cancellable, error))
        return FALSE;

      return TRUE;
    }

  if (!flatpak_dir_lock (self, &lock,
                         cancellable, error))
    return FALSE;

  deploy_data = flatpak_dir_get_deploy_data (self, ref,
                                             cancellable, error);
  if (deploy_data == NULL)
    return FALSE;

  repository = flatpak_deploy_data_get_origin (deploy_data);
  if (repository == NULL)
    return FALSE;

  g_debug ("dropping active ref");
  if (!flatpak_dir_set_active (self, ref, NULL, cancellable, error))
    return FALSE;

  is_app = g_str_has_prefix (ref, "app/");
  if (is_app)
    {
      current_ref = flatpak_dir_current_ref (self, name, cancellable);
      if (g_strcmp0 (ref, current_ref) == 0)
        {
          g_debug ("dropping current ref");
          if (!flatpak_dir_drop_current_ref (self, name, cancellable, error))
            return FALSE;
        }
    }

  if (!flatpak_dir_undeploy_all (self, ref, force_remove, &was_deployed, cancellable, error))
    return FALSE;

  if (!keep_ref &&
      !flatpak_dir_remove_ref (self, repository, ref, cancellable, error))
    return FALSE;

  if (is_app &&
      !flatpak_dir_update_exports (self, name, cancellable, error))
    return FALSE;

  glnx_release_lock_file (&lock);

  if (repository != NULL &&
      g_str_has_suffix (repository, "-origin") &&
      flatpak_dir_get_remote_noenumerate (self, repository))
    ostree_repo_remote_delete (self->repo, repository, NULL, NULL);

  if (!keep_ref)
    flatpak_dir_prune (self, cancellable, NULL);

  flatpak_dir_cleanup_removed (self, cancellable, NULL);

  if (!flatpak_dir_mark_changed (self, error))
    return FALSE;

  if (!was_deployed)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   _("%s branch %s is not installed"), name, parts[3]);
    }

  return TRUE;
}

gboolean
flatpak_dir_collect_deployed_refs (FlatpakDir   *self,
                                   const char   *type,
                                   const char   *name_prefix,
                                   const char   *branch,
                                   const char   *arch,
                                   GHashTable   *hash,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  gboolean ret = FALSE;

  g_autoptr(GFile) dir = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  GError *temp_error = NULL;

  dir = g_file_get_child (self->basedir, type);
  if (!g_file_query_exists (dir, cancellable))
    return TRUE;

  dir_enum = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name = g_file_info_get_name (child_info);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY &&
          name[0] != '.' && (name_prefix == NULL || g_str_has_prefix (name, name_prefix)))
        {
          g_autoptr(GFile) child1 = g_file_get_child (dir, name);
          g_autoptr(GFile) child2 = g_file_get_child (child1, branch);
          g_autoptr(GFile) child3 = g_file_get_child (child2, arch);
          g_autoptr(GFile) active = g_file_get_child (child3, "active");

          if (g_file_query_exists (active, cancellable))
            g_hash_table_add (hash, g_strdup (name));
        }

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
out:
  return ret;
}

gboolean
flatpak_dir_collect_unmaintained_refs (FlatpakDir   *self,
                                       const char   *name_prefix,
                                       const char   *arch,
                                       const char   *branch,
                                       GHashTable   *hash,
                                       GCancellable *cancellable,
                                       GError      **error)
{
  gboolean ret = FALSE;

  g_autoptr(GFile) unmaintained_dir = NULL;
  g_autoptr(GFileEnumerator) unmaintained_dir_enum = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  GError *temp_error = NULL;

  unmaintained_dir = g_file_get_child (self->basedir, "extension");
  if (!g_file_query_exists (unmaintained_dir, cancellable))
    return TRUE;

  unmaintained_dir_enum = g_file_enumerate_children (unmaintained_dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                     G_FILE_QUERY_INFO_NONE,
                                                     cancellable,
                                                     error);
  if (!unmaintained_dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (unmaintained_dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name = g_file_info_get_name (child_info);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY &&
          name[0] != '.' && (name_prefix == NULL || g_str_has_prefix (name, name_prefix)))
        {
          g_autoptr(GFile) child1 = g_file_get_child (unmaintained_dir, name);
          g_autoptr(GFile) child2 = g_file_get_child (child1, arch);
          g_autoptr(GFile) child3 = g_file_get_child (child2, branch);

          if (g_file_query_exists (child3, cancellable))
            g_hash_table_add (hash, g_strdup (name));
        }

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
out:
  return ret;
}

gboolean
flatpak_dir_list_deployed (FlatpakDir   *self,
                           const char   *ref,
                           char       ***deployed_checksums,
                           GCancellable *cancellable,
                           GError      **error)
{
  gboolean ret = FALSE;

  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GPtrArray) checksums = NULL;
  GError *temp_error = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFile) child = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  g_autoptr(GError) my_error = NULL;

  deploy_base = flatpak_dir_get_deploy_dir (self, ref);

  checksums = g_ptr_array_new_with_free_func (g_free);

  dir_enum = g_file_enumerate_children (deploy_base, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        &my_error);
  if (!dir_enum)
    {
      if (g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        ret = TRUE; /* Success, but empty */
      else
        g_propagate_error (error, g_steal_pointer (&my_error));
      goto out;
    }

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name;

      name = g_file_info_get_name (child_info);

      g_clear_object (&child);
      child = g_file_get_child (deploy_base, name);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY &&
          name[0] != '.' &&
          strlen (name) == 64)
        g_ptr_array_add (checksums, g_strdup (name));

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;

out:
  if (ret)
    {
      g_ptr_array_add (checksums, NULL);
      *deployed_checksums = (char **) g_ptr_array_free (g_steal_pointer (&checksums), FALSE);
    }

  return ret;

}

static gboolean
dir_is_locked (GFile *dir)
{
  glnx_fd_close int ref_fd = -1;
  struct flock lock = {0};

  g_autoptr(GFile) reffile = NULL;

  reffile = g_file_resolve_relative_path (dir, "files/.ref");

  ref_fd = open (flatpak_file_get_path_cached (reffile), O_RDWR | O_CLOEXEC);
  if (ref_fd != -1)
    {
      lock.l_type = F_WRLCK;
      lock.l_whence = SEEK_SET;
      lock.l_start = 0;
      lock.l_len = 0;

      if (fcntl (ref_fd, F_GETLK, &lock) == 0)
        return lock.l_type != F_UNLCK;
    }

  return FALSE;
}

gboolean
flatpak_dir_undeploy (FlatpakDir   *self,
                      const char   *ref,
                      const char   *checksum,
                      gboolean      is_update,
                      gboolean      force_remove,
                      GCancellable *cancellable,
                      GError      **error)
{
  gboolean ret = FALSE;

  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) checkoutdir = NULL;
  g_autoptr(GFile) removed_subdir = NULL;
  g_autoptr(GFile) removed_dir = NULL;
  g_autofree char *tmpname = g_strdup_printf ("removed-%s-XXXXXX", checksum);
  g_autofree char *active = NULL;
  g_autoptr(GFile) change_file = NULL;
  int i;

  g_assert (ref != NULL);
  g_assert (checksum != NULL);

  deploy_base = flatpak_dir_get_deploy_dir (self, ref);

  checkoutdir = g_file_get_child (deploy_base, checksum);
  if (!g_file_query_exists (checkoutdir, cancellable))
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   _("%s branch %s not installed"), ref, checksum);
      goto out;
    }

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    goto out;

  active = flatpak_dir_read_active (self, ref, cancellable);
  if (active != NULL && strcmp (active, checksum) == 0)
    {
      g_auto(GStrv) deployed_checksums = NULL;
      const char *some_deployment;

      /* We're removing the active deployment, start by repointing that
         to another deployment if one exists */

      if (!flatpak_dir_list_deployed (self, ref,
                                      &deployed_checksums,
                                      cancellable, error))
        goto out;

      some_deployment = NULL;
      for (i = 0; deployed_checksums[i] != NULL; i++)
        {
          if (strcmp (deployed_checksums[i], checksum) == 0)
            continue;

          some_deployment = deployed_checksums[i];
          break;
        }

      if (!flatpak_dir_set_active (self, ref, some_deployment, cancellable, error))
        goto out;
    }

  removed_dir = flatpak_dir_get_removed_dir (self);
  if (!flatpak_mkdir_p (removed_dir, cancellable, error))
    goto out;

  glnx_gen_temp_name (tmpname);
  removed_subdir = g_file_get_child (removed_dir, tmpname);

  if (!flatpak_file_rename (checkoutdir,
                            removed_subdir,
                            cancellable, error))
    goto out;

  if (is_update)
    change_file = g_file_resolve_relative_path (removed_subdir, "files/.updated");
  else
    change_file = g_file_resolve_relative_path (removed_subdir, "files/.removed");
  g_file_replace_contents (change_file, "", 0, NULL, FALSE,
                           G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, NULL);

  if (force_remove || !dir_is_locked (removed_subdir))
    {
      GError *tmp_error = NULL;

      if (!flatpak_rm_rf (removed_subdir, cancellable, &tmp_error))
        {
          g_warning ("Unable to remove old checkout: %s\n", tmp_error->message);
          g_error_free (tmp_error);
        }
    }

  ret = TRUE;
out:
  return ret;
}

gboolean
flatpak_dir_undeploy_all (FlatpakDir   *self,
                          const char   *ref,
                          gboolean      force_remove,
                          gboolean     *was_deployed_out,
                          GCancellable *cancellable,
                          GError      **error)
{
  g_auto(GStrv) deployed = NULL;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) arch_dir = NULL;
  g_autoptr(GFile) top_dir = NULL;
  GError *temp_error = NULL;
  int i;
  gboolean was_deployed;

  if (!flatpak_dir_list_deployed (self, ref, &deployed, cancellable, error))
    return FALSE;

  for (i = 0; deployed[i] != NULL; i++)
    {
      g_debug ("undeploying %s", deployed[i]);
      if (!flatpak_dir_undeploy (self, ref, deployed[i], FALSE, force_remove, cancellable, error))
        return FALSE;
    }

  deploy_base = flatpak_dir_get_deploy_dir (self, ref);
  was_deployed = g_file_query_exists (deploy_base, cancellable);
  if (was_deployed)
    {
      g_debug ("removing deploy base");
      if (!flatpak_rm_rf (deploy_base, cancellable, error))
        return FALSE;
    }

  g_debug ("cleaning up empty directories");
  arch_dir = g_file_get_parent (deploy_base);
  if (g_file_query_exists (arch_dir, cancellable) &&
      !g_file_delete (arch_dir, cancellable, &temp_error))
    {
      if (!g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY))
        {
          g_propagate_error (error, temp_error);
          return FALSE;
        }
      g_clear_error (&temp_error);
    }

  top_dir = g_file_get_parent (arch_dir);
  if (g_file_query_exists (top_dir, cancellable) &&
      !g_file_delete (top_dir, cancellable, &temp_error))
    {
      if (!g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY))
        {
          g_propagate_error (error, temp_error);
          return FALSE;
        }
      g_clear_error (&temp_error);
    }

  if (was_deployed_out)
    *was_deployed_out = was_deployed;

  return TRUE;
}

gboolean
flatpak_dir_remove_ref (FlatpakDir   *self,
                        const char   *remote_name,
                        const char   *ref,
                        GCancellable *cancellable,
                        GError      **error)
{
  if (!ostree_repo_set_ref_immediate (self->repo, remote_name, ref, NULL, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_dir_cleanup_removed (FlatpakDir   *self,
                             GCancellable *cancellable,
                             GError      **error)
{
  gboolean ret = FALSE;

  g_autoptr(GFile) removed_dir = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  GError *temp_error = NULL;

  removed_dir = flatpak_dir_get_removed_dir (self);
  if (!g_file_query_exists (removed_dir, cancellable))
    return TRUE;

  dir_enum = g_file_enumerate_children (removed_dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable,
                                        error);
  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)) != NULL)
    {
      const char *name = g_file_info_get_name (child_info);
      g_autoptr(GFile) child = g_file_get_child (removed_dir, name);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY &&
          !dir_is_locked (child))
        {
          GError *tmp_error = NULL;
          if (!flatpak_rm_rf (child, cancellable, &tmp_error))
            {
              g_warning ("Unable to remove old checkout: %s\n", tmp_error->message);
              g_error_free (tmp_error);
            }
        }

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  ret = TRUE;
out:
  return ret;
}

gboolean
flatpak_dir_prune (FlatpakDir   *self,
                   GCancellable *cancellable,
                   GError      **error)
{
  gboolean ret = FALSE;
  gint objects_total, objects_pruned;
  guint64 pruned_object_size_total;
  g_autofree char *formatted_freed_size = NULL;
  g_autoptr(GError) local_error = NULL;

  if (error == NULL)
    error = &local_error;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    goto out;

  if (!ostree_repo_prune (self->repo,
                          OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY,
                          0,
                          &objects_total,
                          &objects_pruned,
                          &pruned_object_size_total,
                          cancellable, error))
    goto out;

  formatted_freed_size = g_format_size_full (pruned_object_size_total, 0);
  g_debug ("Pruned %d/%d objects, size %s", objects_total, objects_pruned, formatted_freed_size);

  ret = TRUE;

 out:

  /* There was an issue in ostree where for local pulls we don't get a .commitpartial (now fixed),
     which caused errors when pruning. We print these here, but don't stop processing. */
  if (local_error != NULL)
    g_print ("Pruning repo failed: %s", local_error->message);

  return ret;

}

GFile *
flatpak_dir_get_if_deployed (FlatpakDir   *self,
                             const char   *ref,
                             const char   *checksum,
                             GCancellable *cancellable)
{
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) deploy_dir = NULL;

  deploy_base = flatpak_dir_get_deploy_dir (self, ref);

  if (checksum != NULL)
    {
      deploy_dir = g_file_get_child (deploy_base, checksum);
    }
  else
    {
      g_autoptr(GFile) active_link = g_file_get_child (deploy_base, "active");
      g_autoptr(GFileInfo) info = NULL;
      const char *target;

      info = g_file_query_info (active_link,
                                G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                NULL,
                                NULL);
      if (info == NULL)
        return NULL;

      target = g_file_info_get_symlink_target (info);
      if (target == NULL)
        return NULL;

      deploy_dir = g_file_get_child (deploy_base, target);
    }

  if (g_file_query_file_type (deploy_dir, G_FILE_QUERY_INFO_NONE, cancellable) == G_FILE_TYPE_DIRECTORY)
    return g_object_ref (deploy_dir);
  return NULL;
}

GFile *
flatpak_dir_get_unmaintained_extension_dir_if_exists (FlatpakDir *self,
                                                      const char *name,
                                                      const char *arch,
                                                      const char *branch,
                                                      GCancellable *cancellable)
{
  g_autoptr(GFile) extension_dir = NULL;
  g_autoptr(GFileInfo) extension_dir_info = NULL;

  extension_dir = flatpak_dir_get_unmaintained_extension_dir (self, name, arch, branch);

  extension_dir_info = g_file_query_info (extension_dir,
                                          G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable,
                                          NULL);
  if (extension_dir_info == NULL)
    return NULL;

  if (g_file_info_get_is_symlink (extension_dir_info))
      return g_file_new_for_path (g_file_info_get_symlink_target (extension_dir_info));
  else
      return g_steal_pointer (&extension_dir);
}

G_LOCK_DEFINE_STATIC (cache);

static void
cached_summary_free (CachedSummary *summary)
{
  g_bytes_unref (summary->bytes);
  g_free (summary->remote);
  g_free (summary->url);
  g_free (summary);
}

static CachedSummary *
cached_summary_new (GBytes *bytes,
                    const char *remote,
                    const char *url)
{
  CachedSummary *summary = g_new0 (CachedSummary, 1);
  summary->bytes = g_bytes_ref (bytes);
  summary->url = g_strdup (url);
  summary->remote = g_strdup (remote);
  summary->time = g_get_monotonic_time ();
  return summary;
}

static GBytes *
flatpak_dir_lookup_cached_summary (FlatpakDir  *self,
                                   const char  *name,
                                   const char  *url)
{
  CachedSummary *summary;
  GBytes *res = NULL;

  G_LOCK (cache);

  if (self->summary_cache == NULL)
    self->summary_cache = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify)cached_summary_free);

  summary = g_hash_table_lookup (self->summary_cache, name);
  if (summary)
    {
      guint64 now = g_get_monotonic_time ();
      if ((now - summary->time) < (1000 * 1000 * (SUMMARY_CACHE_TIMEOUT_SEC)) &&
          strcmp (url, summary->url) == 0)
        {
          g_debug ("Using cached summary for remote %s", name);
          res = g_bytes_ref (summary->bytes);
        }
    }

  G_UNLOCK (cache);

  return res;
}

static void
flatpak_dir_cache_summary (FlatpakDir  *self,
                           GBytes      *bytes,
                           const char  *name,
                           const char  *url)
{
  CachedSummary *summary;

  /* No sense caching the summary if there isn't one */
  if (!bytes)
      return;

  G_LOCK (cache);

  /* This was already initialized in the cache-miss lookup */
  g_assert (self->summary_cache != NULL);

  summary = cached_summary_new (bytes, name, url);
  g_hash_table_replace (self->summary_cache, summary->remote, summary);

  G_UNLOCK (cache);
}

static gboolean
flatpak_dir_remote_make_oci_summary (FlatpakDir   *self,
                                     const char   *remote,
                                     GBytes      **out_summary,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  g_autoptr(FlatpakOciRegistry) registry = NULL;
  g_autofree char *oci_ref = NULL;
  g_autofree char *remote_oci_ref = NULL;
  g_autofree char *oci_uri = NULL;
  g_autofree char *oci_tag = NULL;
  g_autofree char *oci_digest = NULL;
  g_autoptr(GVariantBuilder) refs_builder = NULL;
  g_autoptr(GVariantBuilder) additional_metadata_builder = NULL;
  g_autoptr(GVariantBuilder) summary_builder = NULL;
  g_autoptr(FlatpakOciManifest) manifest = NULL;
  g_autoptr(GVariant) summary = NULL;
  GHashTable *annotations;
  g_autoptr(GVariantBuilder) ref_data_builder = NULL;

  oci_uri = flatpak_dir_get_remote_oci_uri (self, remote);
  g_assert (oci_uri != NULL);

  oci_tag = flatpak_dir_get_remote_oci_tag (self, remote);
  if (oci_tag == NULL)
    oci_tag = g_strdup ("latest");

  oci_ref = flatpak_dir_get_remote_main_ref (self, remote);

  registry = flatpak_oci_registry_new (oci_uri, FALSE, -1, NULL, error);
  if (registry == NULL)
    return FALSE;

  manifest = flatpak_oci_registry_chose_image (registry, oci_tag, &oci_digest,
                                               NULL, error);
  if (manifest == NULL)
    return FALSE;

  annotations = flatpak_oci_manifest_get_annotations (manifest);
  if (annotations)
    flatpak_oci_parse_commit_annotations (annotations, NULL,
                                          NULL, NULL,
                                          &remote_oci_ref, NULL, NULL,
                                          NULL);

  refs_builder = g_variant_builder_new (G_VARIANT_TYPE ("a(s(taya{sv}))"));
  ref_data_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{s(tts)}"));
  additional_metadata_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

  if (remote_oci_ref != NULL && g_strcmp0 (remote_oci_ref, oci_ref) == 0 && g_str_has_prefix (oci_digest, "sha256:"))
    {
      const char *fake_commit = oci_digest + strlen ("sha256:");
      guint64 installed_size = 0;
      guint64 download_size = 0;
      const char *installed_size_str;
      const char *metadata_contents = NULL;
      int i;

      g_variant_builder_add_value (refs_builder,
                                   g_variant_new ("(s(t@ay@a{sv}))", oci_ref,
                                                  0,
                                                  ostree_checksum_to_bytes_v (fake_commit),
                                                  flatpak_gvariant_new_empty_string_dict ()));

      for (i = 0; manifest->layers != NULL && manifest->layers[i] != NULL; i++)
        download_size += manifest->layers[i]->size;

      installed_size_str = g_hash_table_lookup (annotations, "org.flatpak.InstalledSize");
      if (installed_size_str)
        installed_size = g_ascii_strtoull (installed_size_str, NULL, 10);

      metadata_contents = g_hash_table_lookup (annotations, "org.flatpak.Metadata");

      g_variant_builder_add (ref_data_builder, "{s(tts)}",
                             oci_ref,
                             GUINT64_TO_BE (installed_size),
                             GUINT64_TO_BE (download_size),
                             metadata_contents ? metadata_contents : "");
    }

  g_variant_builder_add (additional_metadata_builder, "{sv}", "xa.cache",
                         g_variant_new_variant (g_variant_builder_end (ref_data_builder)));

  summary_builder = g_variant_builder_new (OSTREE_SUMMARY_GVARIANT_FORMAT);

  g_variant_builder_add_value (summary_builder, g_variant_builder_end (refs_builder));
  g_variant_builder_add_value (summary_builder, g_variant_builder_end (additional_metadata_builder));

  summary = g_variant_ref_sink (g_variant_builder_end (summary_builder));

  *out_summary = g_variant_get_data_as_bytes (summary);
  return TRUE;
}

static gboolean
flatpak_dir_remote_fetch_summary (FlatpakDir   *self,
                                  const char   *name,
                                  GBytes      **out_summary,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  g_autofree char *url = NULL;
  gboolean is_local;
  g_autoptr(GError) local_error = NULL;
  g_autofree char *oci_uri = NULL;
  GBytes *summary;

  if (!ostree_repo_remote_get_url (self->repo, name, &url, error))
    return FALSE;

  is_local = g_str_has_prefix (url, "file:");

  /* No caching for local files */
  if (!is_local)
    {
      GBytes *cached_summary = flatpak_dir_lookup_cached_summary (self, name, url);
      if (cached_summary)
        {
          *out_summary = cached_summary;
          return TRUE;
        }
    }

  /* Seems ostree asserts if this is NULL */
  if (error == NULL)
    error = &local_error;

  oci_uri = flatpak_dir_get_remote_oci_uri (self, name);

  if (oci_uri != NULL)
    {
      if (!flatpak_dir_remote_make_oci_summary (self, name,
                                                &summary,
                                                cancellable,
                                                error))
        return FALSE;
    }
  else
    {
      if (!ostree_repo_remote_fetch_summary (self->repo, name,
                                             &summary, NULL,
                                             cancellable,
                                             error))
        return FALSE;
    }

  if (summary == NULL)
    return flatpak_fail (error, "Remote listing for %s not available; server has no summary file\n" \
                         "Check the URL passed to remote-add was valid\n", name);

  if (!is_local)
    flatpak_dir_cache_summary (self, summary, name, url);

  *out_summary = summary;

  return TRUE;
}

gboolean
flatpak_dir_remote_has_ref (FlatpakDir   *self,
                            const char   *remote,
                            const char   *ref)
{
  g_autoptr(GBytes) summary_bytes = NULL;
  g_autoptr(GVariant) summary = NULL;
  g_autoptr(GError) local_error = NULL;

  if (!flatpak_dir_remote_fetch_summary (self, remote,
                                         &summary_bytes,
                                         NULL, &local_error))
    {
      g_debug ("Can't get summary for remote %s: %s\n", remote, local_error->message);
      return FALSE;
    }

  summary = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                                          summary_bytes, FALSE));

  return flatpak_summary_lookup_ref (summary, ref, NULL);
}

/* This duplicates ostree_repo_list_refs so it can use flatpak_dir_remote_fetch_summary
   and get caching */
static gboolean
flatpak_dir_remote_list_refs (FlatpakDir       *self,
                              const char       *remote_name,
                              GHashTable      **out_all_refs,
                              GCancellable     *cancellable,
                              GError          **error)
{
  g_autoptr(GBytes) summary_bytes = NULL;
  g_autoptr(GHashTable) ret_all_refs = NULL;
  g_autoptr(GVariant) summary = NULL;
  g_autoptr(GVariant) ref_map = NULL;
  GVariantIter iter;
  GVariant *child;

  if (!flatpak_dir_remote_fetch_summary (self, remote_name,
                                         &summary_bytes,
                                         cancellable, error))
    return FALSE;

  ret_all_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  summary = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                                          summary_bytes, FALSE));

  ref_map = g_variant_get_child_value (summary, 0);

  g_variant_iter_init (&iter, ref_map);
  while ((child = g_variant_iter_next_value (&iter)) != NULL)
    {
      const char *ref_name = NULL;
      g_autoptr(GVariant) csum_v = NULL;
      char tmp_checksum[65];

      g_variant_get_child (child, 0, "&s", &ref_name);

      if (ref_name != NULL)
        {
          const guchar *csum_bytes;

          g_variant_get_child (child, 1, "(t@aya{sv})", NULL, &csum_v, NULL);
          csum_bytes = ostree_checksum_bytes_peek_validate (csum_v, error);
          if (csum_bytes == NULL)
            return FALSE;

          ostree_checksum_inplace_from_bytes (csum_bytes, tmp_checksum);

          g_hash_table_insert (ret_all_refs,
                               g_strdup (ref_name),
                               g_strdup (tmp_checksum));
        }

      g_variant_unref (child);
    }


  *out_all_refs = g_steal_pointer (&ret_all_refs);

  return TRUE;
}

static GPtrArray *
find_matching_refs (GHashTable *refs,
                    const char   *opt_name,
                    const char   *opt_branch,
                    const char   *opt_arch,
                    FlatpakKinds  kinds,
                    GError      **error)
{
  g_autoptr(GPtrArray) matched_refs = NULL;
  const char **arches = flatpak_get_arches ();
  const char *opt_arches[] = {opt_arch, NULL};
  GHashTableIter hash_iter;
  gpointer key;
  g_autoptr(GError) local_error = NULL;

  if (opt_arch != NULL)
    arches = opt_arches;

  if (opt_name && !flatpak_is_valid_name (opt_name, &local_error))
    {
      flatpak_fail (error, "'%s' is not a valid name: %s", opt_name, local_error->message);
      return NULL;
    }

  if (opt_branch && !flatpak_is_valid_branch (opt_branch, &local_error))
    {
      flatpak_fail (error, "'%s' is not a valid branch name: %s", opt_branch, local_error->message);
      return NULL;
    }

  matched_refs = g_ptr_array_new_with_free_func (g_free);

  g_hash_table_iter_init (&hash_iter, refs);
  while (g_hash_table_iter_next (&hash_iter, &key, NULL))
    {
      g_autofree char *ref = NULL;
      g_auto(GStrv) parts = NULL;
      gboolean is_app, is_runtime;

      /* Unprefix any remote name if needed */
      ostree_parse_refspec (key, NULL, &ref, NULL);
      if (ref == NULL)
        continue;

      is_app = g_str_has_prefix (ref, "app/");
      is_runtime = g_str_has_prefix (ref, "runtime/");

      if ((!(kinds & FLATPAK_KINDS_APP) && is_app) ||
          (!(kinds & FLATPAK_KINDS_RUNTIME) && is_runtime) ||
          (!is_app && !is_runtime))
        continue;

      parts = flatpak_decompose_ref (ref, NULL);
      if (parts == NULL)
        continue;

      if (opt_name != NULL && strcmp (opt_name, parts[1]) != 0)
        continue;

      if (!g_strv_contains (arches, parts[2]))
        continue;

      if (opt_branch != NULL && strcmp (opt_branch, parts[3]) != 0)
        continue;

      g_ptr_array_add (matched_refs, g_steal_pointer (&ref));
    }

  return g_steal_pointer (&matched_refs);
}


static char *
find_matching_ref (GHashTable *refs,
                   const char   *name,
                   const char   *opt_branch,
                   const char   *opt_default_branch,
                   const char   *opt_arch,
                   FlatpakKinds  kinds,
                   GError      **error)
{
  const char **arches = flatpak_get_arches ();
  const char *opt_arches[] = {opt_arch, NULL};
  int i;

  if (opt_arch != NULL)
    arches = opt_arches;

  /* We stop at the first arch (in prio order) that has a match */
  for (i = 0; arches[i] != NULL; i++)
    {
      g_autoptr(GPtrArray) matched_refs = NULL;
      int j;

      matched_refs = find_matching_refs (refs, name, opt_branch, arches[i],
                                         kinds, error);
      if (matched_refs == NULL)
        return NULL;

      if (matched_refs->len == 0)
        continue;

      if (matched_refs->len == 1)
        return g_strdup (g_ptr_array_index (matched_refs, 0));

      /* Multiple refs found, see if some belongs to the default branch, if passed */
      if (opt_default_branch != NULL)
        {
          for (j = 0; j < matched_refs->len; j++)
            {
              char *current_ref = g_ptr_array_index (matched_refs, j);
              g_auto(GStrv) parts = flatpak_decompose_ref (current_ref, NULL);

              if (g_strcmp0 (opt_default_branch, parts[3]) == 0)
                return g_strdup (current_ref);
            }
        }

      /* Nothing to do other than reporting the different choices */
      g_autoptr(GString) err = g_string_new ("");
      g_string_printf (err, "Multiple branches available for %s, you must specify one of: ", name);
      g_ptr_array_sort (matched_refs, flatpak_strcmp0_ptr);
      for (j = 0; j < matched_refs->len; j++)
        {
          g_auto(GStrv) parts = flatpak_decompose_ref (g_ptr_array_index (matched_refs, j), NULL);
          if (j != 0)
            g_string_append (err, ", ");

          g_string_append (err,
                           g_strdup_printf ("%s/%s/%s",
                                            name,
                                            opt_arch ? opt_arch : "",
                                            parts[3]));
        }

      flatpak_fail (error, err->str);
      return NULL;
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
               _("Nothing matches %s"), name);
  return NULL;
}

char **
flatpak_dir_find_remote_refs (FlatpakDir   *self,
                             const char   *remote,
                             const char   *name,
                             const char   *opt_branch,
                             const char   *opt_arch,
                             FlatpakKinds  kinds,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GHashTable) remote_refs = NULL;
  GPtrArray *matched_refs;

  if (!flatpak_dir_ensure_repo (self, NULL, error))
    return NULL;

  if (!flatpak_dir_remote_list_refs (self, remote,
                                     &remote_refs, cancellable, error))
    return NULL;

  matched_refs = find_matching_refs (remote_refs, name, opt_branch,
                                      opt_arch, kinds, error);
  if (matched_refs == NULL)
    return NULL;

  g_ptr_array_add (matched_refs, NULL);
  return (char **)g_ptr_array_free (matched_refs, FALSE);
}

char *
flatpak_dir_find_remote_ref (FlatpakDir   *self,
                             const char   *remote,
                             const char   *name,
                             const char   *opt_branch,
                             const char   *opt_default_branch,
                             const char   *opt_arch,
                             FlatpakKinds  kinds,
                             FlatpakKinds *out_kind,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autofree char *remote_ref = NULL;
  g_autoptr(GHashTable) remote_refs = NULL;
  g_autoptr(GError) my_error = NULL;

  if (!flatpak_dir_ensure_repo (self, NULL, error))
    return NULL;

  if (!flatpak_dir_remote_list_refs (self, remote,
                                     &remote_refs, cancellable, error))
    return NULL;

  remote_ref = find_matching_ref (remote_refs, name, opt_branch, opt_default_branch,
                                  opt_arch, kinds, &my_error);
  if (remote_ref == NULL)
    {
      if (g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_clear_error (&my_error);
      else
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return NULL;
        }
    }
  else
    {
      if (out_kind != NULL)
        {
          if (g_str_has_prefix (remote_ref, "app/"))
            *out_kind = FLATPAK_KINDS_APP;
          else
            *out_kind = FLATPAK_KINDS_RUNTIME;
        }

      return g_steal_pointer (&remote_ref);
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
               _("Can't find %s%s%s%s%s in remote %s"), name,
               (opt_arch != NULL || opt_branch != NULL) ? "/" : "",
               opt_arch ? opt_arch : "",
               opt_branch ? "/" : "",
               opt_branch ? opt_branch : "",
               remote);

  return NULL;
}


static GHashTable *
flatpak_dir_get_all_installed_refs (FlatpakDir  *self,
                                    FlatpakKinds kinds,
                                    GError     **error)
{
  g_autoptr(GHashTable) local_refs = NULL;
  int i;

  if (!flatpak_dir_ensure_repo (self, NULL, error))
    return NULL;

  local_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (kinds & FLATPAK_KINDS_APP)
    {
      g_auto(GStrv) app_refs = NULL;

      if (!flatpak_dir_list_refs (self, "app", &app_refs, NULL, error))
        return NULL;

      for (i = 0; app_refs[i] != NULL; i++)
        g_hash_table_insert (local_refs, g_strdup (app_refs[i]),
                             GINT_TO_POINTER (1));
    }
  if (kinds & FLATPAK_KINDS_RUNTIME)
    {
      g_auto(GStrv) runtime_refs = NULL;

      if (!flatpak_dir_list_refs (self, "runtime", &runtime_refs, NULL, error))
        return NULL;

      for (i = 0; runtime_refs[i] != NULL; i++)
        g_hash_table_insert (local_refs, g_strdup (runtime_refs[i]),
                             GINT_TO_POINTER (1));
    }

  return g_steal_pointer (&local_refs);
}

char **
flatpak_dir_find_installed_refs (FlatpakDir *self,
                                 const char *opt_name,
                                 const char *opt_branch,
                                 const char *opt_arch,
                                 FlatpakKinds kinds,
                                 GError    **error)
{
  g_autoptr(GHashTable) local_refs = NULL;
  GPtrArray *matched_refs;

  local_refs = flatpak_dir_get_all_installed_refs (self, kinds, error);
  if (local_refs == NULL)
    return NULL;

  matched_refs = find_matching_refs (local_refs, opt_name, opt_branch,
                                      opt_arch, kinds, error);
  if (matched_refs == NULL)
    return NULL;

  g_ptr_array_add (matched_refs, NULL);
  return (char **)g_ptr_array_free (matched_refs, FALSE);

}

char *
flatpak_dir_find_installed_ref (FlatpakDir   *self,
                                const char   *opt_name,
                                const char   *opt_branch,
                                const char   *opt_arch,
                                FlatpakKinds  kinds,
                                FlatpakKinds *out_kind,
                                GError      **error)
{
  g_autofree char *local_ref = NULL;
  g_autoptr(GHashTable) local_refs = NULL;
  g_autoptr(GError) my_error = NULL;

  local_refs = flatpak_dir_get_all_installed_refs (self, kinds, error);
  if (local_refs == NULL)
    return NULL;

  local_ref = find_matching_ref (local_refs, opt_name, opt_branch, NULL,
                                  opt_arch, kinds, &my_error);
  if (local_ref == NULL)
    {
      if (g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_clear_error (&my_error);
      else
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return NULL;
        }
    }
  else
    {
      if (out_kind != NULL)
        {
          if (g_str_has_prefix (local_ref, "app/"))
            *out_kind = FLATPAK_KINDS_APP;
          else
            *out_kind = FLATPAK_KINDS_RUNTIME;
        }

      return g_steal_pointer (&local_ref);
    }

  g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
               _("%s %s not installed"), opt_name ? opt_name : "*unspecified*", opt_branch ? opt_branch : "master");
  return NULL;
}

static FlatpakDir *
flatpak_dir_new_full (GFile *path, gboolean user, DirExtraData *extra_data)
{
  FlatpakDir *dir = g_object_new (FLATPAK_TYPE_DIR, "path", path, "user", user, NULL);

  if (extra_data != NULL)
    dir->extra_data = dir_extra_data_clone (extra_data);

  return dir;
}

FlatpakDir *
flatpak_dir_new (GFile *path, gboolean user)
{
  /* We are only interested on extra data for system-wide installations, in which
     case we use _new_full() directly, so here we just call it passing NULL */
  return flatpak_dir_new_full (path, user, NULL);
}

FlatpakDir *
flatpak_dir_clone (FlatpakDir *self)
{
  return flatpak_dir_new_full (self->basedir, self->user, self->extra_data);
}

FlatpakDir *
flatpak_dir_get_system_default (void)
{
  g_autoptr(GFile) path = flatpak_get_system_default_base_dir_location ();
  return flatpak_dir_new_full (path, FALSE, NULL);
}

FlatpakDir *
flatpak_dir_get_system_by_id (const char   *id,
                              GCancellable *cancellable,
                              GError      **error)
{
  g_autoptr(GError) local_error = NULL;
  GPtrArray *locations = NULL;
  FlatpakDir *ret = NULL;
  int i;

  if (id == NULL)
    return flatpak_dir_get_system_default ();

  /* An error in flatpak_get_system_base_dir_locations() will still return
   * return an empty array with the GError set, but we want to return NULL.
   */
  locations = flatpak_get_system_base_dir_locations (cancellable, &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  for (i = 0; i < locations->len; i++)
    {
      GFile *path = g_ptr_array_index (locations, i);
      DirExtraData *extra_data = g_object_get_data (G_OBJECT (path), "extra-data");
      if (extra_data != NULL && g_strcmp0 (extra_data->id, id) == 0)
        {
          ret = flatpak_dir_new_full (path, FALSE, extra_data);
          break;
        }
    }

  if (ret == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   _("Could not find installation %s"), id);
    }

  return ret;
}

GPtrArray *
flatpak_dir_get_system_list (GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GPtrArray) result = NULL;
  g_autoptr(GError) local_error = NULL;
  GPtrArray *locations = NULL;
  int i;

  /* An error in flatpak_get_system_base_dir_locations() will still return
   * return an empty array with the GError set, but we want to return NULL.
   */
  locations = flatpak_get_system_base_dir_locations (cancellable, &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  result = g_ptr_array_new_with_free_func (g_object_unref);
  for (i = 0; i < locations->len; i++)
    {
      GFile *path = g_ptr_array_index (locations, i);
      DirExtraData *extra_data = g_object_get_data (G_OBJECT (path), "extra-data");
      g_ptr_array_add (result, flatpak_dir_new_full (path, FALSE, extra_data));
    }

  return g_steal_pointer (&result);
}

FlatpakDir *
flatpak_dir_get_user (void)
{
  g_autoptr(GFile) path = flatpak_get_user_base_dir_location ();
  return flatpak_dir_new (path, TRUE);
}

static char *
get_group (const char *remote_name)
{
  return g_strdup_printf ("remote \"%s\"", remote_name);
}

char *
flatpak_dir_get_remote_title (FlatpakDir *self,
                              const char *remote_name)
{
  GKeyFile *config = ostree_repo_get_config (self->repo);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_string (config, group, "xa.title", NULL);

  return NULL;
}

char *
flatpak_dir_get_remote_oci_uri (FlatpakDir *self,
                                const char *remote_name)
{
  GKeyFile *config = ostree_repo_get_config (self->repo);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_string (config, group, "xa.oci-uri", NULL);

  return NULL;
}

char *
flatpak_dir_get_remote_main_ref (FlatpakDir *self,
                                const char *remote_name)
{
  GKeyFile *config = ostree_repo_get_config (self->repo);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_string (config, group, "xa.main-ref", NULL);

  return NULL;
}

char *
flatpak_dir_get_remote_oci_tag (FlatpakDir *self,
                                const char *remote_name)
{
  GKeyFile *config = ostree_repo_get_config (self->repo);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_string (config, group, "xa.oci-tag", NULL);

  return NULL;
}

char *
flatpak_dir_get_remote_default_branch (FlatpakDir *self,
                                       const char *remote_name)
{
  GKeyFile *config = ostree_repo_get_config (self->repo);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_string (config, group, "xa.default-branch", NULL);

  return NULL;
}

int
flatpak_dir_get_remote_prio (FlatpakDir *self,
                             const char *remote_name)
{
  GKeyFile *config = ostree_repo_get_config (self->repo);
  g_autofree char *group = get_group (remote_name);

  if (config && g_key_file_has_key (config, group, "xa.prio", NULL))
    return g_key_file_get_integer (config, group, "xa.prio", NULL);

  return 1;
}

gboolean
flatpak_dir_get_remote_noenumerate (FlatpakDir *self,
                                    const char *remote_name)
{
  GKeyFile *config = ostree_repo_get_config (self->repo);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_boolean (config, group, "xa.noenumerate", NULL);

  return TRUE;
}

gboolean
flatpak_dir_get_remote_nodeps (FlatpakDir *self,
                               const char *remote_name)
{
  GKeyFile *config = ostree_repo_get_config (self->repo);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_boolean (config, group, "xa.nodeps", NULL);

  return TRUE;
}

gboolean
flatpak_dir_get_remote_disabled (FlatpakDir *self,
                                 const char *remote_name)
{
  GKeyFile *config = ostree_repo_get_config (self->repo);
  g_autofree char *group = get_group (remote_name);
  g_autofree char *url = NULL;
  g_autofree char *oci_uri = NULL;

  if (config &&
      g_key_file_get_boolean (config, group, "xa.disable", NULL))
    return TRUE;

  if (ostree_repo_remote_get_url (self->repo, remote_name, &url, NULL) && *url == 0)
    {
      oci_uri = flatpak_dir_get_remote_oci_uri (self, remote_name);
      if (oci_uri == NULL)
        return TRUE; /* Empty URL => disabled */
    }

  return FALSE;
}

gint
cmp_remote (gconstpointer a,
            gconstpointer b,
            gpointer      user_data)
{
  FlatpakDir *self = user_data;
  const char *a_name = *(const char **) a;
  const char *b_name = *(const char **) b;
  int prio_a, prio_b;

  prio_a = flatpak_dir_get_remote_prio (self, a_name);
  prio_b = flatpak_dir_get_remote_prio (self, b_name);

  return prio_b - prio_a;
}

static char *
create_origin_remote_config (OstreeRepo   *repo,
                             const char   *url,
                             const char   *id,
                             const char   *title,
                             const char   *main_ref,
                             const char   *oci_uri,
                             const char   *oci_tag,
                             gboolean      gpg_verify,
                             GKeyFile     *new_config)
{
  g_autofree char *remote = NULL;
  g_auto(GStrv) remotes = NULL;
  int version = 0;
  g_autofree char *group = NULL;

  remotes = ostree_repo_remote_list (repo, NULL);

  do
    {
      g_autofree char *name = NULL;
      if (version == 0)
        name = g_strdup_printf ("%s-origin", id);
      else
        name = g_strdup_printf ("%s-%d-origin", id, version);
      version++;

      if (remotes == NULL ||
          !g_strv_contains ((const char * const *) remotes, name))
        remote = g_steal_pointer (&name);
    }
  while (remote == NULL);

  group = g_strdup_printf ("remote \"%s\"", remote);

  g_key_file_set_string (new_config, group, "url", url ? url : "");
  g_key_file_set_string (new_config, group, "xa.title", title);
  g_key_file_set_string (new_config, group, "xa.noenumerate", "true");
  g_key_file_set_string (new_config, group, "xa.prio", "0");
  if (gpg_verify)
    {
      g_key_file_set_string (new_config, group, "gpg-verify", "true");
      g_key_file_set_string (new_config, group, "gpg-verify-summary", "true");
    }
  else
    {
      g_key_file_set_string (new_config, group, "gpg-verify", "false");
      g_key_file_set_string (new_config, group, "gpg-verify-summary", "false");
    }
  if (main_ref)
    g_key_file_set_string (new_config, group, "xa.main-ref", main_ref);
  if (oci_uri)
    g_key_file_set_string (new_config, group, "xa.oci-uri", oci_uri);
  if (oci_tag)
    g_key_file_set_string (new_config, group, "xa.oci-tag", oci_tag);

  return g_steal_pointer (&remote);
}

char *
flatpak_dir_create_origin_remote (FlatpakDir   *self,
                                  const char   *url,
                                  const char   *id,
                                  const char   *title,
                                  const char   *main_ref,
                                  const char   *oci_uri,
                                  const char   *oci_tag,
                                  GBytes       *gpg_data,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  g_autoptr(GKeyFile) new_config = g_key_file_new ();
  g_autofree char *remote = NULL;

  remote = create_origin_remote_config (self->repo, url, id, title, main_ref, oci_uri, oci_tag, gpg_data != NULL, new_config);

  if (!flatpak_dir_modify_remote (self, remote, new_config,
                                  gpg_data, cancellable, error))
    return NULL;

  return g_steal_pointer (&remote);
}

GKeyFile *
flatpak_dir_parse_repofile (FlatpakDir   *self,
                            const char   *remote_name,
                            GBytes       *data,
                            GBytes      **gpg_data_out,
                            GCancellable *cancellable,
                            GError      **error)
{
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GBytes) gpg_data = NULL;
  g_autofree char *uri = NULL;
  g_autofree char *title = NULL;
  g_autofree char *gpg_key = NULL;
  g_autofree char *default_branch = NULL;
  gboolean nodeps;
  GKeyFile *config = g_key_file_new ();
  g_autofree char *group = g_strdup_printf ("remote \"%s\"", remote_name);

  if (!g_key_file_load_from_data (keyfile,
                                  g_bytes_get_data (data, NULL),
                                  g_bytes_get_size (data),
                                  0, &local_error))
    {
      flatpak_fail (error, "Invalid .flatpakref: %s\n", local_error->message);
      return NULL;
    }

  if (!g_key_file_has_group (keyfile, FLATPAK_REPO_GROUP))
    {
      flatpak_fail (error, "Invalid .flatpakref\n");
      return NULL;
    }

  uri = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                               FLATPAK_REPO_URL_KEY, NULL);
  if (uri == NULL)
    {
      flatpak_fail (error, "Invalid .flatpakref\n");
      return NULL;
    }

  g_key_file_set_string (config, group, "url", uri);

  title = g_key_file_get_locale_string (keyfile, FLATPAK_REPO_GROUP,
                                        FLATPAK_REPO_TITLE_KEY, NULL, NULL);
  if (title != NULL)
    g_key_file_set_string (config, group, "xa.title", title);

  default_branch = g_key_file_get_locale_string (keyfile, FLATPAK_REPO_GROUP,
                                                 FLATPAK_REPO_DEFAULT_BRANCH_KEY, NULL, NULL);
  if (default_branch != NULL)
    g_key_file_set_string (config, group, "xa.default-branch", default_branch);

  nodeps = g_key_file_get_boolean (keyfile, FLATPAK_REPO_GROUP,
                                   FLATPAK_REPO_NODEPS_KEY, NULL);
  if (nodeps)
    g_key_file_set_boolean (config, group, "xa.nodeps", TRUE);

  gpg_key = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                                   FLATPAK_REPO_GPGKEY_KEY, NULL);
  if (gpg_key != NULL)
    {
      guchar *decoded;
      gsize decoded_len;

      gpg_key = g_strstrip (gpg_key);
      decoded = g_base64_decode (gpg_key, &decoded_len);
      if (decoded_len < 10) /* Check some minimal size so we don't get crap */
        {
          flatpak_fail (error, "Invalid gpg key\n");
          return NULL;
        }

      gpg_data = g_bytes_new_take (decoded, decoded_len);
      g_key_file_set_boolean (config, group, "gpg-verify", TRUE);
      g_key_file_set_boolean (config, group, "gpg-verify-summary", TRUE);
    }

  *gpg_data_out = g_steal_pointer (&gpg_data);

  return g_steal_pointer (&config);
}

static gboolean
parse_ref_file (GBytes *data,
                char **name_out,
                char **branch_out,
                char **url_out,
                char **title_out,
                GBytes **gpg_data_out,
                gboolean *is_runtime_out,
                GError **error)
{
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  g_autofree char *url = NULL;
  g_autofree char *title = NULL;
  g_autofree char *name = NULL;
  g_autofree char *branch = NULL;
  g_autofree char *version = NULL;
  g_autoptr(GBytes) gpg_data = NULL;
  gboolean is_runtime = FALSE;
  char *str;

  *name_out = NULL;
  *branch_out = NULL;
  *url_out = NULL;
  *title_out = NULL;
  *gpg_data_out = NULL;
  *is_runtime_out = FALSE;

  if (!g_key_file_load_from_data (keyfile, g_bytes_get_data (data, NULL), g_bytes_get_size (data),
                                  0, error))
    return FALSE;

  if (!g_key_file_has_group (keyfile, FLATPAK_REF_GROUP))
    return flatpak_fail (error, "Invalid file format, no %s group", FLATPAK_REF_GROUP);

  version = g_key_file_get_string (keyfile, FLATPAK_REF_GROUP,
                                   FLATPAK_REF_VERSION_KEY, NULL);
  if (version != NULL && strcmp (version, "1") != 0)
    return flatpak_fail (error, "Invalid version %s, only 1 supported", version);

  url = g_key_file_get_string (keyfile, FLATPAK_REF_GROUP,
                               FLATPAK_REF_URL_KEY, NULL);
  if (url == NULL)
    return flatpak_fail (error, "Invalid file format, no Url specified");

  name = g_key_file_get_string (keyfile, FLATPAK_REF_GROUP,
                                FLATPAK_REF_NAME_KEY, NULL);
  if (name == NULL)
    return flatpak_fail (error, "Invalid file format, no Name specified");

  branch = g_key_file_get_string (keyfile, FLATPAK_REF_GROUP,
                                  FLATPAK_REF_BRANCH_KEY, NULL);
  if (branch == NULL)
    branch = g_strdup ("master");

  title = g_key_file_get_string (keyfile, FLATPAK_REF_GROUP,
                                 FLATPAK_REF_TITLE_KEY, NULL);

  is_runtime = g_key_file_get_boolean (keyfile, FLATPAK_REF_GROUP,
                                       FLATPAK_REF_IS_RUNTIME_KEY, NULL);

  str = g_key_file_get_string (keyfile, FLATPAK_REF_GROUP,
                               FLATPAK_REF_GPGKEY_KEY, NULL);
  if (str != NULL)
    {
      guchar *decoded;
      gsize decoded_len;

      str = g_strstrip (str);
      decoded = g_base64_decode (str, &decoded_len);
      if (decoded_len < 10) /* Check some minimal size so we don't get crap */
        return flatpak_fail (error, "Invalid file format, gpg key invalid");

      gpg_data = g_bytes_new_take (decoded, decoded_len);
    }

  *name_out = g_steal_pointer (&name);
  *branch_out = g_steal_pointer (&branch);
  *url_out = g_steal_pointer (&url);
  *title_out = g_steal_pointer (&title);
  *gpg_data_out = g_steal_pointer (&gpg_data);
  *is_runtime_out = is_runtime;

  return TRUE;
}

gboolean
flatpak_dir_create_remote_for_ref_file (FlatpakDir *self,
                                        GBytes     *data,
                                        const char *default_arch,
                                        char      **remote_name_out,
                                        char      **ref_out,
                                        GError    **error)
{
  g_autoptr(GBytes) gpg_data = NULL;
  g_autofree char *name = NULL;
  g_autofree char *branch = NULL;
  g_autofree char *url = NULL;
  g_autofree char *title = NULL;
  g_autofree char *ref = NULL;
  g_autofree char *remote = NULL;
  gboolean is_runtime = FALSE;
  g_autoptr(GFile) deploy_dir = NULL;

  if (!parse_ref_file (data, &name, &branch, &url, &title, &gpg_data, &is_runtime, error))
    return FALSE;

  ref = flatpak_compose_ref (!is_runtime, name, branch, default_arch, error);
  if (ref == NULL)
    return FALSE;

  deploy_dir = flatpak_dir_get_if_deployed (self, ref, NULL, NULL);
  if (deploy_dir != NULL)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                   is_runtime ? _("Runtime %s, branch %s is already installed")
                   : _("App %s, branch %s is already installed"),
                   name, branch);
      return FALSE;
    }

  /* First try to reuse existing remote */
  remote = flatpak_dir_find_remote_by_uri (self, url);

  if (remote == NULL)
    {
      remote = flatpak_dir_create_origin_remote (self, url, name, title, ref, NULL, NULL,
                                                 gpg_data, NULL, error);
      if (remote == NULL)
        return FALSE;
    }

  *remote_name_out = g_steal_pointer (&remote);
  *ref_out = (char *)g_steal_pointer (&ref);
  return TRUE;
}

char *
flatpak_dir_find_remote_by_uri (FlatpakDir   *self,
                                const char   *uri)
{
  g_auto(GStrv) remotes = NULL;

  if (!flatpak_dir_ensure_repo (self, NULL, NULL))
    return NULL;

  remotes = flatpak_dir_list_enumerated_remotes (self, NULL, NULL);
  if (remotes)
    {
      int i;

      for (i = 0; remotes != NULL && remotes[i] != NULL; i++)
        {
          const char *remote = remotes[i];
          g_autofree char *remote_uri = NULL;

          if (!ostree_repo_remote_get_url (self->repo,
                                           remote,
                                           &remote_uri,
                                           NULL))
            continue;

          if (strcmp (uri, remote_uri) == 0)
            return g_strdup (remote);
        }
    }

  return NULL;
}

char **
flatpak_dir_list_remotes (FlatpakDir   *self,
                          GCancellable *cancellable,
                          GError      **error)
{
  char **res;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return NULL;

  res = ostree_repo_remote_list (self->repo, NULL);
  if (res == NULL)
    res = g_new0 (char *, 1); /* Return empty array, not error */

  g_qsort_with_data (res, g_strv_length (res), sizeof (char *),
                     cmp_remote, self);

  return res;
}

char **
flatpak_dir_list_enumerated_remotes (FlatpakDir   *self,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  g_autoptr(GPtrArray) res = g_ptr_array_new_with_free_func (g_free);
  g_auto(GStrv) remotes = NULL;
  int i;

  remotes = flatpak_dir_list_remotes (self, cancellable, error);
  if (remotes == NULL)
    return NULL;

  for (i = 0; remotes != NULL && remotes[i] != NULL; i++)
    {
      const char *remote = remotes[i];

      if (flatpak_dir_get_remote_disabled (self, remote))
        continue;

      if (flatpak_dir_get_remote_noenumerate (self, remote))
        continue;

      g_ptr_array_add (res, g_strdup (remote));
    }

  g_ptr_array_add (res, NULL);
  return (char **)g_ptr_array_free (g_steal_pointer (&res), FALSE);
}

char **
flatpak_dir_search_for_dependency (FlatpakDir   *self,
                                   const char   *runtime_ref,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  g_autoptr(GPtrArray) found = g_ptr_array_new_with_free_func (g_free);
  g_auto(GStrv) remotes = NULL;
  int i;

  remotes = flatpak_dir_list_enumerated_remotes (self, cancellable, error);
  if (remotes == NULL)
    return NULL;

  for (i = 0; remotes != NULL && remotes[i] != NULL; i++)
    {
      const char *remote = remotes[i];

      if (flatpak_dir_get_remote_nodeps (self, remote))
        continue;

      if (flatpak_dir_remote_has_ref (self, remote, runtime_ref))
        g_ptr_array_add (found, g_strdup (remote));
    }

  g_ptr_array_add (found, NULL);

  return (char **)g_ptr_array_free (g_steal_pointer (&found), FALSE);
}

gboolean
flatpak_dir_remove_remote (FlatpakDir   *self,
                           gboolean      force_remove,
                           const char   *remote_name,
                           GCancellable *cancellable,
                           GError      **error)
{
  g_autofree char *prefix = NULL;
  g_autoptr(GHashTable) refs = NULL;
  GHashTableIter hash_iter;
  gpointer key;

  if (flatpak_dir_use_system_helper (self))
    {
      FlatpakSystemHelper *system_helper;
      g_autoptr(GVariant) gpg_data_v = NULL;
      FlatpakHelperConfigureRemoteFlags flags = 0;
      const char *installation = flatpak_dir_get_id (self);

      gpg_data_v = g_variant_ref_sink (g_variant_new_from_data (G_VARIANT_TYPE ("ay"), "", 0, TRUE, NULL, NULL));

      system_helper = flatpak_dir_get_system_helper (self);
      g_assert (system_helper != NULL);

      if (force_remove)
        flags |= FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_FORCE_REMOVE;

      g_debug ("Calling system helper: ConfigureRemote");
      if (!flatpak_system_helper_call_configure_remote_sync (system_helper,
                                                             flags, remote_name,
                                                             "",
                                                             gpg_data_v,
                                                             installation ? installation : "",
                                                             cancellable, error))
        return FALSE;

      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  if (!ostree_repo_list_refs (self->repo,
                              NULL,
                              &refs,
                              cancellable, error))
    return FALSE;

  prefix = g_strdup_printf ("%s:", remote_name);

  if (!force_remove)
    {
      g_hash_table_iter_init (&hash_iter, refs);
      while (g_hash_table_iter_next (&hash_iter, &key, NULL))
        {
          const char *refspec = key;

          if (g_str_has_prefix (refspec, prefix))
            {
              const char *unprefixed_refspec = refspec + strlen (prefix);
              g_autofree char *origin = flatpak_dir_get_origin (self, unprefixed_refspec,
                                                                cancellable, NULL);

              if (g_strcmp0 (origin, remote_name) == 0)
                return flatpak_fail (error, "Can't remove remote '%s' with installed ref %s (at least)",
                                     remote_name, unprefixed_refspec);
            }
        }
    }

  /* Remove all refs */
  g_hash_table_iter_init (&hash_iter, refs);
  while (g_hash_table_iter_next (&hash_iter, &key, NULL))
    {
      const char *refspec = key;

      if (g_str_has_prefix (refspec, prefix) &&
          !flatpak_dir_remove_ref (self, remote_name, refspec + strlen (prefix), cancellable, error))
        return FALSE;
    }

  if (!flatpak_dir_remove_appstream (self, remote_name,
                                     cancellable, error))
    return FALSE;

  if (!ostree_repo_remote_change (self->repo, NULL,
                                  OSTREE_REPO_REMOTE_CHANGE_DELETE,
                                  remote_name, NULL,
                                  NULL,
                                  cancellable, error))
    return FALSE;

  if (!flatpak_dir_mark_changed (self, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_dir_modify_remote (FlatpakDir   *self,
                           const char   *remote_name,
                           GKeyFile     *config,
                           GBytes       *gpg_data,
                           GCancellable *cancellable,
                           GError      **error)
{
  g_autofree char *group = g_strdup_printf ("remote \"%s\"", remote_name);
  g_autofree char *url = NULL;
  g_autofree char *metalink = NULL;
  g_autoptr(GKeyFile) new_config = NULL;
  g_auto(GStrv) keys = NULL;
  int i;

  if (strchr (remote_name, '/') != NULL)
    return flatpak_fail (error, "Invalid character '/' in remote name: %s",
                         remote_name);


  if (!g_key_file_has_group (config, group))
    return flatpak_fail (error, "No configuration for remote %s specified",
                         remote_name);


  if (flatpak_dir_use_system_helper (self))
    {
      FlatpakSystemHelper *system_helper;
      g_autofree char *config_data = g_key_file_to_data (config, NULL, NULL);
      g_autoptr(GVariant) gpg_data_v = NULL;
      const char *installation = flatpak_dir_get_id (self);

      if (gpg_data != NULL)
        gpg_data_v = variant_new_ay_bytes (gpg_data);
      else
        gpg_data_v = g_variant_ref_sink (g_variant_new_from_data (G_VARIANT_TYPE ("ay"), "", 0, TRUE, NULL, NULL));

      system_helper = flatpak_dir_get_system_helper (self);
      g_assert (system_helper != NULL);

      g_debug ("Calling system helper: ConfigureRemote");
      if (!flatpak_system_helper_call_configure_remote_sync (system_helper,
                                                             0, remote_name,
                                                             config_data,
                                                             gpg_data_v,
                                                             installation ? installation : "",
                                                             cancellable, error))
        return FALSE;

      return TRUE;
    }

  metalink = g_key_file_get_string (config, group, "metalink", NULL);
  if (metalink != NULL && *metalink != 0)
    url = g_strconcat ("metalink=", metalink, NULL);
  else
    url = g_key_file_get_string (config, group, "url", NULL);

  /* No url => disabled */
  if (url == NULL)
    url = g_strdup ("");

  /* Add it if its not there yet */
  if (!ostree_repo_remote_change (self->repo, NULL,
                                  OSTREE_REPO_REMOTE_CHANGE_ADD_IF_NOT_EXISTS,
                                  remote_name,
                                  url, NULL, cancellable, error))
    return FALSE;

  new_config = ostree_repo_copy_config (self->repo);

  g_key_file_remove_group (new_config, group, NULL);

  keys = g_key_file_get_keys (config,
                              group,
                              NULL, error);
  if (keys == NULL)
    return FALSE;

  for (i = 0; keys[i] != NULL; i++)
    {
      g_autofree gchar *value = g_key_file_get_value (config, group, keys[i], NULL);
      if (value)
        g_key_file_set_value (new_config, group, keys[i], value);
    }

  if (!ostree_repo_write_config (self->repo, new_config, error))
    return FALSE;

  if (gpg_data != NULL)
    {
      g_autoptr(GInputStream) input_stream = g_memory_input_stream_new_from_bytes (gpg_data);
      guint imported = 0;

      if (!ostree_repo_remote_gpg_import (self->repo, remote_name, input_stream,
                                          NULL, &imported, cancellable, error))
        return FALSE;

      /* XXX If we ever add internationalization, use ngettext() here. */
      g_debug ("Imported %u GPG key%s to remote \"%s\"",
               imported, (imported == 1) ? "" : "s", remote_name);
    }

  if (!flatpak_dir_mark_changed (self, error))
    return FALSE;

  return TRUE;
}

static gboolean
remove_unless_in_hash (gpointer key,
                       gpointer value,
                       gpointer user_data)
{
  GHashTable *table = user_data;

  return !g_hash_table_contains (table, key);
}

gboolean
flatpak_dir_list_remote_refs (FlatpakDir   *self,
                              const char   *remote,
                              GHashTable  **refs,
                              GCancellable *cancellable,
                              GError      **error)
{
  g_autoptr(GError) my_error = NULL;

  if (error == NULL)
    error = &my_error;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  if (!flatpak_dir_remote_list_refs (self, remote, refs,
                                     cancellable, error))
    return FALSE;

  if (flatpak_dir_get_remote_noenumerate (self, remote))
    {
      g_autoptr(GHashTable) unprefixed_local_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      g_autoptr(GHashTable) local_refs = NULL;
      GHashTableIter hash_iter;
      gpointer key;
      g_autofree char *refspec_prefix = g_strconcat (remote, ":.", NULL);

      /* For noenumerate remotes, only return data for already locally
       * available refs */

      if (!ostree_repo_list_refs (self->repo, refspec_prefix, &local_refs,
                                  cancellable, error))
        return FALSE;

      /* First we need to unprefix the remote name from the local refs */
      g_hash_table_iter_init (&hash_iter, local_refs);
      while (g_hash_table_iter_next (&hash_iter, &key, NULL))
        {
          char *ref = NULL;
          ostree_parse_refspec (key, NULL, &ref, NULL);

          if (ref)
            g_hash_table_insert (unprefixed_local_refs, ref, NULL);
        }

      /* Then we remove all remote refs not in the local refs set */
      g_hash_table_foreach_remove (*refs,
                                   remove_unless_in_hash,
                                   unprefixed_local_refs);
    }

  return TRUE;
}

static GVariant *
fetch_remote_summary_file (FlatpakDir   *self,
                           const char   *remote,
                           GCancellable *cancellable,
                           GError      **error)
{
  g_autoptr(GError) my_error = NULL;
  g_autoptr(GBytes) summary_bytes = NULL;

  if (error == NULL)
    error = &my_error;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return NULL;

  if (!flatpak_dir_remote_fetch_summary (self, remote,
                                         &summary_bytes,
                                         cancellable, error))
    return NULL;

  return g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                                       summary_bytes, FALSE));
}


char *
flatpak_dir_fetch_remote_title (FlatpakDir   *self,
                                const char   *remote,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_autoptr(GVariant) summary = NULL;
  g_autoptr(GVariant) extensions = NULL;
  g_autofree char *title = NULL;

  summary = fetch_remote_summary_file (self, remote, cancellable, error);
  if (summary == NULL)
    return NULL;

  extensions = g_variant_get_child_value (summary, 1);

  g_variant_lookup (extensions, "xa.title", "s", &title);

  if (title == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           _("Remote title not set"));
      return FALSE;
    }

  return g_steal_pointer (&title);
}

char *
flatpak_dir_fetch_remote_default_branch (FlatpakDir   *self,
                                         const char   *remote,
                                         GCancellable *cancellable,
                                         GError      **error)
{
  g_autoptr(GVariant) summary = NULL;
  g_autoptr(GVariant) extensions = NULL;
  g_autofree char *default_branch = NULL;

  summary = fetch_remote_summary_file (self, remote, cancellable, error);
  if (summary == NULL)
    return NULL;

  extensions = g_variant_get_child_value (summary, 1);
  g_variant_lookup (extensions, "xa.default-branch", "s", &default_branch);

  if (default_branch == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           _("Remote default-branch not set"));
      return FALSE;
    }

  return g_steal_pointer (&default_branch);
}

gboolean
flatpak_dir_update_remote_configuration (FlatpakDir   *self,
                                         const char   *remote,
                                         GCancellable *cancellable,
                                         GError      **error)
{
  /* We only support those configuration parameters that can
     be set in the server when building the repo (see the
     flatpak_repo_set_* () family of functions) */
  static const char *const supported_params[] = {
    "xa.title",
    "xa.default-branch", NULL
  };

  g_autoptr(GVariant) summary = NULL;
  g_autoptr(GVariant) extensions = NULL;
  g_autoptr(GPtrArray) updated_params = NULL;
  GVariantIter iter;

  updated_params = g_ptr_array_new_with_free_func (g_free);
  summary = fetch_remote_summary_file (self, remote, cancellable, error);
  if (summary == NULL)
    return FALSE;

  extensions = g_variant_get_child_value (summary, 1);

  g_variant_iter_init (&iter, extensions);
  if (g_variant_iter_n_children (&iter) > 0)
    {
      GVariant *value_var = NULL;
      char *key = NULL;

      while (g_variant_iter_next (&iter, "{sv}", &key, &value_var))
        {
          /* At the moment, every supported parameter are strings */
          if (g_strv_contains (supported_params, key) &&
              g_variant_get_type_string (value_var))
            {
              const char *value = g_variant_get_string(value_var, NULL);
              if (value != NULL && *value != 0)
                {
                  g_ptr_array_add (updated_params, g_strdup (key));
                  g_ptr_array_add (updated_params, g_strdup (value));
                }
            }

          g_variant_unref (value_var);
          g_free (key);
        }
    }

  if (updated_params->len > 0)
  {
    g_autoptr(GKeyFile) config = NULL;
    g_autofree char *group = NULL;
    gboolean has_changed = FALSE;
    int i;

    config = ostree_repo_copy_config (flatpak_dir_get_repo (self));
    group = g_strdup_printf ("remote \"%s\"", remote);

    i = 0;
    while (i < (updated_params->len - 1))
      {
        /* This array should have an even number of elements with
           keys in the odd positions and values on even ones. */
        const char *key = g_ptr_array_index (updated_params, i);
        const char *new_val = g_ptr_array_index (updated_params, i+1);
        g_autofree char *current_val = NULL;

        current_val = g_key_file_get_string (config, group, key, NULL);
        if (g_strcmp0 (current_val, new_val) != 0)
          {
            has_changed = TRUE;
            g_key_file_set_string (config, group, key, new_val);
          }

        i += 2;
      }

    if (!has_changed)
      return TRUE;

    if (flatpak_dir_use_system_helper (self))
      {
        FlatpakSystemHelper *system_helper;
        g_autofree char *config_data = g_key_file_to_data (config, NULL, NULL);
        g_autoptr(GVariant) gpg_data_v = NULL;
        const char *installation = flatpak_dir_get_id (self);

        gpg_data_v = g_variant_ref_sink (g_variant_new_from_data (G_VARIANT_TYPE ("ay"), "", 0, TRUE, NULL, NULL));

        system_helper = flatpak_dir_get_system_helper (self);
        g_assert (system_helper != NULL);

        g_debug ("Calling system helper: ConfigureRemote");
        if (!flatpak_system_helper_call_configure_remote_sync (system_helper,
                                                               0, remote,
                                                               config_data,
                                                               gpg_data_v,
                                                               installation ? installation : "",
                                                               cancellable, error))
          return FALSE;

        return TRUE;
      }

    /* Update the local remote configuration with the updated info. */
    if (!flatpak_dir_modify_remote (self, remote, config, NULL, cancellable, error))
      return FALSE;
  }

  return TRUE;
}

static gboolean
flatpak_dir_parse_summary_for_ref (FlatpakDir   *self,
                                   GVariant     *summary,
                                   const char   *ref,
                                   guint64      *download_size,
                                   guint64      *installed_size,
                                   char        **metadata,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  g_autoptr(GVariant) extensions = NULL;
  g_autoptr(GVariant) cache_v = NULL;
  g_autoptr(GVariant) cache = NULL;
  g_autoptr(GVariant) res = NULL;

  extensions = g_variant_get_child_value (summary, 1);

  cache_v = g_variant_lookup_value (extensions, "xa.cache", NULL);
  if (cache_v == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           _("No flatpak cache in remote summary"));
      return FALSE;
    }

  cache = g_variant_get_child_value (cache_v, 0);
  res = g_variant_lookup_value (cache, ref, NULL);
  if (res == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   _("No entry for %s in remote summary flatpak cache "), ref);
      return FALSE;
    }

  if (installed_size)
    {
      guint64 v;
      g_variant_get_child (res, 0, "t", &v);
      *installed_size = GUINT64_FROM_BE (v);
    }

  if (download_size)
    {
      guint64 v;
      g_variant_get_child (res, 1, "t", &v);
      *download_size = GUINT64_FROM_BE (v);
    }

  if (metadata)
    g_variant_get_child (res, 2, "s", metadata);

  return TRUE;
}

gboolean
flatpak_dir_fetch_ref_cache (FlatpakDir   *self,
                             const char   *remote_name,
                             const char   *ref,
                             guint64      *download_size,
                             guint64      *installed_size,
                             char        **metadata,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GBytes) summary_bytes = NULL;
  g_autoptr(GVariant) summary = NULL;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  if (!flatpak_dir_remote_fetch_summary (self, remote_name,
                                         &summary_bytes,
                                         cancellable, error))
    return FALSE;

  summary = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                                          summary_bytes, FALSE));

  return flatpak_dir_parse_summary_for_ref (self, summary, ref,
                                            download_size, installed_size,
                                            metadata,
                                            cancellable, error);
}

void
flatpak_related_free (FlatpakRelated *self)
{
  g_free (self->ref);
  g_free (self->commit);
  g_strfreev (self->subpaths);
  g_free (self);
}

static gboolean
string_in_array (GPtrArray *array,
                 const char *str)
{
  int i;

  for (i = 0; i < array->len; i++)
    {
      if (strcmp (g_ptr_array_index (array, i), str) == 0)
        return TRUE;
    }

  return FALSE;
}

static void
add_related (FlatpakDir *self,
             GPtrArray *related,
             const char *extension,
             const char *extension_ref,
             const char *checksum,
             gboolean no_autodownload,
             const char *download_if,
             gboolean autodelete)
{
  g_autoptr(GVariant) deploy_data = NULL;
  const char **old_subpaths = NULL;
  g_autoptr(GPtrArray) subpaths = g_ptr_array_new_with_free_func (g_free);
  int i;
  FlatpakRelated *rel;
  gboolean download;
  gboolean delete = autodelete;
  g_auto(GStrv) ref_parts = g_strsplit (extension_ref, "/", -1);

  deploy_data = flatpak_dir_get_deploy_data (self, extension_ref, NULL, NULL);

  if (deploy_data)
    old_subpaths = flatpak_deploy_data_get_subpaths (deploy_data);

  /* Only respect no-autodownload/download-if for uninstalled refs, we
     always want to update if you manually installed something */
  download =
    flatpak_extension_matches_reason (ref_parts[1], download_if, !no_autodownload) ||
    deploy_data != NULL;

  if (g_str_has_suffix (extension, ".Debug"))
    {
      /* debug files only updated if already installed */
      if (deploy_data == NULL)
        download = FALSE;

      /* Always remove debug */
      delete = TRUE;
    }

  if (old_subpaths)
    {
      for (i = 0; old_subpaths[i] != NULL; i++)
        g_ptr_array_add (subpaths, g_strdup (old_subpaths[i]));
    }

  if (g_str_has_suffix (extension, ".Locale"))
    {
      g_autofree char ** current_subpaths = flatpak_get_current_locale_subpaths ();
      for (i = 0; current_subpaths[i] != NULL; i++)
        {
          g_autofree char *subpath = current_subpaths[i];

          if (!string_in_array (subpaths, subpath))
            g_ptr_array_add (subpaths, g_steal_pointer (&subpath));
        }

      /* Always remove debug */
      delete = TRUE;
    }

  g_ptr_array_add (subpaths, NULL);

  rel = g_new0 (FlatpakRelated, 1);
  rel->ref = g_strdup (extension_ref);
  rel->commit = g_strdup (checksum);
  rel->subpaths = (char **)g_ptr_array_free (g_steal_pointer (&subpaths), FALSE);
  rel->download = download;
  rel->delete = delete;

  g_ptr_array_add (related, rel);
}

GPtrArray *
flatpak_dir_find_remote_related (FlatpakDir *self,
                                 const char *ref,
                                 const char *remote_name,
                                 GCancellable *cancellable,
                                 GError **error)
{
  g_autoptr(GBytes) summary_bytes = NULL;
  g_autoptr(GVariant) summary = NULL;
  g_autofree char *metadata = NULL;
  g_autoptr(GKeyFile) metakey = g_key_file_new ();
  int i;
  g_auto(GStrv) parts = NULL;
  g_autoptr(GPtrArray) related = g_ptr_array_new_with_free_func ((GDestroyNotify)flatpak_related_free);
  g_autofree char *url = NULL;

  parts = flatpak_decompose_ref (ref, error);
  if (parts == NULL)
    return NULL;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return NULL;

  if (!ostree_repo_remote_get_url (self->repo,
                                   remote_name,
                                   &url,
                                   error))
    return FALSE;

  if (*url == 0)
    return g_steal_pointer (&related);  /* Empty url, silently disables updates */

  if (!flatpak_dir_remote_fetch_summary (self, remote_name,
                                         &summary_bytes,
                                         cancellable, error))
    return NULL;

  summary = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                                          summary_bytes, FALSE));

  if (flatpak_dir_parse_summary_for_ref (self, summary, ref,
                                         NULL, NULL, &metadata,
                                         NULL, NULL) &&
      g_key_file_load_from_data (metakey, metadata, -1, 0, NULL))
    {
      g_auto(GStrv) groups = NULL;

      groups = g_key_file_get_groups (metakey, NULL);
      for (i = 0; groups[i] != NULL; i++)
        {
          char *extension;

          if (g_str_has_prefix (groups[i], "Extension ") &&
              *(extension = (groups[i] + strlen ("Extension "))) != 0)
            {
              g_autofree char *version = g_key_file_get_string (metakey, groups[i],
                                                                "version", NULL);
              gboolean subdirectories = g_key_file_get_boolean (metakey, groups[i],
                                                                "subdirectories", NULL);
              gboolean no_autodownload = g_key_file_get_boolean (metakey, groups[i],
                                                                 "no-autodownload", NULL);
              g_autofree char *download_if = g_key_file_get_string (metakey, groups[i],
                                                                    "download-if", NULL);
              gboolean autodelete = g_key_file_get_boolean (metakey, groups[i],
                                                            "autodelete", NULL);
              const char *branch;
              g_autofree char *extension_ref = NULL;
              g_autofree char *checksum = NULL;

              if (version)
                branch = version;
              else
                branch = parts[3];

              extension_ref = g_build_filename ("runtime", extension, parts[2], branch, NULL);

              if (flatpak_summary_lookup_ref (summary,
                                              extension_ref,
                                              &checksum))
                {
                  add_related (self, related, extension, extension_ref, checksum, no_autodownload, download_if, autodelete);
                }
              else if (subdirectories)
                {
                  g_auto(GStrv) refs = flatpak_summary_match_subrefs (summary, extension_ref);
                  int j;
                  for (j = 0; refs[j] != NULL; j++)
                    {
                      if (flatpak_summary_lookup_ref (summary,
                                                      refs[j],
                                                      &checksum))
                        add_related (self, related, extension, refs[j], checksum, no_autodownload, download_if, autodelete);
                    }
                }
            }
        }
    }

  return g_steal_pointer (&related);
}

static GPtrArray *
local_match_prefix (FlatpakDir *self,
                    const char *extension_ref,
                    const char *remote)
{
  GPtrArray *matches = g_ptr_array_new_with_free_func (g_free);
  g_auto(GStrv) parts = NULL;
  g_autofree char *parts_prefix = NULL;
  g_autoptr(GHashTable) refs = NULL;
  g_autofree char *list_prefix = NULL;

  parts = g_strsplit (extension_ref, "/", -1);
  parts_prefix = g_strconcat (parts[1], ".", NULL);

  list_prefix = g_strdup_printf ("%s:%s", remote, parts[0]);
  if (ostree_repo_list_refs (self->repo, list_prefix, &refs, NULL, NULL))
    {
      GHashTableIter hash_iter;
      gpointer key;

      g_hash_table_iter_init (&hash_iter, refs);
      while (g_hash_table_iter_next (&hash_iter, &key, NULL))
        {
          char *ref = key;
          g_auto(GStrv) cur_parts = g_strsplit (ref, "/", -1);

          /* Must match type, arch, branch */
          if (strcmp (parts[0], cur_parts[0]) != 0 ||
              strcmp (parts[2], cur_parts[2]) != 0 ||
              strcmp (parts[3], cur_parts[3]) != 0)
            continue;

          /* But only prefix of id */
          if (!g_str_has_prefix (cur_parts[1], parts_prefix))
            continue;

          g_ptr_array_add (matches, g_strdup (ref));
        }
    }

  return matches;
}

GPtrArray *
flatpak_dir_find_local_related (FlatpakDir *self,
                                const char *ref,
                                const char *remote_name,
                                GCancellable *cancellable,
                                GError **error)
{
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_autofree char *metadata_contents = NULL;
  gsize metadata_size;
  g_autoptr(GKeyFile) metakey = g_key_file_new ();
  int i;
  g_auto(GStrv) parts = NULL;
  g_autoptr(GPtrArray) related = g_ptr_array_new_with_free_func ((GDestroyNotify)flatpak_related_free);

  parts = flatpak_decompose_ref (ref, error);
  if (parts == NULL)
    return NULL;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return NULL;

  deploy_dir = flatpak_dir_get_if_deployed (self, ref, NULL, cancellable);
  if (deploy_dir == NULL)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   _("%s not installed"), ref);
      return NULL;
    }

  metadata = g_file_get_child (deploy_dir, "metadata");
  if (!g_file_load_contents (metadata, cancellable, &metadata_contents, &metadata_size, NULL, NULL))
    return g_steal_pointer (&related); /* No metadata => no related, but no error */

  if (g_key_file_load_from_data (metakey, metadata_contents, metadata_size, 0, NULL))
    {
      g_auto(GStrv) groups = NULL;

      groups = g_key_file_get_groups (metakey, NULL);
      for (i = 0; groups[i] != NULL; i++)
        {
          char *extension;

          if (g_str_has_prefix (groups[i], "Extension ") &&
              *(extension = (groups[i] + strlen ("Extension "))) != 0)
            {
              g_autofree char *version = g_key_file_get_string (metakey, groups[i],
                                                                "version", NULL);
              gboolean subdirectories = g_key_file_get_boolean (metakey, groups[i],
                                                                "subdirectories", NULL);
              gboolean no_autodownload = g_key_file_get_boolean (metakey, groups[i],
                                                                 "no-autodownload", NULL);
              g_autofree char *download_if = g_key_file_get_string (metakey, groups[i],
                                                                    "download-if", NULL);
              gboolean autodelete = g_key_file_get_boolean (metakey, groups[i],
                                                            "autodelete", NULL);
              const char *branch;
              g_autofree char *extension_ref = NULL;
              g_autofree char *prefixed_extension_ref = NULL;
              g_autofree char *checksum = NULL;

              if (version)
                branch = version;
              else
                branch = parts[3];

              extension_ref = g_build_filename ("runtime", extension, parts[2], branch, NULL);
              prefixed_extension_ref = g_strdup_printf ("%s:%s", remote_name, extension_ref);
              if (ostree_repo_resolve_rev (self->repo,
                                           prefixed_extension_ref,
                                           FALSE,
                                           &checksum,
                                           NULL))
                {
                  add_related (self, related, extension, extension_ref,
                               checksum, no_autodownload, download_if, autodelete);
                }
              else if (subdirectories)
                {
                  g_autoptr(GPtrArray) matches = local_match_prefix (self, extension_ref, remote_name);
                  int j;
                  for (j = 0; j < matches->len; j++)
                    {
                      const char *match = g_ptr_array_index (matches, j);
                      g_autofree char *prefixed_match = NULL;
                      g_autofree char *match_checksum = NULL;

                      prefixed_match = g_strdup_printf ("%s:%s", remote_name, match);

                      if (ostree_repo_resolve_rev (self->repo,
                                                   prefixed_match,
                                                   FALSE,
                                                   &match_checksum,
                                                   NULL))
                        {
                          add_related (self, related, extension,
                                       match, match_checksum,
                                       no_autodownload, download_if, autodelete);
                        }
                    }
                }
            }
        }
    }

  return g_steal_pointer (&related);
}
