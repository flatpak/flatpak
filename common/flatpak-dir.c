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

#include <glib/gi18n.h>

#include <gio/gio.h>
#include "libglnx/libglnx.h"
#include "lib/flatpak-error.h"

#include "flatpak-dir.h"
#include "flatpak-utils.h"
#include "flatpak-run.h"

#include "errno.h"

#define NO_SYSTEM_HELPER ((FlatpakSystemHelper *) (gpointer) 1)

#define SUMMARY_CACHE_TIMEOUT_SEC 5*60

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

struct FlatpakDir
{
  GObject              parent;

  gboolean             user;
  GFile               *basedir;
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
flatpak_get_system_base_dir_location (void)
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
      g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (system_helper), G_MAXINT);
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

  if (self->no_system_helper || self->user || getuid () == 0)
    return FALSE;

  system_helper = flatpak_dir_get_system_helper (self);

  return system_helper != NULL;
}

static OstreeRepo *
system_ostree_repo_new (GFile *repodir)
{
  return g_object_new (OSTREE_TYPE_REPO, "path", repodir,
                       "remotes-config-dir", FLATPAK_CONFIGDIR "/remotes.d",
                       NULL);
}

static void
flatpak_dir_finalize (GObject *object)
{
  FlatpakDir *self = FLATPAK_DIR (object);

  g_clear_object (&self->repo);
  g_clear_object (&self->basedir);

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

  dir = flatpak_dir_get (user);

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
    base_dir = flatpak_get_system_base_dir_location ();

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

/**
 * flatpak_deploy_data_get_subpaths:
 *
 * Returns: (array length=length zero-terminated=1) (transfer container): an array of constant strings
 **/
const char **
flatpak_deploy_data_get_subpaths (GVariant *deploy_data)
{
  const char **subpaths;

  g_variant_get_child (deploy_data, 2, "^as", &subpaths);
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
  OstreeRepoCheckoutOptions options = { 0, };
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
  options.disable_fsync = TRUE; /* We checkout to a temp dir and sync before moving it in place */

  if (!ostree_repo_checkout_tree_at (self->repo, &options,
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

      if (!flatpak_dir_pull (self, remote, branch, NULL,
                             child_repo, OSTREE_REPO_PULL_FLAGS_MIRROR,
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
          if (!flatpak_system_helper_call_deploy_appstream_sync (system_helper,
                                                                 flatpak_file_get_path_cached (ostree_repo_get_path (child_repo)),
                                                                 remote,
                                                                 arch,
                                                                 cancellable,
                                                                 error))
            return FALSE;
        }

      (void) flatpak_rm_rf (ostree_repo_get_path (child_repo),
                            NULL, NULL);

      return TRUE;
    }

  if (!flatpak_dir_pull (self, remote, branch, NULL, NULL, OSTREE_REPO_PULL_FLAGS_NONE, progress,
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

/* This is a copy of ostree_repo_pull_one_dir that always disables
   static deltas if subdir is used */
static gboolean
repo_pull_one_dir (OstreeRepo          *self,
                   const char          *remote_name,
                   const char          *dir_to_pull,
                   char               **refs_to_fetch,
                   OstreeRepoPullFlags  flags,
                   OstreeAsyncProgress *progress,
                   GCancellable        *cancellable,
                   GError             **error)
{
  GVariantBuilder builder;
  gboolean force_disable_deltas = FALSE;
  g_auto(GLnxConsoleRef) console = { 0, };
  g_autoptr(OstreeAsyncProgress) console_progress = NULL;
  gboolean res;

  if (progress == NULL)
    {
      glnx_console_lock (&console);
      if (console.is_tty)
        {
          console_progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, &console);
          progress = console_progress;
        }
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

  if (dir_to_pull)
    {
      g_variant_builder_add (&builder, "{s@v}", "subdir",
                             g_variant_new_variant (g_variant_new_string (dir_to_pull)));
      force_disable_deltas = TRUE;
    }

  if (force_disable_deltas)
    g_variant_builder_add (&builder, "{s@v}", "disable-static-deltas",
                           g_variant_new_variant (g_variant_new_boolean (TRUE)));

  g_variant_builder_add (&builder, "{s@v}", "flags",
                         g_variant_new_variant (g_variant_new_int32 (flags)));
  if (refs_to_fetch)
    g_variant_builder_add (&builder, "{s@v}", "refs",
                           g_variant_new_variant (g_variant_new_strv ((const char * const *) refs_to_fetch, -1)));

  res = ostree_repo_pull_with_options (self, remote_name, g_variant_builder_end (&builder),
                                       progress, cancellable, error);

  if (progress)
    ostree_async_progress_finish (progress);

  return res;
}

gboolean
flatpak_dir_pull (FlatpakDir          *self,
                  const char          *repository,
                  const char          *ref,
                  const char         **subpaths,
                  OstreeRepo          *repo,
                  OstreeRepoPullFlags  flags,
                  OstreeAsyncProgress *progress,
                  GCancellable        *cancellable,
                  GError             **error)
{
  gboolean ret = FALSE;
  const char *refs[2];
  g_autofree char *url = NULL;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    goto out;

  if (!ostree_repo_remote_get_url (self->repo,
                                   repository,
                                   &url,
                                   error))
    goto out;

  if (*url == 0)
    return TRUE; /* Empty url, silently disables updates */

  if (repo == NULL)
    repo = self->repo;

  refs[0] = ref;
  refs[1] = NULL;

  if (subpaths == NULL || subpaths[0] == NULL)
    {
      if (!repo_pull_one_dir (repo, repository, NULL,
                              (char **) refs, flags,
                              progress,
                              cancellable, error))
        {
          g_prefix_error (error, _("While pulling %s from remote %s: "), ref, repository);
          goto out;
        }
    }
  else
    {
      int i;

      if (!repo_pull_one_dir (repo, repository,
                              "/metadata",
                              (char **) refs, flags,
                              progress,
                              cancellable, error))
        {
          g_prefix_error (error, _("While pulling %s from remote %s, metadata: "),
                          ref, repository);
          goto out;
        }

      for (i = 0; subpaths[i] != NULL; i++)
        {
          g_autofree char *subpath = g_build_filename ("/files", subpaths[i], NULL);
          if (!repo_pull_one_dir (repo, repository,
                                  subpath,
                                  (char **) refs, flags,
                                  progress,
                                  cancellable, error))
            {
              g_prefix_error (error, _("While pulling %s from remote %s, subpath %s: "),
                              ref, repository, subpaths[i]);
              goto out;
            }
        }
    }

  ret = TRUE;

out:

  return ret;
}

static gboolean
repo_pull_one_untrusted (OstreeRepo          *self,
                         const char          *remote_name,
                         const char          *url,
                         const char          *dir_to_pull,
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
          console_progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed, &console);
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

  if (dir_to_pull)
    {
      g_variant_builder_add (&builder, "{s@v}", "subdir",
                             g_variant_new_variant (g_variant_new_string (dir_to_pull)));
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

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  if (!ostree_repo_remote_get_gpg_verify_summary (self->repo, remote_name,
                                                  &gpg_verify_summary, error))
    return FALSE;

  if (!ostree_repo_remote_get_gpg_verify (self->repo, remote_name,
                                          &gpg_verify, error))
    return FALSE;

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

  if (old_commit)
    {
      g_autoptr(OstreeRepo) src_repo = ostree_repo_new (path_file);
      g_autoptr(GVariant) new_commit = NULL;
      guint64 old_timestamp;
      guint64 new_timestamp;

      if (!ostree_repo_open (src_repo, cancellable, error))
        return FALSE;

      if (!ostree_repo_load_commit (src_repo, checksum, &new_commit, NULL, error))
        return FALSE;

      old_timestamp = ostree_commit_get_timestamp (old_commit);
      new_timestamp = ostree_commit_get_timestamp (new_commit);

      if (new_timestamp < old_timestamp)
        return flatpak_fail (error, "Not allowed to downgrade %s", ref);
    }

  if (subpaths == NULL || subpaths[0] == NULL)
    {
      if (!repo_pull_one_untrusted (self->repo, remote_name, url,
                                    NULL, ref, checksum, progress,
                                    cancellable, error))
        {
          g_prefix_error (error, _("While pulling %s from remote %s: "), ref, remote_name);
          return FALSE;
        }
    }
  else
    {
      int i;

      if (!repo_pull_one_untrusted (self->repo, remote_name, url,
                                    "/metadata", ref, checksum, progress,
                                    cancellable, error))
        {
          g_prefix_error (error, _("While pulling %s from remote %s, metadata: "),
                          ref, remote_name);
          return FALSE;
        }

      for (i = 0; subpaths[i] != NULL; i++)
        {
          g_autofree char *subpath = g_build_filename ("/files", subpaths[i], NULL);
          if (!repo_pull_one_untrusted (self->repo, remote_name, url,
                                        subpath, ref, checksum, progress,
                                        cancellable, error))
            {
              g_prefix_error (error, _("While pulling %s from remote %s, subpath %s: "),
                              ref, remote_name, subpaths[i]);
              return FALSE;
            }
        }
    }

  return TRUE;
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

  g_autoptr(GFile) base;
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
                         GCancellable *cancellable,
                         GError      **error)
{
  g_autofree char *remote_and_ref = NULL;
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
  g_autoptr(GKeyFile) keyfile = NULL;
  guint64 installed_size = 0;
  OstreeRepoCheckoutOptions options = { 0, };
  const char *checksum;
  glnx_fd_close int checkoutdir_dfd = -1;
  g_autoptr(GFile) tmp_dir_template = NULL;
  g_autofree char *tmp_dir_path = NULL;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  deploy_base = flatpak_dir_get_deploy_dir (self, ref);

  if (checksum_or_latest == NULL)
    {
      g_debug ("No checksum specified, getting tip of %s", ref);

      resolved_ref = flatpak_dir_read_latest (self, origin, ref, cancellable, error);
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
  options.disable_fsync = TRUE; /* We checkout to a temp dir and sync before moving it in place */
  checkoutdirpath = g_file_get_path (checkoutdir);

  if (subpaths == NULL || *subpaths == NULL)
    {
      if (!ostree_repo_checkout_tree_at (self->repo, &options,
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

      if (!ostree_repo_checkout_tree_at (self->repo, &options,
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
          if (!ostree_repo_checkout_tree_at (self->repo, &options,
                                             AT_FDCWD, dstpath,
                                             checksum,
                                             cancellable, error))
            {
              g_prefix_error (error, _("While trying to checkout metadata subpath: "));
              return FALSE;
            }
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

  deploy_data = flatpak_dir_new_deploy_data (origin,
                                             checksum,
                                             (char **) subpaths,
                                             installed_size,
                                             NULL);

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
  g_autoptr(GError) my_error = NULL;
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
      const char *empty_subpaths[] = {NULL};
      const char **subpaths;
      g_autofree char *child_repo_path = NULL;
      FlatpakSystemHelper *system_helper;
      FlatpakHelperDeployFlags helper_flags = 0;
      g_autofree char *url = NULL;

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

      if (no_pull)
        {
          /* Do nothing */
        }
      else if (g_str_has_prefix (url, "file:"))
        {
          /* In the local case we let the system-helper do all the work. That way we can trust its
             reading from the right source, and its not doing any network i/o. */

          helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL;
        }
      else
        {
          /* We're pulling from a remote source, we do the network mirroring pull as a
             user and hand back the resulting data to the system-helper, that trusts us
             due to the GPG signatures in the repo */

          child_repo = flatpak_dir_create_system_child_repo (self, &child_repo_lock, error);
          if (child_repo == NULL)
            return FALSE;

          if (!flatpak_dir_pull (self, remote_name, ref, subpaths,
                                 child_repo, OSTREE_REPO_PULL_FLAGS_MIRROR,
                                 progress, cancellable, error))
            return FALSE;

          child_repo_path = g_file_get_path (ostree_repo_get_path (child_repo));
        }

      if (no_deploy)
        helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY;

      if (!flatpak_system_helper_call_deploy_sync (system_helper,
                                                   child_repo_path ? child_repo_path : "",
                                                   helper_flags, ref, remote_name,
                                                   (const char * const *) subpaths,
                                                   cancellable,
                                                   error))
        return FALSE;

      if (child_repo_path)
        (void) glnx_shutil_rm_rf_at (AT_FDCWD, child_repo_path, NULL, NULL);

      return TRUE;
    }

  if (!no_pull)
    {
      if (!flatpak_dir_pull (self, remote_name, ref, opt_subpaths, NULL, OSTREE_REPO_PULL_FLAGS_NONE, progress,
                             cancellable, error))
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

gboolean
flatpak_dir_install_bundle (FlatpakDir          *self,
                            GFile               *file,
                            GBytes              *extra_gpg_data,
                            char               **out_ref,
                            GCancellable        *cancellable,
                            GError             **error)
{
  g_autofree char *ref = NULL;
  gboolean added_remote = FALSE;
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *origin = NULL;
  g_auto(GStrv) parts = NULL;
  g_autofree char *basename = NULL;
  g_autoptr(GBytes) included_gpg_data = NULL;
  GBytes *gpg_data = NULL;
  g_autofree char *to_checksum = NULL;
  g_autofree char *remote = NULL;
  gboolean ret = FALSE;

  if (flatpak_dir_use_system_helper (self))
    {
      FlatpakSystemHelper *system_helper;
      g_autoptr(GVariant) gpg_data_v = NULL;

      system_helper = flatpak_dir_get_system_helper (self);
      g_assert (system_helper != NULL);

      if (gpg_data != NULL)
        gpg_data_v = variant_new_ay_bytes (gpg_data);
      else
        gpg_data_v = g_variant_ref_sink (g_variant_new_from_data (G_VARIANT_TYPE ("ay"), "", 0, TRUE, NULL, NULL));

      if (!flatpak_system_helper_call_install_bundle_sync (system_helper,
                                                           flatpak_file_get_path_cached (file),
                                                           0, gpg_data_v,
                                                           &ref,
                                                           cancellable,
                                                           error))
        return FALSE;

      if (out_ref)
        *out_ref = g_steal_pointer (&ref);

      return TRUE;
    }

  metadata = flatpak_bundle_load (file, &to_checksum,
                                  &ref,
                                  &origin,
                                  NULL,
                                  &included_gpg_data,
                                  error);
  if (metadata == NULL)
    return FALSE;

  gpg_data = extra_gpg_data ? extra_gpg_data : included_gpg_data;

  parts = flatpak_decompose_ref (ref, error);
  if (parts == NULL)
    return FALSE;

  deploy_dir = flatpak_dir_get_if_deployed (self, ref, NULL, cancellable);
  if (deploy_dir != NULL)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                   _("%s branch %s already installed"), parts[1], parts[3]);
      return FALSE;
    }

  /* Add a remote for later updates */
  basename = g_file_get_basename (file);
  remote = flatpak_dir_create_origin_remote (self,
                                             origin,
                                             parts[1],
                                             basename,
                                             gpg_data,
                                             cancellable,
                                             error);
  if (remote == NULL)
    return FALSE;

  /* From here we need to goto out on error, to clean up */
  added_remote = TRUE;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    goto out;

  if (!flatpak_pull_from_bundle (self->repo,
                                 file,
                                 remote,
                                 ref,
                                 gpg_data != NULL,
                                 cancellable,
                                 error))
    goto out;

  if (!flatpak_dir_deploy_install (self, ref, remote, NULL, cancellable, error))
    goto out;

  if (out_ref)
    *out_ref = g_steal_pointer (&ref);

  ret = TRUE;

out:
  if (added_remote && !ret)
    ostree_repo_remote_delete (self->repo, remote, NULL, NULL);

  return ret;
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

  if (*url == 0)
    return TRUE; /* Empty URL => disabled */

  is_local = g_str_has_prefix (url, "file:");

  /* Quick check to terminate early if nothing changed in cached summary
     (and subpaths didn't change) */
  if (!is_local && deploy_data != NULL &&
      _g_strv_equal0 ((char **)subpaths, (char **)old_subpaths))
    {
      const char *installed_commit = flatpak_deploy_data_get_commit (deploy_data);

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
                                                 cancellable, error))
        {
          g_autoptr(GVariant) summary =
            g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                                          summary_bytes, FALSE));
          g_autofree char *latest = NULL;

          if (flatpak_summary_lookup_ref (summary,
                                          ref,
                                          &latest))
            {
              if (strcmp (latest, installed_commit) == 0)
                {
                  g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                               _("%s branch %s already installed"), ref, installed_commit);
                  return FALSE;
                }
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

      if (no_pull)
        {
          if (!ostree_repo_resolve_rev (self->repo, ref, FALSE, &latest_checksum, error))
            return FALSE;
        }
      else if (g_str_has_prefix (url, "file:"))
        {
          /* In the local case we let the system-helper do all the work. That way we can trust its
             reading from the right source, and its not doing any network i/o. */

          helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL;
        }
      else
        {
          /* We're pulling from a remote source, we do the network mirroring pull as a
             user and hand back the resulting data to the system-helper, that trusts us
             due to the GPG signatures in the repo */

          child_repo = flatpak_dir_create_system_child_repo (self, &child_repo_lock, error);
          if (child_repo == NULL)
            return FALSE;

          if (!flatpak_dir_pull (self, remote_name, ref, subpaths,
                                 child_repo, OSTREE_REPO_PULL_FLAGS_MIRROR,
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
          if (!flatpak_system_helper_call_deploy_sync (system_helper,
                                                       child_repo_path ? child_repo_path : "",
                                                       helper_flags, ref, remote_name,
                                                       subpaths,
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
      if (!flatpak_dir_pull (self, remote_name, ref, subpaths,
                             NULL, OSTREE_REPO_PULL_FLAGS_NONE, progress,
                             cancellable, error))
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

      system_helper = flatpak_dir_get_system_helper (self);
      g_assert (system_helper != NULL);

      if (!flatpak_system_helper_call_uninstall_sync (system_helper,
                                                      flags, ref,
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
      if ((now - summary->time) < (1000 * (SUMMARY_CACHE_TIMEOUT_SEC)) &&
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
  g_hash_table_insert (self->summary_cache, summary->remote, summary);

  G_UNLOCK (cache);
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

  if (!ostree_repo_remote_fetch_summary (self->repo, name,
                                         out_summary, NULL,
                                         cancellable,
                                         error))
    return FALSE;

  if (!is_local)
    flatpak_dir_cache_summary (self, *out_summary, name, url);

  return TRUE;
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

  if (summary_bytes == NULL)
    return flatpak_fail (error, "Remote listing not available; server has no summary file\n" \
                                "Check the URL passed to remote-add was valid\n");

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
                    gboolean      app,
                    gboolean      runtime,
                    GError      **error)
{
  g_autoptr(GPtrArray) matched_refs = NULL;
  const char **arches = flatpak_get_arches ();
  const char *opt_arches[] = {opt_arch, NULL};
  GHashTableIter hash_iter;
  gpointer key;

  if (opt_arch != NULL)
    arches = opt_arches;

  if (opt_name && !flatpak_is_valid_name (opt_name))
    {
      flatpak_fail (error, "'%s' is not a valid name", opt_name);
      return NULL;
    }

  if (opt_branch && !flatpak_is_valid_branch (opt_branch))
    {
      flatpak_fail (error, "'%s' is not a valid branch name", opt_branch);
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

      if ((!app && is_app) ||
          (!runtime && is_runtime) ||
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
                   const char   *opt_arch,
                   gboolean      app,
                   gboolean      runtime,
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

      matched_refs = find_matching_refs (refs, name, opt_branch, arches[i],
                                         app, runtime, error);
      if (matched_refs == NULL)
        return NULL;

      if (matched_refs->len == 0)
        continue;

      if (matched_refs->len > 1)
        {
          int i;
          g_autoptr(GString) err = g_string_new ("");
          g_string_printf (err, "Multiple branches available for %s, you must specify one of: ", name);
          for (i = 0; i < matched_refs->len; i++)
            {
              g_auto(GStrv) parts = flatpak_decompose_ref (g_ptr_array_index (matched_refs, i), NULL);
              if (i != 0)
                g_string_append (err, ", ");

              g_string_append (err, parts[3]);
            }

          flatpak_fail (error, err->str);
          return NULL;
        }

      return g_strdup (g_ptr_array_index (matched_refs, 0));
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
                             gboolean      app,
                             gboolean      runtime,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autofree char *refspec_prefix = NULL;
  g_autoptr(GHashTable) remote_refs = NULL;
  GPtrArray *matched_refs;

  if (!flatpak_dir_ensure_repo (self, NULL, error))
    return NULL;

  refspec_prefix = g_strconcat (remote, ":.", NULL);

  if (!flatpak_dir_remote_list_refs (self, remote,
                                     &remote_refs, cancellable, error))
    return NULL;

  matched_refs = find_matching_refs (remote_refs, name, opt_branch,
                                      opt_arch, app, runtime, error);
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
                             const char   *opt_arch,
                             gboolean      app,
                             gboolean      runtime,
                             gboolean     *is_app,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autofree char *refspec_prefix = NULL;
  g_autofree char *remote_ref = NULL;
  g_autoptr(GHashTable) remote_refs = NULL;
  g_autoptr(GError) my_error = NULL;

  if (!flatpak_dir_ensure_repo (self, NULL, error))
    return NULL;

  refspec_prefix = g_strconcat (remote, ":.", NULL);

  if (!flatpak_dir_remote_list_refs (self, remote,
                                     &remote_refs, cancellable, error))
    return NULL;

  remote_ref = find_matching_ref (remote_refs, name, opt_branch,
                                  opt_arch, app, runtime, &my_error);
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
      if (is_app)
        *is_app = g_str_has_prefix (remote_ref, "app/");

      return g_steal_pointer (&remote_ref);
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
               _("Can't find %s in remote %s"), name, remote);

  return NULL;
}


static GHashTable *
flatpak_dir_get_all_installed_refs (FlatpakDir *self,
                                    gboolean    app,
                                    gboolean    runtime,
                                    GError    **error)
{
  g_autoptr(GHashTable) local_refs = NULL;
  int i;

  if (!flatpak_dir_ensure_repo (self, NULL, error))
    return NULL;

  local_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (app)
    {
      g_auto(GStrv) app_refs = NULL;

      if (!flatpak_dir_list_refs (self, "app", &app_refs, NULL, error))
        return NULL;

      for (i = 0; app_refs[i] != NULL; i++)
        g_hash_table_insert (local_refs, g_strdup (app_refs[i]),
                             GINT_TO_POINTER (1));
    }
  if (runtime)
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
                                 gboolean    app,
                                 gboolean    runtime,
                                 GError    **error)
{
  g_autoptr(GHashTable) local_refs = NULL;
  GPtrArray *matched_refs;

  local_refs = flatpak_dir_get_all_installed_refs (self, app, runtime, error);
  if (local_refs == NULL)
    return NULL;

  matched_refs = find_matching_refs (local_refs, opt_name, opt_branch,
                                      opt_arch, app, runtime, error);
  if (matched_refs == NULL)
    return NULL;

  g_ptr_array_add (matched_refs, NULL);
  return (char **)g_ptr_array_free (matched_refs, FALSE);

}

char *
flatpak_dir_find_installed_ref (FlatpakDir *self,
                                const char *opt_name,
                                const char *opt_branch,
                                const char *opt_arch,
                                gboolean    app,
                                gboolean    runtime,
                                gboolean   *is_app,
                                GError    **error)
{
  g_autofree char *local_ref = NULL;
  g_autoptr(GHashTable) local_refs = NULL;
  g_autoptr(GError) my_error = NULL;

  local_refs = flatpak_dir_get_all_installed_refs (self, app, runtime, error);
  if (local_refs == NULL)
    return NULL;

  local_ref = find_matching_ref (local_refs, opt_name, opt_branch,
                                  opt_arch, app, runtime, &my_error);
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
      if (is_app)
        *is_app = g_str_has_prefix (local_ref, "app/");

      return g_steal_pointer (&local_ref);
    }

  g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
               _("%s %s not installed"), opt_name ? opt_name : "*unspecified*", opt_branch ? opt_branch : "master");
  return NULL;
}

FlatpakDir *
flatpak_dir_new (GFile *path, gboolean user)
{
  return g_object_new (FLATPAK_TYPE_DIR, "path", path, "user", user, NULL);
}

FlatpakDir *
flatpak_dir_clone (FlatpakDir *self)
{
  return flatpak_dir_new (self->basedir, self->user);
}

FlatpakDir *
flatpak_dir_get_system (void)
{
  g_autoptr(GFile) path = flatpak_get_system_base_dir_location ();
  return flatpak_dir_new (path, FALSE);
}

FlatpakDir *
flatpak_dir_get_user (void)
{
  g_autoptr(GFile) path = flatpak_get_user_base_dir_location ();
  return flatpak_dir_new (path, TRUE);
}

FlatpakDir *
flatpak_dir_get (gboolean user)
{
  if (user)
    return flatpak_dir_get_user ();
  else
    return flatpak_dir_get_system ();
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
flatpak_dir_get_remote_disabled (FlatpakDir *self,
                                 const char *remote_name)
{
  GKeyFile *config = ostree_repo_get_config (self->repo);
  g_autofree char *group = get_group (remote_name);
  g_autofree char *url = NULL;

  if (config &&
      g_key_file_get_boolean (config, group, "xa.disable", NULL))
    return TRUE;

  if (ostree_repo_remote_get_url (self->repo, remote_name, &url, NULL) && *url == 0)
    return TRUE; /* Empty URL => disabled */

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

char *
flatpak_dir_create_origin_remote (FlatpakDir   *self,
                                  const char   *url,
                                  const char   *id,
                                  const char   *title,
                                  GBytes       *gpg_data,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  g_autofree char *remote = NULL;

  g_auto(GStrv) remotes = NULL;
  int version = 0;
  g_autoptr(GVariantBuilder) optbuilder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  remotes = ostree_repo_remote_list (self->repo, NULL);

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

  g_variant_builder_add (optbuilder, "{s@v}",
                         "xa.title",
                         g_variant_new_variant (g_variant_new_string (title)));

  g_variant_builder_add (optbuilder, "{s@v}",
                         "xa.noenumerate",
                         g_variant_new_variant (g_variant_new_boolean (TRUE)));

  g_variant_builder_add (optbuilder, "{s@v}",
                         "xa.prio",
                         g_variant_new_variant (g_variant_new_string ("0")));

  if (!ostree_repo_remote_add (self->repo,
                               remote, url ? url : "", g_variant_builder_end (optbuilder), cancellable, error))
    return NULL;

  if (gpg_data)
    {
      g_autoptr(GInputStream) gpg_data_as_stream = g_memory_input_stream_new_from_bytes (gpg_data);

      if (!ostree_repo_remote_gpg_import (self->repo, remote, gpg_data_as_stream,
                                          NULL, NULL, cancellable, error))
        {
          ostree_repo_remote_delete (self->repo, remote,
                                     NULL, NULL);
          return NULL;
        }
    }

  return g_steal_pointer (&remote);
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

      gpg_data_v = g_variant_ref_sink (g_variant_new_from_data (G_VARIANT_TYPE ("ay"), "", 0, TRUE, NULL, NULL));

      system_helper = flatpak_dir_get_system_helper (self);
      g_assert (system_helper != NULL);

      if (force_remove)
        flags |= FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_FORCE_REMOVE;

      if (!flatpak_system_helper_call_configure_remote_sync (system_helper,
                                                             flags, remote_name,
                                                             "",
                                                             gpg_data_v,
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

      if (gpg_data != NULL)
        gpg_data_v = variant_new_ay_bytes (gpg_data);
      else
        gpg_data_v = g_variant_ref_sink (g_variant_new_from_data (G_VARIANT_TYPE ("ay"), "", 0, TRUE, NULL, NULL));

      system_helper = flatpak_dir_get_system_helper (self);
      g_assert (system_helper != NULL);

      if (!flatpak_system_helper_call_configure_remote_sync (system_helper,
                                                             0, remote_name,
                                                             config_data,
                                                             gpg_data_v,
                                                             cancellable, error))
        return FALSE;

      return TRUE;
    }

  metalink = g_key_file_get_string (config, group, "metalink", NULL);
  if (metalink != NULL && *metalink != 0)
    url = g_strconcat ("metalink=", metalink, NULL);
  else
    url = g_key_file_get_string (config, group, "url", NULL);

  if (url == NULL || *url == 0)
    return flatpak_fail (error, "No url for remote %s specified",
                         remote_name);

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

char *
flatpak_dir_fetch_remote_title (FlatpakDir   *self,
                                const char   *remote,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_autoptr(GError) my_error = NULL;
  g_autoptr(GBytes) summary_bytes = NULL;
  g_autoptr(GVariant) summary = NULL;
  g_autoptr(GVariant) extensions = NULL;
  GVariantDict dict;
  g_autofree char *title = NULL;

  if (error == NULL)
    error = &my_error;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return NULL;

  if (!flatpak_dir_remote_fetch_summary (self, remote,
                                         &summary_bytes,
                                         cancellable, error))
    return FALSE;

  if (summary_bytes == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Remote title not available; server has no summary file"));
      return FALSE;
    }

  summary = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                                          summary_bytes, FALSE));
  extensions = g_variant_get_child_value (summary, 1);

  g_variant_dict_init (&dict, extensions);
  g_variant_dict_lookup (&dict, "xa.title", "s", &title);
  g_variant_dict_end (&dict);

  if (title == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           _("Remote title not set"));
      return FALSE;
    }

  return g_steal_pointer (&title);
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
                           _("Data not found"));
      return FALSE;
    }

  cache = g_variant_get_child_value (cache_v, 0);
  res = g_variant_lookup_value (cache, ref, NULL);
  if (res == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   _("Data not found for ref %s"), ref);
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

  if (summary_bytes == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Data not available; server has no summary file"));
      return FALSE;
    }

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
             gboolean autodelete)
{
  g_autoptr(GVariant) deploy_data = NULL;
  const char **old_subpaths = NULL;
  g_autoptr(GPtrArray) subpaths = g_ptr_array_new_with_free_func (g_free);
  int i;
  FlatpakRelated *rel;
  gboolean download = TRUE;
  gboolean delete = autodelete;

  deploy_data = flatpak_dir_get_deploy_data (self, extension_ref, NULL, NULL);

  if (deploy_data)
    old_subpaths = flatpak_deploy_data_get_subpaths (deploy_data);

  /* Only apply no-autodownload for uninstalled refs, we want to update
     if you manually installed them */

  if (no_autodownload && deploy_data == NULL)
    download = FALSE;

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

  parts = flatpak_decompose_ref (ref, error);
  if (parts == NULL)
    return NULL;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return NULL;

  if (!flatpak_dir_remote_fetch_summary (self, remote_name,
                                         &summary_bytes,
                                         cancellable, error))
    return NULL;

  if (summary_bytes == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Data not available; server has no summary file"));
      return NULL;
    }

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
                  add_related (self, related, extension, extension_ref, checksum, no_autodownload, autodelete);
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
                        add_related (self, related, extension, refs[j], checksum, no_autodownload, autodelete);
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
                               checksum, no_autodownload, autodelete);
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
                                       no_autodownload, autodelete);
                        }
                    }
                }
            }
        }
    }

  return g_steal_pointer (&related);
}
