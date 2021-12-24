/*
 * Copyright © 2014 Red Hat, Inc
 * Copyright © 2017 Endless Mobile, Inc.
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
 *       Philip Withnall <withnall@endlessm.com>
 *       Matthew Leeds <matthew.leeds@endlessm.com>
 */

#include "config.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/types.h>
#include <utime.h>

#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include "libglnx/libglnx.h"
#include "flatpak-error.h"
#include <ostree.h>

#include "flatpak-dir-private.h"
#include "flatpak-utils-private.h"
#include "flatpak-oci-registry-private.h"
#include "flatpak-run-private.h"
#include "flatpak-appdata-private.h"

#include "errno.h"

#ifdef HAVE_LIBSYSTEMD
#define SD_JOURNAL_SUPPRESS_LOCATION
#include <systemd/sd-journal.h>
#endif


#define NO_SYSTEM_HELPER ((FlatpakSystemHelper *) (gpointer) 1)

#define SUMMARY_CACHE_TIMEOUT_SEC 5 *60

#define SYSCONF_INSTALLATIONS_DIR "installations.d"
#define SYSCONF_INSTALLATIONS_FILE_EXT ".conf"
#define SYSCONF_REMOTES_DIR "remotes.d"

#define SYSTEM_DIR_DEFAULT_ID "default"
#define SYSTEM_DIR_DEFAULT_DISPLAY_NAME _("Default system installation")
#define SYSTEM_DIR_DEFAULT_STORAGE_TYPE FLATPAK_DIR_STORAGE_TYPE_DEFAULT
#define SYSTEM_DIR_DEFAULT_PRIORITY 0

static FlatpakOciRegistry *flatpak_dir_create_system_child_oci_registry (FlatpakDir   *self,
                                                                         GLnxLockFile *file_lock,
                                                                         GError      **error);

static OstreeRepo * flatpak_dir_create_child_repo (FlatpakDir   *self,
                                                   GFile        *cache_dir,
                                                   GLnxLockFile *file_lock,
                                                   const char   *optional_commit,
                                                   GError      **error);
static OstreeRepo * flatpak_dir_create_system_child_repo (FlatpakDir   *self,
                                                          GLnxLockFile *file_lock,
                                                          const char   *optional_commit,
                                                          GError      **error);

static gboolean flatpak_dir_mirror_oci (FlatpakDir          *self,
                                        FlatpakOciRegistry  *dst_registry,
                                        FlatpakRemoteState  *state,
                                        const char          *ref,
                                        const char          *skip_if_current_is,
                                        OstreeAsyncProgress *progress,
                                        GCancellable        *cancellable,
                                        GError             **error);

static gboolean flatpak_dir_remote_fetch_summary (FlatpakDir   *self,
                                                  const char   *name,
                                                  GBytes      **out_summary,
                                                  GBytes      **out_summary_sig,
                                                  GCancellable *cancellable,
                                                  GError      **error);

static gboolean flatpak_dir_cleanup_remote_for_url_change (FlatpakDir   *self,
                                                           const char   *remote_name,
                                                           const char   *url,
                                                           GCancellable *cancellable,
                                                           GError      **error);

static gboolean _flatpak_dir_fetch_remote_state_metadata_branch (FlatpakDir         *self,
                                                                 FlatpakRemoteState *state,
                                                                 GCancellable       *cancellable,
                                                                 GError            **error);

static void ensure_soup_session (FlatpakDir *self);

static void flatpak_dir_log (FlatpakDir *self,
                             const char *file,
                             int line,
                             const char *func,
                             const char *source,
                             const char *change,
                             const char *remote,
                             const char *ref,
                             const char *commit,
                             const char *old_commit,
                             const char *url,
                             const char *format,
                             ...);

#define flatpak_dir_log(self,change,remote,ref,commit,old_commit,url,format,...) \
   (flatpak_dir_log) (self,__FILE__, __LINE__, __FUNCTION__, \
                      NULL, change, remote, ref, commit, old_commit, url, format, __VA_ARGS__)

static GVariant *upgrade_deploy_data (GVariant *deploy_data, GFile *deploy_dir, const char *ref);

typedef struct
{
  GBytes *bytes;
  GBytes *bytes_sig;
  char   *remote;
  char   *url;
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
  GObject          parent;

  gboolean         user;
  GFile           *basedir;
  DirExtraData    *extra_data;
  OstreeRepo      *repo;
  gboolean         no_system_helper;
  gboolean         no_interaction;
  pid_t            source_pid;

  GDBusConnection *system_helper_bus;

  GHashTable      *summary_cache;

  SoupSession     *soup_session;
};

typedef struct
{
  GObjectClass parent_class;
} FlatpakDirClass;

struct FlatpakDeploy
{
  GObject         parent;

  char           *ref;
  GFile          *dir;
  GKeyFile       *metadata;
  FlatpakContext *system_overrides;
  FlatpakContext *user_overrides;
  FlatpakContext *system_app_overrides;
  FlatpakContext *user_app_overrides;
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
        setup_value = (gsize) config_dir;
      else
        setup_value = (gsize) FLATPAK_CONFIGDIR;
      g_once_init_leave (&path, setup_value);
    }

  return (const char *) path;
}


static FlatpakRemoteState *
flatpak_remote_state_new (void)
{
  FlatpakRemoteState *state = g_new0 (FlatpakRemoteState, 1);
  state->refcount = 1;
  return state;
}

FlatpakRemoteState *
flatpak_remote_state_ref (FlatpakRemoteState *remote_state)
{
  g_assert (remote_state->refcount > 0);
  remote_state->refcount++;
  return remote_state;
}

void
flatpak_remote_state_unref (FlatpakRemoteState *remote_state)
{
  g_assert (remote_state->refcount > 0);
  remote_state->refcount--;

   if (remote_state->refcount == 0)
     {
       g_free (remote_state->remote_name);
       g_free (remote_state->collection_id);
       g_clear_pointer (&remote_state->summary, g_variant_unref);
       g_clear_pointer (&remote_state->summary_sig_bytes, g_bytes_unref);
       g_clear_error (&remote_state->summary_fetch_error);
       g_clear_pointer (&remote_state->metadata, g_variant_unref);
       g_clear_error (&remote_state->metadata_fetch_error);

       g_free (remote_state);
     }
}

gboolean
flatpak_remote_state_ensure_summary (FlatpakRemoteState *self,
                                     GError            **error)
{
  if (self->summary == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Unable to load summary from remote %s: %s"), self->remote_name,
                         self->summary_fetch_error != NULL ? self->summary_fetch_error->message : "unknown error");

  return TRUE;
}

gboolean
flatpak_remote_state_ensure_metadata (FlatpakRemoteState *self,
                                      GError            **error)
{
  if (self->metadata == NULL)
    {
      g_autofree char *error_msg = NULL;

      /* If the collection ID is NULL the metadata comes from the summary */
      if (self->metadata_fetch_error != NULL)
        error_msg = g_strdup (self->metadata_fetch_error->message);
      else if (self->collection_id == NULL && self->summary_fetch_error != NULL)
        error_msg = g_strdup_printf ("summary fetch error: %s", self->summary_fetch_error->message);

      return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Unable to load metadata from remote %s: %s"), self->remote_name,
                           error_msg != NULL ? error_msg : "unknown error");
    }

  return TRUE;
}

/* Returns TRUE if the ref is found in the summary or cache. out_checksum and
 * out_variant are not guaranteed to be set even when the ref is found. */
gboolean
flatpak_remote_state_lookup_ref (FlatpakRemoteState *self,
                                 const char         *ref,
                                 char              **out_checksum,
                                 GVariant          **out_variant,
                                 GError            **error)
{
  if (self->collection_id == NULL || self->summary != NULL)
    {
      if (!flatpak_remote_state_ensure_summary (self, error))
        return FALSE;

      if (!flatpak_summary_lookup_ref (self->summary, self->collection_id, ref, out_checksum, out_variant))
        {
          if (self->collection_id != NULL)
            flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("No such ref (%s, %s) in remote %s"), self->collection_id, ref, self->remote_name);
          else
            flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("No such ref '%s' in remote %s"), ref, self->remote_name);
          return FALSE;
        }
    }
  else
    {
      if (!flatpak_remote_state_ensure_metadata (self, error))
        return FALSE;

      if (!flatpak_remote_state_lookup_cache (self, ref, NULL, NULL, NULL, error))
        return FALSE;
    }

  return TRUE;
}

char **
flatpak_remote_state_match_subrefs (FlatpakRemoteState *self,
                                    const char         *ref)
{
  if (self->summary == NULL)
    {
      const char *empty[] = { NULL };
      g_debug ("flatpak_remote_state_match_subrefs with no summary");
      return g_strdupv ((char **) empty);
    }

  return flatpak_summary_match_subrefs (self->summary, self->collection_id, ref);
}


gboolean
flatpak_remote_state_lookup_repo_metadata (FlatpakRemoteState *self,
                                           const char         *key,
                                           const char         *format_string,
                                           ...)
{
  g_autoptr(GVariant) value = NULL;
  va_list args;

  if (self->metadata == NULL)
    return FALSE;

  /* Extract the metadata from it, if set. */
  value = g_variant_lookup_value (self->metadata, key, NULL);
  if (value == NULL)
    return FALSE;

  if (!g_variant_check_format_string (value, format_string, FALSE))
    return FALSE;

  va_start (args, format_string);
  g_variant_get_va (value, format_string, NULL, &args);
  va_end (args);

  return TRUE;
}

gboolean
flatpak_remote_state_lookup_cache (FlatpakRemoteState *self,
                                   const char         *ref,
                                   guint64            *download_size,
                                   guint64            *installed_size,
                                   const char        **metadata,
                                   GError            **error)
{
  g_autoptr(GVariant) cache_v = NULL;
  g_autoptr(GVariant) cache = NULL;
  g_autoptr(GVariant) res = NULL;
  g_autoptr(GVariant) refdata = NULL;
  int pos;

  if (!flatpak_remote_state_ensure_metadata (self, error))
    return FALSE;

  cache_v = g_variant_lookup_value (self->metadata, "xa.cache", NULL);
  if (cache_v == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   _("No flatpak cache in remote '%s' summary"), self->remote_name);
      return FALSE;
    }

  cache = g_variant_get_child_value (cache_v, 0);

  if (!flatpak_variant_bsearch_str (cache, ref, &pos))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   _("No entry for %s in remote '%s' summary flatpak cache "), ref, self->remote_name);
      return FALSE;
    }

  refdata = g_variant_get_child_value (cache, pos);
  res = g_variant_get_child_value (refdata, 1);

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
    g_variant_get_child (res, 2, "&s", metadata);

  return TRUE;
}

GVariant *
flatpak_remote_state_lookup_sparse_cache (FlatpakRemoteState *self,
                                          const char         *ref,
                                          GError            **error)
{
  g_autoptr(GVariant) cache = NULL;
  int pos;

  if (!flatpak_remote_state_ensure_metadata (self, error))
    return FALSE;

  cache = g_variant_lookup_value (self->metadata, "xa.sparse-cache", NULL);
  if (cache != NULL && flatpak_variant_bsearch_str (cache, ref, &pos))
    {
      g_autoptr(GVariant) refdata = g_variant_get_child_value (cache, pos);
      return g_variant_get_child_value (refdata, 1);
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
               _("No entry for %s in remote summary flatpak sparse cache "), ref);

  return FALSE;
}

static gboolean
flatpak_remote_state_save_summary (FlatpakRemoteState *self,
                                   GFile              *dir,
                                   GCancellable       *cancellable,
                                   GError            **error)
{
  g_autoptr(GFile) summary_file = g_file_get_child (dir, "summary");
  g_autoptr(GBytes) summary_bytes = NULL;

  /* For non-p2p case we always require a summary */
  if (!flatpak_remote_state_ensure_summary (self, error))
    return FALSE;

  summary_bytes = g_variant_get_data_as_bytes (self->summary);

  if (!g_file_replace_contents (summary_file,
                                g_bytes_get_data (summary_bytes, NULL),
                                g_bytes_get_size (summary_bytes),
                                NULL, FALSE, 0, NULL, cancellable, error))
    return FALSE;

  if (self->summary_sig_bytes != NULL)
    {
      g_autoptr(GFile) summary_sig_file = g_file_get_child (dir, "summary.sig");
      if (!g_file_replace_contents (summary_sig_file,
                                    g_bytes_get_data (self->summary_sig_bytes, NULL),
                                    g_bytes_get_size (self->summary_sig_bytes),
                                    NULL, FALSE, 0, NULL, cancellable, error))
        return FALSE;
    }

  return TRUE;
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
                                                      TRUE, (GDestroyNotify) g_bytes_unref, bytes));
}

static void
flatpak_deploy_finalize (GObject *object)
{
  FlatpakDeploy *self = FLATPAK_DEPLOY (object);

  g_clear_pointer (&self->ref, g_free);
  g_clear_object (&self->dir);
  g_clear_pointer (&self->metadata, g_key_file_unref);
  g_clear_pointer (&self->system_overrides, flatpak_context_free);
  g_clear_pointer (&self->user_overrides, flatpak_context_free);
  g_clear_pointer (&self->system_app_overrides, flatpak_context_free);
  g_clear_pointer (&self->user_app_overrides, flatpak_context_free);

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

GVariant *
flatpak_load_deploy_data (GFile        *deploy_dir,
                          const char   *ref,
                          int           required_version,
                          GCancellable *cancellable,
                          GError      **error)
{
  g_autoptr(GFile) data_file = NULL;
  char *data = NULL;
  gsize data_size;
  g_autoptr(GVariant) deploy_data = NULL;

  data_file = g_file_get_child (deploy_dir, "deploy");
  if (!g_file_load_contents (data_file, cancellable, &data, &data_size, NULL, error))
    return NULL;

  deploy_data = g_variant_ref_sink (g_variant_new_from_data (FLATPAK_DEPLOY_DATA_GVARIANT_FORMAT,
                                                             data, data_size,
                                                             FALSE, g_free, data));


  if (flatpak_deploy_data_get_version (deploy_data) < required_version)
    return upgrade_deploy_data (deploy_data, deploy_dir, ref);

  return g_steal_pointer (&deploy_data);
}


GVariant *
flatpak_deploy_get_deploy_data (FlatpakDeploy *deploy,
                                int            required_version,
                                GCancellable  *cancellable,
                                GError       **error)
{
  return flatpak_load_deploy_data (deploy->dir,
                                   deploy->ref,
                                   required_version,
                                   cancellable,
                                   error);
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

  if (deploy->system_app_overrides)
    flatpak_context_merge (overrides, deploy->system_app_overrides);

  if (deploy->user_overrides)
    flatpak_context_merge (overrides, deploy->user_overrides);

  if (deploy->user_app_overrides)
    flatpak_context_merge (overrides, deploy->user_app_overrides);

  return overrides;
}

GKeyFile *
flatpak_deploy_get_metadata (FlatpakDeploy *deploy)
{
  return g_key_file_ref (deploy->metadata);
}

static FlatpakDeploy *
flatpak_deploy_new (GFile *dir, const char *ref, GKeyFile *metadata)
{
  FlatpakDeploy *deploy;

  deploy = g_object_new (FLATPAK_TYPE_DEPLOY, NULL);
  deploy->ref = g_strdup (ref);
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
        setup_value = (gsize) system_dir;
      else
        setup_value = (gsize) FLATPAK_SYSTEMDIR;
      g_once_init_leave (&path, setup_value);
    }

  return g_file_new_for_path ((char *) path);
}

static FlatpakDirStorageType
parse_storage_type (const char *type_string)
{
  if (type_string != NULL)
    {
      g_autofree char *type_low = NULL;

      type_low = g_ascii_strdown (type_string, -1);
      if (g_strcmp0 (type_low, "network") == 0)
        return FLATPAK_DIR_STORAGE_TYPE_NETWORK;

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
                          (GDestroyNotify) dir_extra_data_free);

  g_ptr_array_add (locations, location);
}

static gboolean
is_good_installation_id (const char *id)
{
  if (strcmp (id, "") == 0 ||
      strcmp (id, "user") == 0 ||
      strcmp (id, "default") == 0 ||
      strcmp (id, "system") == 0)
    return FALSE;

  if (!g_str_is_ascii (id) ||
      strpbrk (id, " /\n"))
    return FALSE;

  if (strlen (id) > 80)
    return FALSE;

  return TRUE;
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

  if (!g_key_file_load_from_file (keyfile, file_path, G_KEY_FILE_NONE, &my_error))
    {
      g_debug ("Could not get list of system installations from '%s': %s", file_path, my_error->message);
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
        {
          if (g_str_has_prefix (groups[i], "Installation "))
            g_warning ("Installation without quotes (%s). Ignoring", groups[i]);
          continue;
        }

      id = g_strdup (&groups[i][14]);
      if (!g_str_has_suffix (id, "\""))
        {
          g_warning ("While reading '%s': Installation without closing quote (%s). Ignoring", file_path, groups[i]);
          continue;
        }

      len = strlen (id);
      if (len > 0)
        id[len - 1] = '\0';

      if (!is_good_installation_id (id))
        {
          g_warning ("While reading '%s': Bad installation ID '%s'. Ignoring", file_path, id);
          continue;
        }

      if (has_system_location (locations, id))
        {
          g_warning ("While reading '%s': Duplicate installation ID '%s'. Ignoring", file_path, id);
          continue;
        }

      path = g_key_file_get_string (keyfile, groups[i], "Path", &my_error);
      if (path == NULL)
        {
          g_debug ("While reading '%s': Unable to get path for installation '%s': %s", file_path, id, my_error->message);
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
  const GFile *location_object_a = *(const GFile **) location_a;
  const GFile *location_object_b = *(const GFile **) location_b;
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

  return g_object_ref ((GFile *) file);
}

static gboolean
validate_commit_metadata (GVariant   *commit_data,
                          const char *ref,
                          const char *required_metadata,
                          gsize       required_metadata_size,
                          gboolean   require_xa_metadata,
                          GError   **error)
{
  g_autoptr(GVariant) commit_metadata = NULL;
  g_autoptr(GVariant) xa_metadata_v = NULL;
  const char *xa_metadata = NULL;
  gsize xa_metadata_size = 0;

  commit_metadata = g_variant_get_child_value (commit_data, 0);

  if (commit_metadata != NULL)
    {
      xa_metadata_v = g_variant_lookup_value (commit_metadata,
                                              "xa.metadata",
                                              G_VARIANT_TYPE_STRING);
      if (xa_metadata_v)
        xa_metadata = g_variant_get_string (xa_metadata_v, &xa_metadata_size);
    }

  if ((xa_metadata == NULL && require_xa_metadata) ||
      (xa_metadata != NULL && (xa_metadata_size != required_metadata_size ||
                               memcmp (xa_metadata, required_metadata, xa_metadata_size) != 0)))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                   _("Commit metadata for %s not matching expected metadata"), ref);
      return FALSE;
    }

  return TRUE;
}

/* This is a cache directory similar to ~/.cache/flatpak/system-cache,
 * but in /var/tmp. This is useful for things like the system child
 * repos, because it is more likely to be on the same filesystem as
 * the system repo (thus increasing chances for e.g. reflink copying),
 * and avoids filling the users homedirectory with temporary data.
 *
 * In order to re-use this between instances we create a symlink
 * in /run to it and verify it before use.
 */
static GFile *
flatpak_ensure_system_user_cache_dir_location (GError **error)
{
  g_autofree char *path = NULL;
  g_autofree char *symlink_path = NULL;
  struct stat st_buf;
  const char *custom_path = g_getenv ("FLATPAK_SYSTEM_CACHE_DIR");

  if (custom_path != NULL && *custom_path != 0)
    {
      if (g_mkdir_with_parents (custom_path, 0755) != 0)
        {
          glnx_set_error_from_errno (error);
          return NULL;
        }

      return g_file_new_for_path (custom_path);
    }

  symlink_path = g_build_filename (g_get_user_runtime_dir (), ".flatpak-cache", NULL);
  path = flatpak_readlink (symlink_path, NULL);

  if (stat (path, &st_buf) == 0 &&
      /* Must be owned by us */
      st_buf.st_uid == getuid () &&
      /* and not writeable by others */
      (st_buf.st_mode & 0022) == 0)
    return g_file_new_for_path (path);

  path = g_strdup ("/var/tmp/flatpak-cache-XXXXXX");

  if (g_mkdtemp_full (path, 0755) == NULL)
    {
      flatpak_fail (error, "Can't create temporary directory");
      return NULL;
    }

  unlink (symlink_path);
  if (symlink (path, symlink_path) != 0)
    {
      glnx_set_error_from_errno (error);
      return NULL;
    }

  return g_file_new_for_path (path);
}

static GFile *
flatpak_get_user_cache_dir_location (void)
{
  g_autoptr(GFile) base_dir = g_file_new_for_path (g_get_user_cache_dir ());

  return g_file_resolve_relative_path (base_dir, "flatpak/system-cache");
}

static GFile *
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

static GFile *
flatpak_dir_get_oci_cache_file (FlatpakDir *self,
                                const char *remote,
                                const char *suffix,
                                GError    **error)
{
  g_autoptr(GFile) oci_dir = NULL;
  g_autofree char *filename = NULL;

  oci_dir = g_file_get_child (flatpak_dir_get_path (self), "oci");
  if (g_mkdir_with_parents (flatpak_file_get_path_cached (oci_dir), 0755) != 0)
    {
      glnx_set_error_from_errno (error);
      return NULL;
    }

  filename = g_strconcat (remote, suffix, NULL);
  return g_file_get_child (oci_dir, filename);
}

static GFile *
flatpak_dir_get_oci_index_location (FlatpakDir *self,
                                    const char *remote,
                                    GError    **error)
{
  return flatpak_dir_get_oci_cache_file (self, remote, ".index.gz", error);
}

static GFile *
flatpak_dir_get_oci_summary_location (FlatpakDir *self,
                                      const char *remote,
                                      GError    **error)
{
  return flatpak_dir_get_oci_cache_file (self, remote, ".summary", error);
}

static gboolean
flatpak_dir_remove_oci_file (FlatpakDir   *self,
                             const char   *remote,
                             const char   *suffix,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GFile) file = flatpak_dir_get_oci_cache_file (self, remote, suffix, error);
  g_autoptr(GError) local_error = NULL;

  if (file == NULL)
    return FALSE;

  if (!g_file_delete (file, cancellable, &local_error) &&
      !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  return TRUE;
}

static gboolean
flatpak_dir_remove_oci_files (FlatpakDir   *self,
                              const char   *remote,
                              GCancellable *cancellable,
                              GError      **error)
{
  if (!flatpak_dir_remove_oci_file (self, remote, ".index.gz", cancellable, error) ||
      !flatpak_dir_remove_oci_file (self, remote, ".summary", cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
flatpak_dir_use_system_helper (FlatpakDir *self,
                               const char *installing_from_remote)
{
#ifdef USE_SYSTEM_HELPER
  if (self->no_system_helper || self->user || getuid () == 0)
    return FALSE;

  /* OCI doesn't do signatures atm, so we can't use the system helper for this */
  if (installing_from_remote != NULL && flatpak_dir_get_remote_oci (self, installing_from_remote))
    return FALSE;

  return TRUE;
#else
  return FALSE;
#endif
}

static GVariant *
flatpak_dir_system_helper_call (FlatpakDir   *self,
                                const gchar  *method_name,
                                GVariant     *parameters,
                                GCancellable *cancellable,
                                GError      **error)
{
  GVariant *res;

  if (g_once_init_enter (&self->system_helper_bus))
    {
      const char *on_session = g_getenv ("FLATPAK_SYSTEM_HELPER_ON_SESSION");
      GDBusConnection *system_helper_bus =
        g_bus_get_sync (on_session != NULL ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM,
                        cancellable, NULL);

      /* To ensure reverse mapping */
      flatpak_error_quark ();

      g_once_init_leave (&self->system_helper_bus, system_helper_bus ? system_helper_bus : (gpointer) 1 );
    }

  if (self->system_helper_bus == (gpointer) 1)
    {
      flatpak_fail (error, _("Unable to connect to system bus"));
      return NULL;
    }

  g_debug ("Calling system helper: %s", method_name);
  res = g_dbus_connection_call_sync (self->system_helper_bus,
                                     "org.freedesktop.Flatpak.SystemHelper",
                                     "/org/freedesktop/Flatpak/SystemHelper",
                                     "org.freedesktop.Flatpak.SystemHelper",
                                     method_name,
                                     parameters,
                                     NULL, G_DBUS_CALL_FLAGS_NONE, G_MAXINT,
                                     cancellable,
                                     error);
  if (res == NULL && error)
    g_dbus_error_strip_remote_error (*error);

  return res;
}

static gboolean
flatpak_dir_system_helper_call_deploy (FlatpakDir         *self,
                                       const gchar        *arg_repo_path,
                                       guint               arg_flags,
                                       const gchar        *arg_ref,
                                       const gchar        *arg_origin,
                                       const gchar *const *arg_subpaths,
                                       const gchar        *arg_installation,
                                       GCancellable       *cancellable,
                                       GError            **error)
{
  if (flatpak_dir_get_no_interaction (self))
    arg_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_NO_INTERACTION;

  g_autoptr(GVariant) ret =
    flatpak_dir_system_helper_call (self, "Deploy",
                                    g_variant_new ("(^ayuss^ass)",
                                                   arg_repo_path,
                                                   arg_flags,
                                                   arg_ref,
                                                   arg_origin,
                                                   arg_subpaths,
                                                   arg_installation),
                                    cancellable, error);
  return ret != NULL;
}

static gboolean
flatpak_dir_system_helper_call_deploy_appstream (FlatpakDir   *self,
                                                 const gchar  *arg_repo_path,
                                                 guint         arg_flags,
                                                 const gchar  *arg_origin,
                                                 const gchar  *arg_arch,
                                                 const gchar  *arg_installation,
                                                 GCancellable *cancellable,
                                                 GError      **error)
{
  if (flatpak_dir_get_no_interaction (self))
    arg_flags |= FLATPAK_HELPER_DEPLOY_APPSTREAM_FLAGS_NO_INTERACTION;

  g_autoptr(GVariant) ret =
    flatpak_dir_system_helper_call (self, "DeployAppstream",
                                    g_variant_new ("(^ayusss)",
                                                   arg_repo_path,
                                                   arg_flags,
                                                   arg_origin,
                                                   arg_arch,
                                                   arg_installation),
                                    cancellable, error);
  return ret != NULL;
}

static gboolean
flatpak_dir_system_helper_call_uninstall (FlatpakDir   *self,
                                          guint         arg_flags,
                                          const gchar  *arg_ref,
                                          const gchar  *arg_installation,
                                          GCancellable *cancellable,
                                          GError      **error)
{
  if (flatpak_dir_get_no_interaction (self))
    arg_flags |= FLATPAK_HELPER_UNINSTALL_FLAGS_NO_INTERACTION;

  g_autoptr(GVariant) ret =
    flatpak_dir_system_helper_call (self, "Uninstall",
                                    g_variant_new ("(uss)",
                                                   arg_flags,
                                                   arg_ref,
                                                   arg_installation),
                                    cancellable, error);
  return ret != NULL;
}

static gboolean
flatpak_dir_system_helper_call_install_bundle (FlatpakDir   *self,
                                               const gchar  *arg_bundle_path,
                                               guint         arg_flags,
                                               const gchar  *arg_remote,
                                               const gchar  *arg_installation,
                                               gchar       **out_ref,
                                               GCancellable *cancellable,
                                               GError      **error)
{
  if (flatpak_dir_get_no_interaction (self))
    arg_flags |= FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_NO_INTERACTION;

  g_autoptr(GVariant) ret =
    flatpak_dir_system_helper_call (self, "InstallBundle",
                                    g_variant_new ("(^ayuss)",
                                                   arg_bundle_path,
                                                   arg_flags,
                                                   arg_remote,
                                                   arg_installation),
                                    cancellable, error);
  if (ret == NULL)
    return FALSE;

  g_variant_get (ret, "(s)", out_ref);
  return TRUE;
}

static gboolean
flatpak_dir_system_helper_call_configure_remote (FlatpakDir   *self,
                                                 guint         arg_flags,
                                                 const gchar  *arg_remote,
                                                 const gchar  *arg_config,
                                                 GVariant     *arg_gpg_key,
                                                 const gchar  *arg_installation,
                                                 GCancellable *cancellable,
                                                 GError      **error)
{
  if (flatpak_dir_get_no_interaction (self))
    arg_flags |= FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_NO_INTERACTION;

  g_autoptr(GVariant) ret =
    flatpak_dir_system_helper_call (self, "ConfigureRemote",
                                    g_variant_new ("(uss@ays)",
                                                   arg_flags,
                                                   arg_remote,
                                                   arg_config,
                                                   arg_gpg_key,
                                                   arg_installation),
                                    cancellable,
                                    error);
  return ret != NULL;
}

static gboolean
flatpak_dir_system_helper_call_configure (FlatpakDir   *self,
                                          guint         arg_flags,
                                          const gchar  *arg_key,
                                          const gchar  *arg_value,
                                          const gchar  *arg_installation,
                                          GCancellable *cancellable,
                                          GError      **error)
{
  if (flatpak_dir_get_no_interaction (self))
    arg_flags |= FLATPAK_HELPER_CONFIGURE_FLAGS_NO_INTERACTION;

  g_autoptr(GVariant) ret =
    flatpak_dir_system_helper_call (self, "Configure",
                                    g_variant_new ("(usss)",
                                                   arg_flags,
                                                   arg_key,
                                                   arg_value,
                                                   arg_installation),
                                    cancellable, error);
  return ret != NULL;
}

static gboolean
flatpak_dir_system_helper_call_update_remote (FlatpakDir   *self,
                                              guint         arg_flags,
                                              const gchar  *arg_remote,
                                              const gchar  *arg_installation,
                                              const gchar  *arg_summary_path,
                                              const gchar  *arg_summary_sig_path,
                                              GCancellable *cancellable,
                                              GError      **error)
{
  if (flatpak_dir_get_no_interaction (self))
    arg_flags |= FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_NO_INTERACTION;

  g_autoptr(GVariant) ret =
    flatpak_dir_system_helper_call (self, "UpdateRemote",
                                    g_variant_new ("(uss^ay^ay)",
                                                   arg_flags,
                                                   arg_remote,
                                                   arg_installation,
                                                   arg_summary_path,
                                                   arg_summary_sig_path),
                                    cancellable, error);
  return ret != NULL;
}

static gboolean
flatpak_dir_system_helper_call_remove_local_ref (FlatpakDir   *self,
                                                 guint         arg_flags,
                                                 const gchar  *arg_remote,
                                                 const gchar  *arg_ref,
                                                 const gchar  *arg_installation,
                                                 GCancellable *cancellable,
                                                 GError      **error)
{
  if (flatpak_dir_get_no_interaction (self))
    arg_flags |= FLATPAK_HELPER_REMOVE_LOCAL_REF_FLAGS_NO_INTERACTION;

  g_autoptr(GVariant) ret =
    flatpak_dir_system_helper_call (self, "RemoveLocalRef",
                                    g_variant_new ("(usss)",
                                                   arg_flags,
                                                   arg_remote,
                                                   arg_ref,
                                                   arg_installation),
                                    cancellable, error);
  return ret != NULL;
}

static gboolean
flatpak_dir_system_helper_call_prune_local_repo (FlatpakDir   *self,
                                                 guint         arg_flags,
                                                 const gchar  *arg_installation,
                                                 GCancellable *cancellable,
                                                 GError      **error)
{
  if (flatpak_dir_get_no_interaction (self))
    arg_flags |= FLATPAK_HELPER_PRUNE_LOCAL_REPO_FLAGS_NO_INTERACTION;

  g_autoptr(GVariant) ret =
    flatpak_dir_system_helper_call (self, "PruneLocalRepo",
                                    g_variant_new ("(us)",
                                                   arg_flags,
                                                   arg_installation),
                                    cancellable, error);
  return ret != NULL;
}

static gboolean
flatpak_dir_system_helper_call_run_triggers (FlatpakDir   *self,
                                             guint         arg_flags,
                                             const gchar  *arg_installation,
                                             GCancellable *cancellable,
                                             GError      **error)
{
  if (flatpak_dir_get_no_interaction (self))
    arg_flags |= FLATPAK_HELPER_RUN_TRIGGERS_FLAGS_NO_INTERACTION;

  g_autoptr(GVariant) ret =
    flatpak_dir_system_helper_call (self, "RunTriggers",
                                    g_variant_new ("(us)",
                                                   arg_flags,
                                                   arg_installation),
                                    cancellable, error);
  return ret != NULL;
}

static gboolean
flatpak_dir_system_helper_call_ensure_repo (FlatpakDir   *self,
                                            guint         arg_flags,
                                            const gchar  *arg_installation,
                                            GCancellable *cancellable,
                                            GError      **error)
{
  if (flatpak_dir_get_no_interaction (self))
    arg_flags |= FLATPAK_HELPER_ENSURE_REPO_FLAGS_NO_INTERACTION;

  g_autoptr(GVariant) ret =
    flatpak_dir_system_helper_call (self, "EnsureRepo",
                                    g_variant_new ("(us)",
                                                   arg_flags,
                                                   arg_installation),
                                    cancellable, error);
  return ret != NULL;
}

static gboolean
flatpak_dir_system_helper_call_update_summary (FlatpakDir   *self,
                                               guint         arg_flags,
                                               const gchar  *arg_installation,
                                               GCancellable *cancellable,
                                               GError      **error)
{
  if (flatpak_dir_get_no_interaction (self))
    arg_flags |= FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_NO_INTERACTION;

  g_autoptr(GVariant) ret =
    flatpak_dir_system_helper_call (self, "UpdateSummary",
                                    g_variant_new ("(us)",
                                                   arg_flags,
                                                   arg_installation),
                                    cancellable, error);
  return ret != NULL;
}

static gboolean
flatpak_dir_system_helper_call_generate_oci_summary (FlatpakDir   *self,
                                                     guint         arg_flags,
                                                     const gchar  *arg_origin,
                                                     const gchar  *arg_installation,
                                                     GCancellable *cancellable,
                                                     GError      **error)
{
  if (flatpak_dir_get_no_interaction (self))
    arg_flags |= FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_NO_INTERACTION;

  g_autoptr(GVariant) ret =
    flatpak_dir_system_helper_call (self, "GenerateOciSummary",
                                    g_variant_new ("(uss)",
                                                   arg_flags,
                                                   arg_origin,
                                                   arg_installation),
                                    cancellable, error);
  return ret != NULL;
}

static OstreeRepo *
system_ostree_repo_new (GFile *repodir)
{
  g_autofree char *config_dir = NULL;

  config_dir = g_strdup_printf ("%s/%s",
                                get_config_dir_location (),
                                SYSCONF_REMOTES_DIR);

  if (!g_file_test (config_dir, G_FILE_TEST_IS_DIR))
    g_clear_pointer (&config_dir, g_free);

  return g_object_new (OSTREE_TYPE_REPO,
                       "path", repodir,
                       "remotes-config-dir", config_dir,
                       NULL);
}

static void
flatpak_dir_finalize (GObject *object)
{
  FlatpakDir *self = FLATPAK_DIR (object);

  g_clear_object (&self->repo);
  g_clear_object (&self->basedir);
  g_clear_pointer (&self->extra_data, dir_extra_data_free);

  if (self->system_helper_bus != (gpointer)1)
    g_clear_object (&self->system_helper_bus);

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

void
flatpak_dir_set_no_interaction (FlatpakDir *self,
                                gboolean    no_interaction)
{
  self->no_interaction = no_interaction;
}

gboolean
flatpak_dir_get_no_interaction (FlatpakDir *self)
{
  return self->no_interaction;
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
  if (self->user)
    return "user";

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
flatpak_dir_get_name_cached (FlatpakDir *self)
{
  char *name;

  name = g_object_get_data (G_OBJECT (self), "cached-name");
  if (!name)
    {
      name = flatpak_dir_get_name (self),
      g_object_set_data_full (G_OBJECT (self), "cached-name", name, g_free);
    }

  return (const char *)name;
}

char *
flatpak_dir_get_display_name (FlatpakDir *self)
{
  if (self->user)
    return g_strdup (_("User installation"));

  if (self->extra_data != NULL)
    {
      if (self->extra_data->display_name)
        return g_strdup (self->extra_data->display_name);

      return g_strdup_printf (_("System (%s) installation"), self->extra_data->id);
    }

  return g_strdup (SYSTEM_DIR_DEFAULT_DISPLAY_NAME);
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

  if (app_id)
    file = g_file_get_child (override_dir, app_id);
  else
    file = g_file_get_child (override_dir, "global");

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

  if (app_id)
    file = g_file_get_child (override_dir, app_id);
  else
    file = g_file_get_child (override_dir, "global");

  filename = g_file_get_path (file);
  parent = g_path_get_dirname (filename);
  if (g_mkdir_with_parents (parent, 0755))
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  return g_key_file_save_to_file (metakey, filename, error);
}

gboolean
flatpak_remove_override_keyfile (const char *app_id,
                                 gboolean    user,
                                 GError    **error)
{
  g_autoptr(GFile) base_dir = NULL;
  g_autoptr(GFile) override_dir = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GError) local_error = NULL;

  if (user)
    base_dir = flatpak_get_user_base_dir_location ();
  else
    base_dir = flatpak_get_system_default_base_dir_location ();

  override_dir = g_file_get_child (base_dir, "overrides");

  if (app_id)
    file = g_file_get_child (override_dir, app_id);
  else
    file = g_file_get_child (override_dir, "global");

  if (!g_file_delete (file, NULL, &local_error) &&
      !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  return TRUE;
}

/* Note: passing a checksum only works here for non-sub-set deploys, not
   e.g. a partial locale install, because it will not find the real
   deploy directory. This is ok for now, because checksum is only
   currently passed from flatpak_installation_launch() when launching
   a particular version of an app, which is not used for locales. */
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
      if (checksum == NULL)
        g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                     _("%s not installed"), ref);
      else
        g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                     _("%s (commit %s) not installed"), ref, checksum);
      return NULL;
    }

  metadata = g_file_get_child (deploy_dir, "metadata");
  if (!g_file_load_contents (metadata, cancellable, &metadata_contents, &metadata_size, NULL, error))
    return NULL;

  metakey = g_key_file_new ();
  if (!g_key_file_load_from_data (metakey, metadata_contents, metadata_size, 0, error))
    return NULL;

  deploy = flatpak_deploy_new (deploy_dir, ref, metakey);

  ref_parts = g_strsplit (ref, "/", -1);
  g_assert (g_strv_length (ref_parts) == 4);

  /* Only load system global overrides for system installed apps */
  if (!self->user)
    {
      deploy->system_overrides = flatpak_load_override_file (NULL, FALSE, error);
      if (deploy->system_overrides == NULL)
        return NULL;
    }

  /* Always load user global overrides */
  deploy->user_overrides = flatpak_load_override_file (NULL, TRUE, error);
  if (deploy->user_overrides == NULL)
    return NULL;

  /* Only apps have app overrides */
  if (strcmp (ref_parts[0], "app") == 0)
    {
      /* Only load system overrides for system installed apps */
      if (!self->user)
        {
          deploy->system_app_overrides = flatpak_load_override_file (ref_parts[1], FALSE, error);
          if (deploy->system_app_overrides == NULL)
            return NULL;
        }

      /* Always load user overrides */
      deploy->user_app_overrides = flatpak_load_override_file (ref_parts[1], TRUE, error);
      if (deploy->user_app_overrides == NULL)
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

char *
flatpak_dir_get_deploy_subdir (FlatpakDir          *self,
                               const char          *checksum,
                               const char * const * subpaths)
{
  if (subpaths == NULL || *subpaths == NULL)
    return g_strdup (checksum);
  else
    {
      GString *str = g_string_new (checksum);
      int i;
      for (i = 0; subpaths[i] != NULL; i++)
        {
          const char *s = subpaths[i];
          g_string_append_c (str, '-');
          while (*s)
            {
              if (*s != '/')
                g_string_append_c (str, *s);
              s++;
            }
        }
      return g_string_free (str, FALSE);
    }
}

GFile *
flatpak_dir_get_unmaintained_extension_dir (FlatpakDir *self,
                                            const char *name,
                                            const char *arch,
                                            const char *branch)
{
  g_autofree char *unmaintained_ref = NULL;

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


/* This is an lock that protects the repo itself. Any operation that
 * relies on objects not disappearing from the repo need to hold this
 * in a non-exclusive mode, while anything that can remove objects
 * (i.e. prune) need to take it in exclusive mode.
 *
 * The following operations depends on objects not disappearing:
 *  * pull into a staging directory (pre-existing objects are not downloaded)
 *  * moving a staging directory into the repo (no ref keeps the object alive during copy)
 *  * Deploying a ref (a parallel update + prune could cause objects to be removed)
 *
 * In practice this means we hold a shared lock during deploy and
 * pull, and an excusive lock during prune.
 */
gboolean
flatpak_dir_repo_lock (FlatpakDir   *self,
                       GLnxLockFile *lockfile,
                       int           operation,
                       GCancellable *cancellable,
                       GError      **error)
{
  g_autoptr(GFile) lock_file = g_file_get_child (flatpak_dir_get_path (self), "repo-lock");
  g_autofree char *lock_path = g_file_get_path (lock_file);

  return glnx_make_lock_file (AT_FDCWD, lock_path, operation, lockfile, error);
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

int
flatpak_deploy_data_get_version (GVariant *deploy_data)
{
  g_autoptr(GVariant) metadata = g_variant_get_child_value (deploy_data, 4);
  int version = 0;

  g_variant_lookup (metadata, "deploy-version", "i", &version);

  return version;
}

static const char *
flatpak_deploy_data_get_string (GVariant *deploy_data, const char *key)
{
  g_autoptr(GVariant) metadata = g_variant_get_child_value (deploy_data, 4);
  const char *value = NULL;

  g_variant_lookup (metadata, key, "&s", &value);

  return value;
}

static const char *
flatpak_deploy_data_get_localed_string (GVariant *deploy_data, const char *key)
{
  g_autoptr(GVariant) metadata = g_variant_get_child_value (deploy_data, 4);
  const char * const * languages = g_get_language_names ();
  const char *value = NULL;
  int i;

  for (i = 0; languages[i]; ++i)
    {
      g_autofree char *localed_key = NULL;
      if (strcmp (languages[i], "C") == 0)
        localed_key = g_strdup (key);
      else
        localed_key = g_strdup_printf ("%s@%s", key, languages[i]);

      if (g_variant_lookup (metadata, localed_key, "&s", &value))
        return value;
    }

  return NULL;
}

const char *
flatpak_deploy_data_get_alt_id (GVariant *deploy_data)
{
  return flatpak_deploy_data_get_string (deploy_data, "alt-id");
}

const char *
flatpak_deploy_data_get_eol (GVariant *deploy_data)
{
  return flatpak_deploy_data_get_string (deploy_data, "eol");
}

const char *
flatpak_deploy_data_get_eol_rebase (GVariant *deploy_data)
{
  return flatpak_deploy_data_get_string (deploy_data, "eolr");
}

const char *
flatpak_deploy_data_get_runtime (GVariant *deploy_data)
{
  return flatpak_deploy_data_get_string (deploy_data, "runtime");
}

const char *
flatpak_deploy_data_get_appdata_name (GVariant *deploy_data)
{
  return flatpak_deploy_data_get_localed_string (deploy_data, "appdata-name");
}

const char *
flatpak_deploy_data_get_appdata_summary (GVariant *deploy_data)
{
  return flatpak_deploy_data_get_localed_string (deploy_data, "appdata-summary");
}

const char *
flatpak_deploy_data_get_appdata_version (GVariant *deploy_data)
{
    return flatpak_deploy_data_get_string (deploy_data, "appdata-version");
}

const char *
flatpak_deploy_data_get_appdata_license (GVariant *deploy_data)
{
    return flatpak_deploy_data_get_string (deploy_data, "appdata-license");
}

/*<private>
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

static char *
read_appdata_xml_from_deploy_dir (GFile *deploy_dir, const char *id)
{
  g_autoptr(GFile) appdata_file = NULL;
  g_autofree char *appdata_name = NULL;
  g_autoptr(GFileInputStream) appdata_in = NULL;
  gsize size;

  appdata_name = g_strconcat (id, ".xml.gz", NULL);
  appdata_file  = flatpak_build_file (deploy_dir, "files/share/app-info/xmls", appdata_name, NULL);

  appdata_in = g_file_read (appdata_file, NULL, NULL);
  if (appdata_in)
    {
      g_autoptr(GZlibDecompressor) decompressor = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
      g_autoptr(GInputStream) converter = g_converter_input_stream_new (G_INPUT_STREAM (appdata_in), G_CONVERTER (decompressor));
      g_autoptr(GBytes) appdata_xml = NULL;

      appdata_xml = flatpak_read_stream (converter, TRUE, NULL);
      if (appdata_xml)
        return g_bytes_unref_to_data (g_steal_pointer (&appdata_xml), &size);
    }

  return NULL;
}

static void
add_locale_metadata_string (GVariantBuilder *metadata_builder,
                            const char *keyname,
                            GHashTable *values)
{
  if (values == NULL)
    return;

  GLNX_HASH_TABLE_FOREACH_KV (values, const char *, locale, const char *, value)
    {
      const char *key;
      g_autofree char *key_free = NULL;
      if (strcmp (locale, "C") == 0)
        key = keyname;
      else
        {
          key_free = g_strdup_printf ("%s@%s", keyname, locale);
          key = key_free;
        }

      g_variant_builder_add (metadata_builder, "{s@v}", key,
                             g_variant_new_variant (g_variant_new_string (value)));
    }
}

static void
add_appdata_to_deploy_data (GVariantBuilder *metadata_builder,
                            GFile      *deploy_dir,
                            const char *id)
{
  g_autofree char *appdata_xml = NULL;
  g_autoptr(GHashTable) names = NULL;
  g_autoptr(GHashTable) comments = NULL;
  g_autofree char *version = NULL;
  g_autofree char *license = NULL;

  appdata_xml = read_appdata_xml_from_deploy_dir (deploy_dir, id);
  if (appdata_xml == NULL)
    return;

  if (flatpak_parse_appdata (appdata_xml, id, &names, &comments, &version, &license))
    {
      add_locale_metadata_string (metadata_builder, "appdata-name", names);
      add_locale_metadata_string (metadata_builder, "appdata-summary", comments);
      if (version)
        g_variant_builder_add (metadata_builder, "{s@v}", "appdata-version",
                               g_variant_new_variant (g_variant_new_string (version)));
      if (license)
        g_variant_builder_add (metadata_builder, "{s@v}", "appdata-license",
                               g_variant_new_variant (g_variant_new_string (license)));
    }
}

static GVariant *
flatpak_dir_new_deploy_data (FlatpakDir *self,
                             GFile      *deploy_dir,
                             GVariant   *commit_metadata,
                             GKeyFile   *metadata,
                             const char *id,
                             const char *origin,
                             const char *commit,
                             char      **subpaths,
                             guint64     installed_size)
{
  char *empty_subpaths[] = {NULL};
  GVariantBuilder metadata_builder;
  g_autofree char *application_runtime = NULL;
  const char *alt_id = NULL;
  const char *eol = NULL;
  const char *eol_rebase = NULL;

  g_variant_lookup (commit_metadata, "xa.alt-id", "&s", &alt_id);
  g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE, "&s", &eol);
  g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE_REBASE, "&s", &eol_rebase);

  application_runtime = g_key_file_get_string (metadata,
                                               FLATPAK_METADATA_GROUP_APPLICATION,
                                               FLATPAK_METADATA_KEY_RUNTIME, NULL);

  g_variant_builder_init (&metadata_builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&metadata_builder, "{s@v}", "deploy-version",
                         g_variant_new_variant (g_variant_new_int32 (FLATPAK_DEPLOY_VERSION_CURRENT)));
  if (alt_id)
    g_variant_builder_add (&metadata_builder, "{s@v}", "alt-id",
                           g_variant_new_variant (g_variant_new_string (alt_id)));
  if (eol)
    g_variant_builder_add (&metadata_builder, "{s@v}", "eol",
                           g_variant_new_variant (g_variant_new_string (eol)));
  if (eol_rebase)
    g_variant_builder_add (&metadata_builder, "{s@v}", "eolr",
                           g_variant_new_variant (g_variant_new_string (eol_rebase)));
  if (application_runtime)
    g_variant_builder_add (&metadata_builder, "{s@v}", "runtime",
                           g_variant_new_variant (g_variant_new_string (application_runtime)));

  add_appdata_to_deploy_data (&metadata_builder, deploy_dir, id);

  return g_variant_ref_sink (g_variant_new ("(ss^ast@a{sv})",
                                            origin,
                                            commit,
                                            subpaths ? subpaths : empty_subpaths,
                                            GUINT64_TO_BE (installed_size),
                                            g_variant_builder_end (&metadata_builder)));
}

static GVariant *
upgrade_deploy_data (GVariant *deploy_data, GFile *deploy_dir, const char *ref)
{
  g_autoptr(GVariant) metadata = g_variant_get_child_value (deploy_data, 4);
  GVariantBuilder metadata_builder;
  g_autofree const char **subpaths = NULL;
  int i, n, old_version;

  g_variant_builder_init (&metadata_builder, G_VARIANT_TYPE ("a{sv}"));

  g_variant_builder_add (&metadata_builder, "{s@v}", "deploy-version",
                         g_variant_new_variant (g_variant_new_int32 (FLATPAK_DEPLOY_VERSION_CURRENT)));

  /* Copy all metadata except version from old */
  n = g_variant_n_children (metadata);
  for (i = 0; i < n; i++)
    {
      g_autoptr(GVariant) child = g_variant_get_child_value (metadata, i);
      const char *key;
      g_variant_get_child (child, 0, "&s", &key);
      if (strcmp (key, "deploy-version") == 0)
        continue;
      g_variant_builder_add_value (&metadata_builder, child);
    }


  old_version = flatpak_deploy_data_get_version (deploy_data);
  if (old_version < 1)
    {
      g_auto(GStrv) ref_parts = NULL;
      ref_parts = g_strsplit (ref, "/", -1);
      add_appdata_to_deploy_data (&metadata_builder, deploy_dir, ref_parts[1]);
    }

  subpaths = flatpak_deploy_data_get_subpaths (deploy_data);
  return g_variant_ref_sink (g_variant_new ("(ss^ast@a{sv})",
                                            flatpak_deploy_data_get_origin (deploy_data),
                                            flatpak_deploy_data_get_commit (deploy_data),
                                            subpaths,
                                            GUINT64_TO_BE (flatpak_deploy_data_get_installed_size (deploy_data)),
                                            g_variant_builder_end (&metadata_builder)));
}

GVariant *
flatpak_dir_get_deploy_data (FlatpakDir   *self,
                             const char   *ref,
                             int           required_version,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GFile) deploy_dir = NULL;

  deploy_dir = flatpak_dir_get_if_deployed (self, ref, NULL, cancellable);
  if (deploy_dir == NULL)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   _("%s not installed"), ref);
      return NULL;
    }

  return flatpak_load_deploy_data (deploy_dir,
                                   ref,
                                   required_version,
                                   cancellable,
                                   error);
}


char *
flatpak_dir_get_origin (FlatpakDir   *self,
                        const char   *ref,
                        GCancellable *cancellable,
                        GError      **error)
{
  g_autoptr(GVariant) deploy_data = NULL;

  deploy_data = flatpak_dir_get_deploy_data (self, ref, FLATPAK_DEPLOY_VERSION_ANY,
                                             cancellable, error);
  if (deploy_data == NULL)
    return NULL;

  return g_strdup (flatpak_deploy_data_get_origin (deploy_data));
}

gboolean
flatpak_dir_ensure_path (FlatpakDir   *self,
                         GCancellable *cancellable,
                         GError      **error)
{
  /* In the system case, we use default perms */
  if (!self->user)
    return flatpak_mkdir_p (self->basedir, cancellable, error);
  else
    {
      /* First make the parent */
      g_autoptr(GFile) parent = g_file_get_parent (self->basedir);
      if (!flatpak_mkdir_p (parent, cancellable, error))
        return FALSE;
      glnx_autofd int parent_dfd = -1;
      if (!glnx_opendirat (AT_FDCWD, flatpak_file_get_path_cached (parent), TRUE,
                           &parent_dfd, error))
        return FALSE;
      g_autofree char *name = g_file_get_basename (self->basedir);
      /* Use 0700 in the user case to neuter any suid or world-writable
       * bits that happen to be in content; see
       * https://github.com/flatpak/flatpak/pull/837
       */
      if (mkdirat (parent_dfd, name, 0700) < 0)
        {
          if (errno == EEXIST)
            {
              /* And fix up any existing installs that had too-wide perms */
              struct stat stbuf;
              if (fstatat (parent_dfd, name, &stbuf, 0) < 0)
                return glnx_throw_errno_prefix (error, "fstatat");
              if (stbuf.st_mode & S_IXOTH)
                {
                  if (fchmodat (parent_dfd, name, 0700, 0) < 0)
                    return glnx_throw_errno_prefix (error, "fchmodat");
                }
            }
          else
            return glnx_throw_errno_prefix (error, "mkdirat");
        }

      return TRUE;
    }
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

static gboolean
_flatpak_dir_ensure_repo (FlatpakDir   *self,
                          gboolean      allow_empty,
                          GCancellable *cancellable,
                          GError      **error)
{
  g_autoptr(GFile) repodir = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  g_autoptr(GError) my_error = NULL;

  if (self->repo != NULL)
    return TRUE;

  if (!g_file_query_exists (self->basedir, cancellable))
    {
      if (flatpak_dir_use_system_helper (self, NULL))
        {
          g_autoptr(GError) local_error = NULL;
          const char *installation = flatpak_dir_get_id (self);

          if (!flatpak_dir_system_helper_call_ensure_repo (self,
                                                           FLATPAK_HELPER_ENSURE_REPO_FLAGS_NONE,
                                                           installation ? installation : "",
                                                           NULL, &local_error))
            {
              if (allow_empty)
                return TRUE;

              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }
        }
      else
        {
          g_autoptr(GError) local_error = NULL;
          if (!flatpak_dir_ensure_path (self, cancellable, &local_error))
            {
              if (allow_empty)
                return TRUE;

              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }
        }
    }

  repodir = g_file_get_child (self->basedir, "repo");

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      g_autoptr(GFile) cache_dir = NULL;
      g_autofree char *cache_path = NULL;

      repo = system_ostree_repo_new (repodir);

      cache_dir = flatpak_ensure_user_cache_dir_location (error);
      if (cache_dir == NULL)
        return FALSE;

      cache_path = g_file_get_path (cache_dir);
      if (!ostree_repo_set_cache_dir (repo,
                                      AT_FDCWD, cache_path,
                                      cancellable, error))
        return FALSE;
    }
  else if (self->user)
    repo = ostree_repo_new (repodir);
  else
    repo = system_ostree_repo_new (repodir);

  if (!g_file_query_exists (repodir, cancellable))
    {
      /* We always use bare-user-only these days, except old installations
         that still user bare-user */
      OstreeRepoMode mode = OSTREE_REPO_MODE_BARE_USER_ONLY;

      if (!ostree_repo_create (repo, mode, cancellable, &my_error))
        {
          flatpak_rm_rf (repodir, cancellable, NULL);

          if (allow_empty)
            return TRUE;

          g_propagate_error (error, g_steal_pointer (&my_error));
          return FALSE;
        }

      /* Create .changes file early to avoid polling non-existing file in monitor */
      if (!flatpak_dir_mark_changed (self, &my_error))
        {
          g_warning ("Error marking directory as changed: %s", my_error->message);
          g_clear_error (&my_error);
        }
    }
  else
    {
      if (!ostree_repo_open (repo, cancellable, error))
        {
          g_autofree char *repopath = NULL;

          repopath = g_file_get_path (repodir);
          g_prefix_error (error, _("While opening repository %s: "), repopath);
          return FALSE;
        }
    }

  /* Earlier flatpak used to reset min-free-space-percent to 0 everytime, but now we
   * favor min-free-space-size instead of it (See below).
   */
  if (!flatpak_dir_use_system_helper (self, NULL))
    {
      GKeyFile *orig_config = NULL;
      g_autoptr(GKeyFile) new_config = NULL;
      g_autofree char *orig_min_free_space_percent = NULL;
      g_autofree char *orig_min_free_space_size = NULL;
      const char *min_free_space_size = "500MB";
      guint64 min_free_space_percent_int;

      orig_config = ostree_repo_get_config (repo);
      orig_min_free_space_percent = g_key_file_get_value (orig_config, "core", "min-free-space-percent", NULL);
      orig_min_free_space_size = g_key_file_get_value (orig_config, "core", "min-free-space-size", NULL);

      if (orig_min_free_space_size == NULL)
        new_config = ostree_repo_copy_config (repo);

      /* Scrap previously written min-free-space-percent=0 and replace it with min-free-space-size */
      if (orig_min_free_space_size == NULL &&
          orig_min_free_space_percent != NULL &&
          flatpak_utils_ascii_string_to_unsigned (orig_min_free_space_percent, 10,
                                                  0, G_MAXUINT64,
                                                  &min_free_space_percent_int, &my_error))
        {
          if (min_free_space_percent_int == 0)
            {
              g_key_file_remove_key (new_config, "core", "min-free-space-percent", NULL);
              g_key_file_set_string (new_config, "core", "min-free-space-size", min_free_space_size);
            }
        }
      else if (my_error != NULL)
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return FALSE;
        }

      if (orig_min_free_space_size == NULL &&
          orig_min_free_space_percent == NULL)
        g_key_file_set_string (new_config, "core", "min-free-space-size", min_free_space_size);

      if (new_config != NULL)
        {
          if (!ostree_repo_write_config (repo, new_config, error))
            return FALSE;

          if (!ostree_repo_reload_config (repo, cancellable, error))
            return FALSE;
        }
    }

  /* Make sure we didn't reenter weirdly */
  g_assert (self->repo == NULL);
  self->repo = g_object_ref (repo);

  return TRUE;
}

gboolean
flatpak_dir_ensure_repo (FlatpakDir   *self,
                         GCancellable *cancellable,
                         GError      **error)
{
  return _flatpak_dir_ensure_repo (self, FALSE, cancellable, error);
}

gboolean
flatpak_dir_maybe_ensure_repo (FlatpakDir   *self,
                               GCancellable *cancellable,
                               GError      **error)
{
  return _flatpak_dir_ensure_repo (self, TRUE, cancellable, error);
}

char *
flatpak_dir_get_config (FlatpakDir *self,
                        const char *key,
                        GError    **error)
{
  GKeyFile *config;
  g_autofree char *ostree_key = NULL;

  if (!flatpak_dir_maybe_ensure_repo (self, NULL, error))
    return NULL;

  if (self->repo == NULL)
    {
      g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND,
                   _("The config key %s is not set"), key);
      return NULL;
    }

  config = ostree_repo_get_config (self->repo);
  ostree_key = g_strconcat ("xa.", key, NULL);

  return g_key_file_get_string (config, "core", ostree_key, error);
}

gboolean
flatpak_dir_set_config (FlatpakDir *self,
                        const char *key,
                        const char *value,
                        GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;
  g_autofree char *ostree_key = NULL;

  if (!flatpak_dir_ensure_repo (self, NULL, error))
    return FALSE;

  config = ostree_repo_copy_config (self->repo);
  ostree_key = g_strconcat ("xa.", key, NULL);

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      FlatpakHelperConfigureFlags flags = 0;
      const char *installation = flatpak_dir_get_id (self);

      if (value == NULL)
        {
          flags |= FLATPAK_HELPER_CONFIGURE_FLAGS_UNSET;
          value = "";
        }

      if (!flatpak_dir_system_helper_call_configure (self,
                                                     flags, key, value,
                                                     installation ? installation : "",
                                                     NULL, error))
        return FALSE;

      return TRUE;
    }

  if (value == NULL)
    g_key_file_remove_key (config, "core", ostree_key, NULL);
  else
    g_key_file_set_value (config, "core", ostree_key, value);

  if (!ostree_repo_write_config (self->repo, config, error))
    return FALSE;

  if (!ostree_repo_reload_config (self->repo, NULL, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_dir_mark_changed (FlatpakDir *self,
                          GError    **error)
{
  g_autoptr(GFile) changed_file = NULL;
  g_autofree char * changed_path = NULL;

  changed_file = flatpak_dir_get_changed_path (self);
  changed_path = g_file_get_path (changed_file);

  if (!g_utime (changed_path, NULL))
    return TRUE;

  if (errno != ENOENT)
    return glnx_throw_errno (error);

  if (!g_file_replace_contents (changed_file, "", 0, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, error))
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
flatpak_dir_deploy_appstream (FlatpakDir   *self,
                              const char   *remote,
                              const char   *arch,
                              gboolean     *out_changed,
                              GCancellable *cancellable,
                              GError      **error)
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
  glnx_autofd int dfd = -1;
  g_autoptr(GFileInfo) file_info = NULL;
  g_autofree char *tmpname = g_strdup (".active-XXXXXX");
  g_auto(GLnxLockFile) lock = { 0, };
  gboolean do_compress = FALSE;

  /* Keep a shared repo lock to avoid prunes removing objects we're relying on
   * while we do the checkout. This could happen if the ref changes after we
   * read its current value for the checkout. */
  if (!flatpak_dir_repo_lock (self, &lock, LOCK_SH, cancellable, error))
    return FALSE;

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

  branch = g_strdup_printf ("appstream2/%s", arch);
  remote_and_branch = g_strdup_printf ("%s:%s", remote, branch);
  if (!ostree_repo_resolve_rev (self->repo, remote_and_branch, TRUE, &new_checksum, error))
    return FALSE;

  if (new_checksum == NULL)
    {
      /* Fall back to old branch */
      g_clear_pointer (&branch, g_free);
      g_clear_pointer (&remote_and_branch, g_free);
      branch = g_strdup_printf ("appstream/%s", arch);
      remote_and_branch = g_strdup_printf ("%s:%s", remote, branch);
      if (!ostree_repo_resolve_rev (self->repo, remote_and_branch, TRUE, &new_checksum, error))
        return FALSE;
      do_compress = FALSE;
    }
  else
    do_compress = TRUE;

  if (new_checksum == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("No appstream commit to deploy"));

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
  options.bareuseronly_dirs = TRUE; /* https://github.com/ostreedev/ostree/pull/927 */

  if (!ostree_repo_checkout_at (self->repo, &options,
                                AT_FDCWD, checkout_dir_path, new_checksum,
                                cancellable, error))
    return FALSE;

  if (do_compress)
    {
      g_autoptr(GFile) appstream_xml = g_file_get_child (checkout_dir, "appstream.xml");
      g_autoptr(GFile) appstream_gz_xml = g_file_get_child (checkout_dir, "appstream.xml.gz");
      g_autoptr(GZlibCompressor) compressor = NULL;
      g_autoptr(GOutputStream) out2 = NULL;
      g_autoptr(GFileOutputStream) out = NULL;
      g_autoptr(GFileInputStream) in = NULL;

      in = g_file_read (appstream_xml, NULL, NULL);
      if (in)
        {
          compressor = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1);
          out = g_file_replace (appstream_gz_xml, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION,
                                NULL, error);
          if (out == NULL)
            return FALSE;

          out2 = g_converter_output_stream_new (G_OUTPUT_STREAM (out), G_CONVERTER (compressor));
          if (g_output_stream_splice (out2, G_INPUT_STREAM (in), G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                      NULL, error) < 0)
            return FALSE;
        }
    }

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
        g_warning ("Unable to remove old appstream checkout: %s", tmp_error->message);
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

static gboolean repo_get_remote_collection_id (OstreeRepo *repo,
                                               const char *remote_name,
                                               char      **collection_id_out,
                                               GError    **error);

static void
async_result_cb (GObject      *obj,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  GAsyncResult **result_out = user_data;

  *result_out = g_object_ref (result);
}

gboolean
flatpak_dir_find_latest_rev (FlatpakDir               *self,
                             FlatpakRemoteState       *state,
                             const char               *ref,
                             const char               *checksum_or_latest,
                             char                    **out_rev,
                             OstreeRepoFinderResult ***out_results,
                             GCancellable             *cancellable,
                             GError                  **error)
{
  g_autofree char *latest_rev = NULL;

  g_return_val_if_fail (out_rev != NULL, FALSE);

  if (state->collection_id != NULL)
    {
      /* Find the latest rev from the remote and its available mirrors, including
       * LAN and USB sources. */
      g_auto(GVariantBuilder) find_builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;
      g_autoptr(GVariant) find_options = NULL;
      g_autoptr(GAsyncResult) find_result = NULL;
      g_auto(OstreeRepoFinderResultv) results = NULL;
      OstreeCollectionRef collection_ref = { state->collection_id, (char *) ref };
      OstreeCollectionRef *collection_refs_to_fetch[2] = { &collection_ref, NULL };
      gsize i;
      g_autoptr(GMainContextPopDefault) context = NULL;

      /* Find options */
      g_variant_builder_init (&find_builder, G_VARIANT_TYPE ("a{sv}"));

      if (checksum_or_latest != NULL)
        {
          g_variant_builder_add (&find_builder, "{s@v}", "override-commit-ids",
                                 g_variant_new_variant (g_variant_new_strv (&checksum_or_latest, 1)));
        }

      find_options = g_variant_ref_sink (g_variant_builder_end (&find_builder));

      context = flatpak_main_context_new_default ();

      ostree_repo_find_remotes_async (self->repo, (const OstreeCollectionRef * const *) collection_refs_to_fetch,
                                      find_options,
                                      NULL /* default finders */,
                                      NULL /* no progress reporting */,
                                      cancellable, async_result_cb, &find_result);

      while (find_result == NULL)
        g_main_context_iteration (context, TRUE);

      results = ostree_repo_find_remotes_finish (self->repo, find_result, error);

      if (results == NULL)
        return FALSE;

      for (i = 0; results[i] != NULL && latest_rev == NULL; i++)
        latest_rev = g_strdup (g_hash_table_lookup (results[i]->ref_to_checksum, &collection_ref));

      if (latest_rev == NULL)
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("No such ref (%s, %s) in remote %s or elsewhere"),
                        collection_ref.collection_id, collection_ref.ref_name, state->remote_name);
          return FALSE;
        }

      if (out_results != NULL)
        *out_results = g_steal_pointer (&results);

      if (out_rev != NULL)
        *out_rev = g_steal_pointer (&latest_rev);
    }
  else
    {
      flatpak_remote_state_lookup_ref (state, ref, &latest_rev, NULL, error);
      if (latest_rev == NULL)
        {
          if (error != NULL && *error == NULL)
            flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Couldn't find latest checksum for ref %s in remote %s"),
                          ref, state->remote_name);
          return FALSE;
        }

      if (out_rev != NULL)
        *out_rev = g_steal_pointer (&latest_rev);
    }

  return TRUE;
}

FlatpakDirResolve *
flatpak_dir_resolve_new (const char *remote,
                         const char *ref,
                         const char *opt_commit)
{
  FlatpakDirResolve *resolve = g_new0 (FlatpakDirResolve, 1);

  resolve->remote = g_strdup (remote);
  resolve->ref = g_strdup (ref);
  resolve->opt_commit = g_strdup (opt_commit);
  return resolve;
}

void
flatpak_dir_resolve_free (FlatpakDirResolve *resolve)
{
  if (resolve)
    {
      g_free (resolve->remote);
      g_free (resolve->ref);
      g_free (resolve->opt_commit);
      g_free (resolve->resolved_commit);
      g_bytes_unref (resolve->resolved_metadata);
      g_free (resolve);
    }
}

static const char *
find_latest_p2p_result (OstreeRepoFinderResult **results, OstreeCollectionRef *cr)
{
  const char *latest_rev = NULL;
  int i;

  for (i = 0; results[i] != NULL && latest_rev == NULL; i++)
    latest_rev = g_hash_table_lookup (results[i]->ref_to_checksum, cr);

  return latest_rev;
}

static void
remove_ref_from_p2p_results (OstreeRepoFinderResult **results, OstreeCollectionRef *cr)
{
  int i;

  for (i = 0; results[i] != NULL; i++)
    g_hash_table_remove (results[i]->ref_to_checksum, cr);
}

typedef struct
{
  FlatpakDirResolve  *resolve; /* not owned */
  OstreeCollectionRef collection_ref; /* owns the collection_id member only */
  char               *local_commit;
} FlatpakDirResolveData;

static void
flatpak_dir_resolve_data_free (FlatpakDirResolveData *data)
{
  if (data == NULL)
    return;

  g_free (data->collection_ref.collection_id);
  g_free (data->local_commit);
  g_free (data);
}

static void
resolve_p2p_update_from_commit (FlatpakDirResolve *resolve,
                                GVariant *commit_data)
{
  g_autoptr(GVariant) commit_metadata = NULL;
  const char *xa_metadata = NULL;
  guint64 download_size = 0;
  guint64 installed_size = 0;

  commit_metadata = g_variant_get_child_value (commit_data, 0);
  g_variant_lookup (commit_metadata, "xa.metadata", "&s", &xa_metadata);
  if (xa_metadata == NULL)
    g_message ("Warning: No xa.metadata in commit %s ref %s", resolve->resolved_commit, resolve->ref);
  else
    resolve->resolved_metadata = g_bytes_new (xa_metadata, strlen (xa_metadata));

  if (g_variant_lookup (commit_metadata, "xa.download-size", "t", &download_size))
    resolve->download_size = GUINT64_FROM_BE (download_size);

  if (g_variant_lookup (commit_metadata, "xa.installed-size", "t", &installed_size))
    resolve->installed_size = GUINT64_FROM_BE (installed_size);
}

static gboolean
flatpak_dir_do_resolve_p2p_refs (FlatpakDir             *self,
                                 FlatpakDirResolveData **datas,
                                 gboolean                with_commit_ids,
                                 GCancellable           *cancellable,
                                 GError                **error)
{
  g_autoptr(GPtrArray) collection_refs_to_fetch = g_ptr_array_new ();
  g_autoptr(GPtrArray) commit_ids_to_fetch = NULL;
  g_autoptr(GAsyncResult) find_result = NULL;
  g_auto(OstreeRepoFinderResultv) results = NULL;
  g_autoptr(GVariant) find_options = NULL;
  g_auto(GVariantBuilder) find_builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;
  GVariantBuilder pull_builder;
  g_autoptr(GVariant) pull_options = NULL;
  g_autoptr(GAsyncResult) pull_result = NULL;
  g_autoptr(GMainContextPopDefault) main_context = NULL;
  g_autoptr(FlatpakRepoTransaction) transaction = NULL;
  g_autoptr(OstreeRepo) child_repo = NULL;
  g_auto(GLnxLockFile) child_repo_lock = { 0, };
  g_autoptr(GFile) user_cache_dir = NULL;
  g_autoptr(FlatpakTempDir) child_repo_tmp_dir = NULL;
  int i;

  main_context = flatpak_main_context_new_default ();

  if (with_commit_ids)
    commit_ids_to_fetch = g_ptr_array_new ();

  for (i = 0; datas[i] != NULL; i++)
    {
      FlatpakDirResolveData *data = datas[i];
      FlatpakDirResolve *resolve = data->resolve;

      g_ptr_array_add (collection_refs_to_fetch, &data->collection_ref);
      if (commit_ids_to_fetch)
        {
          g_assert (resolve->opt_commit != NULL);
          g_ptr_array_add (commit_ids_to_fetch, resolve->opt_commit);
        }
    }

  g_ptr_array_add (collection_refs_to_fetch, NULL);

  g_variant_builder_init (&find_builder, G_VARIANT_TYPE ("a{sv}"));
  if (commit_ids_to_fetch)
    g_variant_builder_add (&find_builder, "{s@v}", "override-commit-ids",
                           g_variant_new_variant (g_variant_new_strv ((const char * const *) commit_ids_to_fetch->pdata,
                                                                      commit_ids_to_fetch->len)));
  find_options = g_variant_ref_sink (g_variant_builder_end (&find_builder));

  /* We create a temporary child repo in the user homedir so that we can just blow it away when we're done.
   * This lets us always write to the directory in the system-helper case, but also lets us properly clean up
   * the transaction state directory, as that doesn't happen on abort.   */
  user_cache_dir = flatpak_ensure_user_cache_dir_location (error);
  if (user_cache_dir == NULL)
    return FALSE;

  child_repo = flatpak_dir_create_child_repo (self, user_cache_dir, &child_repo_lock, NULL, error);
  if (child_repo == NULL)
    return FALSE;

  /* Ensure we clean up the child repo */
  child_repo_tmp_dir = g_object_ref (ostree_repo_get_path (child_repo));

  ostree_repo_find_remotes_async (child_repo,
                                  (const OstreeCollectionRef * const *) collection_refs_to_fetch->pdata,
                                  find_options,
                                  NULL /* default finders */,
                                  NULL /* no progress reporting */,
                                  cancellable, async_result_cb, &find_result);

  while (find_result == NULL)
    g_main_context_iteration (main_context, TRUE);

  results = ostree_repo_find_remotes_finish (child_repo, find_result, error);
  if (results == NULL)
    return FALSE;

  /* Drop from the results all ops that are no-op updates */
  for (i = 0; datas[i] != NULL; i++)
    {
      FlatpakDirResolveData *data = datas[i];
      FlatpakDirResolve *resolve = data->resolve;
      const char *latest_rev = NULL;

      if (data->local_commit == NULL)
        continue;

      latest_rev = find_latest_p2p_result (results, &data->collection_ref);
      if (g_strcmp0 (latest_rev, data->local_commit) == 0)
        {
          g_autoptr(GVariant) commit_data = NULL;

          /* We already have the latest commit, so resolve it from
           * the local commit and remove from all results. This way we
           * avoid pulling this ref from all remotes. */

          if (!ostree_repo_load_commit (child_repo, data->local_commit, &commit_data, NULL, NULL))
            return FALSE;

          resolve->resolved_commit = g_strdup (data->local_commit);
          resolve_p2p_update_from_commit (resolve, commit_data);
          remove_ref_from_p2p_results (results, &data->collection_ref);
        }
    }

  g_variant_builder_init (&pull_builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&pull_builder, "{s@v}", "flags",
                         g_variant_new_variant (g_variant_new_int32 (OSTREE_REPO_PULL_FLAGS_COMMIT_ONLY)));
  g_variant_builder_add (&pull_builder, "{s@v}", "inherit-transaction",
                         g_variant_new_variant (g_variant_new_boolean (TRUE)));
  pull_options = g_variant_ref_sink (g_variant_builder_end (&pull_builder));

  transaction = flatpak_repo_transaction_start (child_repo, cancellable, error);
  if (transaction == NULL)
    return FALSE;

  ostree_repo_pull_from_remotes_async (child_repo, (const OstreeRepoFinderResult * const *) results,
                                       pull_options, NULL,
                                       cancellable, async_result_cb,
                                       &pull_result);

  while (pull_result == NULL)
    g_main_context_iteration (main_context, TRUE);

  if (!ostree_repo_pull_from_remotes_finish (child_repo, pull_result, error))
    return FALSE;

  for (i = 0; datas[i] != NULL; i++)
    {
      FlatpakDirResolveData *data = datas[i];
      FlatpakDirResolve *resolve = data->resolve;
      g_autoptr(GVariant) commit_data = NULL;
      g_autofree char *refspec = NULL;

      if (resolve->resolved_commit != NULL)
        continue;

      refspec = g_strdup_printf ("%s:%s", resolve->remote, resolve->ref);
      if (!ostree_repo_resolve_rev (child_repo, refspec, FALSE, &resolve->resolved_commit, error))
        return FALSE;

      if (!ostree_repo_load_commit (child_repo, resolve->resolved_commit, &commit_data, NULL, error))
        return FALSE;

      resolve_p2p_update_from_commit (resolve, commit_data);
    }

  return TRUE;
}

gboolean
flatpak_dir_resolve_p2p_refs (FlatpakDir         *self,
                              FlatpakDirResolve **resolves,
                              GCancellable       *cancellable,
                              GError            **error)
{
  g_autoptr(GPtrArray) latest_resolves = g_ptr_array_new_with_free_func ((GDestroyNotify) flatpak_dir_resolve_data_free);
  g_autoptr(GPtrArray) specific_resolves = g_ptr_array_new_with_free_func ((GDestroyNotify) flatpak_dir_resolve_data_free);
  int i;

  for (i = 0; resolves[i] != NULL; i++)
    {
      FlatpakDirResolve *resolve = resolves[i];
      FlatpakDirResolveData *data = g_new0 (FlatpakDirResolveData, 1);
      g_autofree char *refspec = g_strdup_printf ("%s:%s", resolve->remote, resolve->ref);

      g_assert (resolve->ref != NULL);
      g_assert (resolve->remote != NULL);

      data->resolve = resolve;
      data->collection_ref.ref_name = resolve->ref;
      data->collection_ref.collection_id = flatpak_dir_get_remote_collection_id (self, resolve->remote);

      g_assert (data->collection_ref.collection_id != NULL);

      if (resolve->opt_commit == NULL)
        ostree_repo_resolve_rev (self->repo, refspec, TRUE, &data->local_commit, NULL);

      /* The ostree p2p api doesn't let you mix pulls with specific commit IDs
       * and HEAD (https://github.com/ostreedev/ostree/issues/1622) so we need
       * to split these into two pull ops */
      if (resolve->opt_commit)
        g_ptr_array_add (specific_resolves, data);
      else
        g_ptr_array_add (latest_resolves, data);
    }

  if (specific_resolves->len > 0)
    {
      g_ptr_array_add (specific_resolves, NULL);
      if (!flatpak_dir_do_resolve_p2p_refs (self, (FlatpakDirResolveData **) specific_resolves->pdata, TRUE,
                                            cancellable, error))
        return FALSE;
    }

  if (latest_resolves->len > 0)
    {
      g_ptr_array_add (latest_resolves, NULL);
      if (!flatpak_dir_do_resolve_p2p_refs (self, (FlatpakDirResolveData **) latest_resolves->pdata, FALSE,
                                            cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
child_repo_ensure_summary (OstreeRepo         *child_repo,
                           FlatpakRemoteState *state,
                           GCancellable       *cancellable,
                           GError            **error)
{
  if (state->collection_id != NULL)
    {
      /* Regenerate the summary in the child repo because the summary copied
       * into the repo by flatpak_dir_pull() is reflective of the refs on the
       * remote that was pulled from, which might be a peer remote and might not
       * have the full set of refs that was pulled. It's also possible that
       * ostree didn't copy the remote summary into the repo at all if the
       * "branches" key is set in the remote config. See
       * https://github.com/ostreedev/ostree/issues/1461 */
      if (!ostree_repo_regenerate_summary (child_repo, NULL, cancellable, error))
        return FALSE;
    }
  else
    {
      if (!flatpak_remote_state_save_summary (state, ostree_repo_get_path (child_repo),
                                              cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
get_mtime (GFile        *file,
           GTimeVal     *result,
           GCancellable *cancellable)
{
  g_autoptr(GFileInfo) info = g_file_query_info (file,
                                                 G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                                 G_FILE_QUERY_INFO_NONE,
                                                 cancellable, NULL);
  if (info)
    {
      g_file_info_get_modification_time (info, result);
      return TRUE;
    }
  else
    {
      return FALSE;
    }
}

static gboolean
check_destination_mtime (GFile        *src,
                         GFile        *dest,
                         GCancellable *cancellable)
{
  GTimeVal src_mtime;
  GTimeVal dest_mtime;

  return get_mtime (src, &src_mtime, cancellable) &&
         get_mtime (dest, &dest_mtime, cancellable) &&
         (src_mtime.tv_sec < dest_mtime.tv_sec ||
          (src_mtime.tv_sec == dest_mtime.tv_sec && src_mtime.tv_usec < dest_mtime.tv_usec));
}

static GFile *
flatpak_dir_update_oci_index (FlatpakDir   *self,
                              const char   *remote,
                              char        **index_uri_out,
                              GCancellable *cancellable,
                              GError      **error)
{
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GFile) index_cache = NULL;
  g_autofree char *oci_uri = NULL;

  index_cache = flatpak_dir_get_oci_index_location (self, remote, error);
  if (index_cache == NULL)
    return NULL;

  ensure_soup_session (self);

  if (!ostree_repo_remote_get_url (self->repo,
                                   remote,
                                   &oci_uri,
                                   error))
    return NULL;

  if (!flatpak_oci_index_ensure_cached (self->soup_session, oci_uri,
                                        index_cache, index_uri_out,
                                        cancellable, &local_error))
    {
      if (!g_error_matches (local_error, FLATPAK_OCI_ERROR, FLATPAK_OCI_ERROR_NOT_CHANGED))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return NULL;
        }

      g_clear_error (&local_error);
    }

  return g_steal_pointer (&index_cache);
}

static gboolean
replace_contents_compressed (GFile        *dest,
                             GBytes       *contents,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GZlibCompressor) compressor = NULL;
  g_autoptr(GFileOutputStream) out = NULL;
  g_autoptr(GOutputStream) out2 = NULL;

  compressor = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1);
  out = g_file_replace (dest, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION,
                        NULL, error);
  out2 = g_converter_output_stream_new (G_OUTPUT_STREAM (out), G_CONVERTER (compressor));
  if (out == NULL)
    return FALSE;

  if (!g_output_stream_write_all (out2,
                                  g_bytes_get_data (contents, NULL),
                                  g_bytes_get_size (contents),
                                  NULL,
                                  cancellable, error))
    return FALSE;

  if (!g_output_stream_close (out2, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
flatpak_dir_update_appstream_oci (FlatpakDir          *self,
                                  const char          *remote,
                                  const char          *arch,
                                  gboolean            *out_changed,
                                  OstreeAsyncProgress *progress,
                                  GCancellable        *cancellable,
                                  GError             **error)
{
  g_autoptr(GFile) arch_dir = NULL;
  g_autoptr(GFile) lock_file = NULL;
  g_auto(GLnxLockFile) lock = { 0, };
  g_autoptr(GFile) index_cache = NULL;
  g_autofree char *index_uri = NULL;
  g_autoptr(GFile) timestamp_file = NULL;
  g_autoptr(GFile) icons_dir = NULL;
  glnx_autofd int icons_dfd = -1;
  g_autoptr(GBytes) appstream = NULL;
  g_autoptr(GFile) new_appstream_file = NULL;

  arch_dir = flatpak_build_file (flatpak_dir_get_path (self),
                                 "appstream", remote, arch, NULL);
  if (g_mkdir_with_parents (flatpak_file_get_path_cached (arch_dir), 0755) != 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  lock_file = g_file_get_child (arch_dir, "lock");
  if (!glnx_make_lock_file (AT_FDCWD, flatpak_file_get_path_cached (lock_file),
                            LOCK_EX, &lock, error))
    return FALSE;

  index_cache = flatpak_dir_update_oci_index (self, remote, &index_uri, cancellable, error);
  if (index_cache == NULL)
    return FALSE;

  timestamp_file = g_file_get_child (arch_dir, ".timestamp");
  if (check_destination_mtime (index_cache, timestamp_file, cancellable))
    return TRUE;

  icons_dir = g_file_get_child (arch_dir, "icons");
  if (g_mkdir_with_parents (flatpak_file_get_path_cached (icons_dir), 0755) != 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  if (!glnx_opendirat (AT_FDCWD, flatpak_file_get_path_cached (icons_dir),
                       FALSE, &icons_dfd, error))
    return FALSE;

  ensure_soup_session (self);

  appstream = flatpak_oci_index_make_appstream (self->soup_session,
                                                index_cache,
                                                index_uri,
                                                arch,
                                                icons_dfd,
                                                cancellable,
                                                error);
  if (appstream == NULL)
    return FALSE;

  new_appstream_file = g_file_get_child (arch_dir, "appstream.xml.gz");
  if (!replace_contents_compressed (new_appstream_file, appstream, cancellable, error))
    return FALSE;

  if (!g_file_replace_contents (timestamp_file, "", 0, NULL, FALSE,
                                G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, error))
    return FALSE;

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
  g_autofree char *new_branch = NULL;
  g_autofree char *old_branch = NULL;
  const char *used_branch = NULL;
  g_autofree char *remote_and_branch = NULL;
  g_autofree char *new_checksum = NULL;

  g_autoptr(GError) first_error = NULL;
  g_autoptr(GError) second_error = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;
  const char *installation;
  gboolean is_oci;

  if (out_changed)
    *out_changed = FALSE;

  if (arch == NULL)
    arch = flatpak_get_arch ();

  new_branch = g_strdup_printf ("appstream2/%s", arch);
  old_branch = g_strdup_printf ("appstream/%s", arch);

  is_oci = flatpak_dir_get_remote_oci (self, remote);

  state = flatpak_dir_get_remote_state_optional (self, remote, cancellable, error);
  if (state == NULL)
    return FALSE;

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      g_auto(GLnxLockFile) child_repo_lock = { 0, };
      g_autofree char *url = NULL;
      g_autoptr(GFile) child_repo_file = NULL;
      g_autofree char *child_repo_path = NULL;
      gboolean gpg_verify_summary;
      gboolean gpg_verify;

      if (!ostree_repo_remote_get_url (self->repo,
                                       state->remote_name,
                                       &url,
                                       error))
        return FALSE;

      if (!ostree_repo_remote_get_gpg_verify_summary (self->repo, state->remote_name,
                                                      &gpg_verify_summary, error))
        return FALSE;

      if (!ostree_repo_remote_get_gpg_verify (self->repo, state->remote_name,
                                              &gpg_verify, error))
        return FALSE;

      if (is_oci)
        {
          /* In the OCI case, we just ask the system helper do the network i/o, since
           * there is no way to verify the index validity without actually downloading it.
           * While we try to avoid network i/o as root, there's no hard line where doing
           * network i/o as root is much worse than parsing the results of network i/o
           * as root. A trusted, but unprivileged helper could be used to do the download
           * if necessary.
           */
        }
      else if ((!gpg_verify_summary && state->collection_id == NULL) || !gpg_verify)
        {
          /* The remote is not gpg verified, so we don't want to allow installation via
             a download in the home directory, as there is no way to verify you're not
             injecting anything into the remote. However, in the case of a remote
             configured to a local filesystem we can just let the system helper do
             the installation, as it can then avoid network i/o and be certain the
             data comes from the right place.

             If @collection_id is non-%NULL, we can verify the refs in commit
             metadata, so don’t need to verify the summary. */
          if (!g_str_has_prefix (url, "file:"))
            return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("Can't pull from untrusted non-gpg verified remote"));
        }
      else
        {
          g_autoptr(OstreeRepo) child_repo = flatpak_dir_create_system_child_repo (self, &child_repo_lock, NULL, error);
          if (child_repo == NULL)
            return FALSE;

          /* No need to use an existing OstreeRepoFinderResult array, since
           * appstream updates do not need to be atomic wrt other updates. */
          used_branch = new_branch;
          if (!flatpak_dir_pull (self, state, used_branch, NULL, NULL, NULL, NULL, NULL,
                                 child_repo, FLATPAK_PULL_FLAGS_NONE, OSTREE_REPO_PULL_FLAGS_MIRROR,
                                 progress, cancellable, &first_error))
            {
              used_branch = old_branch;
              if (!flatpak_dir_pull (self, state, used_branch, NULL, NULL, NULL, NULL, NULL,
                                     child_repo, FLATPAK_PULL_FLAGS_NONE, OSTREE_REPO_PULL_FLAGS_MIRROR,
                                     progress, cancellable, &second_error))
                {
                  g_prefix_error (&first_error, "Error updating appstream2: ");
                  g_prefix_error (&second_error, "Error updating appstream: ");
                  g_propagate_prefixed_error (error, g_steal_pointer (&second_error), "%s; ", first_error->message);
                  return FALSE;
                }
            }

          if (!child_repo_ensure_summary (child_repo, state, cancellable, error))
            return FALSE;

          remote_and_branch = g_strdup_printf ("%s:%s", remote, used_branch);

          if (!ostree_repo_resolve_rev (child_repo, remote_and_branch, TRUE, &new_checksum, error))
            return FALSE;

          child_repo_file = g_object_ref (ostree_repo_get_path (child_repo));
        }

      if (child_repo_file)
        child_repo_path = g_file_get_path (child_repo_file);

      installation = flatpak_dir_get_id (self);

      if (!flatpak_dir_system_helper_call_deploy_appstream (self,
                                                            child_repo_path ? child_repo_path : "",
                                                            FLATPAK_HELPER_DEPLOY_APPSTREAM_FLAGS_NONE,
                                                            remote,
                                                            arch,
                                                            installation ? installation : "",
                                                            cancellable,
                                                            error))
        return FALSE;

      if (child_repo_file)
        (void) flatpak_rm_rf (child_repo_file, NULL, NULL);

      return TRUE;
    }

  if (is_oci)
    {
      return flatpak_dir_update_appstream_oci (self, remote, arch,
                                               out_changed, progress, cancellable,
                                               error);
    }

  /* No need to use an existing OstreeRepoFinderResult array, since
   * appstream updates do not need to be atomic wrt other updates. */
  used_branch = new_branch;
  if (!flatpak_dir_pull (self, state, used_branch, NULL, NULL, NULL, NULL, NULL, NULL,
                         FLATPAK_PULL_FLAGS_NONE, OSTREE_REPO_PULL_FLAGS_NONE, progress,
                         cancellable, &first_error))
    {
      used_branch = old_branch;
      if (!flatpak_dir_pull (self, state, used_branch, NULL, NULL, NULL, NULL, NULL, NULL,
                             FLATPAK_PULL_FLAGS_NONE, OSTREE_REPO_PULL_FLAGS_NONE, progress,
                             cancellable, &second_error))
        {
          g_prefix_error (&first_error, "Error updating appstream2: ");
          g_prefix_error (&second_error, "Error updating appstream: ");
          g_propagate_prefixed_error (error, g_steal_pointer (&second_error), "%s; ", first_error->message);
          return FALSE;
        }
    }

  remote_and_branch = g_strdup_printf ("%s:%s", remote, used_branch);

  if (!ostree_repo_resolve_rev (self->repo, remote_and_branch, TRUE, &new_checksum, error))
    return FALSE;

  return flatpak_dir_deploy_appstream (self,
                                       remote,
                                       arch,
                                       out_changed,
                                       cancellable,
                                       error);
}

/* Get the configured collection-id for @remote_name, squashing empty strings into
 * %NULL. Return %TRUE if the ID was fetched successfully, or if it was unset or
 * empty. */
static gboolean
repo_get_remote_collection_id (OstreeRepo *repo,
                               const char *remote_name,
                               char      **collection_id_out,
                               GError    **error)
{
  if (collection_id_out != NULL)
    {
      if (!ostree_repo_get_remote_option (repo, remote_name, "collection-id",
                                          NULL, collection_id_out, error))
        return FALSE;
      if (*collection_id_out != NULL && **collection_id_out == '\0')
        g_clear_pointer (collection_id_out, g_free);
    }

  return TRUE;
}

/* Get options for the OSTree pull operation which can be shared between
 * collection-based and normal pulls. Update @builder in place. */
static void
get_common_pull_options (GVariantBuilder     *builder,
                         const char          *ref_to_fetch,
                         const gchar * const *dirs_to_pull,
                         const char          *current_local_checksum,
                         gboolean             force_disable_deltas,
                         OstreeRepoPullFlags  flags,
                         OstreeAsyncProgress *progress)
{
  guint32 update_freq = 0;
  GVariantBuilder hdr_builder;

  if (dirs_to_pull)
    {
      g_variant_builder_add (builder, "{s@v}", "subdirs",
                             g_variant_new_variant (g_variant_new_strv ((const char * const *) dirs_to_pull, -1)));
      force_disable_deltas = TRUE;
    }

  if (force_disable_deltas)
    {
      g_variant_builder_add (builder, "{s@v}", "disable-static-deltas",
                             g_variant_new_variant (g_variant_new_boolean (TRUE)));
    }

  g_variant_builder_add (builder, "{s@v}", "inherit-transaction",
                         g_variant_new_variant (g_variant_new_boolean (TRUE)));

  g_variant_builder_add (builder, "{s@v}", "flags",
                         g_variant_new_variant (g_variant_new_int32 (flags)));


  g_variant_builder_init (&hdr_builder, G_VARIANT_TYPE ("a(ss)"));
  g_variant_builder_add (&hdr_builder, "(ss)", "Flatpak-Ref", ref_to_fetch);
  if (current_local_checksum)
    g_variant_builder_add (&hdr_builder, "(ss)", "Flatpak-Upgrade-From", current_local_checksum);
  g_variant_builder_add (builder, "{s@v}", "http-headers",
                         g_variant_new_variant (g_variant_builder_end (&hdr_builder)));
  g_variant_builder_add (builder, "{s@v}", "append-user-agent",
                         g_variant_new_variant (g_variant_new_string ("flatpak/" PACKAGE_VERSION)));

  if (progress != NULL)
    update_freq = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (progress), "update-frequency"));
  if (update_freq == 0)
    update_freq = FLATPAK_DEFAULT_UPDATE_FREQUENCY;

  g_variant_builder_add (builder, "{s@v}", "update-frequency",
                         g_variant_new_variant (g_variant_new_uint32 (update_freq)));
}

static gboolean
translate_ostree_repo_pull_errors (GError **error)
{
  if (*error)
    {
      if (strstr ((*error)->message, "min-free-space-size") ||
          strstr ((*error)->message, "min-free-space-percent"))
        {
          (*error)->domain = FLATPAK_ERROR;
          (*error)->code = FLATPAK_ERROR_OUT_OF_SPACE;
        }
    }

  return FALSE;
}

static gboolean
repo_pull (OstreeRepo                           *self,
           const char                           *remote_name,
           const char                          **dirs_to_pull,
           const char                           *ref_to_fetch,
           const char                           *rev_to_fetch,
           const OstreeRepoFinderResult * const *results_to_fetch,
           FlatpakPullFlags                      flatpak_flags,
           OstreeRepoPullFlags                   flags,
           OstreeAsyncProgress                  *progress,
           GCancellable                         *cancellable,
           GError                              **error)
{
  gboolean force_disable_deltas = (flatpak_flags & FLATPAK_PULL_FLAGS_NO_STATIC_DELTAS) != 0;
  g_autofree char *remote_and_branch = NULL;
  g_autofree char *current_checksum = NULL;

  g_autoptr(GVariant) old_commit = NULL;
  g_autoptr(GVariant) new_commit = NULL;
  const char *revs_to_fetch[2];
  gboolean res = FALSE;
  g_autofree gchar *collection_id = NULL;
  g_autoptr(GError) dummy_error = NULL;

  /* The ostree fetcher asserts if error is NULL */
  if (error == NULL)
    error = &dummy_error;

  /* If @results_to_fetch is set, @rev_to_fetch must be. */
  g_assert (results_to_fetch == NULL || rev_to_fetch != NULL);

  /* We always want this on for every type of pull */
  flags |= OSTREE_REPO_PULL_FLAGS_BAREUSERONLY_FILES;

  remote_and_branch = g_strdup_printf ("%s:%s", remote_name, ref_to_fetch);
  if (!ostree_repo_resolve_rev (self, remote_and_branch, TRUE, &current_checksum, error))
    return FALSE;

  if (current_checksum != NULL &&
      !ostree_repo_load_commit (self, current_checksum, &old_commit, NULL, error))
    return FALSE;

  if (!repo_get_remote_collection_id (self, remote_name, &collection_id, NULL))
    g_clear_pointer (&collection_id, g_free);

  if (collection_id != NULL)
    {
      GVariantBuilder find_builder, pull_builder;
      g_autoptr(GVariant) find_options = NULL, pull_options = NULL;
      g_autoptr(GAsyncResult) find_result = NULL, pull_result = NULL;
      g_auto(OstreeRepoFinderResultv) results = NULL;
      OstreeCollectionRef collection_ref;
      OstreeCollectionRef *collection_refs_to_fetch[2];
      guint32 update_freq = 0;
      g_autoptr(GMainContextPopDefault) context = NULL;

      /* Find options */
      g_variant_builder_init (&find_builder, G_VARIANT_TYPE ("a{sv}"));

      if (force_disable_deltas)
        {
          g_variant_builder_add (&find_builder, "{s@v}", "disable-static-deltas",
                                 g_variant_new_variant (g_variant_new_boolean (TRUE)));
        }

      collection_ref.collection_id = collection_id;
      collection_ref.ref_name = (char *) ref_to_fetch;

      collection_refs_to_fetch[0] = &collection_ref;
      collection_refs_to_fetch[1] = NULL;

      if (progress != NULL)
        update_freq = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (progress), "update-frequency"));
      if (update_freq == 0)
        update_freq = FLATPAK_DEFAULT_UPDATE_FREQUENCY;

      g_variant_builder_add (&find_builder, "{s@v}", "update-frequency",
                             g_variant_new_variant (g_variant_new_uint32 (update_freq)));

      if (rev_to_fetch != NULL)
        {
          g_variant_builder_add (&find_builder, "{s@v}", "override-commit-ids",
                                 g_variant_new_variant (g_variant_new_strv (&rev_to_fetch, 1)));
        }

      find_options = g_variant_ref_sink (g_variant_builder_end (&find_builder));

      /* Pull options */
      g_variant_builder_init (&pull_builder, G_VARIANT_TYPE ("a{sv}"));
      get_common_pull_options (&pull_builder, ref_to_fetch, dirs_to_pull, current_checksum,
                               force_disable_deltas, flags, progress);
      pull_options = g_variant_ref_sink (g_variant_builder_end (&pull_builder));

      context = flatpak_main_context_new_default ();

      if (results_to_fetch == NULL)
        {
          ostree_repo_find_remotes_async (self, (const OstreeCollectionRef * const *) collection_refs_to_fetch,
                                          find_options,
                                          NULL /* default finders */, progress, cancellable,
                                          async_result_cb, &find_result);

          while (find_result == NULL)
            g_main_context_iteration (context, TRUE);

          results = ostree_repo_find_remotes_finish (self, find_result, error);
          results_to_fetch = (const OstreeRepoFinderResult * const *) results;
        }

      if (results_to_fetch != NULL)
        {
          ostree_repo_pull_from_remotes_async (self, results_to_fetch,
                                               pull_options, progress,
                                               cancellable, async_result_cb,
                                               &pull_result);

          while (pull_result == NULL)
            g_main_context_iteration (context, TRUE);

          res = ostree_repo_pull_from_remotes_finish (self, pull_result, error);
        }
      else
        res = FALSE;
    }
  else
    res = FALSE;

  if (!res)
    {
      if (error != NULL && *error != NULL)
        g_debug ("Failed to pull using find-remotes; falling back to normal pull: %s", (*error)->message);
      g_clear_error (error);
    }

  if (!res)
    {
      GVariantBuilder builder;
      g_autoptr(GVariant) options = NULL;
      const char *refs_to_fetch[2];

      /* Pull options */
      g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
      get_common_pull_options (&builder, ref_to_fetch, dirs_to_pull, current_checksum,
                               force_disable_deltas, flags, progress);

      refs_to_fetch[0] = ref_to_fetch;
      refs_to_fetch[1] = NULL;
      g_variant_builder_add (&builder, "{s@v}", "refs",
                             g_variant_new_variant (g_variant_new_strv ((const char * const *) refs_to_fetch, -1)));

      revs_to_fetch[0] = rev_to_fetch;
      revs_to_fetch[1] = NULL;
      g_variant_builder_add (&builder, "{s@v}", "override-commit-ids",
                             g_variant_new_variant (g_variant_new_strv ((const char * const *) revs_to_fetch, -1)));

      options = g_variant_ref_sink (g_variant_builder_end (&builder));

      if (!ostree_repo_pull_with_options (self, remote_name, options,
                                          progress, cancellable, error))
        return translate_ostree_repo_pull_errors (error);
    }

  if (old_commit &&
      (flatpak_flags & FLATPAK_PULL_FLAGS_ALLOW_DOWNGRADE) == 0)
    {
      guint64 old_timestamp;
      guint64 new_timestamp;

      if (!ostree_repo_load_commit (self, rev_to_fetch, &new_commit, NULL, error))
        return FALSE;

      old_timestamp = ostree_commit_get_timestamp (old_commit);
      new_timestamp = ostree_commit_get_timestamp (new_commit);

      if (new_timestamp < old_timestamp)
        return flatpak_fail_error (error, FLATPAK_ERROR_DOWNGRADE, "Update is older than current version");
    }

  return TRUE;
}

static void
ensure_soup_session (FlatpakDir *self)
{
  if (g_once_init_enter (&self->soup_session))
    {
      SoupSession *soup_session;

      soup_session = flatpak_create_soup_session (PACKAGE_STRING);

      if (g_getenv ("OSTREE_DEBUG_HTTP"))
        soup_session_add_feature (soup_session, (SoupSessionFeature *) soup_logger_new (SOUP_LOGGER_LOG_BODY, 500));

      g_once_init_leave (&self->soup_session, soup_session);
    }
}

typedef struct
{
  OstreeAsyncProgress *progress;
  guint64              previous_dl;
} ExtraDataProgress;

static void
extra_data_progress_report (guint64  downloaded_bytes,
                            gpointer user_data)
{
  ExtraDataProgress *extra_progress = user_data;

  if (extra_progress->progress)
    ostree_async_progress_set_uint64 (extra_progress->progress, "transferred-extra-data-bytes",
                                      extra_progress->previous_dl + downloaded_bytes);

}

static gboolean
flatpak_dir_setup_extra_data (FlatpakDir                           *self,
                              OstreeRepo                           *repo,
                              const char                           *repository,
                              const char                           *ref,
                              const char                           *rev,
                              const OstreeRepoFinderResult * const *results,
                              FlatpakPullFlags                      flatpak_flags,
                              OstreeAsyncProgress                  *progress,
                              GCancellable                         *cancellable,
                              GError                              **error)
{
  g_autoptr(GVariant) extra_data_sources = NULL;
  int i;
  gsize n_extra_data;
  guint64 total_download_size;

  /* If @results is set, @rev must be. */
  g_assert (results == NULL || rev != NULL);

  extra_data_sources = flatpak_repo_get_extra_data_sources (repo, rev, cancellable, NULL);
  if (extra_data_sources == NULL)
    {
      /* Pull the commits (and only the commits) to check for extra data
       * again. Here we don't pass the progress because we don't want any
       * reports coming out of it. */
      if (!repo_pull (repo, repository,
                      NULL,
                      ref,
                      rev,
                      results,
                      flatpak_flags,
                      OSTREE_REPO_PULL_FLAGS_COMMIT_ONLY,
                      NULL,
                      cancellable,
                      error))
        return FALSE;

      extra_data_sources = flatpak_repo_get_extra_data_sources (repo, rev, cancellable, NULL);
    }

  n_extra_data = 0;
  total_download_size = 0;

  if (extra_data_sources != NULL)
    n_extra_data = g_variant_n_children (extra_data_sources);

  if (n_extra_data > 0)
    {
      if ((flatpak_flags & FLATPAK_PULL_FLAGS_DOWNLOAD_EXTRA_DATA) == 0)
        return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("Extra data not supported for non-gpg-verified local system installs"));

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
    }

  if (progress)
    {
      ostree_async_progress_set (progress,
                                 "outstanding-extra-data", "u", n_extra_data,
                                 "total-extra-data", "u", n_extra_data,
                                 "total-extra-data-bytes", "t", total_download_size,
                                 "transferred-extra-data-bytes", "t", (guint64) 0,
                                 "downloading-extra-data", "u", 0,
                                 NULL);
    }

  return TRUE;
}

static inline void
reset_async_progress_extra_data (OstreeAsyncProgress *progress)
{
  if (progress)
    ostree_async_progress_set_uint (progress, "downloading-extra-data", 0);
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
  g_autoptr(GFile) base_dir = NULL;
  int i;
  gsize n_extra_data;
  ExtraDataProgress extra_data_progress = { NULL };

  extra_data_sources = flatpak_repo_get_extra_data_sources (repo, rev, cancellable, NULL);
  if (extra_data_sources == NULL)
    return TRUE;

  n_extra_data = g_variant_n_children (extra_data_sources);
  if (n_extra_data == 0)
    return TRUE;

  if ((flatpak_flags & FLATPAK_PULL_FLAGS_DOWNLOAD_EXTRA_DATA) == 0)
    return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("Extra data not supported for non-gpg-verified local system installs"));

  extra_data_builder = g_variant_builder_new (G_VARIANT_TYPE ("a(ayay)"));

  /* Other fields were already set in flatpak_dir_setup_extra_data() */
  if (progress)
    {
      ostree_async_progress_set (progress,
                                 "start-time-extra-data", "t", g_get_monotonic_time (),
                                 "downloading-extra-data", "u", 1,
                                 NULL);
    }

  extra_data_progress.progress = progress;

  base_dir = flatpak_get_user_base_dir_location ();

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
      g_autoptr(GFile) extra_local_file = NULL;

      flatpak_repo_parse_extra_data_sources (extra_data_sources, i,
                                             &extra_data_name,
                                             &download_size,
                                             &installed_size,
                                             &sha256_bytes,
                                             &extra_data_uri);

      if (sha256_bytes == NULL)
        return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid checksum for extra data uri %s"), extra_data_uri);

      extra_data_sha256 = ostree_checksum_from_bytes (sha256_bytes);

      if (*extra_data_name == 0)
        return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Empty name for extra data uri %s"), extra_data_uri);

      /* Don't allow file uris here as that could read local files based on remote data */
      if (!g_str_has_prefix (extra_data_uri, "http:") &&
          !g_str_has_prefix (extra_data_uri, "https:"))
        {
          reset_async_progress_extra_data (progress);
          return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Unsupported extra data uri %s"), extra_data_uri);
        }

      /* TODO: Download to disk to support resumed downloads on error */

      extra_local_file = flatpak_build_file (base_dir, "extra-data", extra_data_sha256, extra_data_name, NULL);
      if (g_file_query_exists (extra_local_file, cancellable))
        {
          g_debug ("Loading extra-data from local file %s", flatpak_file_get_path_cached (extra_local_file));
          gsize extra_local_size;
          g_autofree char *extra_local_contents = NULL;
          g_autoptr(GError) my_error = NULL;

          if (!g_file_load_contents (extra_local_file, cancellable, &extra_local_contents, &extra_local_size, NULL, &my_error))
            return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Failed to load local extra-data %s: %s"),
                                 flatpak_file_get_path_cached (extra_local_file), my_error->message);
          if (extra_local_size != download_size)
            return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Wrong size for extra-data %s"), flatpak_file_get_path_cached (extra_local_file));

          bytes = g_bytes_new (extra_local_contents, extra_local_size);
        }
      else
        {
          ensure_soup_session (self);
          bytes = flatpak_load_http_uri (self->soup_session, extra_data_uri, 0,
                                         extra_data_progress_report, &extra_data_progress,
                                         cancellable, error);
        }

      if (bytes == NULL)
        {
          reset_async_progress_extra_data (progress);
          g_prefix_error (error, _("While downloading %s: "), extra_data_uri);
          return FALSE;
        }

      if (g_bytes_get_size (bytes) != download_size)
        {
          reset_async_progress_extra_data (progress);
          return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Wrong size for extra data %s"), extra_data_uri);
        }

      extra_data_progress.previous_dl += download_size;
      if (progress)
        ostree_async_progress_set_uint (progress, "outstanding-extra-data", n_extra_data - i - 1);

      sha256 = g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, bytes);
      if (strcmp (sha256, extra_data_sha256) != 0)
        {
          reset_async_progress_extra_data (progress);
          return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid checksum for extra data %s"), extra_data_uri);
        }

      g_variant_builder_add (extra_data_builder,
                             "(^ay@ay)",
                             extra_data_name,
                             g_variant_new_from_bytes (G_VARIANT_TYPE ("ay"), bytes, TRUE));
    }

  extra_data = g_variant_ref_sink (g_variant_builder_end (extra_data_builder));

  reset_async_progress_extra_data (progress);

  if (!ostree_repo_read_commit_detached_metadata (repo, rev, &detached_metadata,
                                                  cancellable, error))
    return FALSE;

  g_variant_dict_init (&new_metadata_dict, detached_metadata);
  g_variant_dict_insert_value (&new_metadata_dict, "xa.extra-data", extra_data);
  new_detached_metadata = g_variant_ref_sink (g_variant_dict_end (&new_metadata_dict));

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

static char *
lookup_oci_registry_uri_from_summary (GVariant *summary,
                                      GError  **error)
{
  g_autoptr(GVariant) extensions = g_variant_get_child_value (summary, 1);
  g_autofree char *registry_uri = NULL;

  if (!g_variant_lookup (extensions, "xa.oci-registry-uri", "s", &registry_uri))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Remote OCI index has no registry uri"));
      return NULL;
    }

  return g_steal_pointer (&registry_uri);
}

static void
oci_pull_init_progress (OstreeAsyncProgress *progress)
{
  guint64 start_time = g_get_monotonic_time () - 2;

  if (progress == NULL)
    return;

  ostree_async_progress_set (progress,
                             "outstanding-fetches", "u", 0,
                             "outstanding-writes", "u", 0,
                             "fetched", "u", 0,
                             "requested", "u", 0,
                             "scanning", "u", 0,
                             "scanned-metadata", "u", 0,
                             "bytes-transferred", "t", (guint64) 0,
                             "start-time", "t", start_time,
                             "outstanding-metadata-fetches", "u", 0,
                             "metadata-fetched", "u", 0,
                             "outstanding-extra-data", "u", 0,
                             "total-extra-data", "u", 0,
                             "total-extra-data-bytes", "t", (guint64) 0,
                             "transferred-extra-data-bytes", "t", (guint64) 0,
                             "downloading-extra-data", "u", 0,
                             "fetched-delta-parts", "u", 0,
                             "total-delta-parts", "u", 0,
                             "fetched-delta-fallbacks", "u", 0,
                             "total-delta-fallbacks", "u", 0,
                             "fetched-delta-part-size", "t", (guint64) 0,
                             "total-delta-part-size", "t", (guint64) 0,
                             "total-delta-part-usize", "t", (guint64) 0,
                             "total-delta-superblocks", "u", 0,
                             "status", "s", "",
                             "caught-error", "b", FALSE,
                             NULL);
}

static void
oci_pull_progress_cb (guint64 total_size, guint64 pulled_size,
                      guint32 n_layers, guint32 pulled_layers,
                      gpointer data)
{
  OstreeAsyncProgress *progress = data;

  if (progress == NULL)
    return;

  /* Deltas */
  ostree_async_progress_set (progress,
                             "outstanding-fetches", "u", n_layers - pulled_layers,
                             "fetched-delta-parts", "u", pulled_layers,
                             "total-delta-parts", "u", n_layers,
                             "fetched-delta-fallbacks", "u", 0,
                             "total-delta-fallbacks", "u", 0,
                             "bytes-transferred", "t", pulled_size,
                             "total-delta-part-size", "t", total_size,
                             "total-delta-part-usize", "t", total_size,
                             "total-delta-superblocks", "u", 0,
                             NULL);
}

static gboolean
flatpak_dir_mirror_oci (FlatpakDir          *self,
                        FlatpakOciRegistry  *dst_registry,
                        FlatpakRemoteState  *state,
                        const char          *ref,
                        const char          *skip_if_current_is,
                        OstreeAsyncProgress *progress,
                        GCancellable        *cancellable,
                        GError             **error)
{
  g_autoptr(FlatpakOciRegistry) registry = NULL;
  g_autofree char *registry_uri = NULL;
  g_autofree char *oci_digest = NULL;
  g_autofree char *latest_rev = NULL;
  g_autoptr(GVariant) summary_element = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *oci_repository = NULL;
  gboolean res;

  /* We use the summary so that we can reuse any cached json */
  flatpak_remote_state_lookup_ref (state, ref, &latest_rev, &summary_element, error);
  if (latest_rev == NULL)
    {
      if (error != NULL && *error == NULL)
        flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Couldn't find latest checksum for ref %s in remote %s"),
                      ref, state->remote_name);
      return FALSE;
    }

  if (skip_if_current_is != NULL && strcmp (latest_rev, skip_if_current_is) == 0)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_ALREADY_INSTALLED, _("%s commit %s already installed"),
                          ref, latest_rev);
      return FALSE;
    }

  metadata = g_variant_get_child_value (summary_element, 2);
  g_variant_lookup (metadata, "xa.oci-repository", "s", &oci_repository);

  oci_digest = g_strconcat ("sha256:", latest_rev, NULL);

  registry_uri = lookup_oci_registry_uri_from_summary (state->summary, error);
  if (registry_uri == NULL)
    return FALSE;

  registry = flatpak_oci_registry_new (registry_uri, FALSE, -1, NULL, error);
  if (registry == NULL)
    return FALSE;

  g_assert (progress != NULL);
  oci_pull_init_progress (progress);

  g_debug ("Mirroring OCI image %s", oci_digest);

  res = flatpak_mirror_image_from_oci (dst_registry, registry, oci_repository, oci_digest, oci_pull_progress_cb,
                                       progress, cancellable, error);

  if (progress)
    ostree_async_progress_finish (progress);

  if (!res)
    return FALSE;

  return TRUE;
}

static gboolean
flatpak_dir_pull_oci (FlatpakDir          *self,
                      FlatpakRemoteState  *state,
                      const char          *ref,
                      OstreeRepo          *repo,
                      FlatpakPullFlags     flatpak_flags,
                      OstreeRepoPullFlags  flags,
                      OstreeAsyncProgress *progress,
                      GCancellable        *cancellable,
                      GError             **error)
{
  g_autoptr(FlatpakOciRegistry) registry = NULL;
  g_autoptr(FlatpakOciVersioned) versioned = NULL;
  g_autofree char *full_ref = NULL;
  g_autofree char *registry_uri = NULL;
  g_autofree char *oci_repository = NULL;
  g_autofree char *oci_digest = NULL;
  g_autofree char *checksum = NULL;
  g_autoptr(GVariant) summary_element = NULL;
  g_autofree char *latest_alt_commit = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *latest_rev = NULL;
  G_GNUC_UNUSED g_autofree char *latest_commit =
    flatpak_dir_read_latest (self, state->remote_name, ref, &latest_alt_commit, cancellable, NULL);
  g_autofree char *name = NULL;

  /* We use the summary so that we can reuse any cached json */
  flatpak_remote_state_lookup_ref (state, ref, &latest_rev, &summary_element, error);
  if (latest_rev == NULL)
    {
      if (error != NULL && *error == NULL)
        flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Couldn't find latest checksum for ref %s in remote %s"),
                      ref, state->remote_name);
      return FALSE;
    }

  metadata = g_variant_get_child_value (summary_element, 2);
  g_variant_lookup (metadata, "xa.oci-repository", "s", &oci_repository);

  oci_digest = g_strconcat ("sha256:", latest_rev, NULL);

  /* Short circuit if we've already got this commit */
  if (latest_alt_commit != NULL && strcmp (oci_digest + strlen ("sha256:"), latest_alt_commit) == 0)
    return TRUE;

  registry_uri = lookup_oci_registry_uri_from_summary (state->summary, error);
  if (registry_uri == NULL)
    return FALSE;

  registry = flatpak_oci_registry_new (registry_uri, FALSE, -1, NULL, error);
  if (registry == NULL)
    return FALSE;

  versioned = flatpak_oci_registry_load_versioned (registry, oci_repository, oci_digest,
                                                   NULL, cancellable, error);
  if (versioned == NULL)
    return FALSE;

  if (!FLATPAK_IS_OCI_MANIFEST (versioned))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Image is not a manifest"));

  full_ref = g_strdup_printf ("%s:%s", state->remote_name, ref);

  if (repo == NULL)
    repo = self->repo;

  g_assert (progress != NULL);
  oci_pull_init_progress (progress);

  g_debug ("Pulling OCI image %s", oci_digest);

  checksum = flatpak_pull_from_oci (repo, registry, oci_repository, oci_digest, FLATPAK_OCI_MANIFEST (versioned),
                                    state->remote_name, ref, oci_pull_progress_cb, progress, cancellable, error);

  if (progress)
    ostree_async_progress_finish (progress);

  if (checksum == NULL)
    return FALSE;

  g_debug ("Imported OCI image as checksum %s", checksum);

  if (repo == self->repo)
    name = flatpak_dir_get_name (self);
  else
    {
      GFile *file = ostree_repo_get_path (repo);
      name = g_file_get_path (file);
    }

  (flatpak_dir_log) (self, __FILE__, __LINE__, __FUNCTION__, name,
                     "pull oci", registry_uri, ref, NULL, NULL, NULL,
                      "Pulled %s from %s", ref, registry_uri);

  return TRUE;
}

gboolean
flatpak_dir_pull (FlatpakDir                           *self,
                  FlatpakRemoteState                   *state,
                  const char                           *ref,
                  const char                           *opt_rev,
                  const OstreeRepoFinderResult * const *opt_results,
                  const char                          **subpaths,
                  GBytes                               *require_metadata,
                  const char                           *token,
                  OstreeRepo                           *repo,
                  FlatpakPullFlags                      flatpak_flags,
                  OstreeRepoPullFlags                   flags,
                  OstreeAsyncProgress                  *progress,
                  GCancellable                         *cancellable,
                  GError                              **error)
{
  gboolean ret = FALSE;
  g_autofree char *rev = NULL;
  g_autofree char *url = NULL;

  g_autoptr(GPtrArray) subdirs_arg = NULL;
  g_auto(OstreeRepoFinderResultv) allocated_results = NULL;
  const OstreeRepoFinderResult * const *results;
  g_auto(GLnxLockFile) lock = { 0, };
  g_autofree char *name = NULL;
  g_autofree char *remote_and_branch = NULL;
  g_autofree char *current_checksum = NULL;

  /* If @opt_results is set, @opt_rev must be. */
  g_return_val_if_fail (opt_results == NULL || opt_rev != NULL, FALSE);

  /* This is not actually used for anything, so must be NULL in
   * this branch. */
  g_return_val_if_fail (token == NULL, FALSE);

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  /* Keep a shared repo lock to avoid prunes removing objects we're relying on
   * while we do the pull. There are two cases we protect against. 1) objects we
   * need but that we already decided are locally available could be removed,
   * and 2) during the transaction commit objects that don't yet have a ref to
   * them could be considered unreachable.
   */
  if (repo == NULL && !flatpak_dir_repo_lock (self, &lock, LOCK_SH, cancellable, error))
    return FALSE;

  if (flatpak_dir_get_remote_oci (self, state->remote_name))
    return flatpak_dir_pull_oci (self, state, ref, repo, flatpak_flags,
                                 flags, progress, cancellable, error);

  if (!ostree_repo_remote_get_url (self->repo,
                                   state->remote_name,
                                   &url,
                                   error))
    return FALSE;

  if (*url == 0)
    return TRUE; /* Empty url, silently disables updates */

  g_assert (progress != NULL);

  /* We get the rev ahead of time so that we know it for looking up e.g. extra-data
     and to make sure we're atomically using a single rev if we happen to do multiple
     pulls (e.g. with subpaths) */
  if (opt_rev != NULL)
    {
      rev = g_strdup (opt_rev);
      results = opt_results;
    }
  else
    {
      if (state->collection_id)
        {
          GVariantBuilder find_builder;
          g_autoptr(GVariant) find_options = NULL;
          g_autoptr(GAsyncResult) find_result = NULL;
          OstreeCollectionRef collection_ref;
          OstreeCollectionRef *collection_refs_to_fetch[2];
          gboolean force_disable_deltas = (flatpak_flags & FLATPAK_PULL_FLAGS_NO_STATIC_DELTAS) != 0;
          guint update_freq = 0;
          gsize i;
          g_autoptr(GMainContextPopDefault) context = NULL;

          g_variant_builder_init (&find_builder, G_VARIANT_TYPE ("a{sv}"));

          if (force_disable_deltas)
            {
              g_variant_builder_add (&find_builder, "{s@v}", "disable-static-deltas",
                                     g_variant_new_variant (g_variant_new_boolean (TRUE)));
            }

          collection_ref.collection_id = state->collection_id;
          collection_ref.ref_name = (char *) ref;

          collection_refs_to_fetch[0] = &collection_ref;
          collection_refs_to_fetch[1] = NULL;

          if (progress != NULL)
            update_freq = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (progress), "update-frequency"));
          if (update_freq == 0)
            update_freq = FLATPAK_DEFAULT_UPDATE_FREQUENCY;

          g_variant_builder_add (&find_builder, "{s@v}", "update-frequency",
                                 g_variant_new_variant (g_variant_new_uint32 (update_freq)));

          find_options = g_variant_ref_sink (g_variant_builder_end (&find_builder));

          context = flatpak_main_context_new_default ();

          ostree_repo_find_remotes_async (self->repo, (const OstreeCollectionRef * const *) collection_refs_to_fetch,
                                          find_options,
                                          NULL /* default finders */, progress, cancellable,
                                          async_result_cb, &find_result);

          while (find_result == NULL)
            g_main_context_iteration (context, TRUE);

          allocated_results = ostree_repo_find_remotes_finish (self->repo, find_result, error);

          results = (const OstreeRepoFinderResult * const *) allocated_results;
          if (results == NULL)
            return FALSE;

          for (i = 0, rev = NULL; results[i] != NULL && rev == NULL; i++)
            rev = g_strdup (g_hash_table_lookup (results[i]->ref_to_checksum, &collection_ref));

          if (rev == NULL)
            return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("No such ref (%s, %s) in remote %s or elsewhere"),
                                 collection_ref.collection_id, collection_ref.ref_name, state->remote_name);
        }
      else
        {
          flatpak_remote_state_lookup_ref (state, ref, &rev, NULL, error);
          if (rev == NULL && error != NULL && *error == NULL)
            flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Couldn't find latest checksum for ref %s in remote %s"),
                          ref, state->remote_name);

          results = NULL;
        }

      if (rev == NULL)
        {
          g_assert (error == NULL || *error != NULL);
          return FALSE;
        }
    }

  if (repo == NULL)
    repo = self->repo;

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

  /* Setup extra data information before starting to pull, so we can have precise
   * progress reports */
  if (!flatpak_dir_setup_extra_data (self, repo, state->remote_name,
                                     ref, rev, results,
                                     flatpak_flags,
                                     progress,
                                     cancellable,
                                     error))
    goto out;

  remote_and_branch = g_strdup_printf ("%s:%s", state->remote_name, ref);
  ostree_repo_resolve_rev (repo, remote_and_branch, TRUE, &current_checksum, NULL);

  if (!repo_pull (repo, state->remote_name,
                  subdirs_arg ? (const char **) subdirs_arg->pdata : NULL,
                  ref, rev, results, flatpak_flags, flags,
                  progress,
                  cancellable, error))
    {
      g_prefix_error (error, _("While pulling %s from remote %s: "), ref, state->remote_name);
      goto out;
    }


  if (require_metadata)
    {
      g_autoptr(GVariant) commit_data = NULL;
      if (!ostree_repo_load_commit (repo, rev, &commit_data, NULL, error) ||
          !validate_commit_metadata (commit_data,
                                     ref,
                                     (const char *)g_bytes_get_data (require_metadata, NULL),
                                     g_bytes_get_size (require_metadata),
                                     TRUE,
                                     error))
        goto out;
    }

  if (!flatpak_dir_pull_extra_data (self, repo,
                                    state->remote_name,
                                    ref, rev,
                                    flatpak_flags,
                                    progress,
                                    cancellable,
                                    error))
    goto out;


  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    goto out;

  ret = TRUE;

  if (repo == self->repo)
    name = flatpak_dir_get_name (self);
  else
    {
      GFile *file = ostree_repo_get_path (repo);
      name = g_file_get_path (file);
    }

  (flatpak_dir_log) (self, __FILE__, __LINE__, __FUNCTION__, name,
                     "pull", state->remote_name, ref, rev, current_checksum, NULL,
                      "Pulled %s from %s", ref, state->remote_name);

out:
  if (!ret)
    ostree_repo_abort_transaction (repo, cancellable, NULL);

  if (progress)
    ostree_async_progress_finish (progress);

  return ret;
}

static gboolean
repo_pull_local_untrusted (FlatpakDir          *self,
                           OstreeRepo          *repo,
                           const char          *remote_name,
                           const char          *url,
                           const char         **dirs_to_pull,
                           const char          *ref,
                           const char          *checksum,
                           OstreeAsyncProgress *progress,
                           GCancellable        *cancellable,
                           GError             **error)
{
  /* The latter flag was introduced in https://github.com/ostreedev/ostree/pull/926 */
  const OstreeRepoPullFlags flags = OSTREE_REPO_PULL_FLAGS_UNTRUSTED | OSTREE_REPO_PULL_FLAGS_BAREUSERONLY_FILES;
  GVariantBuilder builder;
  g_autoptr(GVariant) options = NULL;
  g_auto(GVariantBuilder) refs_builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;
  gboolean res;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  const char *refs[2] = { NULL, NULL };
  const char *commits[2] = { NULL, NULL };
  g_autofree char *collection_id = NULL;
  g_autoptr(GError) dummy_error = NULL;

  /* The ostree fetcher asserts if error is NULL */
  if (error == NULL)
    error = &dummy_error;

  g_assert (progress != NULL);

  if (!repo_get_remote_collection_id (repo, remote_name, &collection_id, error))
    return FALSE;

  if (collection_id != NULL)
    {
      g_variant_builder_init (&refs_builder, G_VARIANT_TYPE ("a(sss)"));
      g_variant_builder_add (&refs_builder, "(sss)", collection_id, ref, checksum);

      g_variant_builder_add (&builder, "{s@v}", "collection-refs",
                             g_variant_new_variant (g_variant_builder_end (&refs_builder)));
    }
  else
    {
      refs[0] = ref;
      commits[0] = checksum;

      g_variant_builder_add (&builder, "{s@v}", "refs",
                             g_variant_new_variant (g_variant_new_strv ((const char * const *) refs, -1)));
      g_variant_builder_add (&builder, "{s@v}", "override-commit-ids",
                             g_variant_new_variant (g_variant_new_strv ((const char * const *) commits, -1)));
    }

  g_variant_builder_add (&builder, "{s@v}", "flags",
                         g_variant_new_variant (g_variant_new_int32 (flags)));
  g_variant_builder_add (&builder, "{s@v}", "override-remote-name",
                         g_variant_new_variant (g_variant_new_string (remote_name)));
  g_variant_builder_add (&builder, "{s@v}", "gpg-verify",
                         g_variant_new_variant (g_variant_new_boolean (TRUE)));
  g_variant_builder_add (&builder, "{s@v}", "gpg-verify-summary",
                         g_variant_new_variant (g_variant_new_boolean (collection_id == NULL)));
  g_variant_builder_add (&builder, "{s@v}", "inherit-transaction",
                         g_variant_new_variant (g_variant_new_boolean (TRUE)));
  g_variant_builder_add (&builder, "{s@v}", "update-frequency",
                         g_variant_new_variant (g_variant_new_uint32 (FLATPAK_DEFAULT_UPDATE_FREQUENCY)));

  if (dirs_to_pull)
    {
      g_variant_builder_add (&builder, "{s@v}", "subdirs",
                             g_variant_new_variant (g_variant_new_strv ((const char * const *) dirs_to_pull, -1)));
      g_variant_builder_add (&builder, "{s@v}", "disable-static-deltas",
                             g_variant_new_variant (g_variant_new_boolean (TRUE)));
    }

  options = g_variant_ref_sink (g_variant_builder_end (&builder));
  res = ostree_repo_pull_with_options (repo, url, options,
                                       progress, cancellable, error);
  if (!res)
    translate_ostree_repo_pull_errors (error);

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
  g_autofree char *current_checksum = NULL;
  gboolean gpg_verify_summary;
  gboolean gpg_verify;
  g_autofree char *collection_id = NULL;
  char *summary_data = NULL;
  char *summary_sig_data = NULL;
  g_autofree char *remote_and_branch = NULL;
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
  g_auto(GLnxLockFile) lock = { 0, };
  gboolean ret = FALSE;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  /* Keep a shared repo lock to avoid prunes removing objects we're relying on
   * while we do the pull. There are two cases we protect against. 1) objects we
   * need but that we already decided are locally available could be removed,
   * and 2) during the transaction commit objects that don't yet have a ref to
   * them could be considered unreachable.
   */
  if (!flatpak_dir_repo_lock (self, &lock, LOCK_SH, cancellable, error))
    return FALSE;

  if (!ostree_repo_remote_get_gpg_verify_summary (self->repo, remote_name,
                                                  &gpg_verify_summary, error))
    return FALSE;

  if (!repo_get_remote_collection_id (self->repo, remote_name, &collection_id, error))
    return FALSE;

  if (!ostree_repo_remote_get_gpg_verify (self->repo, remote_name,
                                          &gpg_verify, error))
    return FALSE;

  /* This was verified in the client, but lets do it here too */
  if ((!gpg_verify_summary && collection_id == NULL) || !gpg_verify)
    return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("Can't pull from untrusted non-gpg verified remote"));

  /* We verify the summary manually before anything else to make sure
     we've got something right before looking too hard at the repo and
     so we can check for a downgrade before pulling and updating the
     ref */

  if (!g_file_load_contents (summary_file, cancellable,
                             &summary_data, &summary_data_size, NULL, NULL))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("No summary found"));
  summary_bytes = g_bytes_new_take (summary_data, summary_data_size);

  if (gpg_verify_summary)
    {
      if (!g_file_load_contents (summary_sig_file, cancellable,
                                 &summary_sig_data, &summary_sig_data_size, NULL, NULL))
        return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("GPG verification enabled, but no summary signatures found for remote '%s'"), remote_name);

      summary_sig_bytes = g_bytes_new_take (summary_sig_data, summary_sig_data_size);

      gpg_result = ostree_repo_verify_summary (self->repo,
                                               remote_name,
                                               summary_bytes,
                                               summary_sig_bytes,
                                               cancellable, error);
      if (gpg_result == NULL)
        return FALSE;

      if (ostree_gpg_verify_result_count_valid (gpg_result) == 0)
        return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("GPG signatures found for remote '%s', but none are in trusted keyring"), remote_name);
    }

  g_clear_object (&gpg_result);

  remote_and_branch = g_strdup_printf ("%s:%s", remote_name, ref);
  if (!ostree_repo_resolve_rev (self->repo, remote_and_branch, TRUE, &current_checksum, error))
    return FALSE;

  if (current_checksum != NULL &&
      !ostree_repo_load_commit (self->repo, current_checksum, &old_commit, NULL, NULL))
    return FALSE;

  src_repo = ostree_repo_new (path_file);
  if (!ostree_repo_open (src_repo, cancellable, error))
    return FALSE;

  if (collection_id == NULL)
    {
      summary = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, summary_bytes, FALSE));
      if (!flatpak_summary_lookup_ref (summary,
                                       NULL,
                                       ref,
                                       &checksum, NULL))
        return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("No such ref '%s' in remote %s"),
                                     ref, remote_name);

    }
  else
    {
      if (!ostree_repo_resolve_rev (src_repo, remote_and_branch, FALSE, &checksum, error))
        return FALSE;
    }

  if (gpg_verify)
    {
      gpg_result = ostree_repo_verify_commit_for_remote (src_repo, checksum, remote_name, cancellable, error);
      if (gpg_result == NULL)
        return FALSE;

      if (ostree_gpg_verify_result_count_valid (gpg_result) == 0)
        return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("GPG signatures found, but none are in trusted keyring"));
    }

  g_clear_object (&gpg_result);

  if (!ostree_repo_load_commit (src_repo, checksum, &new_commit, NULL, error))
    return FALSE;

  if (gpg_verify)
    {
      /* Verify the commit’s binding to the ref and to the repo. See
       * verify_bindings() in libostree. */
      g_autoptr(GVariant) new_commit_metadata = g_variant_get_child_value (new_commit, 0);
      g_autofree const char **commit_refs = NULL;

      if (!g_variant_lookup (new_commit_metadata,
                             OSTREE_COMMIT_META_KEY_REF_BINDING,
                             "^a&s",
                             &commit_refs))
        {
          /* Early return here - if the remote collection ID is NULL, then
           * we certainly will not verify the collection binding in the
           * commit.
           */
          if (collection_id != NULL)
            return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Expected commit metadata to have ref binding information, found none"));
        }

      if (collection_id != NULL &&
          !g_strv_contains ((const char * const *) commit_refs, ref))
        return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Commit has no requested ref ‘%s’ in ref binding metadata"),
                                   ref);

      if (collection_id != NULL)
        {
          const char *commit_collection_id;
          if (!g_variant_lookup (new_commit_metadata,
                                 "ostree.collection-binding",
                                 "&s",
                                 &commit_collection_id))
            return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Expected commit metadata to have collection ID binding information, found none"));
          if (!g_str_equal (commit_collection_id, collection_id))
            return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA,
                                       _("Commit has collection ID ‘%s’ in collection binding "
                                         "metadata, while the remote it came from has "
                                         "collection ID ‘%s’"),
                                         commit_collection_id, collection_id);
        }
    }

  if (old_commit)
    {
      guint64 old_timestamp;
      guint64 new_timestamp;

      old_timestamp = ostree_commit_get_timestamp (old_commit);
      new_timestamp = ostree_commit_get_timestamp (new_commit);

      if (new_timestamp < old_timestamp)
        return flatpak_fail_error (error, FLATPAK_ERROR_DOWNGRADE, "Not allowed to downgrade %s", ref);
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

  if (!repo_pull_local_untrusted (self, self->repo, remote_name, url,
                                  subdirs_arg ? (const char **) subdirs_arg->pdata : NULL,
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

  flatpak_dir_log (self, "pull local", src_path, ref, checksum, current_checksum, NULL,
                   "Pulled %s from %s", ref, src_path);
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
          const char *branch = g_file_info_get_name (child_info2);

          if (g_file_info_get_file_type (child_info2) == G_FILE_TYPE_DIRECTORY)
            {
              g_autoptr(GFile) deploy = flatpak_build_file (child, branch, "active/deploy", NULL);
              if (g_file_query_exists (deploy, NULL))
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

GVariant *
flatpak_dir_read_latest_commit (FlatpakDir   *self,
                                const char   *remote,
                                const char   *ref,
                                char        **out_checksum,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_autofree char *remote_and_ref = NULL;
  g_autofree char *res = NULL;

  g_autoptr(GVariant) commit_data = NULL;

  /* There may be several remotes with the same branch (if we for
   * instance changed the origin) so prepend the current origin to
   * make sure we get the right one */

  if (remote)
    remote_and_ref = g_strdup_printf ("%s:%s", remote, ref);
  else
    remote_and_ref = g_strdup (ref);

  if (!ostree_repo_resolve_rev (self->repo, remote_and_ref, FALSE, &res, error))
    return NULL;

  if (!ostree_repo_load_commit (self->repo, res, &commit_data, NULL, error))
    return NULL;

  if (out_checksum)
    *out_checksum = g_steal_pointer (&res);

  return g_steal_pointer (&commit_data);
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
  g_autofree char *res = NULL;

  /* There may be several remotes with the same branch (if we for
   * instance changed the origin) so prepend the current origin to
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
        return NULL;

      commit_metadata = g_variant_get_child_value (commit_data, 0);
      g_variant_lookup (commit_metadata, "xa.alt-id", "s", &alt_id);

      *out_alt_id = g_steal_pointer (&alt_id);
    }

  return g_steal_pointer (&res);
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
                        const char   *active_id,
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

  if (active_id != NULL)
    {
      glnx_gen_temp_name (tmpname);
      active_tmp_link = g_file_get_child (deploy_base, tmpname);
      if (!g_file_make_symbolic_link (active_tmp_link, active_id, cancellable, error))
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

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      const char *installation = flatpak_dir_get_id (self);

      if (!flatpak_dir_system_helper_call_run_triggers (self,
                                                        FLATPAK_HELPER_RUN_TRIGGERS_FLAGS_NONE,
                                                        installation ? installation : "",
                                                        cancellable,
                                                        error))
        return FALSE;

      return TRUE;
    }

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
          g_autofree char *basedir = realpath (basedir_orig, NULL);

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

typedef enum {
  INI_FILE_TYPE_SEARCH_PROVIDER = 1,
} ExportedIniFileType;

static gboolean
export_ini_file (int                 parent_fd,
                 const char         *name,
                 ExportedIniFileType ini_type,
                 struct stat        *stat_buf,
                 char              **target,
                 GCancellable       *cancellable,
                 GError            **error)
{
  glnx_autofd int desktop_fd = -1;
  g_autofree char *tmpfile_name = g_strdup_printf ("export-ini-XXXXXX");

  g_autoptr(GOutputStream) out_stream = NULL;
  g_autofree gchar *data = NULL;
  gsize data_len;
  g_autofree gchar *new_data = NULL;
  gsize new_data_len;
  g_autoptr(GKeyFile) keyfile = NULL;

  if (!flatpak_openat_noatime (parent_fd, name, &desktop_fd, cancellable, error) ||
      !read_fd (desktop_fd, stat_buf, &data, &data_len, error))
    return FALSE;

  keyfile = g_key_file_new ();
  if (!g_key_file_load_from_data (keyfile, data, data_len, G_KEY_FILE_KEEP_TRANSLATIONS, error))
    return FALSE;

  if (ini_type == INI_FILE_TYPE_SEARCH_PROVIDER)
    g_key_file_set_boolean (keyfile, "Shell Search Provider", "DefaultDisabled", TRUE);

  new_data = g_key_file_to_data (keyfile, &new_data_len, error);
  if (new_data == NULL)
    return FALSE;

  if (!flatpak_open_in_tmpdir_at (parent_fd, 0755, tmpfile_name, &out_stream, cancellable, error) ||
      !g_output_stream_write_all (out_stream, new_data, new_data_len, NULL, cancellable, error) ||
      !g_output_stream_close (out_stream, cancellable, error))
    return FALSE;

  if (target)
    *target = g_steal_pointer (&tmpfile_name);

  return TRUE;
}

static inline void
xml_autoptr_cleanup_generic_free (void *p)
{
  void **pp = (void **) p;

  if (*pp)
    xmlFree (*pp);
}


#define xml_autofree _GLIB_CLEANUP (xml_autoptr_cleanup_generic_free)

/* This verifies the basic layout of the files, then it removes
 * any magic matches, and makes all glob matches have a very low
 * priority (weight = 5). This should make it pretty safe to
 * export mime types, because the should not override the system
 * ones in any weird ways. */
static gboolean
rewrite_mime_xml (xmlDoc *doc)
{
  xmlNode *root_element = xmlDocGetRootElement (doc);
  xmlNode *top_node = NULL;

  for (top_node = root_element; top_node; top_node = top_node->next)
    {
      xmlNode *mime_node = NULL;
      if (top_node->type != XML_ELEMENT_NODE)
        continue;

      if (strcmp ((char *) top_node->name, "mime-info") != 0)
        return FALSE;

      for (mime_node = top_node->children; mime_node; mime_node = mime_node->next)
        {
          xmlNode *sub_node = NULL;
          xmlNode *next_sub_node = NULL;

          xml_autofree xmlChar *mimetype = NULL;
          if (mime_node->type != XML_ELEMENT_NODE)
            continue;

          if (strcmp ((char *) mime_node->name, "mime-type") != 0)
            return FALSE;

          mimetype = xmlGetProp (mime_node, (xmlChar *) "type");
          for (sub_node = mime_node->children; sub_node; sub_node = next_sub_node)
            {
              next_sub_node = sub_node->next;

              if (sub_node->type != XML_ELEMENT_NODE)
                continue;

              if (strcmp ((char *) sub_node->name, "magic") == 0)
                {
                  g_warning ("Removing magic mime rule from exports");
                  xmlUnlinkNode (sub_node);
                  xmlFreeNode (sub_node);
                }
              else if (strcmp ((char *) sub_node->name, "glob") == 0)
                {
                  xmlSetProp (sub_node,
                              (const xmlChar *) "weight",
                              (const xmlChar *) "5");
                }
            }
        }
    }

  return TRUE;
}

static gboolean
export_mime_file (int           parent_fd,
                  const char   *name,
                  struct stat  *stat_buf,
                  char        **target,
                  GCancellable *cancellable,
                  GError      **error)
{
  glnx_autofd int desktop_fd = -1;
  g_autofree char *tmpfile_name = g_strdup_printf ("export-mime-XXXXXX");

  g_autoptr(GOutputStream) out_stream = NULL;
  g_autofree gchar *data = NULL;
  gsize data_len;
  xmlDoc *doc = NULL;
  xml_autofree xmlChar *xmlbuff = NULL;
  int buffersize;

  if (!flatpak_openat_noatime (parent_fd, name, &desktop_fd, cancellable, error) ||
      !read_fd (desktop_fd, stat_buf, &data, &data_len, error))
    return FALSE;

  doc = xmlReadMemory (data, data_len, NULL, NULL,  0);
  if (doc == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_EXPORT_FAILED, _("Error reading mimetype xml file"));

  if (!rewrite_mime_xml (doc))
    {
      xmlFreeDoc (doc);
      return flatpak_fail_error (error, FLATPAK_ERROR_EXPORT_FAILED, _("Invalid mimetype xml file"));
    }

  xmlDocDumpFormatMemory (doc, &xmlbuff, &buffersize, 1);
  xmlFreeDoc (doc);

  if (!flatpak_open_in_tmpdir_at (parent_fd, 0755, tmpfile_name, &out_stream, cancellable, error) ||
      !g_output_stream_write_all (out_stream, xmlbuff, buffersize, NULL, cancellable, error) ||
      !g_output_stream_close (out_stream, cancellable, error))
    return FALSE;

  if (target)
    *target = g_steal_pointer (&tmpfile_name);

  return TRUE;
}

static char *
format_flatpak_run_args_from_run_opts (GStrv flatpak_run_args)
{
  GString *str;
  GStrv iter = flatpak_run_args;

  if (flatpak_run_args == NULL)
    return NULL;

  str = g_string_new ("");
  for (; *iter != NULL; ++iter)
    {
      if (g_strcmp0 (*iter, "no-a11y-bus") == 0)
        g_string_append_printf (str, " --no-a11y-bus");
      else if (g_strcmp0 (*iter, "no-documents-portal") == 0)
        g_string_append_printf (str, " --no-documents-portal");
    }

  return g_string_free (str, FALSE);
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
  glnx_autofd int desktop_fd = -1;
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
          flatpak_fail_error (error, FLATPAK_ERROR_EXPORT_FAILED, _("D-Bus service file '%s' has wrong name"), name);
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
                                      G_KEY_FILE_DESKTOP_GROUP,
                                      "X-Flatpak-Tags",
                                      (const char * const *) tags, length);
        }

      /* Add a marker so consumers can easily find out that this launches a sandbox */
      g_key_file_set_string (keyfile, G_KEY_FILE_DESKTOP_GROUP, "X-Flatpak", app);
    }

  groups = g_key_file_get_groups (keyfile, NULL);

  for (i = 0; groups[i] != NULL; i++)
    {
      g_auto(GStrv) flatpak_run_opts = g_key_file_get_string_list (keyfile, groups[i], "X-Flatpak-RunOptions", NULL, NULL);
      g_autofree char *flatpak_run_args = format_flatpak_run_args_from_run_opts (flatpak_run_opts);

      g_key_file_remove_key (keyfile, groups[i], "X-Flatpak-RunOptions", NULL);
      g_key_file_remove_key (keyfile, groups[i], "TryExec", NULL);

      /* Remove this to make sure nothing tries to execute it outside the sandbox*/
      g_key_file_remove_key (keyfile, groups[i], "X-GNOME-Bugzilla-ExtraInfoScript", NULL);

      new_exec = g_string_new ("");
      g_string_append_printf (new_exec,
                              FLATPAK_BINDIR "/flatpak run --branch=%s --arch=%s",
                              escaped_branch,
                              escaped_arch);

      if (flatpak_run_args != NULL)
        g_string_append_printf (new_exec, "%s", flatpak_run_args);

      old_exec = g_key_file_get_string (keyfile, groups[i], "Exec", NULL);
      if (old_exec && g_shell_parse_argv (old_exec, &old_argc, &old_argv, NULL) && old_argc >= 1)
        {
          int i;
          g_autofree char *command = maybe_quote (old_argv[0]);

          g_string_append_printf (new_exec, " --command=%s", command);

          for (i = 1; i < old_argc; i++)
            {
              if (strcasecmp (old_argv[i], "%f") == 0 ||
                  strcasecmp (old_argv[i], "%u") == 0)
                {
                  g_string_append (new_exec, " --file-forwarding");
                  break;
                }
            }

          g_string_append (new_exec, " ");
          g_string_append (new_exec, escaped_app);

          for (i = 1; i < old_argc; i++)
            {
              g_autofree char *arg = maybe_quote (old_argv[i]);

              if (strcasecmp (arg, "%f") == 0)
                g_string_append_printf (new_exec, " @@ %s @@", arg);
              else if (strcasecmp (arg, "%u") == 0)
                g_string_append_printf (new_exec, " @@u %s @@", arg);
              else if (g_str_has_prefix (arg, "@@"))
                {
                  flatpak_fail_error (error, FLATPAK_ERROR_EXPORT_FAILED,
                                     _("Invalid Exec argument %s"), arg);
                  goto out;
                }
              else
                g_string_append_printf (new_exec, " %s", arg);
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
rewrite_export_dir (const char     *app,
                    const char     *branch,
                    const char     *arch,
                    GKeyFile       *metadata,
                    FlatpakContext *context,
                    int             source_parent_fd,
                    const char     *source_name,
                    const char     *source_path,
                    GCancellable   *cancellable,
                    GError        **error)
{
  gboolean ret = FALSE;

  g_auto(GLnxDirFdIterator) source_iter = {0};
  g_autoptr(GHashTable) visited_children = NULL;
  struct dirent *dent;
  gboolean exports_allowed = FALSE;
  g_auto(GStrv) allowed_prefixes = NULL;
  g_auto(GStrv) allowed_extensions = NULL;
  gboolean require_exact_match = FALSE;

  if (!glnx_dirfd_iterator_init_at (source_parent_fd, source_name, FALSE, &source_iter, error))
    goto out;

  exports_allowed = flatpak_get_allowed_exports (source_path, app, context,
                                                 &allowed_extensions, &allowed_prefixes, &require_exact_match);

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
          g_autofree char *path = g_build_filename (source_path, dent->d_name, NULL);

          if (!rewrite_export_dir (app, branch, arch, metadata, context,
                                   source_iter.fd, dent->d_name,
                                   path, cancellable, error))
            goto out;
        }
      else if (S_ISREG (stbuf.st_mode) && exports_allowed)
        {
          g_autofree gchar *name_without_extension = NULL;
          g_autofree gchar *new_name = NULL;
          int i;

          for (i = 0; allowed_extensions[i] != NULL; i++)
            {
              if (g_str_has_suffix (dent->d_name, allowed_extensions[i]))
                break;
            }

          if (allowed_extensions[i] == NULL)
            {
              g_warning ("Invalid extension for %s in app %s, removing.", dent->d_name, app);
              if (unlinkat (source_iter.fd, dent->d_name, 0) != 0 && errno != ENOENT)
                {
                  glnx_set_error_from_errno (error);
                  goto out;
                }
              continue;
            }

          name_without_extension = g_strndup (dent->d_name, strlen (dent->d_name) - strlen (allowed_extensions[i]));

          if (!flatpak_name_matches_one_wildcard_prefix (name_without_extension, (const char * const *) allowed_prefixes, require_exact_match))
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
              if (!export_desktop_file (app, branch, arch, metadata,
                                        source_iter.fd, dent->d_name, &stbuf, &new_name, cancellable, error))
                goto out;
            }

          if (strcmp (source_name, "search-providers") == 0 &&
              g_str_has_suffix (dent->d_name, ".ini"))
            {
              if (!export_ini_file (source_iter.fd, dent->d_name, INI_FILE_TYPE_SEARCH_PROVIDER,
                                    &stbuf, &new_name, cancellable, error))
                goto out;
            }

          if (strcmp (source_name, "packages") == 0 &&
              g_str_has_suffix (dent->d_name, ".xml"))
            {
              if (!export_mime_file (source_iter.fd, dent->d_name,
                                     &stbuf, &new_name, cancellable, error))
                goto out;
            }

          if (new_name)
            {
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

static gboolean
flatpak_rewrite_export_dir (const char   *app,
                            const char   *branch,
                            const char   *arch,
                            GKeyFile     *metadata,
                            GFile        *source,
                            GCancellable *cancellable,
                            GError      **error)
{
  gboolean ret = FALSE;

  g_autoptr(GFile) parent = g_file_get_parent (source);
  glnx_autofd int parentfd = -1;
  g_autofree char *name = g_file_get_basename (source);

  /* Start with a source path of "" - we don't care about
   * the "export" component and we want to start path traversal
   * relative to it. */
  const char *source_path = "";
  g_autoptr(FlatpakContext) context = flatpak_context_new ();

  if (!flatpak_context_load_metadata (context, metadata, error))
    return FALSE;

  if (!glnx_opendirat (AT_FDCWD,
                       flatpak_file_get_path_cached (parent),
                       TRUE,
                       &parentfd,
                       error))
    return FALSE;

  /* The fds are closed by this call */
  if (!rewrite_export_dir (app, branch, arch, metadata, context,
                           parentfd, name, source_path,
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
  glnx_autofd int destination_dfd = -1;
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

static gboolean
flatpak_export_dir (GFile        *source,
                    GFile        *destination,
                    const char   *symlink_prefix,
                    GCancellable *cancellable,
                    GError      **error)
{
  const char *exported_subdirs[] = {
    "share/applications",                  "../..",
    "share/icons",                         "../..",
    "share/dbus-1/services",               "../../..",
    "share/gnome-shell/search-providers",  "../../..",
    "share/mime/packages",                 "../../..",
    "bin",                                 "..",
  };
  int i;

  for (i = 0; i < G_N_ELEMENTS (exported_subdirs); i = i + 2)
    {
      /* The fds are closed by this call */
      g_autoptr(GFile) sub_source = g_file_resolve_relative_path (source, exported_subdirs[i]);
      g_autoptr(GFile) sub_destination = g_file_resolve_relative_path (destination, exported_subdirs[i]);
      g_autofree char *sub_symlink_prefix = g_build_filename (exported_subdirs[i + 1], symlink_prefix, exported_subdirs[i], NULL);

      if (!g_file_query_exists (sub_source, cancellable))
        continue;

      if (!flatpak_mkdir_p (sub_destination, cancellable, error))
        return FALSE;

      if (!export_dir (AT_FDCWD, flatpak_file_get_path_cached (sub_source), sub_symlink_prefix, "",
                       AT_FDCWD, flatpak_file_get_path_cached (sub_destination),
                       cancellable, error))
        return FALSE;
    }

  return TRUE;
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

  ret = TRUE;

out:
  return ret;
}

static gboolean
extract_extra_data (FlatpakDir   *self,
                    const char   *checksum,
                    GFile        *extradir,
                    gboolean     *created_extra_data,
                    GCancellable *cancellable,
                    GError      **error)
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

  g_debug ("extracting extra data to %s", flatpak_file_get_path_cached (extradir));

  if (!ostree_repo_read_commit_detached_metadata (self->repo, checksum, &detached_metadata,
                                                  cancellable, error))
    {
      g_prefix_error (error, _("While getting detached metadata: "));
      return FALSE;
    }

  if (detached_metadata == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Extra data missing in detached metadata"));

  extra_data = g_variant_lookup_value (detached_metadata, "xa.extra-data",
                                       G_VARIANT_TYPE ("a(ayay)"));
  if (extra_data == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Extra data missing in detached metadata"));

  n_extra_data = g_variant_n_children (extra_data);
  if (n_extra_data < n_extra_data_sources)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Extra data missing in detached metadata"));

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
        return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid checksum for extra data"));

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
          const guchar *data;
          gsize len;

          g_variant_get_child (extra_data, j, "(^ay@ay)",
                               &extra_data_name,
                               &content);

          if (strcmp (extra_data_source_name, extra_data_name) != 0)
            continue;

          data = g_variant_get_data (content);
          len = g_variant_get_size (content);

          if (len != download_size)
            return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Wrong size for extra data"));

          sha256 = g_compute_checksum_for_data (G_CHECKSUM_SHA256, data, len);
          if (strcmp (sha256, extra_data_sha256) != 0)
            return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid checksum for extra data"));

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
        return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Extra data %s missing in detached metadata"), extra_data_source_name);
    }

  *created_extra_data = TRUE;

  return TRUE;
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
    {
      int fd = g_array_index (fd_array, int, i);

      /* We also seek all fds to the start, because this lets
         us use the same fd_array multiple times */
      if (lseek (fd, 0, SEEK_SET) < 0)
        g_printerr ("lseek error in child setup");

      fcntl (fd, F_SETFD, 0);
    }
}

static gboolean
apply_extra_data (FlatpakDir   *self,
                  GFile        *checkoutdir,
                  GCancellable *cancellable,
                  GError      **error)
{
  g_autoptr(GFile) metadata = NULL;
  g_autofree char *metadata_contents = NULL;
  gsize metadata_size;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autofree char *id = NULL;
  g_autofree char *runtime = NULL;
  g_autofree char *runtime_ref = NULL;
  g_autoptr(FlatpakDeploy) runtime_deploy = NULL;
  g_autoptr(FlatpakBwrap) bwrap = NULL;
  g_autoptr(GFile) app_files = NULL;
  g_autoptr(GFile) apply_extra_file = NULL;
  g_autoptr(GFile) app_export_file = NULL;
  g_autoptr(GFile) extra_export_file = NULL;
  g_autoptr(GFile) extra_files = NULL;
  g_autoptr(GFile) runtime_files = NULL;
  g_auto(GStrv) runtime_ref_parts = NULL;
  g_autoptr(FlatpakContext) app_context = NULL;
  g_auto(GStrv) minimal_envp = NULL;
  int exit_status;
  const char *group = FLATPAK_METADATA_GROUP_APPLICATION;
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

  id = g_key_file_get_string (metakey, group, FLATPAK_METADATA_KEY_NAME,
                              &local_error);
  if (id == NULL)
    {
      group = FLATPAK_METADATA_GROUP_RUNTIME;
      id = g_key_file_get_string (metakey, group, FLATPAK_METADATA_KEY_NAME,
                                  NULL);
      if (id == NULL)
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
      g_clear_error (&local_error);
    }

  runtime = g_key_file_get_string (metakey, group,
                                   FLATPAK_METADATA_KEY_RUNTIME, error);
  if (runtime == NULL)
    runtime = g_key_file_get_string (metakey, FLATPAK_METADATA_GROUP_EXTENSION_OF,
                                     FLATPAK_METADATA_KEY_RUNTIME, NULL);
  if (runtime == NULL)
    return FALSE;

  runtime_ref = g_build_filename ("runtime", runtime, NULL);

  runtime_ref_parts = flatpak_decompose_ref (runtime_ref, error);
  if (runtime_ref_parts == NULL)
    return FALSE;

  if (!g_key_file_get_boolean (metakey, FLATPAK_METADATA_GROUP_EXTRA_DATA,
                               FLATPAK_METADATA_KEY_NO_RUNTIME, NULL))
    {
      runtime_deploy = flatpak_find_deploy_for_ref (runtime_ref, NULL, cancellable, error);
      if (runtime_deploy == NULL)
        return FALSE;
      runtime_files = flatpak_deploy_get_files (runtime_deploy);
    }

  app_files = g_file_get_child (checkoutdir, "files");
  app_export_file = g_file_get_child (checkoutdir, "export");
  extra_files = g_file_get_child (app_files, "extra");
  extra_export_file = g_file_get_child (extra_files, "export");

  minimal_envp = flatpak_run_get_minimal_env (FALSE, FALSE);
  bwrap = flatpak_bwrap_new (minimal_envp);
  flatpak_bwrap_add_args (bwrap, flatpak_get_bwrap (), NULL);

  if (runtime_files)
    flatpak_bwrap_add_args (bwrap,
                            "--ro-bind", flatpak_file_get_path_cached (runtime_files), "/usr",
                            "--lock-file", "/usr/.ref",
                            NULL);

  flatpak_bwrap_add_args (bwrap,
                          "--ro-bind", flatpak_file_get_path_cached (app_files), "/app",
                          "--bind", flatpak_file_get_path_cached (extra_files), "/app/extra",
                          "--chdir", "/app/extra",
                          /* We run as root in the system-helper case, so drop all caps */
                          "--cap-drop", "ALL",
                          NULL);

  if (!flatpak_run_setup_base_argv (bwrap, runtime_files, NULL, runtime_ref_parts[2],
                                    FLATPAK_RUN_FLAG_NO_SESSION_HELPER | FLATPAK_RUN_FLAG_NO_PROC,
                                    error))
    return FALSE;

  app_context = flatpak_context_new ();

  if (!flatpak_run_add_environment_args (bwrap, NULL,
                                         FLATPAK_RUN_FLAG_NO_SESSION_BUS_PROXY |
                                         FLATPAK_RUN_FLAG_NO_SYSTEM_BUS_PROXY |
                                         FLATPAK_RUN_FLAG_NO_A11Y_BUS_PROXY,
                                         id,
                                         app_context, NULL, NULL, cancellable, error))
    return FALSE;

  flatpak_bwrap_envp_to_args (bwrap);

  flatpak_bwrap_add_arg (bwrap, "/app/bin/apply_extra");

  flatpak_bwrap_finish (bwrap);

  g_debug ("Running /app/bin/apply_extra ");

  /* We run the sandbox without caps, but it can still create files owned by itself with
   * arbitrary permissions, including setuid myself. This is extra risky in the case where
   * this runs as root in the system helper case. We canonicalize the permissions at the
   * end, but do avoid non-canonical permissions leaking out before then we make the
   * toplevel dir only accessible to the user */
  if (chmod (flatpak_file_get_path_cached (extra_files), 0700) != 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  if (!g_spawn_sync (NULL,
                     (char **) bwrap->argv->pdata,
                     bwrap->envp,
                     G_SPAWN_SEARCH_PATH,
                     child_setup, bwrap->fds,
                     NULL, NULL,
                     &exit_status,
                     error))
    return FALSE;

  if (!flatpak_canonicalize_permissions (AT_FDCWD, flatpak_file_get_path_cached (extra_files),
                                         getuid() == 0 ? 0 : -1,
                                         getuid() == 0 ? 0 : -1,
                                         error))
    return FALSE;

  if (exit_status != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("apply_extra script failed, exit status %d"), exit_status);
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

/* We create a deploy ref for the currently deployed version of all refs to avoid
   deployed commits being pruned when e.g. we pull --no-deploy. */
static gboolean
flatpak_dir_update_deploy_ref (FlatpakDir          *self,
                               const char          *ref,
                               const char          *checksum,
                               GError             **error)
{
  g_autofree char *deploy_ref = g_strconcat ("deploy/", ref, NULL);

  if (!ostree_repo_set_ref_immediate (self->repo, NULL, deploy_ref, checksum, NULL, error))
    return FALSE;

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
  g_autoptr(GFile) bindir = NULL;
  g_autofree char *checkoutdirpath = NULL;
  g_autoptr(GFile) real_checkoutdir = NULL;
  g_autoptr(GFile) dotref = NULL;
  g_autoptr(GFile) files_etc = NULL;
  g_autoptr(GFile) deploy_data_file = NULL;
  g_autoptr(GVariant) commit_data = NULL;
  g_autoptr(GVariant) deploy_data = NULL;
  g_autoptr(GFile) export = NULL;
  g_autoptr(GFile) extradir = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  guint64 installed_size = 0;
  OstreeRepoCheckoutAtOptions options = { 0, };
  const char *checksum;
  glnx_autofd int checkoutdir_dfd = -1;
  g_autoptr(GFile) tmp_dir_template = NULL;
  g_autofree char *tmp_dir_path = NULL;
  const char *xa_ref = NULL;
  g_autofree char *checkout_basename = NULL;
  gboolean created_extra_data = FALSE;
  g_autoptr(GVariant) commit_metadata = NULL;
  g_auto(GLnxLockFile) lock = { 0, };
  g_autoptr(GFile) metadata_file = NULL;
  g_autofree char *metadata_contents = NULL;
  g_auto(GStrv) ref_parts = NULL;
  gboolean is_app;
  gsize metadata_size = 0;
  gboolean is_oci;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  ref_parts = flatpak_decompose_ref (ref, error);
  if (ref_parts == NULL)
    return FALSE;

  /* Keep a shared repo lock to avoid prunes removing objects we're relying on
   * while we do the checkout. This could happen if the ref changes after we
   * read its current value for the checkout. */
  if (!flatpak_dir_repo_lock (self, &lock, LOCK_SH, cancellable, error))
    return FALSE;

  deploy_base = flatpak_dir_get_deploy_dir (self, ref);

  if (checksum_or_latest == NULL)
    {
      g_debug ("No checksum specified, getting tip of %s from origin %s", ref, origin);

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
        return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("%s is not available"), ref);
    }

  if (!ostree_repo_load_commit (self->repo, checksum, &commit_data, NULL, error))
    return FALSE;

  commit_metadata = g_variant_get_child_value (commit_data, 0);
  checkout_basename = flatpak_dir_get_deploy_subdir (self, checksum, subpaths);

  real_checkoutdir = g_file_get_child (deploy_base, checkout_basename);
  if (g_file_query_exists (real_checkoutdir, cancellable))
    return flatpak_fail_error (error, FLATPAK_ERROR_ALREADY_INSTALLED,
                               _("%s commit %s already installed"), ref, checksum);

  g_autofree char *template = g_strdup_printf (".%s-XXXXXX", checkout_basename);
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
  options.bareuseronly_dirs = TRUE; /* https://github.com/ostreedev/ostree/pull/927 */
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
              g_prefix_error (error, _("While trying to checkout subpath ‘%s’: "), subpath);
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

  g_variant_lookup (commit_metadata, "xa.ref", "&s", &xa_ref);
  if (xa_ref != NULL)
    {
      gboolean gpg_verify_summary;

      if (!ostree_repo_remote_get_gpg_verify_summary (self->repo, origin, &gpg_verify_summary, error))
        return FALSE;

      if (gpg_verify_summary)
        {
          /* If we're using signed summaries, then the security is really due to the signatures on
           * the summary, and the xa.ref is not needed for security. In particular, endless are
           * currently using one single commit on multiple branches to handle devel/stable promotion.
           * So, to support this we report branch discrepancies as a warning, rather than as an error.
           * See https://github.com/flatpak/flatpak/pull/1013 for more discussion.
           */
          g_auto(GStrv) checkout_ref = NULL;
          g_auto(GStrv) commit_ref = NULL;

          checkout_ref = flatpak_decompose_ref (ref, error);
          if (checkout_ref == NULL)
            {
              g_prefix_error (error, _("Invalid deployed ref %s: "), ref);
              return FALSE;
            }

          commit_ref = flatpak_decompose_ref (xa_ref, error);
          if (commit_ref == NULL)
            {
              g_prefix_error (error, _("Invalid commit ref %s: "), xa_ref);
              return FALSE;
            }

          /* Fatal if kind/name/arch don't match. Warn for branch mismatch. */
          if (strcmp (checkout_ref[0], commit_ref[0]) != 0)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           _("Deployed ref %s kind does not match commit (%s)"),
                           ref, xa_ref);
              return FALSE;
            }

          if (strcmp (checkout_ref[1], commit_ref[1]) != 0)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           _("Deployed ref %s name does not match commit (%s)"),
                           ref, xa_ref);
              return FALSE;
            }

          if (strcmp (checkout_ref[2], commit_ref[2]) != 0)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           _("Deployed ref %s arch does not match commit (%s)"),
                           ref, xa_ref);
              return FALSE;
            }

          if (strcmp (checkout_ref[3], commit_ref[3]) != 0)
            g_warning (_("Deployed ref %s branch does not match commit (%s)"),
                       ref, xa_ref);
        }
      else if (strcmp (ref, xa_ref) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                       _("Deployed ref %s does not match commit (%s)"), ref, xa_ref);
          return FALSE;
        }
    }

  keyfile = g_key_file_new ();
  metadata_file = g_file_resolve_relative_path (checkoutdir, "metadata");
  if (g_file_load_contents (metadata_file, NULL,
                            &metadata_contents,
                            &metadata_size, NULL, NULL))
    {
      if (!g_key_file_load_from_data (keyfile,
                                      metadata_contents,
                                      metadata_size,
                                      0, error))
        return FALSE;

      if (!flatpak_check_required_version (ref, keyfile, error))
        return FALSE;
    }

  /* Check the metadata in the commit to make sure it matches the actual
   * deployed metadata, in case we relied on the one in the commit for
   * a decision
   * Note: For historical reason we don't enforce commits to contain xa.metadata
   * since this was lacking in fedora builds.
   */
  is_oci = flatpak_dir_get_remote_oci (self, origin);
  if (!validate_commit_metadata (commit_data, ref,
                                 metadata_contents, metadata_size, !is_oci, error))
    return FALSE;

  dotref = g_file_resolve_relative_path (checkoutdir, "files/.ref");
  if (!g_file_replace_contents (dotref, "", 0, NULL, FALSE,
                                G_FILE_CREATE_REPLACE_DESTINATION, NULL, cancellable, error))
    return TRUE;


  export = g_file_get_child (checkoutdir, "export");

  /* Never export any binaries bundled with the app */
  bindir = g_file_get_child (export, "bin");
  if (!flatpak_rm_rf (bindir, cancellable, error))
    return FALSE;

  is_app = g_str_has_prefix (ref, "app/");

  if (!is_app) /* is runtime */
    {
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

      /* Runtime should never export anything */
      if (!flatpak_rm_rf (export, cancellable, error))
        return FALSE;
    }
  else /* is app */
    {
      g_autoptr(GFile) wrapper = g_file_get_child (bindir, ref_parts[1]);
      g_autofree char *escaped_app = maybe_quote (ref_parts[1]);
      g_autofree char *escaped_branch = maybe_quote (ref_parts[3]);
      g_autofree char *escaped_arch = maybe_quote (ref_parts[2]);
      g_autofree char *bin_data = NULL;
      int r;

      if (!flatpak_mkdir_p (bindir, cancellable, error))
        return FALSE;

      if (!flatpak_rewrite_export_dir (ref_parts[1], ref_parts[3], ref_parts[2],
                                       keyfile, export,
                                       cancellable,
                                       error))
        return FALSE;

      bin_data = g_strdup_printf ("#!/bin/sh\nexec %s/flatpak run --branch=%s --arch=%s %s \"$@\"\n",
                                  FLATPAK_BINDIR, escaped_branch, escaped_arch, escaped_app);
      if (!g_file_replace_contents (wrapper, bin_data, strlen (bin_data), NULL, FALSE,
                                    G_FILE_CREATE_REPLACE_DESTINATION, NULL, cancellable, error))
        return FALSE;

      do
        r = fchmodat (AT_FDCWD, flatpak_file_get_path_cached (wrapper), 0755, 0);
      while (G_UNLIKELY (r == -1 && errno == EINTR));
      if (r == -1)
        return glnx_throw_errno_prefix (error, "fchmodat");

    }

  deploy_data = flatpak_dir_new_deploy_data (self,
                                             checkoutdir,
                                             commit_metadata,
                                             keyfile,
                                             ref_parts[1],
                                             origin,
                                             checksum,
                                             (char **) subpaths,
                                             installed_size);

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

  if (!flatpak_dir_set_active (self, ref, checkout_basename, cancellable, error))
    return FALSE;

  if (!flatpak_dir_update_deploy_ref (self, ref, checksum, error))
    return FALSE;

  return TRUE;
}

/* -origin remotes are deleted when the last ref refering to it is undeployed */
void
flatpak_dir_prune_origin_remote (FlatpakDir *self,
                                 const char *remote)
{
  if (remote != NULL &&
      g_str_has_suffix (remote, "-origin") &&
      flatpak_dir_get_remote_noenumerate (self, remote) &&
      !flatpak_dir_remote_has_deploys (self, remote))
    {
      if (flatpak_dir_use_system_helper (self, NULL))
        {
          const char *installation = flatpak_dir_get_id (self);
          g_autoptr(GVariant) gpg_data_v = NULL;

          gpg_data_v = g_variant_ref_sink (g_variant_new_from_data (G_VARIANT_TYPE ("ay"), "", 0, TRUE, NULL, NULL));

          flatpak_dir_system_helper_call_configure_remote (self,
                                                           FLATPAK_HELPER_CONFIGURE_FLAGS_NONE,
                                                           remote,
                                                           "",
                                                           gpg_data_v,
                                                           installation ? installation : "",
                                                           NULL, NULL);
        }
      else
        flatpak_dir_remove_remote (self, FALSE, remote, NULL, NULL);
    }
}

gboolean
flatpak_dir_deploy_install (FlatpakDir   *self,
                            const char   *ref,
                            const char   *origin,
                            const char  **subpaths,
                            gboolean      reinstall,
                            GCancellable *cancellable,
                            GError      **error)
{
  g_auto(GLnxLockFile) lock = { 0, };
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) old_deploy_dir = NULL;
  gboolean created_deploy_base = FALSE;
  gboolean ret = FALSE;
  g_autoptr(GError) local_error = NULL;
  g_auto(GStrv) ref_parts = g_strsplit (ref, "/", -1);
  g_autofree char *remove_ref_from_remote = NULL;
  g_autofree char *commit = NULL;
  g_autofree char *old_active = NULL;

  if (!flatpak_dir_lock (self, &lock,
                         cancellable, error))
    goto out;

  old_deploy_dir = flatpak_dir_get_if_deployed (self, ref, NULL, cancellable);
  if (old_deploy_dir != NULL)
    {
      old_active = flatpak_dir_read_active (self, ref, cancellable);

      if (reinstall)
        {
          g_autoptr(GVariant) old_deploy = NULL;
          const char *old_origin;

          old_deploy = flatpak_load_deploy_data (old_deploy_dir, ref, FLATPAK_DEPLOY_VERSION_ANY, cancellable, error);
          if (old_deploy == NULL)
            goto out;

          /* If the old install was from a different remote, remove the ref */
          old_origin = flatpak_deploy_data_get_origin (old_deploy);
          if (strcmp (old_origin, origin) != 0)
            remove_ref_from_remote = g_strdup (old_origin);

          g_debug ("Removing old deployment for reinstall");
          if (!flatpak_dir_undeploy (self, ref, old_active,
                                     TRUE, FALSE,
                                     cancellable, error))
            goto out;
        }
      else
        {
          g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                       _("%s branch %s already installed"), ref_parts[1], ref_parts[3]);
          goto out;
        }
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

  /* Remove old ref if the reinstalled was from a different remote */
  if (remove_ref_from_remote != NULL)
    {
      if (!flatpak_dir_remove_ref (self, remove_ref_from_remote, ref, cancellable, error))
        goto out;

      flatpak_dir_prune_origin_remote (self, remove_ref_from_remote);
    }

  /* Release lock before doing possibly slow prune */
  glnx_release_lock_file (&lock);

  flatpak_dir_cleanup_removed (self, cancellable, NULL);

  if (!flatpak_dir_mark_changed (self, error))
    goto out;

  ret = TRUE;

  commit = flatpak_dir_read_active (self, ref, cancellable);
  flatpak_dir_log (self, "deploy install", origin, ref, commit, old_active, NULL,
                   "Installed %s from %s", ref, origin);

out:
  if (created_deploy_base && !ret)
    flatpak_rm_rf (deploy_base, cancellable, NULL);

  return ret;
}


gboolean
flatpak_dir_deploy_update (FlatpakDir   *self,
                           const char   *ref,
                           const char   *checksum_or_latest,
                           const char  **opt_subpaths,
                           GCancellable *cancellable,
                           GError      **error)
{
  g_autoptr(GVariant) old_deploy_data = NULL;
  g_auto(GLnxLockFile) lock = { 0, };
  g_autofree const char **old_subpaths = NULL;
  g_autofree char *old_active = NULL;
  const char *old_origin;
  g_autofree char *commit = NULL;

  if (!flatpak_dir_lock (self, &lock,
                         cancellable, error))
    return FALSE;

  old_deploy_data = flatpak_dir_get_deploy_data (self, ref, FLATPAK_DEPLOY_VERSION_ANY,
                                                 cancellable, error);
  if (old_deploy_data == NULL)
    return FALSE;

  old_active = flatpak_dir_read_active (self, ref, cancellable);

  old_origin = flatpak_deploy_data_get_origin (old_deploy_data);
  old_subpaths = flatpak_deploy_data_get_subpaths (old_deploy_data);
  if (!flatpak_dir_deploy (self,
                           old_origin,
                           ref,
                           checksum_or_latest,
                           opt_subpaths ? opt_subpaths : old_subpaths,
                           old_deploy_data,
                           cancellable, error))
    return FALSE;

  if (old_active &&
      !flatpak_dir_undeploy (self, ref, old_active,
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

  if (!flatpak_dir_mark_changed (self, error))
    return FALSE;

  flatpak_dir_cleanup_removed (self, cancellable, NULL);

  commit = flatpak_dir_read_active (self, ref, cancellable);
  flatpak_dir_log (self, "deploy update", old_origin, ref, commit, old_active, NULL,
                   "Updated %s from %s", ref, old_origin);

  return TRUE;
}

static FlatpakOciRegistry *
flatpak_dir_create_system_child_oci_registry (FlatpakDir   *self,
                                              GLnxLockFile *file_lock,
                                              GError      **error)
{
  g_autoptr(GFile) cache_dir = NULL;
  g_autoptr(GFile) repo_dir = NULL;
  g_autofree char *repo_url = NULL;
  g_autofree char *tmpdir_name = NULL;
  g_autoptr(FlatpakOciRegistry) new_registry = NULL;

  g_assert (!self->user);

  if (!flatpak_dir_ensure_repo (self, NULL, error))
    return NULL;

  cache_dir = flatpak_ensure_system_user_cache_dir_location (error);
  if (cache_dir == NULL)
    return NULL;

  if (!flatpak_allocate_tmpdir (AT_FDCWD,
                                flatpak_file_get_path_cached (cache_dir),
                                "child-oci-", &tmpdir_name,
                                NULL,
                                file_lock,
                                NULL,
                                NULL, error))
    return NULL;

  repo_dir = g_file_get_child (cache_dir, tmpdir_name);
  repo_url = g_file_get_uri (repo_dir);

  new_registry = flatpak_oci_registry_new (repo_url, TRUE, -1,
                                           NULL, error);
  if (new_registry == NULL)
    return NULL;

  return g_steal_pointer (&new_registry);
}

static OstreeRepo *
flatpak_dir_create_child_repo (FlatpakDir   *self,
                               GFile        *cache_dir,
                               GLnxLockFile *file_lock,
                               const char   *optional_commit,
                               GError      **error)
{
  g_autoptr(GFile) repo_dir = NULL;
  g_autoptr(GFile) repo_dir_config = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  g_autofree char *tmpdir_name = NULL;
  g_autoptr(OstreeRepo) new_repo = NULL;
  g_autoptr(GKeyFile) config = NULL;
  g_autofree char *current_mode = NULL;
  GKeyFile *orig_config = NULL;
  g_autofree char *orig_min_free_space_percent = NULL;
  g_autofree char *orig_min_free_space_size = NULL;

  /* We use bare-user-only here now, which means we don't need xattrs
   * for the child repo. This only works as long as the pulled repo
   * is valid in a bare-user-only repo, i.e. doesn't have xattrs or
   * weird permissions, because then the pull into the system repo
   * would complain that the checksum was wrong. However, by now all
   * flatpak builds are likely to be valid, so this is fine.
   */
  OstreeRepoMode mode = OSTREE_REPO_MODE_BARE_USER_ONLY;
  const char *mode_str = "bare-user-only";

  if (!flatpak_dir_ensure_repo (self, NULL, error))
    return NULL;

  orig_config = ostree_repo_get_config (self->repo);

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
      if (!ostree_repo_create (new_repo, mode, NULL, error))
        return NULL;
    }
  else
    {
      /* Try to open, but on failure, re-create */
      if (!ostree_repo_open (new_repo, NULL, NULL))
        {
          flatpak_rm_rf (repo_dir, NULL, NULL);
          if (!ostree_repo_create (new_repo, mode, NULL, error))
            return NULL;
        }
    }

  config = ostree_repo_copy_config (new_repo);

  /* Verify that the mode is the expected one; if it isn't, recreate the repo */
  current_mode = g_key_file_get_string (config, "core", "mode", NULL);
  if (current_mode == NULL || g_strcmp0 (current_mode, mode_str) != 0)
    {
      flatpak_rm_rf (repo_dir, NULL, NULL);

      /* Re-initialize the object because its dir's contents have been deleted (and it
       * holds internal references to them) */
      g_object_unref (new_repo);
      new_repo = ostree_repo_new (repo_dir);

      if (!ostree_repo_create (new_repo, mode, NULL, error))
        return NULL;

      /* Reload the repo config */
      g_key_file_free (config);
      config = ostree_repo_copy_config (new_repo);
    }

  /* Ensure the config is updated */
  g_key_file_set_string (config, "core", "parent",
                         flatpak_file_get_path_cached (ostree_repo_get_path (self->repo)));

  /* Copy the min space percent value so it affects the temporary repo too */
  orig_min_free_space_percent = g_key_file_get_value (orig_config, "core", "min-free-space-percent", NULL);
  if (orig_min_free_space_percent)
    g_key_file_set_value (config, "core", "min-free-space-percent", orig_min_free_space_percent);

  /* Copy the min space size value so it affects the temporary repo too */
  orig_min_free_space_size = g_key_file_get_value (orig_config, "core", "min-free-space-size", NULL);
  if (orig_min_free_space_size)
    g_key_file_set_value (config, "core", "min-free-space-size", orig_min_free_space_size);

  if (!ostree_repo_write_config (new_repo, config, error))
    return NULL;

  /* We need to reopen to apply the parent config */
  if (!self->user)
    repo = system_ostree_repo_new (repo_dir);
  else
    repo = ostree_repo_new (repo_dir);

  if (!ostree_repo_open (repo, NULL, error))
    return NULL;

  /* We don't need to sync the child repos, they are never used for stable storage, and we
     verify + fsync when importing to stable storage */
  ostree_repo_set_disable_fsync (repo, TRUE);

  /* Create a commitpartial in the child repo to ensure we download everything, because
     any commitpartial state in the parent will not be inherited */
  if (optional_commit)
    {
      g_autofree char *commitpartial_basename = g_strconcat (optional_commit, ".commitpartial", NULL);
      g_autoptr(GFile) commitpartial =
        flatpak_build_file (ostree_repo_get_path (repo),
                            "state", commitpartial_basename, NULL);

      g_file_replace_contents (commitpartial, "", 0, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, NULL);
    }
  return g_steal_pointer (&repo);
}

static OstreeRepo *
flatpak_dir_create_system_child_repo (FlatpakDir   *self,
                                      GLnxLockFile *file_lock,
                                      const char   *optional_commit,
                                      GError      **error)
{
  g_autoptr(GFile) cache_dir = NULL;

  g_assert (!self->user);

  cache_dir = flatpak_ensure_system_user_cache_dir_location (error);
  if (cache_dir == NULL)
    return NULL;

  return flatpak_dir_create_child_repo (self, cache_dir, file_lock, optional_commit, error);
}

gboolean
flatpak_dir_install (FlatpakDir          *self,
                     gboolean             no_pull,
                     gboolean             no_deploy,
                     gboolean             no_static_deltas,
                     gboolean             reinstall,
                     gboolean             app_hint,
                     FlatpakRemoteState  *state,
                     const char          *ref,
                     const char          *opt_commit,
                     const char         **opt_subpaths,
                     const char         **opt_previous_ids,
                     GBytes              *require_metadata,
                     const char          *token,
                     OstreeAsyncProgress *progress,
                     GCancellable        *cancellable,
                     GError             **error)
{
  FlatpakPullFlags flatpak_flags;

  /* This is not actually used for anything, so must be NULL in
   * this branch. */
  g_return_val_if_fail (opt_previous_ids == NULL, FALSE);
  g_return_val_if_fail (token == NULL, FALSE);

  flatpak_flags = FLATPAK_PULL_FLAGS_DOWNLOAD_EXTRA_DATA;
  if (no_static_deltas)
    flatpak_flags |= FLATPAK_PULL_FLAGS_NO_STATIC_DELTAS;

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      g_autoptr(OstreeRepo) child_repo = NULL;
      g_auto(GLnxLockFile) child_repo_lock = { 0, };
      const char *installation = flatpak_dir_get_id (self);
      const char *empty_subpaths[] = {NULL};
      const char **subpaths;
      g_autofree char *child_repo_path = NULL;
      FlatpakHelperDeployFlags helper_flags = 0;
      g_autofree char *url = NULL;
      gboolean gpg_verify_summary;
      gboolean gpg_verify;
      gboolean is_oci;

      if (opt_subpaths)
        subpaths = opt_subpaths;
      else
        subpaths = empty_subpaths;

      if (!ostree_repo_remote_get_url (self->repo,
                                       state->remote_name,
                                       &url,
                                       error))
        return FALSE;

      if (!ostree_repo_remote_get_gpg_verify_summary (self->repo, state->remote_name,
                                                      &gpg_verify_summary, error))
        return FALSE;

      if (!ostree_repo_remote_get_gpg_verify (self->repo, state->remote_name,
                                              &gpg_verify, error))
        return FALSE;

      is_oci = flatpak_dir_get_remote_oci (self, state->remote_name);
      if (no_pull)
        {
          /* Do nothing */
        }
      else if (is_oci)
        {
          g_autoptr(FlatpakOciRegistry) registry = NULL;
          g_autoptr(GFile) registry_file = NULL;

          registry = flatpak_dir_create_system_child_oci_registry (self, &child_repo_lock, error);
          if (registry == NULL)
            return FALSE;

          registry_file = g_file_new_for_uri (flatpak_oci_registry_get_uri (registry));

          child_repo_path = g_file_get_path (registry_file);

          if (!flatpak_dir_mirror_oci (self, registry, state, ref, NULL, progress, cancellable, error))
            return FALSE;
        }
      else if ((!gpg_verify_summary && state->collection_id == NULL) || !gpg_verify)
        {
          /* The remote is not gpg verified, so we don't want to allow installation via
             a download in the home directory, as there is no way to verify you're not
             injecting anything into the remote. However, in the case of a remote
             configured to a local filesystem we can just let the system helper do
             the installation, as it can then avoid network i/o and be certain the
             data comes from the right place.

             If a collection ID is available, we can verify the refs in commit
             metadata. */
          if (g_str_has_prefix (url, "file:"))
            helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL;
          else
            return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("Can't pull from untrusted non-gpg verified remote"));
        }
      else
        {
          /* We're pulling from a remote source, we do the network mirroring pull as a
             user and hand back the resulting data to the system-helper, that trusts us
             due to the GPG signatures in the repo */

          child_repo = flatpak_dir_create_system_child_repo (self, &child_repo_lock, NULL, error);
          if (child_repo == NULL)
            return FALSE;

          flatpak_flags |= FLATPAK_PULL_FLAGS_SIDELOAD_EXTRA_DATA;

          if (!flatpak_dir_pull (self, state, ref, opt_commit, NULL, subpaths, require_metadata, token,
                                 child_repo,
                                 flatpak_flags,
                                 OSTREE_REPO_PULL_FLAGS_MIRROR,
                                 progress, cancellable, error))
            return FALSE;

          if (!child_repo_ensure_summary (child_repo, state, cancellable, error))
            return FALSE;

          child_repo_path = g_file_get_path (ostree_repo_get_path (child_repo));
        }

      if (no_deploy)
        helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY;

      if (reinstall)
        helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL;

      if (app_hint)
        helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_APP_HINT;

      helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_INSTALL_HINT;

      if (!flatpak_dir_system_helper_call_deploy (self,
                                                  child_repo_path ? child_repo_path : "",
                                                  helper_flags, ref, state->remote_name,
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
      if (!flatpak_dir_pull (self, state, ref, opt_commit, NULL, opt_subpaths, require_metadata, token, NULL,
                             flatpak_flags, OSTREE_REPO_PULL_FLAGS_NONE,
                             progress, cancellable, error))
        return FALSE;
    }

  if (!no_deploy)
    {
      if (!flatpak_dir_deploy_install (self, ref, state->remote_name, opt_subpaths,
                                       reinstall, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

char *
flatpak_dir_ensure_bundle_remote (FlatpakDir   *self,
                                  GFile        *file,
                                  GBytes       *extra_gpg_data,
                                  char        **out_ref,
                                  char        **out_checksum,
                                  char        **out_metadata,
                                  gboolean     *out_created_remote,
                                  GCancellable *cancellable,
                                  GError      **error)
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
  g_autofree char *collection_id = NULL;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return NULL;

  metadata = flatpak_bundle_load (file, &to_checksum,
                                  &ref,
                                  &origin,
                                  NULL, &fp_metadata, NULL,
                                  &included_gpg_data,
                                  &collection_id,
                                  error);
  if (metadata == NULL)
    return NULL;

  gpg_data = extra_gpg_data ? extra_gpg_data : included_gpg_data;

  parts = flatpak_decompose_ref (ref, error);
  if (parts == NULL)
    return NULL;

  deploy_data = flatpak_dir_get_deploy_data (self, ref, FLATPAK_DEPLOY_VERSION_ANY, cancellable, NULL);
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
                                                 gpg_data,
                                                 collection_id,
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

  if (out_checksum)
    *out_checksum = g_steal_pointer (&to_checksum);

  if (out_metadata)
    *out_metadata = g_steal_pointer (&fp_metadata);


  return g_steal_pointer (&remote);
}

/* If core.add-remotes-config-dir is set for this repository (which is
 * not a common configuration, but it is possible), we will fail to modify
 * remote configuration when using a combination of
 * ostree_repo_remote_[add|change]() and ostree_repo_write_config() due to
 * adding remote config in /etc/flatpak/remotes.d and also in
 * /ostree/repo/config. Avoid that.
 *
 * FIXME: See https://github.com/flatpak/flatpak/issues/1665. In future, we
 * should just write the remote config to the correct place, factoring
 * core.add-remotes-config-dir in. */
static gboolean
flatpak_dir_check_add_remotes_config_dir (FlatpakDir *self,
                                          GError    **error)
{
  g_autoptr(GError) local_error = NULL;
  gboolean val;
  GKeyFile *config;

  if (!flatpak_dir_maybe_ensure_repo (self, NULL, error))
    return FALSE;

  if (self->repo == NULL)
    return TRUE;

  config = ostree_repo_get_config (self->repo);

  if (config == NULL)
    return TRUE;

  val = g_key_file_get_boolean (config, "core", "add-remotes-config-dir", &local_error);

  if (local_error != NULL)
    {
      if (g_error_matches (local_error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND))
        {
          g_clear_error (&local_error);
          val = ostree_repo_is_system (self->repo);
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
    }

  if (!val)
    return TRUE;

  return flatpak_fail (error,
                       "Can’t update remote configuration on a repository with "
                       "core.add-remotes-config-dir=true");
}

gboolean
flatpak_dir_install_bundle (FlatpakDir   *self,
                            GFile        *file,
                            const char   *remote,
                            char        **out_ref,
                            GCancellable *cancellable,
                            GError      **error)
{
  g_autofree char *ref = NULL;

  g_autoptr(GVariant) deploy_data = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *origin = NULL;
  g_auto(GStrv) parts = NULL;
  g_autofree char *to_checksum = NULL;
  gboolean gpg_verify;

  if (!flatpak_dir_check_add_remotes_config_dir (self, error))
    return FALSE;

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      const char *installation = flatpak_dir_get_id (self);

      if (!flatpak_dir_system_helper_call_install_bundle (self,
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
                                  NULL, NULL, NULL,
                                  error);
  if (metadata == NULL)
    return FALSE;

  parts = flatpak_decompose_ref (ref, error);
  if (parts == NULL)
    return FALSE;

  deploy_data = flatpak_dir_get_deploy_data (self, ref, FLATPAK_DEPLOY_VERSION_ANY, cancellable, NULL);
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
          if (!flatpak_dir_cleanup_remote_for_url_change (self, remote,
                                                          origin, cancellable, error))
            return FALSE;

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
      if (!flatpak_dir_deploy_install (self, ref, remote, NULL, FALSE, cancellable, error))
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
flatpak_dir_needs_update_for_commit_and_subpaths (FlatpakDir  *self,
                                                  const char  *remote,
                                                  const char  *ref,
                                                  const char  *target_commit,
                                                  const char **opt_subpaths)
{
  g_autoptr(GVariant) deploy_data = NULL;
  g_autofree const char **old_subpaths = NULL;
  const char **subpaths;
  g_autofree char *url = NULL;
  const char *installed_commit;
  const char *installed_alt_id;

  g_assert (target_commit != NULL);

  /* Never update from disabled remotes */
  if (!ostree_repo_remote_get_url (self->repo, remote, &url, NULL))
    return FALSE;

  if (*url == 0)
    return FALSE;

  deploy_data = flatpak_dir_get_deploy_data (self, ref, FLATPAK_DEPLOY_VERSION_ANY, NULL, NULL);
  if (deploy_data != NULL)
    old_subpaths = flatpak_deploy_data_get_subpaths (deploy_data);
  else
    old_subpaths = g_new0 (const char *, 1); /* Empty strv == all subpaths*/

  if (opt_subpaths)
    subpaths = opt_subpaths;
  else
    subpaths = old_subpaths;

  /* Not deployed => need update */
  if (deploy_data == NULL)
    return TRUE;

  installed_commit = flatpak_deploy_data_get_commit (deploy_data);
  installed_alt_id = flatpak_deploy_data_get_alt_id (deploy_data);

  /* Different target commit than deployed => update */
  if (g_strcmp0 (target_commit, installed_commit) != 0 &&
      g_strcmp0 (target_commit, installed_alt_id) != 0)
    return TRUE;

  /* target commit is the same as current, but maybe something else that is different? */

  /* Same commit, but different subpaths => update */
  if (!_g_strv_equal0 ((char **) subpaths, (char **) old_subpaths))
    return TRUE;

  /* Same subpaths and commit, no need to update */
  return FALSE;
}

char *
flatpak_dir_check_for_update (FlatpakDir               *self,
                              FlatpakRemoteState       *state,
                              const char               *ref,
                              const char               *checksum_or_latest,
                              const char              **opt_subpaths,
                              gboolean                  no_pull,
                              OstreeRepoFinderResult ***out_results,
                              GCancellable             *cancellable,
                              GError                  **error)
{
  g_autofree const char *remote_and_branch = NULL;
  g_autofree char *latest_rev = NULL;
  const char *target_rev = NULL;

  if (no_pull)
    {
      remote_and_branch = g_strdup_printf ("%s:%s", state->remote_name, ref);
      if (!ostree_repo_resolve_rev (self->repo, remote_and_branch, FALSE, &latest_rev, NULL))
        {
          g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                       _("%s already installed"), ref);
          return NULL; /* No update, because nothing to update to */
        }
    }
  else
    {
      if (!flatpak_dir_find_latest_rev (self, state, ref, checksum_or_latest, &latest_rev,
                                        out_results, cancellable, error))
        return NULL;
    }

  if (checksum_or_latest != NULL)
    target_rev = checksum_or_latest;
  else
    target_rev = latest_rev;

  if (flatpak_dir_needs_update_for_commit_and_subpaths (self, state->remote_name, ref, target_rev, opt_subpaths))
    return g_strdup (target_rev);

  g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
               _("%s commit %s already installed"), ref, target_rev);
  return NULL;
}

gboolean
flatpak_dir_update (FlatpakDir                           *self,
                    gboolean                              no_pull,
                    gboolean                              no_deploy,
                    gboolean                              no_static_deltas,
                    gboolean                              allow_downgrade,
                    gboolean                              app_hint,
                    gboolean                              install_hint,
                    FlatpakRemoteState                   *state,
                    const char                           *ref,
                    const char                           *commit,
                    const OstreeRepoFinderResult * const *results,
                    const char                          **opt_subpaths,
                    const char                          **opt_previous_ids,
                    GBytes                               *require_metadata,
                    const char                           *token,
                    OstreeAsyncProgress                  *progress,
                    GCancellable                         *cancellable,
                    GError                              **error)
{
  g_autoptr(GVariant) deploy_data = NULL;
  const char **subpaths = NULL;
  g_autofree char *url = NULL;
  FlatpakPullFlags flatpak_flags;
  g_autofree const char **old_subpaths = NULL;
  gboolean is_oci;

  /* This is not actually used for anything, so must be NULL in
   * this branch. */
  g_return_val_if_fail (opt_previous_ids == NULL, FALSE);
  g_return_val_if_fail (token == NULL, FALSE);

  /* This and @results are calculated in check_for_update. @results will be
   * %NULL if we don’t support collections. */
  g_assert (commit != NULL);

  flatpak_flags = FLATPAK_PULL_FLAGS_DOWNLOAD_EXTRA_DATA;
  if (allow_downgrade)
    flatpak_flags |= FLATPAK_PULL_FLAGS_ALLOW_DOWNGRADE;
  if (no_static_deltas)
    flatpak_flags |= FLATPAK_PULL_FLAGS_NO_STATIC_DELTAS;

  deploy_data = flatpak_dir_get_deploy_data (self, ref, FLATPAK_DEPLOY_VERSION_ANY,
                                             cancellable, NULL);

  if (deploy_data != NULL)
    old_subpaths = flatpak_deploy_data_get_subpaths (deploy_data);

  if (opt_subpaths)
    subpaths = opt_subpaths;
  else
    subpaths = old_subpaths;

  if (!ostree_repo_remote_get_url (self->repo, state->remote_name, &url, error))
    return FALSE;

  if (*url == 0)
    return TRUE; /* Empty URL => disabled */

  is_oci = flatpak_dir_get_remote_oci (self, state->remote_name);

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      const char *installation = flatpak_dir_get_id (self);
      g_autoptr(OstreeRepo) child_repo = NULL;
      g_auto(GLnxLockFile) child_repo_lock = { 0, };
      g_autofree char *child_repo_path = NULL;
      FlatpakHelperDeployFlags helper_flags = 0;
      g_autofree char *url = NULL;
      gboolean gpg_verify_summary;
      gboolean gpg_verify;

      if (allow_downgrade)
        return flatpak_fail_error (error, FLATPAK_ERROR_DOWNGRADE,
                                   _("Can't update to a specific commit without root permissions"));

      if (!flatpak_dir_ensure_repo (self, cancellable, error))
        return FALSE;

      if (!ostree_repo_remote_get_url (self->repo,
                                       state->remote_name,
                                       &url,
                                       error))
        return FALSE;

      helper_flags = FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE;

      if (!ostree_repo_remote_get_gpg_verify_summary (self->repo, state->remote_name,
                                                      &gpg_verify_summary, error))
        return FALSE;

      if (!ostree_repo_remote_get_gpg_verify (self->repo, state->remote_name,
                                              &gpg_verify, error))
        return FALSE;

      if (no_pull)
        {
        }
      else if (is_oci)
        {
          g_autoptr(FlatpakOciRegistry) registry = NULL;
          g_autoptr(GFile) registry_file = NULL;

          registry = flatpak_dir_create_system_child_oci_registry (self, &child_repo_lock, error);
          if (registry == NULL)
            return FALSE;

          registry_file = g_file_new_for_uri (flatpak_oci_registry_get_uri (registry));

          child_repo_path = g_file_get_path (registry_file);

          if (!flatpak_dir_mirror_oci (self, registry, state, ref, NULL, progress, cancellable, error))
            return FALSE;
        }
      else if ((!gpg_verify_summary && state->collection_id == NULL) || !gpg_verify)
        {
          /* The remote is not gpg verified, so we don't want to allow installation via
             a download in the home directory, as there is no way to verify you're not
             injecting anything into the remote. However, in the case of a remote
             configured to a local filesystem we can just let the system helper do
             the installation, as it can then avoid network i/o and be certain the
             data comes from the right place.

             If @collection_id is non-%NULL, we can verify the refs in commit
             metadata, so don’t need to verify the summary. */
          if (g_str_has_prefix (url, "file:"))
            helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL;
          else
            return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("Can't pull from untrusted non-gpg verified remote"));
        }
      else
        {
          /* We're pulling from a remote source, we do the network mirroring pull as a
             user and hand back the resulting data to the system-helper, that trusts us
             due to the GPG signatures in the repo */

          child_repo = flatpak_dir_create_system_child_repo (self, &child_repo_lock, commit, error);
          if (child_repo == NULL)
            return FALSE;

          flatpak_flags |= FLATPAK_PULL_FLAGS_SIDELOAD_EXTRA_DATA;
          if (!flatpak_dir_pull (self, state, ref, commit, results, subpaths, require_metadata, token,
                                 child_repo,
                                 flatpak_flags, OSTREE_REPO_PULL_FLAGS_MIRROR,
                                 progress, cancellable, error))
            return FALSE;

          if (!child_repo_ensure_summary (child_repo, state, cancellable, error))
            return FALSE;

          child_repo_path = g_file_get_path (ostree_repo_get_path (child_repo));
        }

      if (no_deploy)
        helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY;

      if (app_hint)
        helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_APP_HINT;

      if (install_hint)
        helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_INSTALL_HINT;

      if (!flatpak_dir_system_helper_call_deploy (self,
                                                  child_repo_path ? child_repo_path : "",
                                                  helper_flags, ref, state->remote_name,
                                                  subpaths,
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
      if (!flatpak_dir_pull (self, state, ref, commit, results, subpaths, require_metadata, token,
                             NULL, flatpak_flags, OSTREE_REPO_PULL_FLAGS_NONE,
                             progress, cancellable, error))
        return FALSE;
    }

  if (!no_deploy)
    {
      if (!flatpak_dir_deploy_update (self, ref,
                                      /* We don't know the local commit id in the OCI case, and
                                         we only support one version anyway */
                                      is_oci ? NULL : commit,
                                      subpaths,
                                      cancellable, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_dir_uninstall (FlatpakDir                 *self,
                       const char                 *ref,
                       FlatpakHelperUninstallFlags flags,
                       GCancellable               *cancellable,
                       GError                    **error)
{
  const char *repository;
  g_autofree char *current_ref = NULL;
  gboolean was_deployed;
  gboolean is_app;
  const char *name;
  g_autofree char *old_active = NULL;

  g_auto(GStrv) parts = NULL;
  g_auto(GLnxLockFile) lock = { 0, };
  g_autoptr(GVariant) deploy_data = NULL;
  gboolean keep_ref = flags & FLATPAK_HELPER_UNINSTALL_FLAGS_KEEP_REF;
  gboolean force_remove = flags & FLATPAK_HELPER_UNINSTALL_FLAGS_FORCE_REMOVE;

  parts = flatpak_decompose_ref (ref, error);
  if (parts == NULL)
    return FALSE;
  name = parts[1];

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      const char *installation = flatpak_dir_get_id (self);

      if (!flatpak_dir_system_helper_call_uninstall (self,
                                                     flags, ref,
                                                     installation ? installation : "",
                                                     cancellable, error))
        return FALSE;

      return TRUE;
    }

  if (!flatpak_dir_lock (self, &lock,
                         cancellable, error))
    return FALSE;

  deploy_data = flatpak_dir_get_deploy_data (self, ref, FLATPAK_DEPLOY_VERSION_ANY,
                                             cancellable, error);
  if (deploy_data == NULL)
    return FALSE;

  repository = flatpak_deploy_data_get_origin (deploy_data);
  if (repository == NULL)
    return FALSE;

  if (g_str_has_prefix (ref, "runtime/") && !force_remove)
    {
      g_auto(GStrv) app_refs = NULL;
      g_autoptr(GPtrArray) blocking = g_ptr_array_new_with_free_func (g_free);
      const char *pref = ref + strlen ("runtime/");
      int i;

      /* Look for apps that need this runtime */

      flatpak_dir_list_refs (self, "app", &app_refs, NULL, NULL);
      for (i = 0; app_refs != NULL && app_refs[i] != NULL; i++)
        {
          g_autoptr(GVariant) deploy_data = flatpak_dir_get_deploy_data (self, app_refs[i], FLATPAK_DEPLOY_VERSION_ANY, NULL, NULL);

          if (deploy_data)
            {
              const char *app_runtime = flatpak_deploy_data_get_runtime (deploy_data);

              if (g_strcmp0 (app_runtime, pref) == 0)
                g_ptr_array_add (blocking, g_strdup (app_refs[i] + strlen ("app/")));
            }
        }
      g_ptr_array_add (blocking, NULL);

      if (blocking->len > 1)
        {
          g_autofree char *joined = g_strjoinv (", ", (char **) blocking->pdata);
          return flatpak_fail_error (error, FLATPAK_ERROR_RUNTIME_USED,
                                     _("Can't remove %s, it is needed for: %s"), pref, joined);
        }
    }

  old_active = g_strdup (flatpak_deploy_data_get_commit (deploy_data));

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

  if (!flatpak_dir_update_deploy_ref (self, ref, NULL, error))
    return FALSE;

  if (!flatpak_dir_undeploy_all (self, ref, force_remove, &was_deployed, cancellable, error))
    return FALSE;

  if (!keep_ref &&
      !flatpak_dir_remove_ref (self, repository, ref, cancellable, error))
    return FALSE;

  if (is_app &&
      !flatpak_dir_update_exports (self, name, cancellable, error))
    return FALSE;

  glnx_release_lock_file (&lock);

  flatpak_dir_prune_origin_remote (self, repository);

  flatpak_dir_cleanup_removed (self, cancellable, NULL);

  if (!flatpak_dir_mark_changed (self, error))
    return FALSE;

  if (!was_deployed)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   _("%s branch %s is not installed"), name, parts[3]);
      return FALSE;
    }

  flatpak_dir_log (self, "uninstall", NULL, ref, NULL, old_active, NULL,
                   "Uninstalled %s", ref);

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
                           char       ***deployed_ids,
                           GCancellable *cancellable,
                           GError      **error)
{
  gboolean ret = FALSE;

  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GPtrArray) ids = NULL;
  GError *temp_error = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFile) child = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  g_autoptr(GError) my_error = NULL;

  deploy_base = flatpak_dir_get_deploy_dir (self, ref);

  ids = g_ptr_array_new_with_free_func (g_free);

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
        g_ptr_array_add (ids, g_strdup (name));

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
      g_ptr_array_add (ids, NULL);
      *deployed_ids = (char **) g_ptr_array_free (g_steal_pointer (&ids), FALSE);
    }

  return ret;

}

static gboolean
dir_is_locked (GFile *dir)
{
  glnx_autofd int ref_fd = -1;
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
                      const char   *active_id,
                      gboolean      is_update,
                      gboolean      force_remove,
                      GCancellable *cancellable,
                      GError      **error)
{
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) checkoutdir = NULL;
  g_autoptr(GFile) removed_subdir = NULL;
  g_autoptr(GFile) removed_dir = NULL;
  g_autofree char *dirname = NULL;
  g_autofree char *current_active = NULL;
  g_autoptr(GFile) change_file = NULL;
  g_autoptr(GError) child_error = NULL;
  g_auto(GStrv) ref_parts = NULL;
  int i, retry;

  g_assert (ref != NULL);
  g_assert (active_id != NULL);

  deploy_base = flatpak_dir_get_deploy_dir (self, ref);

  checkoutdir = g_file_get_child (deploy_base, active_id);
  if (!g_file_query_exists (checkoutdir, cancellable))
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   _("%s commit %s not installed"), ref, active_id);
      return FALSE;
    }

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  current_active = flatpak_dir_read_active (self, ref, cancellable);
  if (current_active != NULL && strcmp (current_active, active_id) == 0)
    {
      g_auto(GStrv) deployed_ids = NULL;
      const char *some_deployment;

      /* We're removing the active deployment, start by repointing that
         to another deployment if one exists */

      if (!flatpak_dir_list_deployed (self, ref,
                                      &deployed_ids,
                                      cancellable, error))
        return FALSE;

      some_deployment = NULL;
      for (i = 0; deployed_ids[i] != NULL; i++)
        {
          if (strcmp (deployed_ids[i], active_id) == 0)
            continue;

          some_deployment = deployed_ids[i];
          break;
        }

      if (!flatpak_dir_set_active (self, ref, some_deployment, cancellable, error))
        return FALSE;
    }

  removed_dir = flatpak_dir_get_removed_dir (self);
  if (!flatpak_mkdir_p (removed_dir, cancellable, error))
    return FALSE;

  ref_parts = g_strsplit (ref, "/", -1);
  dirname = g_strdup_printf ("%s-%s", ref_parts[1], active_id);

  removed_subdir = g_file_get_child (removed_dir, dirname);

  retry = 0;
  while (TRUE)
    {
      g_autoptr(GError) local_error = NULL;
      g_autoptr(GFile) tmpdir = NULL;
      g_autofree char *tmpname = NULL;

      if (flatpak_file_rename (checkoutdir,
                               removed_subdir,
                               cancellable, &local_error))
        break;

      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS) || retry >= 10)
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }

      retry++;

      /* Destination already existed, move that aside, as we want to use the exact
       * removed dirname for the latest undeployed version */

      tmpname = g_strdup_printf ("%s-XXXXXX", dirname);
      glnx_gen_temp_name (tmpname);
      tmpdir = g_file_get_child (removed_dir, tmpname);

      if (!flatpak_file_rename (removed_subdir,
                                tmpdir,
                                cancellable, error))
        return FALSE;
    }


  if (is_update)
    change_file = g_file_resolve_relative_path (removed_subdir, "files/.updated");
  else
    change_file = g_file_resolve_relative_path (removed_subdir, "files/.removed");

  if (!g_file_replace_contents (change_file, "", 0, NULL, FALSE,
                                G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, &child_error))
    {
      g_autofree gchar *path = g_file_get_path (change_file);
      g_warning ("Unable to clear %s: %s", path, child_error->message);
      g_clear_error (&child_error);
    }

  if (force_remove || !dir_is_locked (removed_subdir))
    {
      GError *tmp_error = NULL;

      if (!flatpak_rm_rf (removed_subdir, cancellable, &tmp_error))
        {
          g_warning ("Unable to remove old checkout: %s", tmp_error->message);
          g_error_free (tmp_error);
        }
    }

  return TRUE;
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

/**
 * flatpak_dir_remove_ref:
 * @self: a #FlatpakDir
 * @remote_name: the name of the remote
 * @ref: the flatpak ref to remove
 * @cancellable: (nullable) (optional): a #GCancellable
 * @error: a #GError
 *
 * Remove the flatpak ref given by @remote_name:@ref from the underlying
 * OSTree repo. Attempting to remove a ref that is currently deployed
 * is an error, you need to uninstall the flatpak first. Note that this does
 * not remove the objects bound to @ref from the disk, you will need to
 * call flatpak_dir_prune() to do that.
 *
 * Returns: %TRUE if removing the ref succeeded, %FALSE otherwise.
 */
gboolean
flatpak_dir_remove_ref (FlatpakDir   *self,
                        const char   *remote_name,
                        const char   *ref,
                        GCancellable *cancellable,
                        GError      **error)
{
  if (flatpak_dir_use_system_helper (self, NULL))
    {
      const char *installation = flatpak_dir_get_id (self);

      if (!flatpak_dir_system_helper_call_remove_local_ref (self,
                                                            FLATPAK_HELPER_REMOVE_LOCAL_REF_FLAGS_NONE,
                                                            remote_name,
                                                            ref,
                                                            installation ? installation : "",
                                                            cancellable,
                                                            error))
        return FALSE;

      return TRUE;
    }

  if (!ostree_repo_set_ref_immediate (self->repo,
                                      remote_name,
                                      ref,
                                      NULL,
                                      cancellable,
                                      error))
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
              g_warning ("Unable to remove old checkout: %s", tmp_error->message);
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
  g_autoptr(GError) lock_error = NULL;
  g_auto(GLnxLockFile) lock = { 0, };

  if (error == NULL)
    error = &local_error;

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      const char *installation = flatpak_dir_get_id (self);

      if (!flatpak_dir_system_helper_call_prune_local_repo (self,
                                                            FLATPAK_HELPER_PRUNE_LOCAL_REPO_FLAGS_NONE,
                                                            installation ? installation : "",
                                                            cancellable,
                                                            error))
        return FALSE;

      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    goto out;

  /* This could remove objects, so take an exclusive repo lock */
  if (!flatpak_dir_repo_lock (self, &lock, LOCK_EX | LOCK_NB, cancellable, &lock_error))
    {
      /* If we can't get an exclusive lock, don't block for a long time. Eventually
         the shared lock operation is released and we will do a prune then */
      if (g_error_matches (lock_error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
        {
          g_debug ("Skipping prune due to in progress operation");
          return TRUE;
        }

      g_propagate_error (error, g_steal_pointer (&lock_error));
      return FALSE;
    }

  g_debug ("Pruning repo");
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
    g_print (_("Pruning repo failed: %s"), local_error->message);

  return ret;

}

gboolean
flatpak_dir_update_summary (FlatpakDir   *self,
                            GCancellable *cancellable,
                            GError      **error)
{
  g_auto(GLnxLockFile) lock = { 0, };

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      const char *installation = flatpak_dir_get_id (self);

      return flatpak_dir_system_helper_call_update_summary (self,
                                                            FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_NONE,
                                                            installation ? installation : "",
                                                            cancellable,
                                                            error);
    }

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  /* Keep a shared repo lock to avoid prunes removing objects we're relying on
   * while generating the summary. */
  if (!flatpak_dir_repo_lock (self, &lock, LOCK_SH, cancellable, error))
    return FALSE;

  g_debug ("Updating summary");
  return ostree_repo_regenerate_summary (self->repo, NULL, cancellable, error);
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

  /* Maybe it was removed but is still living? */
  if (checksum != NULL)
    {
      g_autoptr(GFile) removed_dir = flatpak_dir_get_removed_dir (self);
      g_autoptr(GFile) removed_deploy_dir = NULL;
      g_auto(GStrv) ref_parts = NULL;
      g_autofree char *dirname = NULL;

      ref_parts = g_strsplit (ref, "/", -1);
      dirname = g_strdup_printf ("%s-%s", ref_parts[1], checksum);
      removed_deploy_dir = g_file_get_child (removed_dir, dirname);

      if (g_file_query_file_type (removed_deploy_dir, G_FILE_QUERY_INFO_NONE, cancellable) == G_FILE_TYPE_DIRECTORY)
        return g_object_ref (removed_deploy_dir);
    }

  return NULL;
}

GFile *
flatpak_dir_get_unmaintained_extension_dir_if_exists (FlatpakDir   *self,
                                                      const char   *name,
                                                      const char   *arch,
                                                      const char   *branch,
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

/* FIXME: Move all this caching into libostree. */
static void
cached_summary_free (CachedSummary *summary)
{
  g_bytes_unref (summary->bytes);
  if (summary->bytes_sig)
    g_bytes_unref (summary->bytes_sig);
  g_free (summary->remote);
  g_free (summary->url);
  g_free (summary);
}

static CachedSummary *
cached_summary_new (GBytes     *bytes,
                    GBytes     *bytes_sig,
                    const char *remote,
                    const char *url)
{
  CachedSummary *summary = g_new0 (CachedSummary, 1);

  summary->bytes = g_bytes_ref (bytes);
  if (bytes_sig)
    summary->bytes_sig = g_bytes_ref (bytes_sig);
  summary->url = g_strdup (url);
  summary->remote = g_strdup (remote);
  summary->time = g_get_monotonic_time ();
  return summary;
}

static gboolean
flatpak_dir_lookup_cached_summary (FlatpakDir *self,
                                   GBytes    **bytes_out,
                                   GBytes    **bytes_sig_out,
                                   const char *name,
                                   const char *url)
{
  CachedSummary *summary;
  gboolean res = FALSE;

  G_LOCK (cache);

  if (self->summary_cache == NULL)
    self->summary_cache = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) cached_summary_free);

  summary = g_hash_table_lookup (self->summary_cache, name);
  if (summary)
    {
      guint64 now = g_get_monotonic_time ();
      if ((now - summary->time) < (1000 * 1000 * (SUMMARY_CACHE_TIMEOUT_SEC)) &&
          strcmp (url, summary->url) == 0)
        {
          /* g_debug ("Using cached summary for remote %s", name); */
          *bytes_out = g_bytes_ref (summary->bytes);
          if (bytes_sig_out)
            {
              if (summary->bytes_sig)
                *bytes_sig_out = g_bytes_ref (summary->bytes_sig);
              else
                *bytes_sig_out = NULL;
            }
          res = TRUE;
        }
    }

  G_UNLOCK (cache);

  return res;
}

static void
flatpak_dir_cache_summary (FlatpakDir *self,
                           GBytes     *bytes,
                           GBytes     *bytes_sig,
                           const char *name,
                           const char *url)
{
  CachedSummary *summary;

  /* No sense caching the summary if there isn't one */
  if (!bytes)
    return;

  G_LOCK (cache);

  /* This was already initialized in the cache-miss lookup */
  g_assert (self->summary_cache != NULL);

  summary = cached_summary_new (bytes, bytes_sig, name, url);
  g_hash_table_replace (self->summary_cache, summary->remote, summary);

  G_UNLOCK (cache);
}

gboolean
flatpak_dir_remote_make_oci_summary (FlatpakDir   *self,
                                     const char   *remote,
                                     GBytes      **out_summary,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  g_autoptr(GVariant) summary = NULL;
  g_autoptr(GFile) index_cache = NULL;
  g_autofree char *index_uri = NULL;
  g_autoptr(GFile) summary_cache = NULL;
  g_autofree char *self_name = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GMappedFile) mfile = NULL;
  g_autoptr(GBytes) cache_bytes = NULL;
  g_autoptr(GBytes) summary_bytes = NULL;

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      const char *installation = flatpak_dir_get_id (self);

      if (!flatpak_dir_system_helper_call_generate_oci_summary (self,
                                                                FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_NONE,
                                                                remote,
                                                                installation ? installation : "",
                                                                cancellable, error))
        return FALSE;

      summary_cache = flatpak_dir_get_oci_summary_location (self, remote, error);
      if (summary_cache == NULL)
        return FALSE;
    }
  else
    {
      self_name = flatpak_dir_get_name (self);

      index_cache = flatpak_dir_update_oci_index (self, remote, &index_uri, cancellable, error);
      if (index_cache == NULL)
        return FALSE;

      summary_cache = flatpak_dir_get_oci_summary_location (self, remote, error);
      if (summary_cache == NULL)
        return FALSE;

      if (!check_destination_mtime (index_cache, summary_cache, cancellable))
        {
          summary = flatpak_oci_index_make_summary (index_cache, index_uri, cancellable, &local_error);
          if (summary == NULL)
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }

          summary_bytes = g_variant_get_data_as_bytes (summary);

          if (!g_file_replace_contents (summary_cache,
                                        g_bytes_get_data (summary_bytes, NULL),
                                        g_bytes_get_size (summary_bytes),
                                        NULL, FALSE, 0, NULL, cancellable, error))
            {
              g_prefix_error (error, _("Failed to write summary cache: "));
              return FALSE;
            }

          if (out_summary)
              *out_summary = g_steal_pointer (&summary_bytes);
          return TRUE;
        }
    }

  if (out_summary)
    {
      mfile = g_mapped_file_new (flatpak_file_get_path_cached (summary_cache), FALSE, error);
      if (mfile == NULL)
        return FALSE;

      cache_bytes = g_mapped_file_get_bytes (mfile);
      *out_summary = g_steal_pointer (&cache_bytes);
    }

  return TRUE;
}

static gboolean
flatpak_dir_remote_fetch_summary (FlatpakDir   *self,
                                  const char   *name_or_uri,
                                  GBytes      **out_summary,
                                  GBytes      **out_summary_sig,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  g_autofree char *url = NULL;
  gboolean is_local;

  g_autoptr(GError) local_error = NULL;
  g_autoptr(GBytes) summary = NULL;
  g_autoptr(GBytes) summary_sig = NULL;

  if (!ostree_repo_remote_get_url (self->repo, name_or_uri, &url, error))
    return FALSE;

  if (!g_str_has_prefix (name_or_uri, "file:") && flatpak_dir_get_remote_disabled (self, name_or_uri))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Can't fetch summary from disabled remote ‘%s’", name_or_uri);
      return FALSE;
    }

  is_local = g_str_has_prefix (url, "file:");

  /* No caching for local files */
  if (!is_local)
    {
      if (flatpak_dir_lookup_cached_summary (self, out_summary, out_summary_sig, name_or_uri, url))
        return TRUE;
    }

  /* Seems ostree asserts if this is NULL */
  if (error == NULL)
    error = &local_error;

  if (flatpak_dir_get_remote_oci (self, name_or_uri))
    {
      if (!flatpak_dir_remote_make_oci_summary (self, name_or_uri,
                                                &summary,
                                                cancellable,
                                                error))
        return FALSE;
    }
  else
    {
      g_debug ("Fetching summary file for remote ‘%s’", name_or_uri);
      if (!ostree_repo_remote_fetch_summary (self->repo, name_or_uri,
                                             &summary, &summary_sig,
                                             cancellable,
                                             error))
        return FALSE;
    }

  if (summary == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Remote listing for %s not available; server has no summary file. Check the URL passed to remote-add was valid."), name_or_uri);

  if (!is_local)
    flatpak_dir_cache_summary (self, summary, summary_sig, name_or_uri, url);

  *out_summary = g_steal_pointer (&summary);
  if (out_summary_sig)
    *out_summary_sig = g_steal_pointer (&summary_sig);

  return TRUE;
}

static FlatpakRemoteState *
_flatpak_dir_get_remote_state (FlatpakDir   *self,
                               const char   *remote_or_uri,
                               gboolean      optional,
                               gboolean      local_only,
                               GBytes       *opt_summary,
                               GBytes       *opt_summary_sig,
                               GCancellable *cancellable,
                               GError      **error)
{
  g_autoptr(FlatpakRemoteState) state = flatpak_remote_state_new ();
  g_autoptr(GError) my_error = NULL;
  gboolean is_local;

  if (error == NULL)
    error = &my_error;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return NULL;

  state->remote_name = g_strdup (remote_or_uri);
  is_local = g_str_has_prefix (remote_or_uri, "file:");
  if (!is_local)
    {
      if (!flatpak_dir_has_remote (self, remote_or_uri, error))
        return NULL;
      if (!repo_get_remote_collection_id (self->repo, remote_or_uri, &state->collection_id, error))
        return NULL;
    }

  if (local_only)
    {
      flatpak_fail (&state->summary_fetch_error, "Internal error, local_only state");
      flatpak_fail (&state->metadata_fetch_error, "Internal error, local_only state");
      return g_steal_pointer (&state);
    }

  if (opt_summary)
    {
      if (opt_summary_sig)
        {
          /* If specified, must be valid signature */
          g_autoptr(OstreeGpgVerifyResult) gpg_result =
            ostree_repo_verify_summary (self->repo,
                                        state->remote_name,
                                        opt_summary,
                                        opt_summary_sig,
                                        NULL, error);
          if (gpg_result == NULL ||
              !ostree_gpg_verify_result_require_valid_signature (gpg_result, error))
            return NULL;

          state->summary_sig_bytes = g_bytes_ref (opt_summary_sig);
        }
      state->summary = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                                                     opt_summary, FALSE));
    }
  else
    {
      g_autoptr(GError) local_error = NULL;
      g_autoptr(GBytes) summary_bytes = NULL;
      g_autoptr(GBytes) summary_sig_bytes = NULL;

      if (flatpak_dir_remote_fetch_summary (self, remote_or_uri, &summary_bytes, &summary_sig_bytes,
                                            cancellable, &local_error))
        {
          state->summary_sig_bytes = g_steal_pointer (&summary_sig_bytes);
          state->summary = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                                                         summary_bytes, FALSE));
        }
      else
        {
          if (optional)
            {
              state->summary_fetch_error = g_steal_pointer (&local_error);
              g_debug ("Failed to download optional summary");
            }
          else
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return NULL;
            }
        }
    }

  if (state->collection_id == NULL)
    {
      if (state->summary != NULL) /* In the optional case we might not have a summary */
        state->metadata = g_variant_get_child_value (state->summary, 1);
    }
  else
    {
      g_autofree char *latest_rev = NULL;
      g_autoptr(GVariant) commit_v = NULL;
      g_autoptr(GError) local_error = NULL;

      /* Make sure the branch is up to date, but ignore downgrade errors (see
       * below for the explanation). */
      if (!_flatpak_dir_fetch_remote_state_metadata_branch (self, state, cancellable, &local_error) &&
          !g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_DOWNGRADE))
        {
          if (optional)
            {
              /* This happens for instance in the case where a p2p remote is invalid (wrong signature)
                 and we should just silently fail to update to it. */
              state->metadata_fetch_error = g_steal_pointer (&local_error);
              g_debug ("Failed to download optional metadata");
            }
          else
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return NULL;
            }
        }
      else
        {
          if (g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_DOWNGRADE))
            {
              /* The latest metadata available is a downgrade, which means we're offline and using a
               * LAN/USB source. Downgrading the metadata in the system repo would be a security
               * risk, so instead ignore the downgrade and use the later metadata.  There's some
               * chance its information won't be accurate for the refs that are pulled, but using
               * the old metadata wouldn't always be correct either because there's no guarantee the
               * refs will be pulled from the same peer source as the metadata. Long term, we should
               * figure out how to rely less on it. */
              g_debug ("Ignoring downgrade of ostree-metadata; using the newer one instead");
            }

          /* Look up the commit containing the latest repository metadata. */
          latest_rev = flatpak_dir_read_latest (self, remote_or_uri, OSTREE_REPO_METADATA_REF,
                                                NULL, cancellable, error);
          if (latest_rev == NULL)
            return NULL;

          if (!ostree_repo_load_commit (self->repo, latest_rev, &commit_v, NULL, error))
            return NULL;

          state->metadata = g_variant_get_child_value (commit_v, 0);
        }
    }

  return g_steal_pointer (&state);
}

FlatpakRemoteState *
flatpak_dir_get_remote_state (FlatpakDir   *self,
                              const char   *remote,
                              GCancellable *cancellable,
                              GError      **error)
{
  return _flatpak_dir_get_remote_state (self, remote, FALSE, FALSE, NULL, NULL, cancellable, error);
}

/* This is an alternative way to get the state where the summary is
 * from elsewhere. It is mainly used by the system-helper where the
 * summary is from the user-mode part which downloaded an update
 *
 * It will verify the summary if a signature is passed in, but not
 * otherwise.
 **/
FlatpakRemoteState *
flatpak_dir_get_remote_state_for_summary (FlatpakDir   *self,
                                          const char   *remote,
                                          GBytes       *opt_summary,
                                          GBytes       *opt_summary_sig,
                                          GCancellable *cancellable,
                                          GError      **error)
{
  return _flatpak_dir_get_remote_state (self, remote, FALSE, FALSE, opt_summary, opt_summary_sig, cancellable, error);
}

/* This is an alternative way to get the remote state that doesn't
 * error out if the summary or metadata is not available.
 * For example, we want to be able to update an app even when
 * we can't talk to the main repo, but there is a local (p2p/sdcard)
 * source for apps, and we want to be able to deploy a ref without pulling it,
 * e.g. because we are installing with FLATPAK_INSTALL_FLAGS_NO_PULL, and we
 * already pulled it out of band beforehand.
 */
FlatpakRemoteState *
flatpak_dir_get_remote_state_optional (FlatpakDir   *self,
                                       const char   *remote,
                                       GCancellable *cancellable,
                                       GError      **error)
{
  return _flatpak_dir_get_remote_state (self, remote, TRUE, FALSE, NULL, NULL, cancellable, error);
}


/* This doesn't do any i/o at all, just keeps track of the local details like
   remote and collection-id. Useful when doing no-pull operations */
FlatpakRemoteState *
flatpak_dir_get_remote_state_local_only (FlatpakDir   *self,
                                         const char   *remote,
                                         GCancellable *cancellable,
                                         GError      **error)
{
  return _flatpak_dir_get_remote_state (self, remote, TRUE, TRUE, NULL, NULL, cancellable, error);
}

static gboolean
flatpak_dir_remote_has_ref (FlatpakDir *self,
                            const char *remote,
                            const char *ref)
{
  g_autoptr(GError) local_error = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;

  state = flatpak_dir_get_remote_state_optional (self, remote, NULL, &local_error);
  if (state == NULL)
    {
      g_debug ("Can't get state for remote %s: %s", remote, local_error->message);
      return FALSE;
    }

  return flatpak_remote_state_lookup_ref (state, ref, NULL, NULL, NULL);
}

static void
populate_hash_table_from_refs_map (GHashTable *ret_all_refs, GVariant *ref_map,
                                   const gchar *collection_id)
{
  GVariant *value;
  GVariantIter ref_iter;

  g_variant_iter_init (&ref_iter, ref_map);
  while ((value = g_variant_iter_next_value (&ref_iter)) != NULL)
    {
      /* helper for being able to auto-free the value */
      g_autoptr(GVariant) child = value;
      const char *ref_name = NULL;

      g_variant_get_child (child, 0, "&s", &ref_name);
      if (ref_name == NULL)
        continue;

      g_autoptr(GVariant) csum_v = NULL;
      char tmp_checksum[65];
      const guchar *csum_bytes;
      FlatpakCollectionRef *ref;

      g_variant_get_child (child, 1, "(t@aya{sv})", NULL, &csum_v, NULL);
      csum_bytes = ostree_checksum_bytes_peek_validate (csum_v, NULL);
      if (csum_bytes == NULL)
        continue;

      ref = flatpak_collection_ref_new (collection_id, ref_name);
      ostree_checksum_inplace_from_bytes (csum_bytes, tmp_checksum);

      g_hash_table_insert (ret_all_refs, ref, g_strdup (tmp_checksum));
    }
}

/* This duplicates ostree_repo_remote_list_refs so it can use
 * flatpak_remote_state_ensure_summary and get caching. */
gboolean
flatpak_dir_list_all_remote_refs (FlatpakDir         *self,
                                  FlatpakRemoteState *state,
                                  GHashTable        **out_all_refs,
                                  GCancellable       *cancellable,
                                  GError            **error)
{
  g_autoptr(GHashTable) ret_all_refs = NULL;
  g_autoptr(GVariant) ref_map = NULL;
  g_autoptr(GVariant) exts = NULL;
  g_autoptr(GVariant) collection_map = NULL;
  const gchar *collection_id;
  GVariantIter iter;

  ret_all_refs = g_hash_table_new_full (flatpak_collection_ref_hash,
                                        flatpak_collection_ref_equal,
                                        (GDestroyNotify) flatpak_collection_ref_free,
                                        g_free);

  /* If the remote has P2P enabled and we're offline, get the refs list from
   * xa.cache in ostree-metadata (although it's inferior to the summary refs
   * list in that it lacks checksums). */
  if (state->collection_id != NULL && state->summary == NULL)
    {
      g_autoptr(GVariant) xa_cache = NULL;
      g_autoptr(GVariant) cache = NULL;
      gsize i, n;

      if (!flatpak_remote_state_ensure_metadata (state, error))
        return FALSE;

      if (!flatpak_remote_state_lookup_repo_metadata (state, "xa.cache", "@*", &xa_cache))
        return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("No summary or Flatpak cache available for remote %s"),
                                   state->remote_name);

      cache = g_variant_get_child_value (xa_cache, 0);
      n = g_variant_n_children (cache);
      for (i = 0; i < n; i++)
        {
          g_autoptr(GVariant) child = NULL;
          g_autoptr(GVariant) cur_v = NULL;
          g_autoptr(FlatpakCollectionRef) coll_ref = NULL;
          const char *ref;

          child = g_variant_get_child_value (cache, i);
          cur_v = g_variant_get_child_value (child, 0);
          ref = g_variant_get_string (cur_v, NULL);
          coll_ref = flatpak_collection_ref_new (state->collection_id, ref);

          g_hash_table_insert (ret_all_refs, g_steal_pointer (&coll_ref), NULL);
        }

      goto out;
    }

  if (!flatpak_remote_state_ensure_summary (state, error))
    return FALSE;

  /* refs that match the main collection-id */
  ref_map = g_variant_get_child_value (state->summary, 0);

  exts = g_variant_get_child_value (state->summary, 1);

  if (!g_variant_lookup (exts, "ostree.summary.collection-id", "&s", &collection_id))
    collection_id = NULL;

  populate_hash_table_from_refs_map (ret_all_refs, ref_map, collection_id);

  /* refs that match other collection-ids */
  collection_map = g_variant_lookup_value (exts, "ostree.summary.collection-map",
                                           G_VARIANT_TYPE ("a{sa(s(taya{sv}))}"));
  if (collection_map != NULL)
    {
      g_variant_iter_init (&iter, collection_map);
      while (g_variant_iter_loop (&iter, "{&s@a(s(taya{sv}))}", &collection_id, &ref_map))
        populate_hash_table_from_refs_map (ret_all_refs, ref_map, collection_id);
    }


out:
  *out_all_refs = g_steal_pointer (&ret_all_refs);

  return TRUE;
}

/* Guarantees to return refs which are decomposable. */
static GPtrArray *
find_matching_refs (GHashTable           *refs,
                    const char           *opt_name,
                    const char           *opt_branch,
                    const char           *opt_default_branch,
                    const char           *opt_arch,
                    const char           *opt_default_arch,
                    const char           *opt_collection_id,
                    FlatpakKinds          kinds,
                    FindMatchingRefsFlags flags,
                    GError              **error)
{
  g_autoptr(GPtrArray) matched_refs = NULL;
  const char **arches = flatpak_get_arches ();
  const char *opt_arches[] = {opt_arch, NULL};
  GHashTableIter hash_iter;
  gpointer key;
  g_autoptr(GError) local_error = NULL;
  gboolean found_exact_name_match = FALSE;
  gboolean found_default_branch_match = FALSE;
  gboolean found_default_arch_match = FALSE;

  if (opt_arch != NULL)
    arches = opt_arches;

  if (opt_name && !(flags & FIND_MATCHING_REFS_FLAGS_FUZZY) &&
      !flatpak_is_valid_name (opt_name, &local_error))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("'%s' is not a valid name: %s"), opt_name, local_error->message);
      return NULL;
    }

  if (opt_branch && !flatpak_is_valid_branch (opt_branch, &local_error))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("'%s' is not a valid branch name: %s"), opt_branch, local_error->message);
      return NULL;
    }

  matched_refs = g_ptr_array_new_with_free_func (g_free);

  g_hash_table_iter_init (&hash_iter, refs);
  while (g_hash_table_iter_next (&hash_iter, &key, NULL))
    {
      g_autofree char *ref = NULL;
      g_auto(GStrv) parts = NULL;
      gboolean is_app, is_runtime;
      FlatpakCollectionRef *coll_ref = key;

      /* Unprefix any remote name if needed */
      ostree_parse_refspec (coll_ref->ref_name, NULL, &ref, NULL);
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

      if ((flags & FIND_MATCHING_REFS_FLAGS_FUZZY) && !flatpak_id_has_subref_suffix (parts[1]))
        {
          /* See if the given name looks similar to this ref name. The
           * Levenshtein distance constant was chosen pretty arbitrarily. */
          if (opt_name != NULL && strcasestr (parts[1], opt_name) == NULL &&
              flatpak_levenshtein_distance (opt_name, parts[1]) > 2)
            continue;
        }
      else
        {
          if (opt_name != NULL && strcmp (opt_name, parts[1]) != 0)
            continue;
        }

      if (!g_strv_contains (arches, parts[2]))
        continue;

      if (opt_branch != NULL && strcmp (opt_branch, parts[3]) != 0)
        continue;

      if (opt_collection_id != NULL && strcmp (opt_collection_id, coll_ref->collection_id))
        continue;

      if (opt_name != NULL && strcmp (opt_name, parts[1]) == 0)
        found_exact_name_match = TRUE;

      if (opt_default_arch != NULL && strcmp (opt_default_arch, parts[2]) == 0)
        found_default_arch_match = TRUE;

      if (opt_default_branch != NULL && strcmp (opt_default_branch, parts[3]) == 0)
        found_default_branch_match = TRUE;

      if (flags & FIND_MATCHING_REFS_FLAGS_KEEP_REMOTE)
        g_ptr_array_add (matched_refs, g_strdup (coll_ref->ref_name));
      else
        g_ptr_array_add (matched_refs, g_steal_pointer (&ref));
    }

  /* Don't show fuzzy matches if we found at least one exact name match, and
   * enforce the default arch/branch */
  if (found_exact_name_match || found_default_arch_match || found_default_branch_match)
    {
      guint i;

      /* Walk through the array backwards so we can safely remove */
      for (i = matched_refs->len; i > 0; i--)
        {
          const char *matched_refspec = g_ptr_array_index (matched_refs, i - 1);
          g_auto(GStrv) matched_parts = NULL;
          g_autofree char *matched_ref = NULL;

          ostree_parse_refspec (matched_refspec, NULL, &matched_ref, NULL);
          matched_parts = flatpak_decompose_ref (matched_ref, NULL);

          if (found_exact_name_match && strcmp (matched_parts[1], opt_name) != 0)
            g_ptr_array_remove_index (matched_refs, i - 1);
          else if (found_default_arch_match && strcmp (matched_parts[2], opt_default_arch) != 0)
            g_ptr_array_remove_index (matched_refs, i - 1);
          else if (found_default_branch_match && strcmp (matched_parts[3], opt_default_branch) != 0)
            g_ptr_array_remove_index (matched_refs, i - 1);
        }
    }

  return g_steal_pointer (&matched_refs);
}


static char *
find_matching_ref (GHashTable  *refs,
                   const char  *name,
                   const char  *opt_branch,
                   const char  *opt_default_branch,
                   const char  *opt_arch,
                   const char  *opt_collection_id,
                   FlatpakKinds kinds,
                   GError     **error)
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

      matched_refs = find_matching_refs (refs,
                                         name,
                                         opt_branch,
                                         opt_default_branch,
                                         arches[i],
                                         NULL,
                                         opt_collection_id,
                                         kinds,
                                         FIND_MATCHING_REFS_FLAGS_NONE,
                                         error);
      if (matched_refs == NULL)
        return NULL;

      if (matched_refs->len == 0)
        continue;

      if (matched_refs->len == 1)
        return g_strdup (g_ptr_array_index (matched_refs, 0));

      /* Nothing to do other than reporting the different choices */
      g_autoptr(GString) err = g_string_new ("");
      g_string_printf (err, _("Multiple branches available for %s, you must specify one of: "), name);
      g_ptr_array_sort (matched_refs, flatpak_strcmp0_ptr);
      for (j = 0; j < matched_refs->len; j++)
        {
          g_auto(GStrv) parts = flatpak_decompose_ref (g_ptr_array_index (matched_refs, j), NULL);
          g_assert (parts != NULL);
          if (j != 0)
            g_string_append (err, ", ");

          g_string_append (err,
                           g_strdup_printf ("%s/%s/%s",
                                            name,
                                            opt_arch ? opt_arch : "",
                                            parts[3]));
        }

      flatpak_fail (error, "%s", err->str);
      return NULL;
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
               _("Nothing matches %s"), name);
  return NULL;
}

char *
flatpak_dir_get_remote_collection_id (FlatpakDir *self,
                                      const char *remote_name)
{
  char *collection_id = NULL;

  if (!flatpak_dir_ensure_repo (self, NULL, NULL))
    return NULL;

  repo_get_remote_collection_id (self->repo, remote_name, &collection_id, NULL);

  return collection_id;
}

char **
flatpak_dir_find_remote_refs (FlatpakDir            *self,
                              const char            *remote,
                              const char            *name,
                              const char            *opt_branch,
                              const char            *opt_default_branch,
                              const char            *opt_arch,
                              const char            *opt_default_arch,
                              FlatpakKinds           kinds,
                              FindMatchingRefsFlags  flags,
                              GCancellable          *cancellable,
                              GError               **error)
{
  g_autofree char *collection_id = NULL;
  g_autoptr(GHashTable) remote_refs = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;
  GPtrArray *matched_refs;

  state = flatpak_dir_get_remote_state_optional (self, remote, cancellable, error);
  if (state == NULL)
    return NULL;

  if (!flatpak_dir_list_all_remote_refs (self, state,
                                         &remote_refs, cancellable, error))
    return NULL;

  collection_id = flatpak_dir_get_remote_collection_id (self, remote);
  matched_refs = find_matching_refs (remote_refs,
                                     name,
                                     opt_branch,
                                     opt_default_branch,
                                     opt_arch,
                                     opt_default_arch,
                                     collection_id,
                                     kinds,
                                     flags,
                                     error);
  if (matched_refs == NULL)
    return NULL;

  g_ptr_array_add (matched_refs, NULL);
  return (char **) g_ptr_array_free (matched_refs, FALSE);
}

static char *
find_ref_for_refs_set (GHashTable   *refs,
                       const char   *name,
                       const char   *opt_branch,
                       const char   *opt_default_branch,
                       const char   *opt_arch,
                       const char   *collection_id,
                       FlatpakKinds  kinds,
                       FlatpakKinds *out_kind,
                       GError      **error)
{
  g_autoptr(GError) my_error = NULL;
  g_autofree gchar *ref = find_matching_ref (refs,
                                             name,
                                             opt_branch,
                                             opt_default_branch,
                                             opt_arch,
                                             collection_id,
                                             kinds,
                                             &my_error);
  if (ref == NULL)
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
          if (g_str_has_prefix (ref, "app/"))
            *out_kind = FLATPAK_KINDS_APP;
          else
            *out_kind = FLATPAK_KINDS_RUNTIME;
        }

      return g_steal_pointer (&ref);
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
               _("Can't find ref %s%s%s%s%s"), name,
               (opt_arch != NULL || opt_branch != NULL) ? "/" : "",
               opt_arch ? opt_arch : "",
               opt_branch ? "/" : "",
               opt_branch ? opt_branch : "");

  return NULL;
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
  g_autofree char *collection_id = NULL;
  g_autofree char *remote_ref = NULL;

  g_autoptr(GHashTable) remote_refs = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;
  g_autoptr(GError) my_error = NULL;

  state = flatpak_dir_get_remote_state_optional (self, remote, cancellable, error);
  if (state == NULL)
    return NULL;

  if (!flatpak_dir_list_all_remote_refs (self, state,
                                         &remote_refs, cancellable, error))
    return NULL;

  collection_id = flatpak_dir_get_remote_collection_id (self, remote);
  remote_ref = find_ref_for_refs_set (remote_refs, name, opt_branch,
                                      opt_default_branch, opt_arch, collection_id,
                                      kinds, out_kind, &my_error);
  if (!remote_ref)
    {
      if (g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       _("Error searching remote %s: %s"),
                       remote,
                       my_error->message);
          return NULL;
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return NULL;
        }
    }

  return g_steal_pointer (&remote_ref);
}

static gboolean
list_collection_refs_from_ostree_repo (OstreeRepo   *repo,
                                       const char   *refspec_prefix,
                                       const char   *opt_collection_id,
                                       GHashTable  **out_all_refs,
                                       GCancellable *cancellable,
                                       GError      **error)
{
  GHashTableIter iter;
  gpointer key;
  GHashTable *coll_refs = NULL;

  g_autoptr(GHashTable) refs = NULL;

  /* FIXME: Use ostree_repo_list_collection_refs when it's public */
  if (!ostree_repo_list_refs (repo, refspec_prefix, &refs, cancellable, error))
    return FALSE;

  coll_refs = g_hash_table_new_full (flatpak_collection_ref_hash,
                                     flatpak_collection_ref_equal,
                                     (GDestroyNotify) flatpak_collection_ref_free,
                                     NULL);

  g_hash_table_iter_init (&iter, refs);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      FlatpakCollectionRef *ref = flatpak_collection_ref_new (opt_collection_id, key);
      g_hash_table_add (coll_refs, ref);
    }

  *out_all_refs = coll_refs;

  return TRUE;
}

char **
flatpak_dir_find_local_refs (FlatpakDir           *self,
                             const char           *remote,
                             const char           *name,
                             const char           *opt_branch,
                             const char           *opt_default_branch,
                             const char           *opt_arch,
                             const char           *opt_default_arch,
                             FlatpakKinds          kinds,
                             FindMatchingRefsFlags flags,
                             GCancellable          *cancellable,
                             GError               **error)
{
  g_autofree char *collection_id = NULL;
  g_autoptr(GHashTable) local_refs = NULL;
  g_autoptr(GError) my_error = NULL;
  g_autofree char *refspec_prefix = g_strconcat (remote, ":.", NULL);
  GPtrArray *matched_refs;

  if (!flatpak_dir_ensure_repo (self, NULL, error))
    return NULL;

  collection_id = flatpak_dir_get_remote_collection_id (self, remote);
  if (!list_collection_refs_from_ostree_repo (self->repo, refspec_prefix, collection_id,
                                              &local_refs, cancellable, error))
    return NULL;

  matched_refs = find_matching_refs (local_refs,
                                     name,
                                     opt_branch,
                                     opt_default_branch,
                                     opt_arch,
                                     opt_default_arch,
                                     collection_id,
                                     kinds,
                                     flags,
                                     &my_error);
  if (matched_refs == NULL)
    {
      if (g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       _("Error searching local repository: %s"),
                       my_error->message);
          return NULL;
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return NULL;
        }
    }

  g_ptr_array_add (matched_refs, NULL);
  return (char **) g_ptr_array_free (matched_refs, FALSE);
}

static GHashTable *
flatpak_dir_get_all_installed_refs (FlatpakDir  *self,
                                    FlatpakKinds kinds,
                                    GError     **error)
{
  g_autoptr(GHashTable) local_refs = NULL;
  int i;

  if (!flatpak_dir_maybe_ensure_repo (self, NULL, error))
    return NULL;

  local_refs = g_hash_table_new_full (flatpak_collection_ref_hash,
                                      flatpak_collection_ref_equal,
                                      (GDestroyNotify) flatpak_collection_ref_free,
                                      NULL);
  if (kinds & FLATPAK_KINDS_APP)
    {
      g_auto(GStrv) app_refs = NULL;

      if (!flatpak_dir_list_refs (self, "app", &app_refs, NULL, error))
        return NULL;

      for (i = 0; app_refs[i] != NULL; i++)
        {
          g_autofree char *remote = NULL;
          g_autofree char *collection_id = NULL;
          remote = flatpak_dir_get_origin (self, app_refs[i], NULL, NULL);
          if (remote != NULL)
            collection_id = flatpak_dir_get_remote_collection_id (self, remote);
          FlatpakCollectionRef *ref = flatpak_collection_ref_new (collection_id, app_refs[i]);
          g_hash_table_add (local_refs, ref);
        }
    }
  if (kinds & FLATPAK_KINDS_RUNTIME)
    {
      g_auto(GStrv) runtime_refs = NULL;

      if (!flatpak_dir_list_refs (self, "runtime", &runtime_refs, NULL, error))
        return NULL;

      for (i = 0; runtime_refs[i] != NULL; i++)
        {
          g_autofree char *remote = NULL;
          g_autofree char *collection_id = NULL;
          remote = flatpak_dir_get_origin (self, runtime_refs[i], NULL, NULL);
          if (remote != NULL)
            collection_id = flatpak_dir_get_remote_collection_id (self, remote);
          FlatpakCollectionRef *ref = flatpak_collection_ref_new (collection_id, runtime_refs[i]);
          g_hash_table_add (local_refs, ref);
        }
    }

  return g_steal_pointer (&local_refs);
}

char **
flatpak_dir_find_installed_refs (FlatpakDir            *self,
                                 const char            *opt_name,
                                 const char            *opt_branch,
                                 const char            *opt_arch,
                                 FlatpakKinds           kinds,
                                 FindMatchingRefsFlags  flags,
                                 GError               **error)
{
  g_autoptr(GHashTable) local_refs = NULL;
  GPtrArray *matched_refs;

  local_refs = flatpak_dir_get_all_installed_refs (self, kinds, error);
  if (local_refs == NULL)
    return NULL;

  matched_refs = find_matching_refs (local_refs,
                                     opt_name,
                                     opt_branch,
                                     NULL, /* default branch */
                                     opt_arch,
                                     NULL, /* default arch */
                                     NULL,
                                     kinds,
                                     flags,
                                     error);
  if (matched_refs == NULL)
    return NULL;

  g_ptr_array_add (matched_refs, NULL);
  return (char **) g_ptr_array_free (matched_refs, FALSE);

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
                                 opt_arch, NULL, kinds, &my_error);
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
               _("%s/%s/%s not installed"),
               opt_name ? opt_name : "*unspecified*",
               opt_arch ? opt_arch : "*unspecified*",
               opt_branch ? opt_branch : "*unspecified*");
  return NULL;
}

/* Given a list of refs in local_refspecs, remove any refs that have already
 * been deployed and return a new GPtrArray containing only the undeployed
 * refs. This is used by flatpak_dir_cleanup_undeployed_refs to determine
 * which undeployed refs need to be removed from the local repository.
 *
 * Returns: (transfer-full): A #GPtrArray
 */
static GPtrArray *
filter_out_deployed_refs (FlatpakDir *self,
                          GPtrArray  *local_refspecs,
                          GError    **error)
{
  g_autoptr(GPtrArray) undeployed_refs = g_ptr_array_new_full (local_refspecs->len, g_free);
  gsize i;

  for (i = 0; i < local_refspecs->len; ++i)
    {
      const gchar *refspec = g_ptr_array_index (local_refspecs, i);
      g_autofree gchar *ref = NULL;
      g_autoptr(GVariant) deploy_data = NULL;

      if (!ostree_parse_refspec (refspec, NULL, &ref, error))
        return FALSE;

      deploy_data = flatpak_dir_get_deploy_data (self, ref, FLATPAK_DEPLOY_VERSION_ANY, NULL, NULL);

      if (!deploy_data)
        g_ptr_array_add (undeployed_refs, g_strdup (refspec));
    }

  return g_steal_pointer (&undeployed_refs);
}

/**
 * flatpak_dir_cleanup_undeployed_refs:
 * @self: a #FlatpakDir
 * @cancellable: (nullable) (optional): a #GCancellable
 * @error: a #GError
 *
 * Find all flatpak refs in the local repository which have not been deployed
 * in the dir and remove them from the repository. You might want to call this
 * function if you pulled refs into the dir but then decided that you did
 * not want to deploy them for some reason. Note that this does not prune
 * objects bound to the cleaned up refs from the underlying OSTree repository,
 * you should consider using flatpak_dir_prune() to do that.
 *
 * Since: 0.10.0
 * Returns: %TRUE if cleaning up the refs suceeded, %FALSE otherwise
 */
gboolean
flatpak_dir_cleanup_undeployed_refs (FlatpakDir   *self,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  g_autoptr(GHashTable) local_refspecs = NULL;
  g_autoptr(GPtrArray)  local_flatpak_refspecs = NULL;
  g_autoptr(GPtrArray) undeployed_refs = NULL;
  gsize i = 0;

  if (!list_collection_refs_from_ostree_repo (self->repo, NULL, NULL, &local_refspecs,
                                              cancellable, error))
    return FALSE;

  local_flatpak_refspecs = find_matching_refs (local_refspecs,
                                               NULL, NULL, NULL, NULL, NULL, NULL,
                                               FLATPAK_KINDS_APP |
                                               FLATPAK_KINDS_RUNTIME,
                                               FIND_MATCHING_REFS_FLAGS_KEEP_REMOTE,
                                               error);

  if (!local_flatpak_refspecs)
    return FALSE;

  undeployed_refs = filter_out_deployed_refs (self, local_flatpak_refspecs, error);

  if (!undeployed_refs)
    return FALSE;

  for (; i < undeployed_refs->len; ++i)
    {
      const char *refspec = g_ptr_array_index (undeployed_refs, i);
      g_autofree gchar *remote = NULL;
      g_autofree gchar *ref = NULL;

      if (!ostree_parse_refspec (refspec, &remote, &ref, error))
        return FALSE;

      if (!flatpak_dir_remove_ref (self, remote, ref, cancellable, error))
        return FALSE;
    }

  return TRUE;
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
  FlatpakDir *clone;

  clone = flatpak_dir_new_full (self->basedir, self->user, self->extra_data);

  flatpak_dir_set_no_system_helper (clone, self->no_system_helper);
  flatpak_dir_set_no_interaction (clone, self->no_interaction);

  return clone;
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

static GKeyFile *
flatpak_dir_get_repo_config (FlatpakDir *self)
{
  if (!flatpak_dir_ensure_repo (self, NULL, NULL))
    return NULL;

  return ostree_repo_get_config (self->repo);
}

char *
flatpak_dir_get_remote_title (FlatpakDir *self,
                              const char *remote_name)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_string (config, group, "xa.title", NULL);

  return NULL;
}

gboolean
flatpak_dir_get_remote_oci (FlatpakDir *self,
                            const char *remote_name)
{
  g_autofree char *url = NULL;

  if (!flatpak_dir_ensure_repo (self, NULL, NULL))
    return FALSE;

  if (!ostree_repo_remote_get_url (self->repo, remote_name, &url, NULL))
    return FALSE;

  return url && g_str_has_prefix (url, "oci+");
}

char *
flatpak_dir_get_remote_main_ref (FlatpakDir *self,
                                 const char *remote_name)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_string (config, group, "xa.main-ref", NULL);

  return NULL;
}

char *
flatpak_dir_get_remote_default_branch (FlatpakDir *self,
                                       const char *remote_name)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_string (config, group, "xa.default-branch", NULL);

  return NULL;
}

int
flatpak_dir_get_remote_prio (FlatpakDir *self,
                             const char *remote_name)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_autofree char *group = get_group (remote_name);

  if (config && g_key_file_has_key (config, group, "xa.prio", NULL))
    return g_key_file_get_integer (config, group, "xa.prio", NULL);

  return 1;
}

gboolean
flatpak_dir_get_remote_noenumerate (FlatpakDir *self,
                                    const char *remote_name)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_boolean (config, group, "xa.noenumerate", NULL);

  return TRUE;
}

gboolean
flatpak_dir_get_remote_nodeps (FlatpakDir *self,
                               const char *remote_name)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_boolean (config, group, "xa.nodeps", NULL);

  return TRUE;
}

gboolean
flatpak_dir_get_remote_disabled (FlatpakDir *self,
                                 const char *remote_name)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_autofree char *group = get_group (remote_name);
  g_autofree char *url = NULL;

  if (config &&
      g_key_file_get_boolean (config, group, "xa.disable", NULL))
    return TRUE;

  if (self->repo &&
      ostree_repo_remote_get_url (self->repo, remote_name, &url, NULL) && *url == 0)
    return TRUE; /* Empty URL => disabled */

  return FALSE;
}

gboolean
flatpak_dir_remote_has_deploys (FlatpakDir *self,
                                const char *remote)
{
  g_autoptr(GHashTable) refs = NULL;
  GHashTableIter hash_iter;
  gpointer key;

  refs = flatpak_dir_get_all_installed_refs (self, FLATPAK_KINDS_APP | FLATPAK_KINDS_RUNTIME, NULL);
  if (refs == NULL)
    return FALSE;

  g_hash_table_iter_init (&hash_iter, refs);
  while (g_hash_table_iter_next (&hash_iter, &key, NULL))
    {
      FlatpakCollectionRef *coll_ref = key;
      const char *ref = coll_ref->ref_name;
      g_autofree char *origin = flatpak_dir_get_origin (self, ref, NULL, NULL);

      if (strcmp (remote, origin) == 0)
        return TRUE;
    }

  return FALSE;
}

static gint
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

  if (prio_b != prio_a)
    return prio_b - prio_a;

  /* Ensure we have a well-defined order for same prio */
  return strcmp (a_name, b_name);
}

static gboolean
origin_remote_matches (OstreeRepo *repo,
                       const char *remote_name,
                       const char *url,
                       const char *main_ref,
                       gboolean    gpg_verify,
                       const char *collection_id)
{
  g_autofree char *real_url = NULL;
  g_autofree char *real_main_ref = NULL;
  g_autofree char *real_collection_id = NULL;
  gboolean noenumerate;
  gboolean real_gpg_verify;

  /* Must match url */
  if (url == NULL)
    return FALSE;

  if (!ostree_repo_remote_get_url (repo, remote_name, &real_url, NULL))
    return FALSE;

  if (g_strcmp0 (url, real_url) != 0)
    return FALSE;

  /* Must be noenumerate */
  if (!ostree_repo_get_remote_boolean_option (repo, remote_name,
                                              "xa.noenumerate",
                                              FALSE, &noenumerate,
                                              NULL) ||
      !noenumerate)
    return FALSE;

  /* Must match gpg-verify
   * NOTE: We assume if all else matches the actual gpg key matches too. */
  if (!ostree_repo_get_remote_boolean_option (repo, remote_name,
                                              "gpg-verify",
                                              FALSE, &real_gpg_verify,
                                              NULL) ||
      real_gpg_verify != gpg_verify)
    return FALSE;

  /* Must match main-ref */
  if (ostree_repo_get_remote_option (repo, remote_name,
                                     "xa.main-ref",
                                     NULL, &real_main_ref,
                                     NULL) &&
      g_strcmp0 (main_ref, real_main_ref) != 0)
    return FALSE;

  /* Must match collection ID */
  if (ostree_repo_get_remote_option (repo, remote_name,
                                     "collection-id",
                                     NULL, &real_collection_id,
                                     NULL) &&
      g_strcmp0 (main_ref, real_main_ref) != 0)
    return FALSE;

  return TRUE;
}

static char *
create_origin_remote_config (OstreeRepo *repo,
                             const char *url,
                             const char *id,
                             const char *title,
                             const char *main_ref,
                             gboolean    gpg_verify,
                             const char *collection_id,
                             GKeyFile  **new_config)
{
  g_autofree char *remote = NULL;
  g_auto(GStrv) remotes = NULL;
  int version = 0;
  g_autofree char *group = NULL;
  g_autofree char *prefix = NULL;
  const char *last_dot;

  remotes = ostree_repo_remote_list (repo, NULL);

  last_dot = strrchr (id, '.');
  prefix = g_ascii_strdown (last_dot ? last_dot + 1 : id, -1);

  do
    {
      g_autofree char *name = NULL;
      if (version == 0)
        name = g_strdup_printf ("%s-origin", prefix);
      else
        name = g_strdup_printf ("%s%d-origin", prefix, version);
      version++;

      if (origin_remote_matches (repo, name, url, main_ref, gpg_verify, collection_id))
        return g_steal_pointer (&name);

      if (remotes == NULL ||
          !g_strv_contains ((const char * const *) remotes, name))
        remote = g_steal_pointer (&name);
    }
  while (remote == NULL);

  group = g_strdup_printf ("remote \"%s\"", remote);

  *new_config = g_key_file_new ();

  g_key_file_set_string (*new_config, group, "url", url ? url : "");
  if (title)
    g_key_file_set_string (*new_config, group, "xa.title", title);
  g_key_file_set_string (*new_config, group, "xa.noenumerate", "true");
  g_key_file_set_string (*new_config, group, "xa.prio", "0");
  /* Don’t enable summary verification if a collection ID is set, as collection
   * IDs enable the verification of refs from commit metadata instead. */
  g_key_file_set_string (*new_config, group, "gpg-verify-summary", (gpg_verify && collection_id == NULL) ? "true" : "false");
  g_key_file_set_string (*new_config, group, "gpg-verify", gpg_verify ? "true" : "false");
  if (main_ref)
    g_key_file_set_string (*new_config, group, "xa.main-ref", main_ref);

  if (collection_id)
    g_key_file_set_string (*new_config, group, "collection-id", collection_id);

  return g_steal_pointer (&remote);
}

char *
flatpak_dir_create_origin_remote (FlatpakDir   *self,
                                  const char   *url,
                                  const char   *id,
                                  const char   *title,
                                  const char   *main_ref,
                                  GBytes       *gpg_data,
                                  const char   *collection_id,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  g_autoptr(GKeyFile) new_config = NULL;
  g_autofree char *remote = NULL;

  remote = create_origin_remote_config (self->repo, url, id, title, main_ref, gpg_data != NULL, collection_id, &new_config);

  if (new_config &&
      !flatpak_dir_modify_remote (self, remote, new_config,
                                  gpg_data, cancellable, error))
    return NULL;

  if (!ostree_repo_reload_config (self->repo, cancellable, error))
    return FALSE;

  return g_steal_pointer (&remote);
}

GKeyFile *
flatpak_dir_parse_repofile (FlatpakDir   *self,
                            const char   *remote_name,
                            gboolean      from_ref,
                            GKeyFile     *keyfile,
                            GBytes      **gpg_data_out,
                            GCancellable *cancellable,
                            GError      **error)
{
  g_autoptr(GBytes) gpg_data = NULL;
  g_autofree char *uri = NULL;
  g_autofree char *title = NULL;
  g_autofree char *gpg_key = NULL;
  g_autofree char *collection_id = NULL;
  g_autofree char *default_branch = NULL;
  gboolean nodeps;
  const char *source_group;

  if (from_ref)
    source_group = FLATPAK_REF_GROUP;
  else
    source_group = FLATPAK_REPO_GROUP;

  GKeyFile *config = g_key_file_new ();
  g_autofree char *group = g_strdup_printf ("remote \"%s\"", remote_name);

  if (!g_key_file_has_group (keyfile, source_group))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid %s: Missing group ‘%s’"),
                          from_ref ? ".flatpakref" : ".flatpakrepo", source_group);
      return NULL;
    }

  uri = g_key_file_get_string (keyfile, source_group,
                               FLATPAK_REPO_URL_KEY, NULL);
  if (uri == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid %s: Missing key ‘%s’"),
                          from_ref ? ".flatpakref" : ".flatpakrepo", FLATPAK_REPO_URL_KEY);
      return NULL;
    }

  g_key_file_set_string (config, group, "url", uri);

  title = g_key_file_get_locale_string (keyfile, source_group,
                                        FLATPAK_REPO_TITLE_KEY, NULL, NULL);
  if (title != NULL)
    g_key_file_set_string (config, group, "xa.title", title);

  default_branch = g_key_file_get_locale_string (keyfile, source_group,
                                                 FLATPAK_REPO_DEFAULT_BRANCH_KEY, NULL, NULL);
  if (default_branch != NULL)
    g_key_file_set_string (config, group, "xa.default-branch", default_branch);

  nodeps = g_key_file_get_boolean (keyfile, source_group,
                                   FLATPAK_REPO_NODEPS_KEY, NULL);
  if (nodeps)
    g_key_file_set_boolean (config, group, "xa.nodeps", TRUE);

  gpg_key = g_key_file_get_string (keyfile, source_group,
                                   FLATPAK_REPO_GPGKEY_KEY, NULL);
  if (gpg_key != NULL)
    {
      guchar *decoded;
      gsize decoded_len;

      gpg_key = g_strstrip (gpg_key);
      decoded = g_base64_decode (gpg_key, &decoded_len);
      if (decoded_len < 10) /* Check some minimal size so we don't get crap */
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid gpg key"));
          return NULL;
        }

      gpg_data = g_bytes_new_take (decoded, decoded_len);
      g_key_file_set_boolean (config, group, "gpg-verify", TRUE);
    }
  else
    {
      g_key_file_set_boolean (config, group, "gpg-verify", FALSE);
    }

  collection_id = g_key_file_get_string (keyfile, source_group,
                                         FLATPAK_REPO_DEPLOY_COLLECTION_ID_KEY, NULL);
  if (collection_id == NULL || *collection_id == '\0')
    collection_id = g_key_file_get_string (keyfile, source_group,
                                           FLATPAK_REPO_COLLECTION_ID_KEY, NULL);
  if (collection_id != NULL)
    {
      if (gpg_key == NULL)
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Collection ID requires GPG key to be provided"));
          return NULL;
        }

      g_key_file_set_string (config, group, "collection-id", collection_id);
    }

  /* If a collection ID is set, refs are verified from commit metadata rather
   * than the summary file. */
  g_key_file_set_boolean (config, group, "gpg-verify-summary",
                          (gpg_key != NULL && collection_id == NULL));

  *gpg_data_out = g_steal_pointer (&gpg_data);

  return g_steal_pointer (&config);
}

static gboolean
parse_ref_file (GKeyFile *keyfile,
                char    **name_out,
                char    **branch_out,
                char    **url_out,
                char    **title_out,
                GBytes  **gpg_data_out,
                gboolean *is_runtime_out,
                char    **collection_id_out,
                GError  **error)
{
  g_autofree char *url = NULL;
  g_autofree char *title = NULL;
  g_autofree char *name = NULL;
  g_autofree char *branch = NULL;
  g_autofree char *version = NULL;

  g_autoptr(GBytes) gpg_data = NULL;
  gboolean is_runtime = FALSE;
  g_autofree char *collection_id = NULL;
  g_autofree char *str = NULL;

  *name_out = NULL;
  *branch_out = NULL;
  *url_out = NULL;
  *title_out = NULL;
  *gpg_data_out = NULL;
  *is_runtime_out = FALSE;

  if (!g_key_file_has_group (keyfile, FLATPAK_REF_GROUP))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid file format, no %s group"), FLATPAK_REF_GROUP);

  version = g_key_file_get_string (keyfile, FLATPAK_REF_GROUP,
                                   FLATPAK_REF_VERSION_KEY, NULL);
  if (version != NULL && strcmp (version, "1") != 0)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid version %s, only 1 supported"), version);

  url = g_key_file_get_string (keyfile, FLATPAK_REF_GROUP,
                               FLATPAK_REF_URL_KEY, NULL);
  if (url == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid file format, no %s specified"), FLATPAK_REF_URL_KEY);

  name = g_key_file_get_string (keyfile, FLATPAK_REF_GROUP,
                                FLATPAK_REF_NAME_KEY, NULL);
  if (name == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid file format, no %s specified"), FLATPAK_REF_NAME_KEY);

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
      g_autofree guchar *decoded = NULL;
      gsize decoded_len;

      str = g_strstrip (str);
      decoded = g_base64_decode (str, &decoded_len);
      if (decoded_len < 10) /* Check some minimal size so we don't get crap */
        return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid file format, gpg key invalid"));

      gpg_data = g_bytes_new_take (g_steal_pointer (&decoded), decoded_len);
    }

  collection_id = g_key_file_get_string (keyfile, FLATPAK_REF_GROUP,
                                         FLATPAK_REF_DEPLOY_COLLECTION_ID_KEY, NULL);

  if (collection_id == NULL || *collection_id == '\0')
    {
      collection_id = g_key_file_get_string (keyfile, FLATPAK_REF_GROUP,
                                             FLATPAK_REF_COLLECTION_ID_KEY, NULL);
    }

  if (collection_id != NULL && *collection_id == '\0')
    collection_id = NULL;

  if (collection_id != NULL && gpg_data == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Collection ID requires GPG key to be provided"));

  *name_out = g_steal_pointer (&name);
  *branch_out = g_steal_pointer (&branch);
  *url_out = g_steal_pointer (&url);
  *title_out = g_steal_pointer (&title);
  *gpg_data_out = g_steal_pointer (&gpg_data);
  *is_runtime_out = is_runtime;
  *collection_id_out = g_steal_pointer (&collection_id);

  return TRUE;
}

gboolean
flatpak_dir_create_remote_for_ref_file (FlatpakDir *self,
                                        GKeyFile   *keyfile,
                                        const char *default_arch,
                                        char      **remote_name_out,
                                        char      **collection_id_out,
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
  g_autofree char *collection_id = NULL;
  g_autoptr(GFile) deploy_dir = NULL;

  if (!parse_ref_file (keyfile, &name, &branch, &url, &title, &gpg_data, &is_runtime, &collection_id, error))
    return FALSE;

  ref = flatpak_compose_ref (!is_runtime, name, branch, default_arch, error);
  if (ref == NULL)
    return FALSE;

  deploy_dir = flatpak_dir_get_if_deployed (self, ref, NULL, NULL);
  if (deploy_dir != NULL)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                   is_runtime ? _("Runtime %s, branch %s is already installed") :
                   _("App %s, branch %s is already installed"),
                   name, branch);
      return FALSE;
    }

  /* First try to reuse existing remote */
  remote = flatpak_dir_find_remote_by_uri (self, url, collection_id);

  if (remote == NULL)
    {
      remote = flatpak_dir_create_origin_remote (self, url, name, title, ref,
                                                 gpg_data, collection_id, NULL, error);
      if (remote == NULL)
        return FALSE;
    }

  if (collection_id_out != NULL)
    *collection_id_out = g_steal_pointer (&collection_id);

  *remote_name_out = g_steal_pointer (&remote);
  *ref_out = (char *) g_steal_pointer (&ref);
  return TRUE;
}

static gboolean
_flatpak_uri_equal (const char *uri1,
                    const char *uri2)
{
  g_autofree char *uri1_norm = NULL;
  g_autofree char *uri2_norm = NULL;
  gsize uri1_len = strlen (uri1);
  gsize uri2_len = strlen (uri2);

  /* URIs handled by libostree are equivalent with or without a trailing slash,
   * but this isn't otherwise guaranteed to be the case.
   */
  if (g_str_has_prefix (uri1, "oci+") || g_str_has_prefix (uri2, "oci+"))
    return g_strcmp0 (uri1, uri2) == 0;

  if (g_str_has_suffix (uri1, "/"))
    uri1_norm = g_strndup (uri1, uri1_len - 1);
  else
    uri1_norm = g_strdup (uri1);

  if (g_str_has_suffix (uri2, "/"))
    uri2_norm = g_strndup (uri2, uri2_len - 1);
  else
    uri2_norm = g_strdup (uri2);

  return g_strcmp0 (uri1_norm, uri2_norm) == 0;
}

/* This tries to find a pre-configured remote for the specified uri
 * and (optionally) collection id. This is a bit more complex than it
 * sounds, because a local remote could be configured in different
 * ways for a remote repo (i.e. it could be not using collection ids,
 * even though the remote specifies it, or the flatpakrepo might lack
 * the collection id details). So, we use these rules:
 *
 *  If the url is the same, it is a match even if one part lacks
 *  collection ids. However, if both collection ids are specified and
 *  differ there is no match.
 *
 *  If the collection id is the same (and specified), its going to be
 *  the same remote, even if the url is different (because it could be
 *  some other mirror of the same repo).
 *
 *  We also consider non-OCI URLs equal even if one lacks a trailing slash.
 */
char *
flatpak_dir_find_remote_by_uri (FlatpakDir *self,
                                const char *uri,
                                const char *collection_id)
{
  g_auto(GStrv) remotes = NULL;

  g_return_val_if_fail (self != NULL, NULL);
  g_return_val_if_fail (uri != NULL, NULL);

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
          g_autofree char *remote_collection_id = NULL;

          if (!ostree_repo_remote_get_url (self->repo,
                                           remote,
                                           &remote_uri,
                                           NULL))
            continue;
          if (!repo_get_remote_collection_id (self->repo, remote, &remote_collection_id, NULL))
            continue;

          /* Exact collection ids always match, independent of the uris used */
          if (collection_id != NULL &&
              remote_collection_id != NULL &&
              strcmp (collection_id, remote_collection_id) == 0)
            return g_strdup (remote);

          /* Same repo if uris match, unless both have collection-id
             specified but different */
          if (_flatpak_uri_equal (uri, remote_uri) &&
              !(collection_id != NULL &&
                remote_collection_id != NULL &&
                strcmp (collection_id, remote_collection_id) != 0))
            return g_strdup (remote);
        }
    }

  return NULL;
}

gboolean
flatpak_dir_has_remote (FlatpakDir *self,
                        const char *remote_name,
                        GError    **error)
{
  GKeyFile *config = NULL;
  g_autofree char *group = g_strdup_printf ("remote \"%s\"", remote_name);

  if (flatpak_dir_maybe_ensure_repo (self, NULL, NULL) &&
      self->repo != NULL)
    {
      config = ostree_repo_get_config (self->repo);
      if (config && g_key_file_has_group (config, group))
        return TRUE;
    }

  return flatpak_fail_error (error, FLATPAK_ERROR_REMOTE_NOT_FOUND,
                             "Remote \"%s\" not found", remote_name);
}


char **
flatpak_dir_list_remotes (FlatpakDir   *self,
                          GCancellable *cancellable,
                          GError      **error)
{
  char **res = NULL;

  if (!flatpak_dir_maybe_ensure_repo (self, cancellable, error))
    return NULL;

  if (self->repo)
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
  return (char **) g_ptr_array_free (g_steal_pointer (&res), FALSE);
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

  return (char **) g_ptr_array_free (g_steal_pointer (&found), FALSE);
}

char **
flatpak_dir_search_for_local_dependency (FlatpakDir   *self,
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
      g_autofree char *commit = NULL;

      if (flatpak_dir_get_remote_nodeps (self, remote))
        continue;

      commit = flatpak_dir_read_latest (self, remote, runtime_ref, NULL, NULL, NULL);
      if (commit != NULL)
        g_ptr_array_add (found, g_strdup (remote));
    }

  g_ptr_array_add (found, NULL);

  return (char **) g_ptr_array_free (g_steal_pointer (&found), FALSE);
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
  g_autofree char *url = NULL;

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      g_autoptr(GVariant) gpg_data_v = NULL;
      FlatpakHelperConfigureRemoteFlags flags = 0;
      const char *installation = flatpak_dir_get_id (self);

      gpg_data_v = g_variant_ref_sink (g_variant_new_from_data (G_VARIANT_TYPE ("ay"), "", 0, TRUE, NULL, NULL));

      if (force_remove)
        flags |= FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_FORCE_REMOVE;

      if (!flatpak_dir_system_helper_call_configure_remote (self,
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
                return flatpak_fail_error (error, FLATPAK_ERROR_REMOTE_USED,
                                           _("Can't remove remote '%s' with installed ref %s (at least)"),
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

  if (flatpak_dir_get_remote_oci (self, remote_name) &&
      !flatpak_dir_remove_oci_files (self, remote_name,
                                     cancellable, error))
    return FALSE;

  ostree_repo_remote_get_url (self->repo, remote_name, &url, NULL);
  
  if (!ostree_repo_remote_change (self->repo, NULL,
                                  OSTREE_REPO_REMOTE_CHANGE_DELETE,
                                  remote_name, NULL,
                                  NULL,
                                  cancellable, error))
    return FALSE;

  if (!flatpak_dir_mark_changed (self, error))
    return FALSE;

  flatpak_dir_log (self, "remove remote",
                   remote_name, NULL, NULL, NULL, url,
                   "Removed remote %s", remote_name);

  return TRUE;
}

static gboolean
flatpak_dir_cleanup_remote_for_url_change (FlatpakDir   *self,
                                           const char   *remote_name,
                                           const char   *url,
                                           GCancellable *cancellable,
                                           GError      **error)
{
  g_autofree char *old_url = NULL;

  /* We store things a bit differently for OCI and non-OCI remotes,
   * so when changing from one to the other, we need to clean up cached
   * files.
   */
  if (ostree_repo_remote_get_url (self->repo,
                                  remote_name,
                                  &old_url,
                                  NULL))
    {
      gboolean was_oci = g_str_has_prefix (old_url, "oci+");
      gboolean will_be_oci = g_str_has_prefix (url, "oci+");

      if (was_oci != will_be_oci)
        {
          if (!flatpak_dir_remove_appstream (self, remote_name,
                                             cancellable, error))
            return FALSE;
        }

      if (was_oci && !will_be_oci)
        {
          if (!flatpak_dir_remove_oci_files (self, remote_name,
                                             cancellable, error))
            return FALSE;
        }
    }

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
  gboolean has_remote;

  if (strchr (remote_name, '/') != NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_REMOTE_NOT_FOUND, _("Invalid character '/' in remote name: %s"),
                               remote_name);

  has_remote = flatpak_dir_has_remote (self, remote_name, NULL);

  if (!g_key_file_has_group (config, group))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("No configuration for remote %s specified"),
                               remote_name);

  if (!flatpak_dir_check_add_remotes_config_dir (self, error))
    return FALSE;

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      g_autofree char *config_data = g_key_file_to_data (config, NULL, NULL);
      g_autoptr(GVariant) gpg_data_v = NULL;
      const char *installation = flatpak_dir_get_id (self);

      if (gpg_data != NULL)
        gpg_data_v = variant_new_ay_bytes (gpg_data);
      else
        gpg_data_v = g_variant_ref_sink (g_variant_new_from_data (G_VARIANT_TYPE ("ay"), "", 0, TRUE, NULL, NULL));

      if (!flatpak_dir_system_helper_call_configure_remote (self,
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

  if (!flatpak_dir_cleanup_remote_for_url_change (self, remote_name, url, cancellable, error))
    return FALSE;

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

  if (has_remote)
    flatpak_dir_log (self, "modify remote", remote_name, NULL, NULL, NULL, url,
                     "Modified remote %s to %s", remote_name, url);
  else
    flatpak_dir_log (self, "add remote", remote_name, NULL, NULL, NULL, url,
                     "Added remote %s to %s", remote_name, url);

  return TRUE;
}

static gboolean
remove_unless_in_hash (gpointer key,
                       gpointer value,
                       gpointer user_data)
{
  GHashTable *table = user_data;
  FlatpakCollectionRef *ref = key;

  return !g_hash_table_contains (table, ref->ref_name);
}

gboolean
flatpak_dir_list_remote_refs (FlatpakDir         *self,
                              FlatpakRemoteState *state,
                              GHashTable        **refs,
                              GCancellable       *cancellable,
                              GError            **error)
{
  g_autoptr(GError) my_error = NULL;

  if (error == NULL)
    error = &my_error;

  if (!flatpak_dir_list_all_remote_refs (self, state, refs,
                                         cancellable, error))
    return FALSE;

  if (flatpak_dir_get_remote_noenumerate (self, state->remote_name))
    {
      g_autoptr(GHashTable) unprefixed_local_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      g_autoptr(GHashTable) local_refs = NULL;
      GHashTableIter hash_iter;
      gpointer key;
      g_autofree char *refspec_prefix = g_strconcat (state->remote_name, ":.", NULL);

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

gboolean
_flatpak_dir_fetch_remote_state_metadata_branch (FlatpakDir         *self,
                                                 FlatpakRemoteState *state, /* This state does not have metadata filled out yet */
                                                 GCancellable       *cancellable,
                                                 GError            **error)
{
  g_autoptr(OstreeAsyncProgress) progress = ostree_async_progress_new ();
  FlatpakPullFlags flatpak_flags;
  gboolean gpg_verify;
  g_autofree char *checksum_from_summary = NULL;
  g_autofree char *checksum_from_repo = NULL;
  g_autofree char *refspec = NULL;

  g_assert (state->collection_id != NULL);

  /* We can only fetch metadata if we’re going to verify it with GPG. */
  if (!ostree_repo_remote_get_gpg_verify (self->repo, state->remote_name,
                                          &gpg_verify, error))
    return FALSE;

  if (!gpg_verify)
    return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("Can't pull from untrusted non-gpg verified remote"));

  /* Look up the checksum as advertised by the summary file. If it differs from
   * what we currently have on disk, try and pull the updated ostree-metadata ref.
   * This is how we implement caching. Ignore failure and pull the ref anyway. */
  if (state->summary != NULL)
    flatpak_summary_lookup_ref (state->summary, state->collection_id,
                                OSTREE_REPO_METADATA_REF,
                                &checksum_from_summary, NULL);

  refspec = g_strdup_printf ("%s:%s", state->remote_name, OSTREE_REPO_METADATA_REF);
  if (!ostree_repo_resolve_rev (self->repo, refspec, TRUE, &checksum_from_repo, error))
    return FALSE;

  g_debug ("%s: Comparing %s from summary and %s from repo",
           G_STRFUNC, checksum_from_summary, checksum_from_repo);

  if (checksum_from_summary != NULL && checksum_from_repo != NULL &&
      g_str_equal (checksum_from_summary, checksum_from_repo))
    return TRUE;

  /* Do the pull into the local repository. */
  flatpak_flags = FLATPAK_PULL_FLAGS_DOWNLOAD_EXTRA_DATA;
  flatpak_flags |= FLATPAK_PULL_FLAGS_NO_STATIC_DELTAS;

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      g_autoptr(OstreeRepo) child_repo = NULL;
      g_auto(GLnxLockFile) child_repo_lock = { 0, };
      const char *installation = flatpak_dir_get_id (self);
      const char *subpaths[] = {NULL};
      g_autofree char *child_repo_path = NULL;
      FlatpakHelperDeployFlags helper_flags = 0;
      g_autofree char *url = NULL;
      gboolean gpg_verify_summary;
      gboolean gpg_verify;
      gboolean is_oci;

      if (!ostree_repo_remote_get_url (self->repo,
                                       state->remote_name,
                                       &url,
                                       error))
        return FALSE;

      if (!ostree_repo_remote_get_gpg_verify_summary (self->repo, state->remote_name,
                                                      &gpg_verify_summary, error))
        return FALSE;

      if (!ostree_repo_remote_get_gpg_verify (self->repo, state->remote_name,
                                              &gpg_verify, error))
        return FALSE;

      is_oci = flatpak_dir_get_remote_oci (self, state->remote_name);
      if ((!gpg_verify_summary && state->collection_id == NULL) || !gpg_verify)
        {
          /* The remote is not gpg verified, so we don't want to allow installation via
             a download in the home directory, as there is no way to verify you're not
             injecting anything into the remote. However, in the case of a remote
             configured to a local filesystem we can just let the system helper do
             the installation, as it can then avoid network i/o and be certain the
             data comes from the right place.

             If a collection ID is available, we can verify the refs in commit
             metadata. */
          if (g_str_has_prefix (url, "file:"))
            helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL;
          else
            return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("Can't pull from untrusted non-gpg verified remote"));
        }
      else if (is_oci)
        {
          return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("No metadata branch for OCI"));
        }
      else
        {
          /* We're pulling from a remote source, we do the network mirroring pull as a
             user and hand back the resulting data to the system-helper, that trusts us
             due to the GPG signatures in the repo */
          child_repo = flatpak_dir_create_system_child_repo (self, &child_repo_lock, NULL, error);
          if (child_repo == NULL)
            return FALSE;

          if (!flatpak_dir_pull (self, state, OSTREE_REPO_METADATA_REF, NULL, NULL, NULL, NULL, NULL,
                                 child_repo,
                                 flatpak_flags,
                                 OSTREE_REPO_PULL_FLAGS_MIRROR,
                                 progress, cancellable, error))
            return FALSE;

          if (!child_repo_ensure_summary (child_repo, state, cancellable, error))
            return FALSE;

          child_repo_path = g_file_get_path (ostree_repo_get_path (child_repo));
        }

      helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY;

      if (!flatpak_dir_system_helper_call_deploy (self,
                                                  child_repo_path ? child_repo_path : "",
                                                  helper_flags, OSTREE_REPO_METADATA_REF, state->remote_name,
                                                  (const char * const *) subpaths,
                                                  installation ? installation : "",
                                                  cancellable,
                                                  error))
        return FALSE;

      if (child_repo_path)
        (void) glnx_shutil_rm_rf_at (AT_FDCWD, child_repo_path, NULL, NULL);

      return TRUE;
    }

  if (!flatpak_dir_pull (self, state, OSTREE_REPO_METADATA_REF, NULL, NULL, NULL, NULL, NULL, NULL,
                         flatpak_flags, OSTREE_REPO_PULL_FLAGS_NONE,
                         progress, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_dir_update_remote_configuration_for_state (FlatpakDir         *self,
                                                   FlatpakRemoteState *remote_state,
                                                   gboolean            dry_run,
                                                   gboolean           *has_changed_out,
                                                   GCancellable       *cancellable,
                                                   GError            **error)
{
  /* We only support those configuration parameters that can
     be set in the server when building the repo (see the
     flatpak_repo_set_* () family of functions) */
  static const char *const supported_params[] = {
    "xa.title",
    "xa.default-branch",
    "xa.gpg-keys",
    "xa.redirect-url",
    OSTREE_META_KEY_DEPLOY_COLLECTION_ID,
    NULL
  };

  g_autoptr(GPtrArray) updated_params = NULL;
  GVariantIter iter;
  g_autoptr(GBytes) gpg_keys = NULL;

  updated_params = g_ptr_array_new_with_free_func (g_free);

  g_variant_iter_init (&iter, remote_state->metadata);
  if (g_variant_iter_n_children (&iter) > 0)
    {
      GVariant *value_var = NULL;
      char *key = NULL;

      while (g_variant_iter_next (&iter, "{sv}", &key, &value_var))
        {
          if (g_strv_contains (supported_params, key))
            {
              if (strcmp (key, "xa.gpg-keys") == 0)
                {
                  if (g_variant_is_of_type (value_var, G_VARIANT_TYPE_BYTESTRING))
                    {
                      const guchar *gpg_data = g_variant_get_data (value_var);
                      gsize gpg_size = g_variant_get_size (value_var);
                      g_autofree gchar *gpg_data_checksum = g_compute_checksum_for_data (G_CHECKSUM_SHA256, gpg_data, gpg_size);

                      gpg_keys = g_bytes_new (gpg_data, gpg_size);

                      /* We store the hash so that we can detect when things changed or not
                         instead of re-importing the key over-and-over */
                      g_ptr_array_add (updated_params, g_strdup ("xa.gpg-keys-hash"));
                      g_ptr_array_add (updated_params, g_steal_pointer (&gpg_data_checksum));
                    }
                }
              else if (g_variant_is_of_type (value_var, G_VARIANT_TYPE_STRING))
                {
                  const char *value = g_variant_get_string (value_var, NULL);
                  if (value != NULL && *value != 0)
                    {
                      if (strcmp (key, "xa.redirect-url") == 0)
                        g_ptr_array_add (updated_params, g_strdup ("url"));
                      else if (strcmp (key, OSTREE_META_KEY_DEPLOY_COLLECTION_ID) == 0)
                        g_ptr_array_add (updated_params, g_strdup ("collection-id"));
                      else
                        g_ptr_array_add (updated_params, g_strdup (key));
                      g_ptr_array_add (updated_params, g_strdup (value));
                    }
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
      group = g_strdup_printf ("remote \"%s\"", remote_state->remote_name);

      i = 0;
      while (i < (updated_params->len - 1))
        {
          /* This array should have an even number of elements with
             keys in the odd positions and values on even ones. */
          const char *key = g_ptr_array_index (updated_params, i);
          const char *new_val = g_ptr_array_index (updated_params, i + 1);
          g_autofree char *current_val = NULL;
          g_autofree char *is_set_key = g_strconcat (key, "-is-set", NULL);
          gboolean is_set = FALSE;

          is_set = g_key_file_get_boolean (config, group, is_set_key, NULL);
          if (!is_set)
            {
              current_val = g_key_file_get_string (config, group, key, NULL);
              if ((!g_str_equal (key, "collection-id") &&
                   g_strcmp0 (current_val, new_val) != 0) ||
                  (g_str_equal (key, "collection-id") &&
                   (current_val == NULL || *current_val == '\0') &&
                   new_val != NULL && *new_val != '\0'))
                {
                  has_changed = TRUE;
                  g_key_file_set_string (config, group, key, new_val);

                  /* Special case for collection-id: if it’s set, gpg-verify-summary
                   * must be set to false. The logic above ensures that the
                   * collection-id is only set if we’re transitioning from an
                   * unset to a set collection-ID. We *must not* allow the
                   * collection ID to be changed from one set value to another
                   * without the user manually verifying it; or a malicious
                   * repository could assume the collection ID of another without
                   * the user’s consent. */
                  if (g_str_equal (key, "collection-id") &&
                      new_val != NULL && *new_val != '\0')
                    g_key_file_set_boolean (config, group, "gpg-verify-summary", FALSE);
                }
            }

          i += 2;
        }

      if (has_changed_out)
        *has_changed_out = has_changed;

      if (dry_run || !has_changed)
        return TRUE;

      /* Update the local remote configuration with the updated info. */
      if (!flatpak_dir_modify_remote (self, remote_state->remote_name, config, gpg_keys, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_dir_update_remote_configuration (FlatpakDir   *self,
                                         const char   *remote,
                                         GCancellable *cancellable,
                                         GError      **error)
{
  gboolean is_oci;

  g_autoptr(FlatpakRemoteState) state = NULL;

  if (flatpak_dir_get_remote_disabled (self, remote))
    return TRUE;

  is_oci = flatpak_dir_get_remote_oci (self, remote);
  if (is_oci)
    return TRUE;

  state = flatpak_dir_get_remote_state (self, remote, cancellable, error);
  if (state == NULL)
    return FALSE;

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      gboolean has_changed = FALSE;
      gboolean gpg_verify_summary;
      gboolean gpg_verify;

      if (!ostree_repo_remote_get_gpg_verify_summary (self->repo, remote, &gpg_verify_summary, error))
        return FALSE;

      if (!ostree_repo_remote_get_gpg_verify (self->repo, remote, &gpg_verify, error))
        return FALSE;

      if ((!gpg_verify_summary && state->collection_id == NULL) || !gpg_verify)
        {
          g_debug ("Ignoring automatic updates for system-helper remotes without gpg signatures");
          return TRUE;
        }

      if (!flatpak_dir_update_remote_configuration_for_state (self, state, TRUE, &has_changed, cancellable, error))
        return FALSE;

      if (state->collection_id == NULL && state->summary_sig_bytes == NULL)
        {
          g_debug ("Can't update remote configuration as user, no GPG signature");
          return TRUE;
        }

      if (has_changed)
        {
          g_autoptr(GBytes) bytes = g_variant_get_data_as_bytes (state->summary);
          glnx_autofd int summary_fd = -1;
          g_autofree char *summary_path = NULL;
          glnx_autofd int summary_sig_fd = -1;
          g_autofree char *summary_sig_path = NULL;
          const char *installation;

          summary_fd = g_file_open_tmp ("remote-summary.XXXXXX", &summary_path, error);
          if (summary_fd == -1)
            return FALSE;
          if (glnx_loop_write (summary_fd, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes)) < 0)
            return glnx_throw_errno (error);

          if (state->summary_sig_bytes != NULL)
            {
              summary_sig_fd = g_file_open_tmp ("remote-summary-sig.XXXXXX", &summary_sig_path, error);
              if (summary_sig_fd == -1)
                return FALSE;
              if (glnx_loop_write (summary_sig_fd, g_bytes_get_data (state->summary_sig_bytes, NULL), g_bytes_get_size (state->summary_sig_bytes)) < 0)
                return glnx_throw_errno (error);
            }

          installation = flatpak_dir_get_id (self);

          if (!flatpak_dir_system_helper_call_update_remote (self, 0, remote,
                                                             installation ? installation : "",
                                                             summary_path, summary_sig_path ? summary_sig_path : "",
                                                             cancellable, error))
            return FALSE;

          unlink (summary_path);
          if (summary_sig_path)
            unlink (summary_sig_path);
        }

      return TRUE;
    }

  return flatpak_dir_update_remote_configuration_for_state (self, state, FALSE, NULL, cancellable, error);
}


static GBytes *
flatpak_dir_fetch_remote_object (FlatpakDir   *self,
                                 const char   *remote_name,
                                 const char   *checksum,
                                 const char   *type,
                                 GCancellable *cancellable,
                                 GError      **error)
{
  g_autofree char *base_url = NULL;
  g_autofree char *object_url = NULL;
  g_autofree char *part1 = NULL;
  g_autofree char *part2 = NULL;

  g_autoptr(GBytes) bytes = NULL;

  if (!ostree_repo_remote_get_url (self->repo, remote_name, &base_url, error))
    return NULL;

  ensure_soup_session (self);

  part1 = g_strndup (checksum, 2);
  part2 = g_strdup_printf ("%s.%s", checksum + 2, type);

  object_url = g_build_filename (base_url, "objects", part1, part2, NULL);

  bytes = flatpak_load_http_uri (self->soup_session, object_url, 0,
                                 NULL, NULL,
                                 cancellable, error);
  if (bytes == NULL)
    return NULL;

  return g_steal_pointer (&bytes);
}

GVariant *
flatpak_dir_fetch_remote_commit (FlatpakDir   *self,
                                 const char   *remote_name,
                                 const char   *ref,
                                 const char   *opt_commit,
                                 char        **out_commit,
                                 GCancellable *cancellable,
                                 GError      **error)
{
  g_autoptr(GBytes) commit_bytes = NULL;
  g_autoptr(GVariant) commit_variant = NULL;
  g_autofree char *latest_commit = NULL;
  g_autoptr(GVariant) commit_metadata = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;

  if (opt_commit == NULL)
    {
      state = flatpak_dir_get_remote_state (self, remote_name, cancellable, error);
      if (state == NULL)
        return NULL;

      flatpak_remote_state_lookup_ref (state, ref, &latest_commit, NULL, error);
      if (latest_commit == NULL)
        {
          if (error != NULL && *error == NULL)
            flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Couldn't find latest checksum for ref %s in remote %s"),
                                ref, state->remote_name);
          return NULL;
        }

      opt_commit = latest_commit;
    }

  commit_bytes = flatpak_dir_fetch_remote_object (self, remote_name,
                                                  opt_commit, "commit",
                                                  cancellable, error);
  if (commit_bytes == NULL)
    return NULL;

  commit_variant = g_variant_new_from_bytes (OSTREE_COMMIT_GVARIANT_FORMAT,
                                             commit_bytes, FALSE);
  g_variant_ref_sink (commit_variant);

  if (!ostree_validate_structureof_commit (commit_variant, error))
    return NULL;

  commit_metadata = g_variant_get_child_value (commit_variant, 0);
  if (ref != NULL)
    {
      const char *xa_ref = NULL;
      g_autofree const char **commit_refs = NULL;

      if ((g_variant_lookup (commit_metadata, "xa.ref", "&s", &xa_ref) &&
           g_strcmp0 (xa_ref, ref) != 0) ||
          (g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_REF_BINDING, "^a&s", &commit_refs) &&
           !g_strv_contains ((const char * const *) commit_refs, ref)))
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Commit has no requested ref ‘%s’ in ref binding metadata"),  ref);
          return NULL;
        }
    }

  if (out_commit)
    *out_commit = g_strdup (opt_commit);

  return g_steal_pointer (&commit_variant);
}

void
flatpak_related_free (FlatpakRelated *self)
{
  g_free (self->collection_id);
  g_free (self->ref);
  g_free (self->commit);
  g_strfreev (self->subpaths);
  g_free (self);
}

static void
add_related (FlatpakDir *self,
             GPtrArray  *related,
             const char *extension,
             const char *extension_collection_id,
             const char *extension_ref,
             const char *checksum,
             gboolean    no_autodownload,
             const char *download_if,
             const char *autoprune_unless,
             gboolean    autodelete,
             gboolean    locale_subset)
{
  g_autoptr(GVariant) deploy_data = NULL;
  g_autofree const char **old_subpaths = NULL;
  g_auto(GStrv) extra_subpaths = NULL;
  g_auto(GStrv) subpaths = NULL;
  FlatpakRelated *rel;
  gboolean download;
  gboolean delete = autodelete;
  gboolean auto_prune = FALSE;
  g_auto(GStrv) ref_parts = g_strsplit (extension_ref, "/", -1);
  g_autoptr(GFile) unmaintained_path = NULL;

  deploy_data = flatpak_dir_get_deploy_data (self, extension_ref, FLATPAK_DEPLOY_VERSION_ANY, NULL, NULL);

  if (deploy_data)
    old_subpaths = flatpak_deploy_data_get_subpaths (deploy_data);

  /* Only respect no-autodownload/download-if for uninstalled refs, we
     always want to update if you manually installed something */
  download =
    flatpak_extension_matches_reason (ref_parts[1], download_if, !no_autodownload) ||
    deploy_data != NULL;

  if (!flatpak_extension_matches_reason (ref_parts[1], autoprune_unless, TRUE))
    auto_prune = TRUE;

  /* Don't download if there is an unmaintained extension already installed */
  unmaintained_path =
    flatpak_find_unmaintained_extension_dir_if_exists (ref_parts[1],
                                                       ref_parts[2],
                                                       ref_parts[3], NULL);
  if (unmaintained_path != NULL && deploy_data == NULL)
    {
      g_debug ("Skipping related extension ‘%s’ because it is already "
               "installed as an unmaintained extension in ‘%s’.",
               ref_parts[1], flatpak_file_get_path_cached (unmaintained_path));
      download = FALSE;
    }

  if (g_str_has_suffix (extension, ".Debug"))
    {
      /* debug files only updated if already installed */
      if (deploy_data == NULL)
        download = FALSE;

      /* Always remove debug */
      delete = TRUE;
    }

  if (g_str_has_suffix (extension, ".Locale"))
    locale_subset = TRUE;

  if (locale_subset)
    {
      extra_subpaths = flatpak_dir_get_locale_subpaths (self);

      /* Always remove locale */
      delete = TRUE;
    }

  subpaths = flatpak_subpaths_merge ((char **) old_subpaths, extra_subpaths);

  rel = g_new0 (FlatpakRelated, 1);
  rel->collection_id = g_strdup (extension_collection_id);
  rel->ref = g_strdup (extension_ref);
  rel->commit = g_strdup (checksum);
  rel->subpaths = g_steal_pointer (&subpaths);
  rel->download = download;
  rel->delete = delete;
  rel->auto_prune = auto_prune;

  g_ptr_array_add (related, rel);
}

GPtrArray *
flatpak_dir_find_remote_related_for_metadata (FlatpakDir         *self,
                                              FlatpakRemoteState *state,
                                              const char         *ref,
                                              GKeyFile           *metakey,
                                              GCancellable       *cancellable,
                                              GError            **error)
{
  int i;

  g_auto(GStrv) parts = NULL;
  g_autoptr(GPtrArray) related = g_ptr_array_new_with_free_func ((GDestroyNotify) flatpak_related_free);
  g_autofree char *url = NULL;
  g_auto(GStrv) groups = NULL;

  parts = flatpak_decompose_ref (ref, error);
  if (parts == NULL)
    return NULL;

  if (!ostree_repo_remote_get_url (self->repo,
                                   state->remote_name,
                                   &url,
                                   error))
    return FALSE;

  if (*url == 0)
    return g_steal_pointer (&related);  /* Empty url, silently disables updates */

  groups = g_key_file_get_groups (metakey, NULL);
  for (i = 0; groups[i] != NULL; i++)
    {
      char *tagged_extension;

      if (g_str_has_prefix (groups[i], FLATPAK_METADATA_GROUP_PREFIX_EXTENSION) &&
          *(tagged_extension = (groups[i] + strlen (FLATPAK_METADATA_GROUP_PREFIX_EXTENSION))) != 0)
        {
          g_autofree char *extension = NULL;
          g_autofree char *version = g_key_file_get_string (metakey, groups[i],
                                                            FLATPAK_METADATA_KEY_VERSION, NULL);
          g_auto(GStrv) versions = g_key_file_get_string_list (metakey, groups[i],
                                                               FLATPAK_METADATA_KEY_VERSIONS,
                                                               NULL, NULL);
          gboolean subdirectories = g_key_file_get_boolean (metakey, groups[i],
                                                            FLATPAK_METADATA_KEY_SUBDIRECTORIES, NULL);
          gboolean no_autodownload = g_key_file_get_boolean (metakey, groups[i],
                                                             FLATPAK_METADATA_KEY_NO_AUTODOWNLOAD, NULL);
          g_autofree char *download_if = g_key_file_get_string (metakey, groups[i],
                                                                FLATPAK_METADATA_KEY_DOWNLOAD_IF, NULL);
          g_autofree char *autoprune_unless = g_key_file_get_string (metakey, groups[i],
                                                                     FLATPAK_METADATA_KEY_AUTOPRUNE_UNLESS, NULL);
          gboolean autodelete = g_key_file_get_boolean (metakey, groups[i],
                                                        FLATPAK_METADATA_KEY_AUTODELETE, NULL);
          gboolean locale_subset = g_key_file_get_boolean (metakey, groups[i],
                                                           FLATPAK_METADATA_KEY_LOCALE_SUBSET, NULL);
          g_autofree char *extension_collection_id = NULL;
          const char *default_branches[] = { NULL, NULL};
          const char **branches;
          g_autofree char *checksum = NULL;
          int branch_i;

          /* Parse actual extension name */
          flatpak_parse_extension_with_tag (tagged_extension, &extension, NULL);

          if (versions)
            branches = (const char **) versions;
          else
            {
              if (version)
                default_branches[0] = version;
              else
                default_branches[0] = parts[3];
              branches = default_branches;
            }

          extension_collection_id = g_key_file_get_string (metakey, groups[i],
                                                           FLATPAK_METADATA_KEY_COLLECTION_ID, NULL);

          /* For the moment, none of the related ref machinery handles
           * collection IDs which don’t match the original ref. */
          if (extension_collection_id != NULL && *extension_collection_id != '\0' &&
              g_strcmp0 (extension_collection_id, state->collection_id) != 0)
            {
              g_debug ("Skipping related extension ‘%s’ because it’s in collection "
                       "‘%s’ which does not match the current remote ‘%s’.",
                       extension, extension_collection_id, state->collection_id);
              continue;
            }

          g_clear_pointer (&extension_collection_id, g_free);
          extension_collection_id = g_strdup (state->collection_id);

          for (branch_i = 0; branches[branch_i] != NULL; branch_i++)
            {
              g_autofree char *extension_ref = NULL;
              const char *branch = branches[branch_i];

              extension_ref = g_build_filename ("runtime", extension, parts[2], branch, NULL);

              if (flatpak_remote_state_lookup_ref (state, extension_ref, &checksum, NULL, NULL))
                {
                  add_related (self, related, extension, extension_collection_id, extension_ref, checksum,
                               no_autodownload, download_if, autoprune_unless, autodelete, locale_subset);
                }
              else if (subdirectories)
                {
                  g_auto(GStrv) refs = flatpak_remote_state_match_subrefs (state, extension_ref);
                  int j;
                  for (j = 0; refs[j] != NULL; j++)
                    {
                      g_autofree char *subref_checksum = NULL;

                      if (flatpak_remote_state_lookup_ref (state, refs[j], &subref_checksum, NULL, NULL))
                        add_related (self, related, extension, extension_collection_id, refs[j], subref_checksum,
                                     no_autodownload, download_if, autoprune_unless, autodelete, locale_subset);
                    }
                }
            }
        }
    }

  return g_steal_pointer (&related);
}


GPtrArray *
flatpak_dir_find_remote_related (FlatpakDir         *self,
                                 FlatpakRemoteState *state,
                                 const char         *ref,
                                 GCancellable       *cancellable,
                                 GError            **error)
{
  const char *metadata = NULL;

  g_autoptr(GKeyFile) metakey = g_key_file_new ();
  g_auto(GStrv) parts = NULL;
  g_autoptr(GPtrArray) related = NULL;
  g_autofree char *url = NULL;

  parts = flatpak_decompose_ref (ref, error);
  if (parts == NULL)
    return NULL;

  if (!ostree_repo_remote_get_url (self->repo,
                                   state->remote_name,
                                   &url,
                                   error))
    return FALSE;

  if (*url == 0)
    return g_steal_pointer (&related);  /* Empty url, silently disables updates */

  if (flatpak_remote_state_lookup_cache (state, ref,
                                         NULL, NULL, &metadata,
                                         NULL) &&
      g_key_file_load_from_data (metakey, metadata, -1, 0, NULL))
    related = flatpak_dir_find_remote_related_for_metadata (self, state, ref, metakey, cancellable, error);
  else
    related = g_ptr_array_new_with_free_func ((GDestroyNotify) flatpak_related_free);

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
          const char *partial_ref_and_origin = key;
          g_autofree char *partial_ref = NULL;
          g_auto(GStrv) cur_parts = NULL;

          ostree_parse_refspec (partial_ref_and_origin, NULL, &partial_ref, NULL);

          cur_parts = g_strsplit (partial_ref, "/", -1);

          /* Must match type, arch, branch */
          if (strcmp (parts[2], cur_parts[1]) != 0 ||
              strcmp (parts[3], cur_parts[2]) != 0)
            continue;

          /* But only prefix of id */
          if (!g_str_has_prefix (cur_parts[0], parts_prefix))
            continue;

          g_ptr_array_add (matches, g_strconcat (parts[0], "/", partial_ref, NULL));
        }
    }

  return matches;
}

GPtrArray *
flatpak_dir_find_local_related_for_metadata (FlatpakDir   *self,
                                             const char   *ref,
                                             const char   *remote_name,
                                             GKeyFile     *metakey,
                                             GCancellable *cancellable,
                                             GError      **error)
{
  int i;

  g_auto(GStrv) parts = NULL;
  g_autoptr(GPtrArray) related = g_ptr_array_new_with_free_func ((GDestroyNotify) flatpak_related_free);
  g_autofree char *collection_id = NULL;
  g_auto(GStrv) groups = NULL;

  /* Derive the collection ID from the remote we are querying. This will act as
   * a sanity check on the summary ref lookup. */
  if (!repo_get_remote_collection_id (self->repo, remote_name, &collection_id, error))
    return NULL;

  parts = flatpak_decompose_ref (ref, error);
  if (parts == NULL)
    return NULL;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return NULL;

  groups = g_key_file_get_groups (metakey, NULL);
  for (i = 0; groups[i] != NULL; i++)
    {
      char *tagged_extension;

      if (g_str_has_prefix (groups[i], FLATPAK_METADATA_GROUP_PREFIX_EXTENSION) &&
          *(tagged_extension = (groups[i] + strlen (FLATPAK_METADATA_GROUP_PREFIX_EXTENSION))) != 0)
        {
          g_autofree char *extension = NULL;
          g_autofree char *version = g_key_file_get_string (metakey, groups[i],
                                                            FLATPAK_METADATA_KEY_VERSION, NULL);
          g_auto(GStrv) versions = g_key_file_get_string_list (metakey, groups[i],
                                                               FLATPAK_METADATA_KEY_VERSIONS,
                                                               NULL, NULL);
          gboolean subdirectories = g_key_file_get_boolean (metakey, groups[i],
                                                            FLATPAK_METADATA_KEY_SUBDIRECTORIES, NULL);
          gboolean no_autodownload = g_key_file_get_boolean (metakey, groups[i],
                                                             FLATPAK_METADATA_KEY_NO_AUTODOWNLOAD, NULL);
          g_autofree char *download_if = g_key_file_get_string (metakey, groups[i],
                                                                FLATPAK_METADATA_KEY_DOWNLOAD_IF, NULL);
          g_autofree char *autoprune_unless = g_key_file_get_string (metakey, groups[i],
                                                                     FLATPAK_METADATA_KEY_AUTOPRUNE_UNLESS, NULL);
          gboolean autodelete = g_key_file_get_boolean (metakey, groups[i],
                                                        FLATPAK_METADATA_KEY_AUTODELETE, NULL);
          gboolean locale_subset = g_key_file_get_boolean (metakey, groups[i],
                                                           FLATPAK_METADATA_KEY_LOCALE_SUBSET, NULL);
          g_autofree char *extension_collection_id = NULL;
          const char *default_branches[] = { NULL, NULL};
          const char **branches;
          int branch_i;

          /* Parse actual extension name */
          flatpak_parse_extension_with_tag (tagged_extension, &extension, NULL);

          if (versions)
            branches = (const char **) versions;
          else
            {
              if (version)
                default_branches[0] = version;
              else
                default_branches[0] = parts[3];
              branches = default_branches;
            }

          extension_collection_id = g_key_file_get_string (metakey, groups[i],
                                                           FLATPAK_METADATA_KEY_COLLECTION_ID, NULL);

          /* As we’re looking locally, we can’t support extension
           * collection IDs which don’t match the current remote (since the
           * associated refs could be anywhere). */
          if (extension_collection_id != NULL && *extension_collection_id != '\0' &&
              g_strcmp0 (extension_collection_id, collection_id) != 0)
            {
              g_debug ("Skipping related extension ‘%s’ because it’s in collection "
                       "‘%s’ which does not match the current remote ‘%s’.",
                       extension, extension_collection_id, collection_id);
              continue;
            }

          g_clear_pointer (&extension_collection_id, g_free);
          extension_collection_id = g_strdup (collection_id);

          for (branch_i = 0; branches[branch_i] != NULL; branch_i++)
            {
              g_autofree char *extension_ref = NULL;
              g_autofree char *checksum = NULL;
              g_autofree char *prefixed_extension_ref = NULL;
              const char *branch = branches[branch_i];

              extension_ref = g_build_filename ("runtime", extension, parts[2], branch, NULL);
              prefixed_extension_ref = g_strdup_printf ("%s:%s", remote_name, extension_ref);
              if (ostree_repo_resolve_rev (self->repo,
                                           prefixed_extension_ref,
                                           FALSE,
                                           &checksum,
                                           NULL))
                {
                  add_related (self, related, extension, extension_collection_id, extension_ref,
                               checksum, no_autodownload, download_if, autoprune_unless, autodelete, locale_subset);
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
                                       extension_collection_id, match, match_checksum,
                                       no_autodownload, download_if, autoprune_unless, autodelete, locale_subset);
                        }
                    }
                }
            }
        }
    }

  return g_steal_pointer (&related);
}


GPtrArray *
flatpak_dir_find_local_related (FlatpakDir   *self,
                                const char   *ref,
                                const char   *remote_name,
                                gboolean      deployed,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_autofree char *metadata_contents = NULL;
  g_autoptr(GKeyFile) metakey = g_key_file_new ();
  g_autoptr(GPtrArray) related = NULL;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return NULL;

  if (deployed)
    {
      deploy_dir = flatpak_dir_get_if_deployed (self, ref, NULL, cancellable);
      if (deploy_dir == NULL)
        {
          g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                       _("%s not installed"), ref);
          return NULL;
        }

      metadata = g_file_get_child (deploy_dir, "metadata");
      if (!g_file_load_contents (metadata, cancellable, &metadata_contents, NULL, NULL, NULL))
        {
          g_debug ("No metadata in local deploy");
          /* No metadata => no related, but no error */
        }
    }
  else
    {
      g_autofree char *checksum = NULL;
      g_autoptr(GVariant) commit_data = flatpak_dir_read_latest_commit (self, remote_name, ref, &checksum, NULL, NULL);
      if (commit_data)
        {
          g_autoptr(GVariant) commit_metadata = g_variant_get_child_value (commit_data, 0);
          g_variant_lookup (commit_metadata, "xa.metadata", "s", &metadata_contents);
          if (metadata_contents == NULL)
            g_debug ("No xa.metadata in local commit %s ref %s", checksum, ref);
        }
    }

  if (metadata_contents &&
      g_key_file_load_from_data (metakey, metadata_contents, -1, 0, NULL))
    related = flatpak_dir_find_local_related_for_metadata (self, ref, remote_name, metakey, cancellable, error);
  else
    related = g_ptr_array_new_with_free_func ((GDestroyNotify) flatpak_related_free);

  return g_steal_pointer (&related);
}

static GDBusProxy *
get_localed_dbus_proxy (void)
{
  const char *localed_bus_name = "org.freedesktop.locale1";
  const char *localed_object_path = "/org/freedesktop/locale1";
  const char *localed_interface_name = localed_bus_name;

  return g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                        G_DBUS_PROXY_FLAGS_NONE,
                                        NULL,
                                        localed_bus_name,
                                        localed_object_path,
                                        localed_interface_name,
                                        NULL,
                                        NULL);
}

static void
get_locale_langs_from_localed_dbus (GDBusProxy *proxy, GPtrArray *langs)
{
  g_autoptr(GVariant) locale_variant = NULL;
  g_autofree const gchar **strv = NULL;
  gsize i, j;

  locale_variant = g_dbus_proxy_get_cached_property (proxy, "Locale");
  if (locale_variant == NULL)
    return;

  strv = g_variant_get_strv (locale_variant, NULL);

  for (i = 0; strv[i]; i++)
    {
      const gchar *locale = NULL;
      g_autofree char *lang = NULL;

      /* See locale(7) for these categories */
      const char* const categories[] = { "LANG=", "LC_ALL=", "LC_MESSAGES=", "LC_ADDRESS=",
                                         "LC_COLLATE=", "LC_CTYPE=", "LC_IDENTIFICATION=",
                                         "LC_MONETARY=", "LC_MEASUREMENT=", "LC_NAME=",
                                         "LC_NUMERIC=", "LC_PAPER=", "LC_TELEPHONE=",
                                         "LC_TIME=", NULL };

      for (j = 0; categories[j]; j++)
        {
          if (g_str_has_prefix (strv[i], categories[j]))
            {
              locale = strv[i] + strlen (categories[j]);
              break;
            }
        }

      if (locale == NULL || strcmp (locale, "") == 0)
        continue;

      lang = flatpak_get_lang_from_locale (locale);
      if (lang != NULL && !flatpak_g_ptr_array_contains_string (langs, lang))
        g_ptr_array_add (langs, g_steal_pointer (&lang));
    }
}

static GDBusProxy *
get_accounts_dbus_proxy (void)
{
  const char *accounts_bus_name = "org.freedesktop.Accounts";
  const char *accounts_object_path = "/org/freedesktop/Accounts";
  const char *accounts_interface_name = accounts_bus_name;

  return g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                        G_DBUS_PROXY_FLAGS_NONE,
                                        NULL,
                                        accounts_bus_name,
                                        accounts_object_path,
                                        accounts_interface_name,
                                        NULL,
                                        NULL);
}

static void
get_locale_langs_from_accounts_dbus (GDBusProxy *proxy, GPtrArray *langs)
{
  const char *accounts_bus_name = "org.freedesktop.Accounts";
  const char *accounts_interface_name = "org.freedesktop.Accounts.User";
  g_auto(GStrv) object_paths = NULL;
  int i;
  g_autoptr(GVariant) ret = NULL;

  ret = g_dbus_proxy_call_sync (G_DBUS_PROXY (proxy),
                                "ListCachedUsers",
                                g_variant_new ("()"),
                                G_DBUS_CALL_FLAGS_NONE,
                                -1,
                                NULL,
                                NULL);
  if (ret != NULL)
    g_variant_get (ret,
                   "(^ao)",
                   &object_paths);

  if (object_paths != NULL)
    {
      for (i = 0; object_paths[i] != NULL; i++)
        {
          g_autoptr(GDBusProxy) accounts_proxy = NULL;
          g_autoptr(GVariant) value = NULL;

          accounts_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                          G_DBUS_PROXY_FLAGS_NONE,
                                                          NULL,
                                                          accounts_bus_name,
                                                          object_paths[i],
                                                          accounts_interface_name,
                                                          NULL,
                                                          NULL);

          if (accounts_proxy)
            {
              value = g_dbus_proxy_get_cached_property (accounts_proxy, "Language");
              if (value != NULL)
                {
                  const char *locale = g_variant_get_string (value, NULL);
                  g_autofree char *lang = NULL;

                  if (strcmp (locale, "") == 0)
                    continue; /* This user wants the system default locale */

                  lang = flatpak_get_lang_from_locale (locale);
                  if (lang != NULL && !flatpak_g_ptr_array_contains_string (langs, lang))
                    g_ptr_array_add (langs, g_steal_pointer (&lang));
                }
            }
        }
    }
}

static int
cmpstringp (const void *p1, const void *p2)
{
  return strcmp (*(char * const *) p1, *(char * const *) p2);
}

static char **
sort_strv (char **strv)
{
  qsort (strv, g_strv_length (strv), sizeof (const char *), cmpstringp);
  return strv;
}

char **
flatpak_dir_get_default_locale_languages (FlatpakDir *self)
{
  g_autoptr(GPtrArray) langs = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GDBusProxy) localed_proxy = NULL;
  g_autoptr(GDBusProxy) accounts_proxy = NULL;

  if (flatpak_dir_is_user (self))
    return flatpak_get_current_locale_langs ();

  /* First get the system default locales */
  localed_proxy = get_localed_dbus_proxy ();
  if (localed_proxy != NULL)
    get_locale_langs_from_localed_dbus (localed_proxy, langs);

  /* Now add the user account locales from AccountsService. If accounts_proxy is
   * not NULL, it means that AccountsService exists */
  accounts_proxy = get_accounts_dbus_proxy ();
  if (accounts_proxy != NULL)
    get_locale_langs_from_accounts_dbus (accounts_proxy, langs);

  /* If none were found, fall back to using all languages */
  if (langs->len == 0)
    return g_new0 (char *, 1);

  g_ptr_array_sort (langs, flatpak_strcmp0_ptr);
  g_ptr_array_add (langs, NULL);

  return (char **) g_ptr_array_free (g_steal_pointer (&langs), FALSE);
}

char **
flatpak_dir_get_locale_languages (FlatpakDir *self)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);

  if (config)
    {
      char **langs = g_key_file_get_string_list (config, "core", "xa.languages", NULL, NULL);
      if (langs)
        return sort_strv (langs);
    }

  return flatpak_dir_get_default_locale_languages (self);
}

char **
flatpak_dir_get_locale_subpaths (FlatpakDir *self)
{
  char **subpaths = flatpak_dir_get_locale_languages (self);
  int i;

  /* Convert languages to paths */
  for (i = 0; subpaths[i] != NULL; i++)
    {
      char *lang = subpaths[i];
      /* For backwards compat with old xa.languages we support the configuration having slashes already */
      if (*lang != '/')
        {
          subpaths[i] = g_strconcat ("/", lang, NULL);
          g_free (lang);
        }
    }
  return subpaths;
}

/* The flatpak_collection_ref_* methods were copied from the
 * ostree_collection_ref_* ones */
FlatpakCollectionRef *
flatpak_collection_ref_new (const gchar *collection_id,
                            const gchar *ref_name)
{
  g_autoptr(FlatpakCollectionRef) collection_ref = NULL;

  collection_ref = g_new0 (FlatpakCollectionRef, 1);
  collection_ref->collection_id = g_strdup (collection_id);
  collection_ref->ref_name = g_strdup (ref_name);

  return g_steal_pointer (&collection_ref);
}

void
flatpak_collection_ref_free (FlatpakCollectionRef *ref)
{
  g_return_if_fail (ref != NULL);

  g_free (ref->collection_id);
  g_free (ref->ref_name);
  g_free (ref);
}

guint
flatpak_collection_ref_hash (gconstpointer ref)
{
  const FlatpakCollectionRef *_ref = ref;

  if (_ref->collection_id != NULL)
    return g_str_hash (_ref->collection_id) ^ g_str_hash (_ref->ref_name);
  else
    return g_str_hash (_ref->ref_name);
}

gboolean
flatpak_collection_ref_equal (gconstpointer ref1,
                              gconstpointer ref2)
{
  const FlatpakCollectionRef *_ref1 = ref1, *_ref2 = ref2;

  return g_strcmp0 (_ref1->collection_id, _ref2->collection_id) == 0 &&
         g_strcmp0 (_ref1->ref_name, _ref2->ref_name) == 0;
}

void
flatpak_dir_set_source_pid (FlatpakDir *self,
                            pid_t      pid)
{
  self->source_pid = pid;
}

pid_t
flatpak_dir_get_source_pid (FlatpakDir *self)
{
  return self->source_pid;
}

static void
(flatpak_dir_log) (FlatpakDir *self,
                   const char *file,
                   int line,
                   const char *func,
                   const char *source, /* overrides self->name */
                   const char *change,
                   const char *remote,
                   const char *ref,
                   const char *commit,
                   const char *old_commit,
                   const char *url,
                   const char *format,
                 ...)
{
#ifdef HAVE_LIBSYSTEMD
  const char *installation = source ? source : flatpak_dir_get_name_cached (self);
  pid_t source_pid = flatpak_dir_get_source_pid (self);
  char message[1024];
  int len;
  va_list args;

  len = g_snprintf (message, sizeof (message), "%s: ", installation);

  va_start (args, format);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
  g_vsnprintf (message + len, sizeof (message) - len, format, args);
#pragma GCC diagnostic pop

  va_end (args);

  /* See systemd.journal-fields(7) for the meaning of the
   * standard fields we use, in particular OBJECT_PID
   */
  sd_journal_send ("MESSAGE_ID=" FLATPAK_MESSAGE_ID,
                   "PRIORITY=5",
                   "OBJECT_PID=%d", source_pid,
                   "CODE_FILE=%s", file,
                   "CODE_LINE=%d", line,
                   "CODE_FUNC=%s", func,
                   "MESSAGE=%s", message,
                   /* custom fields below */
                   "FLATPAK_VERSION=" PACKAGE_VERSION,
                   "INSTALLATION=%s", installation,
                   "OPERATION=%s", change,
                   "REMOTE=%s", remote ? remote : "",
                   "REF=%s", ref ? ref : "",
                   "COMMIT=%s", commit ? commit : "",
                   "OLD_COMMIT=%s", old_commit ? old_commit : "",
                   "URL=%s", url ? url : "",
                   NULL);
#endif
}
