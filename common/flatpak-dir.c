/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 *
 * Copyright © 2014-2019 Red Hat, Inc
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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <utime.h>

#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <ostree.h>

#ifdef USE_SYSTEM_HELPER
#include <polkit/polkit.h>
#endif

#include "flatpak-appdata-private.h"
#include "flatpak-dir-private.h"
#include "flatpak-error.h"
#include "flatpak-oci-registry-private.h"
#include "flatpak-ref.h"
#include "flatpak-run-private.h"
#include "flatpak-utils-base-private.h"
#include "flatpak-variant-private.h"
#include "flatpak-variant-impl-private.h"
#include "libglnx.h"
#include "system-helper/flatpak-system-helper.h"

#ifdef HAVE_LIBMALCONTENT
#include <libmalcontent/malcontent.h>
#include "flatpak-parental-controls-private.h"
#endif

#ifdef HAVE_LIBSYSTEMD
#define SD_JOURNAL_SUPPRESS_LOCATION
#include <systemd/sd-journal.h>
#endif

#define NO_SYSTEM_HELPER ((FlatpakSystemHelper *) (gpointer) 1)

#define SUMMARY_CACHE_TIMEOUT_SEC (60 * 5)
#define FILTER_MTIME_CHECK_TIMEOUT_MSEC 500

#define SYSCONF_INSTALLATIONS_DIR "installations.d"
#define SYSCONF_INSTALLATIONS_FILE_EXT ".conf"
#define SYSCONF_REMOTES_DIR "remotes.d"
#define SYSCONF_REMOTES_FILE_EXT ".flatpakrepo"

#define SIDELOAD_REPOS_DIR_NAME "sideload-repos"

#ifdef USE_SYSTEM_HELPER
/* This uses a weird Auto prefix to avoid conflicts with later added polkit types.
 */
typedef PolkitAuthority           AutoPolkitAuthority;
typedef PolkitAuthorizationResult AutoPolkitAuthorizationResult;
typedef PolkitDetails             AutoPolkitDetails;
typedef PolkitSubject             AutoPolkitSubject;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoPolkitAuthority, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoPolkitAuthorizationResult, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoPolkitDetails, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoPolkitSubject, g_object_unref)
#endif

static FlatpakOciRegistry *flatpak_dir_create_system_child_oci_registry (FlatpakDir   *self,
                                                                         GLnxLockFile *file_lock,
                                                                         const char   *token,
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
                                        const char          *opt_rev,
                                        const char          *skip_if_current_is,
                                        const char          *token,
                                        FlatpakProgress     *progress,
                                        GCancellable        *cancellable,
                                        GError             **error);

static gboolean flatpak_dir_remote_fetch_summary (FlatpakDir   *self,
                                                  const char   *name,
                                                  gboolean      only_cached,
                                                  GBytes      **out_summary,
                                                  GBytes      **out_summary_sig,
                                                  GCancellable *cancellable,
                                                  GError      **error);

static gboolean flatpak_dir_remote_fetch_summary_index (FlatpakDir   *self,
                                                        const char   *name_or_uri,
                                                        gboolean      only_cached,
                                                        GBytes      **out_index,
                                                        GBytes      **out_index_sig,
                                                        GCancellable *cancellable,
                                                        GError      **error);

static gboolean flatpak_dir_remote_fetch_indexed_summary (FlatpakDir   *self,
                                                          const char   *name_or_uri,
                                                          const char   *arch,
                                                          GVariant     *subsummary_info_v,
                                                          gboolean      only_cached,
                                                          GBytes      **out_summary,
                                                          GCancellable *cancellable,
                                                          GError      **error);

static gboolean flatpak_dir_gc_cached_digested_summaries (FlatpakDir   *self,
                                                          const char   *remote_name,
                                                          const char   *dont_prune_file,
                                                          GCancellable *cancellable,
                                                          GError      **error);

static gboolean flatpak_dir_cleanup_remote_for_url_change (FlatpakDir   *self,
                                                           const char   *remote_name,
                                                           const char   *url,
                                                           GCancellable *cancellable,
                                                           GError      **error);

static gboolean flatpak_dir_lookup_remote_filter (FlatpakDir *self,
                                                  const char *name,
                                                  gboolean    force_load,
                                                  char      **checksum_out,
                                                  GRegex    **allow_regex,
                                                  GRegex    **deny_regex,
                                                  GError **error);

static void ensure_http_session (FlatpakDir *self);

static void flatpak_dir_log (FlatpakDir *self,
                             const char *file,
                             int         line,
                             const char *func,
                             const char *source,
                             const char *change,
                             const char *remote,
                             const char *ref,
                             const char *commit,
                             const char *old_commit,
                             const char *url,
                             const char *format,
                             ...) G_GNUC_PRINTF (12, 13);

#define flatpak_dir_log(self, change, remote, ref, commit, old_commit, url, format, ...) \
  (flatpak_dir_log) (self, __FILE__, __LINE__, __FUNCTION__, \
                     NULL, change, remote, ref, commit, old_commit, url, format, __VA_ARGS__)

static GBytes *upgrade_deploy_data (GBytes             *deploy_data,
                                    GFile              *deploy_dir,
                                    FlatpakDecomposed  *ref,
                                    OstreeRepo         *repo,
                                    GCancellable       *cancellable,
                                    GError            **error);

typedef struct
{
  GBytes *bytes;
  GBytes *bytes_sig;
  char   *name;
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

typedef struct {
  GFile *path;
  GTimeVal mtime;
  guint64 last_mtime_check;
  char *checksum;
  GRegex *allow;
  GRegex *deny;
} RemoteFilter;

struct FlatpakDir
{
  GObject          parent;

  gboolean         user;
  GFile           *basedir;
  DirExtraData    *extra_data;
  OstreeRepo      *repo;
  GFile           *cache_dir;
  gboolean         no_system_helper;
  gboolean         no_interaction;
  pid_t            source_pid;

  GDBusConnection *system_helper_bus;

  GHashTable      *summary_cache;

  GHashTable      *remote_filters;

  /* Config cache, protected by config_cache lock */
  GRegex          *masked;
  GRegex          *pinned;

  FlatpakHttpSession *http_session;
};

G_LOCK_DEFINE_STATIC (config_cache);

typedef struct
{
  GObjectClass parent_class;
} FlatpakDirClass;

struct FlatpakDeploy
{
  GObject            parent;

  FlatpakDecomposed *ref;
  GFile             *dir;
  GKeyFile          *metadata;
  FlatpakContext    *system_overrides;
  FlatpakContext    *user_overrides;
  FlatpakContext    *system_app_overrides;
  FlatpakContext    *user_app_overrides;
  OstreeRepo        *repo;
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

static const char *
get_run_dir_location (void)
{
  static gsize path = 0;

  if (g_once_init_enter (&path))
    {
      gsize setup_value = 0;
      /* Note: $FLATPAK_RUN_DIR should only be set in the unit tests. At
       * runtime, /run/flatpak is assumed by
       * flatpak-create-sideload-symlinks.sh
       */
      const char *config_dir = g_getenv ("FLATPAK_RUN_DIR");
      if (config_dir != NULL)
        setup_value = (gsize) config_dir;
      else
        setup_value = (gsize) "/run/flatpak";
      g_once_init_leave (&path, setup_value);
    }

  return (const char *) path;
}

static void
flatpak_sideload_state_free (FlatpakSideloadState *sideload_state)
{
  g_object_unref (sideload_state->repo);
  g_variant_unref (sideload_state->summary);
  g_free (sideload_state);
}

static void
variant_maybe_unref (GVariant *variant)
{
  if (variant)
    g_variant_unref (variant);
}

static FlatpakRemoteState *
flatpak_remote_state_new (void)
{
  FlatpakRemoteState *state = g_new0 (FlatpakRemoteState, 1);

  state->refcount = 1;
  state->sideload_repos = g_ptr_array_new_with_free_func ((GDestroyNotify)flatpak_sideload_state_free);
  state->subsummaries = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)variant_maybe_unref);
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
      g_clear_pointer (&remote_state->index, g_variant_unref);
      g_clear_pointer (&remote_state->index_ht, g_hash_table_unref);
      g_clear_pointer (&remote_state->index_sig_bytes, g_bytes_unref);
      g_clear_pointer (&remote_state->subsummaries, g_hash_table_unref);
      g_clear_pointer (&remote_state->summary, g_variant_unref);
      g_clear_pointer (&remote_state->summary_bytes, g_bytes_unref);
      g_clear_pointer (&remote_state->summary_sig_bytes, g_bytes_unref);
      g_clear_error (&remote_state->summary_fetch_error);
      g_clear_pointer (&remote_state->allow_refs, g_regex_unref);
      g_clear_pointer (&remote_state->deny_refs, g_regex_unref);
      g_clear_pointer (&remote_state->sideload_repos, g_ptr_array_unref);

      g_free (remote_state);
    }
}

static gboolean
_validate_summary_for_collection_id (GVariant    *summary_v,
                                     const char  *collection_id,
                                     GError     **error)
{
  VarSummaryRef summary;
  summary = var_summary_from_gvariant (summary_v);

  if (!flatpak_summary_find_ref_map (summary, collection_id, NULL))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA,
                               _("Configured collection ID ‘%s’ not in summary file"), collection_id);

  return TRUE;
}

static void
flatpak_remote_state_add_sideload_repo (FlatpakRemoteState *self,
                                        GFile *dir)
{
  g_autoptr(GFile) summary_path = NULL;
  g_autoptr(GMappedFile) mfile = NULL;
  g_autoptr(OstreeRepo) sideload_repo = NULL;

  /* Sideloading only works if collection id is set */
  if (self->collection_id == NULL)
    return;

  summary_path = g_file_get_child (dir, "summary");
  sideload_repo = ostree_repo_new (dir);

  mfile = g_mapped_file_new (flatpak_file_get_path_cached (summary_path), FALSE, NULL);
  if (mfile != NULL && ostree_repo_open (sideload_repo, NULL, NULL))
    {
      g_autoptr(GError) local_error = NULL;
      g_autoptr(GBytes) summary_bytes = g_mapped_file_get_bytes (mfile);
      FlatpakSideloadState *ss = g_new0 (FlatpakSideloadState, 1);

      ss->repo = g_steal_pointer (&sideload_repo);
      ss->summary = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, summary_bytes, TRUE));

      if (!_validate_summary_for_collection_id (ss->summary, self->collection_id, &local_error))
        {
          /* We expect to hit this code path when the repo is providing things
           * from other remotes
           */
          g_info ("Sideload repo at path %s not valid for remote %s: %s",
                  flatpak_file_get_path_cached (dir), self->remote_name, local_error->message);
          flatpak_sideload_state_free (ss);
        }
      else
        {
          g_ptr_array_add (self->sideload_repos, ss);
          g_info ("Using sideloaded repo %s for remote %s", flatpak_file_get_path_cached (dir), self->remote_name);
        }
    }
}

static void add_sideload_subdirs (GPtrArray *res,
                                  GFile     *parent,
                                  gboolean   recurse);

static void
add_sideload_create_usb_subdirs (GPtrArray *res,
                                 GFile     *parent)
{
  g_autoptr(GFile) ostree_repo_subpath = NULL;
  g_autoptr(GFile) dot_ostree_repo_subpath = NULL;
  g_autoptr(GFile) dot_ostree_repo_d_subpath = NULL;
  g_autoptr(OstreeRepo) ostree_repo_subpath_repo = NULL;
  g_autoptr(OstreeRepo) dot_ostree_repo_subpath_repo = NULL;

  /* This path is not used by "flatpak create-usb" but it's a standard location
   * recognized by libostree; see the man page ostree create-usb(1)
   */
  ostree_repo_subpath = g_file_resolve_relative_path (parent, "ostree/repo");
  ostree_repo_subpath_repo = ostree_repo_new (ostree_repo_subpath);
  if (ostree_repo_open (ostree_repo_subpath_repo, NULL, NULL))
    g_ptr_array_add (res, g_object_ref (ostree_repo_subpath));

  /* These paths are used by "flatpak create-usb" */
  dot_ostree_repo_subpath = g_file_resolve_relative_path (parent, ".ostree/repo");
  dot_ostree_repo_subpath_repo = ostree_repo_new (dot_ostree_repo_subpath);
  if (ostree_repo_open (dot_ostree_repo_subpath_repo, NULL, NULL))
    g_ptr_array_add (res, g_object_ref (dot_ostree_repo_subpath));

  dot_ostree_repo_d_subpath = g_file_resolve_relative_path (parent, ".ostree/repos.d");
  add_sideload_subdirs (res, dot_ostree_repo_d_subpath, FALSE);
}

static void
add_sideload_subdirs (GPtrArray *res,
                      GFile     *parent,
                      gboolean   recurse)
{
  g_autoptr(GFileEnumerator) dir_enum = NULL;

  dir_enum = g_file_enumerate_children (parent,
                                        G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                        G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                        G_FILE_QUERY_INFO_NONE,
                                        NULL, NULL);
  if (dir_enum == NULL)
    return;

  while (TRUE)
    {
      GFileInfo *info;
      GFile *path;

      if (!g_file_enumerator_iterate (dir_enum, &info, &path, NULL, NULL) ||
          info == NULL)
        break;

      /* Here we support either a plain repo or, if @recurse is TRUE, the root
       * directory of a USB created with "flatpak create-usb"
       */
      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        {
          g_autoptr(OstreeRepo) repo = ostree_repo_new (path);

          if (ostree_repo_open (repo, NULL, NULL))
            g_ptr_array_add (res, g_object_ref (path));
          else if (recurse)
            add_sideload_create_usb_subdirs (res, path);
        }
    }
}

void
flatpak_remote_state_add_sideload_dir (FlatpakRemoteState *self,
                                       GFile              *dir)
{
  g_autoptr(GPtrArray) sideload_paths = g_ptr_array_new_with_free_func (g_object_unref);

  /* The directory could be a repo */
  flatpak_remote_state_add_sideload_repo (self, dir);

  /* Or it could be a directory with repos in well-known subdirectories */
  add_sideload_create_usb_subdirs (sideload_paths, dir);
  for (int i = 0; i < sideload_paths->len; i++)
    flatpak_remote_state_add_sideload_repo (self, g_ptr_array_index (sideload_paths, i));
}

gboolean
flatpak_remote_state_ensure_summary (FlatpakRemoteState *self,
                                     GError            **error)
{
  if (self->index == NULL && self->summary == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Unable to load summary from remote %s: %s"), self->remote_name,
                               self->summary_fetch_error != NULL ? self->summary_fetch_error->message : "unknown error");

  return TRUE;
}

gboolean
flatpak_remote_state_ensure_subsummary (FlatpakRemoteState *self,
                                        FlatpakDir         *dir,
                                        const char         *arch,
                                        gboolean            only_cached,
                                        GCancellable       *cancellable,
                                        GError            **error)
{
  GVariant *subsummary;
  const char *alt_arch;
  GVariant *subsummary_info_v;

  g_autoptr(GBytes) bytes = NULL;

  if (self->summary != NULL)
    return TRUE; /* We have them all anyway */

  if (self->index == NULL)
    return TRUE; /* Don't fail unnecessarily in e.g. the sideload case */

  if (g_hash_table_contains (self->subsummaries, arch))
    return TRUE;

  /* If i.e. we already loaded x86_64 subsummary (which has i386 refs),
   * don't load i386 one */
  alt_arch = flatpak_get_compat_arch_reverse (arch);
  if (alt_arch != NULL &&
      g_hash_table_contains (self->subsummaries, alt_arch))
    return TRUE;

  subsummary_info_v = g_hash_table_lookup (self->index_ht, arch);
  if (subsummary_info_v == NULL)
    return TRUE; /* No refs for this arch */

  if (!flatpak_dir_remote_fetch_indexed_summary (dir, self->remote_name, arch, subsummary_info_v, only_cached,
                                                 &bytes, cancellable, error))
    return FALSE;

  subsummary = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, bytes, FALSE));
  g_hash_table_insert (self->subsummaries, g_strdup (arch), subsummary);

  return TRUE;
}

gboolean
flatpak_remote_state_ensure_subsummary_all_arches (FlatpakRemoteState *self,
                                                   FlatpakDir         *dir,
                                                   gboolean            only_cached,
                                                   GCancellable       *cancellable,
                                                   GError            **error)
{
  if (self->index_ht == NULL)
    return TRUE; /* No subsummaries, got all arches anyway */

  GLNX_HASH_TABLE_FOREACH (self->index_ht, const char *, arch)
    {
      g_autoptr(GError) local_error = NULL;

      if (!flatpak_remote_state_ensure_subsummary (self, dir, arch, only_cached, cancellable, &local_error))
        {
          /* Don't error on non-cached subsummaries */
          if (only_cached && g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_CACHED))
            continue;

          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
    }

  return TRUE;
}


gboolean
flatpak_remote_state_allow_ref (FlatpakRemoteState *self,
                                const char *ref)
{
  return flatpak_filters_allow_ref (self->allow_refs, self->deny_refs, ref);
}


static guint64
get_timestamp_from_ref_info (VarRefInfoRef info)
{
  VarMetadataRef metadata = var_ref_info_get_metadata (info);
  return GUINT64_FROM_BE(var_metadata_lookup_uint64 (metadata, OSTREE_COMMIT_TIMESTAMP, 0));
 }


GFile *
flatpak_remote_state_lookup_sideload_checksum (FlatpakRemoteState *self,
                                               char               *checksum)
{
  for (int i = 0; i < self->sideload_repos->len; i++)
    {
      FlatpakSideloadState *ss = g_ptr_array_index (self->sideload_repos, i);
      OstreeRepoCommitState commit_state;

      if (ostree_repo_load_commit (ss->repo, checksum, NULL, &commit_state, NULL) &&
          commit_state == OSTREE_REPO_COMMIT_STATE_NORMAL)
        return g_object_ref (ostree_repo_get_path (ss->repo));
    }

  return NULL;
}

static gboolean
flatpak_remote_state_resolve_sideloaded_ref (FlatpakRemoteState *self,
                                             const char         *ref,
                                             char              **out_checksum,
                                             guint64            *out_timestamp,
                                             VarRefInfoRef      *out_info,
                                             FlatpakSideloadState  **out_sideload_state,
                                             GError            **error)
{
  g_autofree char *latest_checksum = NULL;
  guint64 latest_timestamp = 0;
  FlatpakSideloadState *latest_ss = NULL;
  VarRefInfoRef latest_sideload_info;

  for (int i = 0; i < self->sideload_repos->len; i++)
    {
      FlatpakSideloadState *ss = g_ptr_array_index (self->sideload_repos, i);
      g_autofree char *sideload_checksum = NULL;
      VarRefInfoRef sideload_info;

      if (flatpak_summary_lookup_ref (ss->summary, self->collection_id, ref, &sideload_checksum, &sideload_info))
        {
          guint64 timestamp = get_timestamp_from_ref_info (sideload_info);

          if (latest_checksum == 0 || latest_timestamp < timestamp)
            {
              g_free (latest_checksum);
              latest_checksum = g_steal_pointer (&sideload_checksum);
              latest_timestamp = timestamp;
              latest_sideload_info = sideload_info;
              latest_ss = ss;
            }
        }
    }

  if (latest_checksum == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_REF_NOT_FOUND,
                               _("No such ref '%s' in remote %s"),
                               ref, self->remote_name);

  if (out_checksum)
    *out_checksum = g_steal_pointer (&latest_checksum);
  if (out_timestamp)
    *out_timestamp = latest_timestamp;
  if (out_info)
    *out_info = latest_sideload_info;
  if (out_sideload_state)
    *out_sideload_state = latest_ss;

  return TRUE;
}

static GVariant *
get_summary_for_ref (FlatpakRemoteState *self,
                     const char *ref)
{
  GVariant *summary = NULL;

  if (self->index != NULL)
    {
      g_autofree char * arch = flatpak_get_arch_for_ref (ref);

      if (arch != NULL)
        summary = g_hash_table_lookup (self->subsummaries, arch);

      if (summary == NULL && arch != NULL)
        {
          const char *non_compat_arch = flatpak_get_compat_arch_reverse (arch);

          if (non_compat_arch != NULL)
            summary = g_hash_table_lookup (self->subsummaries, non_compat_arch);
        }
    }
  else
    summary = self->summary;

  return summary;
}

/* Returns TRUE if the ref is found in the summary or cache.
 * out_checksum and out_variant are only set when the ref is found.
 */
gboolean
flatpak_remote_state_lookup_ref (FlatpakRemoteState *self,
                                 const char         *ref,
                                 char              **out_checksum,
                                 guint64            *out_timestamp,
                                 VarRefInfoRef      *out_info,
                                 GFile             **out_sideload_path,
                                 GError            **error)
{
  if (!flatpak_remote_state_allow_ref (self, ref))
    {
      return flatpak_fail_error (error, FLATPAK_ERROR_REF_NOT_FOUND,
                                 _("No entry for %s in remote '%s' summary flatpak cache "),
                                 ref, self->remote_name);
    }

  /* If there is a summary we use it for metadata and for latest. We may later install from a sideloaded source though */
  if (self->summary != NULL || self->index != NULL)
    {
      VarRefInfoRef info;
      g_autofree char *checksum = NULL;
      GVariant *summary;

      summary = get_summary_for_ref (self, ref);
      if (summary == NULL ||
          !flatpak_summary_lookup_ref (summary, NULL, ref, &checksum, &info))
        return flatpak_fail_error (error, FLATPAK_ERROR_REF_NOT_FOUND,
                                   _("No such ref '%s' in remote %s"),
                                   ref, self->remote_name);

      /* Even if its available in the summary we want to install it from a sideload repo if available */

      if (out_sideload_path)
        {
          g_autoptr(GFile) found_sideload_path = NULL;

          for (int i = 0; i < self->sideload_repos->len; i++)
            {
              FlatpakSideloadState *ss = g_ptr_array_index (self->sideload_repos, i);
              OstreeRepoCommitState commit_state;

              if (ostree_repo_load_commit (ss->repo, checksum, NULL, &commit_state, NULL) &&
                  commit_state == OSTREE_REPO_COMMIT_STATE_NORMAL)
                {
                  found_sideload_path = g_object_ref (ostree_repo_get_path (ss->repo));
                  break;
                }
            }

          *out_sideload_path = g_steal_pointer (&found_sideload_path);
        }

      if (out_info)
        *out_info = info;
      if (out_checksum)
        *out_checksum = g_steal_pointer (&checksum);
      if (out_timestamp)
        *out_timestamp = get_timestamp_from_ref_info (info);
    }
  else
    {
      FlatpakSideloadState *ss = NULL;

      if (!flatpak_remote_state_resolve_sideloaded_ref (self, ref, out_checksum, out_timestamp, out_info, &ss, error))
        return FALSE;

      if (out_sideload_path)
        *out_sideload_path = g_object_ref (ostree_repo_get_path (ss->repo));
    }

  return TRUE;
}

GPtrArray *
flatpak_remote_state_match_subrefs (FlatpakRemoteState *self,
                                    FlatpakDecomposed *ref)
{
  GVariant *summary;

  if (self->summary == NULL && self->index == NULL)
    {
      g_info ("flatpak_remote_state_match_subrefs with no summary");
      return g_ptr_array_new_with_free_func ((GDestroyNotify)flatpak_decomposed_unref);
    }

  summary = get_summary_for_ref (self, flatpak_decomposed_get_ref (ref));
  if (summary == NULL)
    return g_ptr_array_new_with_free_func ((GDestroyNotify)flatpak_decomposed_unref);

  return flatpak_summary_match_subrefs (summary, NULL, ref);
}

static VarMetadataRef
flatpak_remote_state_get_main_metadata (FlatpakRemoteState *self)
{
  VarSummaryRef summary;
  VarSummaryIndexRef index;
  VarMetadataRef meta;

  if (self->index)
    {
      index = var_summary_index_from_gvariant (self->index);
      meta = var_summary_index_get_metadata (index);
    }
  else if (self->summary)
    {
      summary = var_summary_from_gvariant (self->summary);
      meta = var_summary_get_metadata (summary);
    }
  else
    g_assert_not_reached ();

  return meta;
}


/* 0 if not specified */
static guint32
flatpak_remote_state_get_cache_version (FlatpakRemoteState *self)
{
  VarMetadataRef meta;

  if (!flatpak_remote_state_ensure_summary (self, NULL))
    return 0;

  meta = flatpak_remote_state_get_main_metadata (self);
  return GUINT32_FROM_LE (var_metadata_lookup_uint32 (meta, "xa.cache-version", 0));
}

gboolean
flatpak_remote_state_lookup_cache (FlatpakRemoteState *self,
                                   const char         *ref,
                                   guint64            *out_download_size,
                                   guint64            *out_installed_size,
                                   const char        **out_metadata,
                                   GError            **error)
{
  VarCacheDataRef cache_data;
  VarMetadataRef meta;
  VarSummaryRef summary;
  guint32 summary_version;
  GVariant *summary_v;

  if (!flatpak_remote_state_ensure_summary (self, error))
    return FALSE;

  summary_v = get_summary_for_ref (self, ref);
  if (summary_v == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_REF_NOT_FOUND,
                               _("No entry for %s in remote '%s' summary flatpak cache "),
                               ref, self->remote_name);


  summary = var_summary_from_gvariant (summary_v);
  meta = var_summary_get_metadata (summary);

  summary_version = GUINT32_FROM_LE (var_metadata_lookup_uint32 (meta, "xa.summary-version", 0));

  if (summary_version == 0)
    {
      VarCacheRef cache;
      gsize pos;
      VarVariantRef cache_vv;
      VarVariantRef cache_v;

      if (!var_metadata_lookup (meta, "xa.cache", NULL, &cache_vv))
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("No summary or Flatpak cache available for remote %s"),
                              self->remote_name);
          return FALSE;
        }

      /* For stupid historical reasons the xa.cache is double-wrapped in a variant */
      cache_v = var_variant_from_variant (cache_vv);
      cache = var_cache_from_variant (cache_v);

      if (!var_cache_lookup (cache, ref, &pos, &cache_data))
        return flatpak_fail_error (error, FLATPAK_ERROR_REF_NOT_FOUND,
                                   _("No entry for %s in remote '%s' summary flatpak cache "),
                                   ref, self->remote_name);
    }
  else if (summary_version == 1)
    {
      VarRefMapRef ref_map = var_summary_get_ref_map (summary);
      VarRefInfoRef info;
      VarMetadataRef commit_metadata;
      VarVariantRef cache_data_v;

      if (!flatpak_var_ref_map_lookup_ref (ref_map, ref, &info))
        return flatpak_fail_error (error, FLATPAK_ERROR_REF_NOT_FOUND,
                                   _("No entry for %s in remote '%s' summary cache "),
                                   ref, self->remote_name);

      commit_metadata = var_ref_info_get_metadata (info);
      if (!var_metadata_lookup (commit_metadata, "xa.data", NULL, &cache_data_v))
        return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Missing xa.data in summary for remote %s"),
                                   self->remote_name);
      cache_data = var_cache_data_from_variant (cache_data_v);
    }
  else
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Unsupported summary version %d for remote %s"),
                          summary_version, self->remote_name);
      return FALSE;
    }

  if (out_installed_size)
    *out_installed_size = var_cache_data_get_installed_size (cache_data);

  if (out_download_size)
    *out_download_size = var_cache_data_get_download_size (cache_data);

  if (out_metadata)
    *out_metadata = var_cache_data_get_metadata (cache_data);

  return TRUE;
}

gboolean
flatpak_remote_state_load_data (FlatpakRemoteState *self,
                                const char         *ref,
                                guint64            *out_download_size,
                                guint64            *out_installed_size,
                                char              **out_metadata,
                                GError            **error)
{
  if (self->summary || self->index)
    {
      const char *metadata = NULL;
      if (!flatpak_remote_state_lookup_cache (self, ref, out_download_size, out_installed_size, &metadata, error))
        return FALSE;

      if (out_metadata)
        *out_metadata = g_strdup (metadata);
    }
  else
    {
      /* Look up from sideload */
      g_autofree char *checksum = NULL;
      guint64 timestamp;
      VarRefInfoRef info;
      FlatpakSideloadState *ss = NULL;
      g_autoptr(GVariant) commit_data = NULL;
      g_autoptr(GVariant) commit_metadata = NULL;
      const char *xa_metadata = NULL;
      guint64 download_size = 0;
      guint64 installed_size = 0;

      /* Use sideload refs if any */

      if (!flatpak_remote_state_resolve_sideloaded_ref (self, ref, &checksum, &timestamp,
                                                        &info, &ss, error))
        return FALSE;

      if (!ostree_repo_load_commit (ss->repo, checksum, &commit_data, NULL, error))
        return FALSE;

      commit_metadata = g_variant_get_child_value (commit_data, 0);
      g_variant_lookup (commit_metadata, "xa.metadata", "&s", &xa_metadata);
      if (xa_metadata == NULL)
        return flatpak_fail (error, "No xa.metadata in sideload commit %s ref %s", checksum, ref);

      if (g_variant_lookup (commit_metadata, "xa.download-size", "t", &download_size))
        download_size = GUINT64_FROM_BE (download_size);
      if (g_variant_lookup (commit_metadata, "xa.installed-size", "t", &installed_size))
        installed_size = GUINT64_FROM_BE (installed_size);

      if (out_installed_size)
        *out_installed_size = installed_size;

      if (out_download_size)
        *out_download_size = download_size;

      if (out_metadata)
        *out_metadata = g_strdup (xa_metadata);
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

static FlatpakOciRegistry *
flatpak_remote_state_new_oci_registry (FlatpakRemoteState *self,
                                       const char   *token,
                                       GCancellable *cancellable,
                                       GError      **error)
{
  g_autofree char *registry_uri = NULL;
  g_autoptr(FlatpakOciRegistry) registry = NULL;

  if (!flatpak_remote_state_ensure_summary (self, error))
    return NULL;

  registry_uri = lookup_oci_registry_uri_from_summary (self->summary, error);
  if (registry_uri == NULL)
    return NULL;

  registry = flatpak_oci_registry_new (registry_uri, FALSE, -1, NULL, error);
  if (registry == NULL)
    return NULL;

  flatpak_oci_registry_set_token (registry, token);

  return g_steal_pointer (&registry);
}

static GVariant *
flatpak_remote_state_fetch_commit_object_oci (FlatpakRemoteState *self,
                                              FlatpakDir   *dir,
                                              const char   *ref,
                                              const char   *checksum,
                                              const char   *token,
                                              GCancellable *cancellable,
                                              GError      **error)
{
  g_autoptr(FlatpakOciRegistry) registry = NULL;
  g_autoptr(FlatpakOciVersioned) versioned = NULL;
  g_autoptr(FlatpakOciImage) image_config = NULL;
  g_autofree char *oci_digest = NULL;
  g_autofree char *latest_rev = NULL;
  VarRefInfoRef latest_rev_info;
  VarMetadataRef metadata;
  const char *oci_repository = NULL;
  GHashTable *labels;
  g_autofree char *subject = NULL;
  g_autofree char *body = NULL;
  g_autofree char *manifest_ref = NULL;
  g_autofree char *parent = NULL;
  guint64 timestamp = 0;
  g_autoptr(GVariantBuilder) metadata_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
  g_autoptr(GVariant) metadata_v = NULL;

  registry = flatpak_remote_state_new_oci_registry (self, token, cancellable, error);
  if (registry == NULL)
    return NULL;

  /* We extract the rev info from the latest, even if we don't use the latest digest, assuming refs don't move */
  if (!flatpak_remote_state_lookup_ref (self, ref, &latest_rev, NULL, &latest_rev_info, NULL, error))
    return NULL;

  if (latest_rev == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_REF_NOT_FOUND,
                          _("Couldn't find ref %s in remote %s"),
                          ref, self->remote_name);
      return NULL;
    }

  metadata = var_ref_info_get_metadata (latest_rev_info);
  oci_repository = var_metadata_lookup_string (metadata, "xa.oci-repository", NULL);

  oci_digest = g_strconcat ("sha256:", checksum, NULL);

  versioned = flatpak_oci_registry_load_versioned (registry, oci_repository, oci_digest,
                                                   NULL, NULL, cancellable, error);
  if (versioned == NULL)
    return NULL;

  if (!FLATPAK_IS_OCI_MANIFEST (versioned))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Image is not a manifest"));
      return NULL;
    }

  image_config = flatpak_oci_registry_load_image_config (registry, oci_repository,
                                                         FLATPAK_OCI_MANIFEST (versioned)->config.digest,
                                                         (const char **)FLATPAK_OCI_MANIFEST (versioned)->config.urls,
                                                         NULL, cancellable, error);
  if (image_config == NULL)
    return NULL;

  labels = flatpak_oci_image_get_labels (image_config);
  if (labels)
    flatpak_oci_parse_commit_labels (labels, &timestamp,
                                     &subject, &body,
                                     &manifest_ref, NULL, &parent,
                                     metadata_builder);


  if (g_strcmp0 (manifest_ref, ref) != 0)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Commit has no requested ref ‘%s’ in ref binding metadata"),  ref);
      return NULL;
    }

  metadata_v = g_variant_ref_sink (g_variant_builder_end (metadata_builder));

  /* This isn't going to be exactly the same as the reconstructed one from the pull, because we don't have the contents, but its useful to get metadata */
  return
    g_variant_ref_sink (g_variant_new ("(@a{sv}@ay@a(say)sst@ay@ay)",
                                       metadata_v,
                                       parent ? ostree_checksum_to_bytes_v (parent) :  g_variant_new_from_data (G_VARIANT_TYPE ("ay"), NULL, 0, FALSE, NULL, NULL),
                                       g_variant_new_array (G_VARIANT_TYPE ("(say)"), NULL, 0),
                                       subject, body,
                                       GUINT64_TO_BE (timestamp),
                                       ostree_checksum_to_bytes_v ("0000000000000000000000000000000000000000000000000000000000000000"),
                                       ostree_checksum_to_bytes_v ("0000000000000000000000000000000000000000000000000000000000000000")));
}

static GVariant *
flatpak_remote_state_fetch_commit_object (FlatpakRemoteState *self,
                                          FlatpakDir   *dir,
                                          const char   *ref,
                                          const char   *checksum,
                                          const char   *token,
                                          GCancellable *cancellable,
                                          GError      **error)
{
  g_autofree char *base_url = NULL;
  g_autofree char *object_url = NULL;
  g_autofree char *part1 = NULL;
  g_autofree char *part2 = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GVariant) commit_data = NULL;
  g_autoptr(GVariant) commit_metadata = NULL;

  if (!ostree_repo_remote_get_url (dir->repo, self->remote_name, &base_url, error))
    return NULL;

  ensure_http_session (dir);

  part1 = g_strndup (checksum, 2);
  part2 = g_strdup_printf ("%s.commit", checksum + 2);

  object_url = g_build_filename (base_url, "objects", part1, part2, NULL);

  bytes = flatpak_load_uri (dir->http_session, object_url, 0, token,
                            NULL, NULL, NULL,
                            cancellable, error);
  if (bytes == NULL)
    return NULL;

  commit_data = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_COMMIT_GVARIANT_FORMAT,
                                                              bytes, FALSE));

  /* We downloaded this without validating the signature, so we do some basic verification
     of it. However, the signature will be checked when the download is done, and the final
     metadata is compared to what we got here, so its pretty ok to use it for resolving
     the transaction op. However, we do some basic checks. */
  if (!ostree_validate_structureof_commit (commit_data, error))
    return NULL;

  commit_metadata = g_variant_get_child_value (commit_data, 0);
  if (ref != NULL)
    {
      const char *xa_ref = NULL;
      const char *collection_binding = NULL;
      g_autofree const char **commit_refs = NULL;

      if ((g_variant_lookup (commit_metadata, "xa.ref", "&s", &xa_ref) &&
           g_strcmp0 (xa_ref, ref) != 0) ||
          (g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_REF_BINDING, "^a&s", &commit_refs) &&
           !g_strv_contains ((const char * const *) commit_refs, ref)))
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Commit has no requested ref ‘%s’ in ref binding metadata"),  ref);
          return NULL;
        }

      /* Check that the locally configured collection ID is correct by looking
       * for it in the commit metadata */
      if (self->collection_id != NULL &&
          (!g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_COLLECTION_BINDING, "&s", &collection_binding) ||
           g_strcmp0 (self->collection_id, collection_binding) != 0))
        {
          g_autoptr(GVariantIter) collection_refs_iter = NULL;
          gboolean found_in_collection_refs_binding = FALSE;
          /* Note: the OSTREE_COMMIT_META_... define for this is not yet merged
           * in https://github.com/ostreedev/ostree/pull/1805 */
          if (g_variant_lookup (commit_metadata, "ostree.collection-refs-binding", "a(ss)", &collection_refs_iter))
            {
              const gchar *crb_collection_id, *crb_ref_name;
              while (g_variant_iter_loop (collection_refs_iter, "(&s&s)", &crb_collection_id, &crb_ref_name))
                {
                  if (g_strcmp0 (self->collection_id, crb_collection_id) == 0 &&
                      g_strcmp0 (ref, crb_ref_name) == 0)
                    {
                      found_in_collection_refs_binding = TRUE;
                      break;
                    }
                }
            }

          if (!found_in_collection_refs_binding)
            {
              flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA,
                                  _("Configured collection ID ‘%s’ not in binding metadata"),
                                  self->collection_id);
              return NULL;
            }
        }
    }

  return g_steal_pointer (&commit_data);
}


/* Tries to load the specified commit object that we resolved from
   this remote.  This either comes from the already available local
   repo, or from one of the sideloading repos, and if not available we
   download it from the actual remote. */
GVariant *
flatpak_remote_state_load_ref_commit (FlatpakRemoteState *self,
                                      FlatpakDir         *dir,
                                      const char         *ref,
                                      const char         *opt_commit,
                                      const char         *token,
                                      char              **out_commit,
                                      GCancellable       *cancellable,
                                      GError            **error)
{
  g_autoptr(GVariant) commit_data = NULL;
  g_autofree char *commit = NULL;

  if (opt_commit == NULL)
    {
      if (!flatpak_remote_state_lookup_ref (self, ref, &commit, NULL, NULL, NULL, error))
        return NULL;

      if (commit == NULL)
        {
          flatpak_fail_error (error, FLATPAK_ERROR_REF_NOT_FOUND,
                              _("Couldn't find latest checksum for ref %s in remote %s"),
                              ref, self->remote_name);
          return NULL;
        }
    }
  else
    commit = g_strdup (opt_commit);

  /* First try local availability */
  if (ostree_repo_load_commit (dir->repo, commit, &commit_data, NULL, NULL))
    goto out;

  for (int i = 0; i < self->sideload_repos->len; i++)
    {
      FlatpakSideloadState *ss = g_ptr_array_index (self->sideload_repos, i);

      if (ostree_repo_load_commit (ss->repo, commit, &commit_data, NULL, NULL))
        goto out;
    }

  if (flatpak_dir_get_remote_oci (dir, self->remote_name))
    commit_data = flatpak_remote_state_fetch_commit_object_oci (self, dir, ref, commit, token,
                                                                cancellable, error);
  else
    commit_data = flatpak_remote_state_fetch_commit_object (self, dir, ref, commit, token,
                                                            cancellable, error);

out:
  if (out_commit)
    *out_commit = g_steal_pointer (&commit);

  return g_steal_pointer (&commit_data);
}


gboolean
flatpak_remote_state_lookup_sparse_cache (FlatpakRemoteState *self,
                                          const char         *ref,
                                          VarMetadataRef     *out_metadata,
                                          GError            **error)
{
  VarSummaryRef summary;
  VarMetadataRef meta;
  VarVariantRef sparse_cache_v;
  guint32 summary_version;
  GVariant *summary_v;

  if (!flatpak_remote_state_ensure_summary (self, error))
    return FALSE;

  summary_v = get_summary_for_ref (self, ref);
  if (summary_v == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_REF_NOT_FOUND,
                               _("No entry for %s in remote %s summary flatpak sparse cache"),
                               ref, self->remote_name);

  summary = var_summary_from_gvariant (summary_v);
  meta = var_summary_get_metadata (summary);

  summary_version = GUINT32_FROM_LE (var_metadata_lookup_uint32 (meta, "xa.summary-version", 0));

  if (summary_version == 0)
    {
      if (var_metadata_lookup (meta, "xa.sparse-cache", NULL, &sparse_cache_v))
        {
          VarSparseCacheRef sparse_cache = var_sparse_cache_from_variant (sparse_cache_v);
          if (var_sparse_cache_lookup (sparse_cache, ref, NULL, out_metadata))
            return TRUE;
        }
    }
  else if (summary_version == 1)
    {
      VarRefMapRef ref_map = var_summary_get_ref_map (summary);
      VarRefInfoRef info;

      if (flatpak_var_ref_map_lookup_ref (ref_map, ref, &info))
        {
          *out_metadata = var_ref_info_get_metadata (info);
          return TRUE;
        }
    }
  else
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Unsupported summary version %d for remote %s"),
                          summary_version, self->remote_name);
      return FALSE;
    }

  return flatpak_fail_error (error, FLATPAK_ERROR_REF_NOT_FOUND,
                             _("No entry for %s in remote %s summary flatpak sparse cache"),
                             ref, self->remote_name);
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

G_DEFINE_AUTOPTR_CLEANUP_FUNC (DirExtraData, dir_extra_data_free);

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

  g_clear_pointer (&self->ref, flatpak_decomposed_unref);
  g_clear_object (&self->dir);
  g_clear_pointer (&self->metadata, g_key_file_unref);
  g_clear_pointer (&self->system_overrides, flatpak_context_free);
  g_clear_pointer (&self->user_overrides, flatpak_context_free);
  g_clear_pointer (&self->system_app_overrides, flatpak_context_free);
  g_clear_pointer (&self->user_app_overrides, flatpak_context_free);
  g_clear_object (&self->repo);

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

GBytes *
flatpak_load_deploy_data (GFile             *deploy_dir,
                          FlatpakDecomposed *ref,
                          OstreeRepo        *repo,
                          int                required_version,
                          GCancellable      *cancellable,
                          GError           **error)
{
  g_autoptr(GFile) data_file = NULL;
  g_autoptr(GBytes) deploy_data = NULL;
  gchar *contents;
  gsize len;

  data_file = g_file_get_child (deploy_dir, "deploy");

  if (!g_file_load_contents (data_file, cancellable, &contents, &len, NULL, error))
    return NULL;

  deploy_data = g_bytes_new_take (contents, len);

  if (flatpak_deploy_data_get_version (deploy_data) < required_version)
    return upgrade_deploy_data (deploy_data, deploy_dir, ref, repo, cancellable, error);

  return g_steal_pointer (&deploy_data);
}


GBytes *
flatpak_deploy_get_deploy_data (FlatpakDeploy *deploy,
                                int            required_version,
                                GCancellable  *cancellable,
                                GError       **error)
{
  return flatpak_load_deploy_data (deploy->dir,
                                   deploy->ref,
                                   deploy->repo,
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
flatpak_deploy_new (GFile             *dir,
                    FlatpakDecomposed *ref,
                    GKeyFile          *metadata,
                    OstreeRepo        *repo)
{
  FlatpakDeploy *deploy;

  deploy = g_object_new (FLATPAK_TYPE_DEPLOY, NULL);
  deploy->ref = flatpak_decomposed_ref (ref);
  deploy->dir = g_object_ref (dir);
  deploy->metadata = g_key_file_ref (metadata);
  deploy->repo = g_object_ref (repo);

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
      strcmp (id, SYSTEM_DIR_DEFAULT_ID) == 0 ||
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
      g_info ("Could not get list of system installations from '%s': %s", file_path, my_error->message);
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
          g_info ("While reading '%s': Unable to get path for installation '%s': %s", file_path, id, my_error->message);
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
      g_info ("No installations directory in %s. Skipping", config_dir);
      goto out;
    }

  conf_dir = g_file_new_for_path (config_dir);
  dir_enum = g_file_enumerate_children (conf_dir,
                                        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                        G_FILE_QUERY_INFO_NONE,
                                        cancellable, &my_error);
  if (my_error != NULL)
    {
      g_info ("Unexpected error retrieving extra installations in %s: %s",
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
          g_info ("Unexpected error reading file in %s: %s",
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

  /* Only fill the details of the default directory if not overridden. */
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

  if (xa_metadata == NULL ||
      xa_metadata_size != required_metadata_size ||
      memcmp (xa_metadata, required_metadata, xa_metadata_size) != 0)
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
      /* and not writeable by others, but readable */
      (st_buf.st_mode & 0777) == 0755)
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

static gchar *
flatpak_dir_revokefs_fuse_create_mountpoint (FlatpakDecomposed *ref,
                                             GError           **error)
{
  g_autoptr(GFile) cache_dir = NULL;
  g_autofree gchar *cache_dir_path = NULL;
  g_autofree gchar *mnt_dir = NULL;
  g_autofree gchar *id = NULL;
  g_autofree gchar *mountpoint = NULL;

  cache_dir = flatpak_ensure_system_user_cache_dir_location (error);
  if (cache_dir == NULL)
    return NULL;

  id = flatpak_decomposed_dup_id (ref);
  cache_dir_path = g_file_get_path (cache_dir);
  mnt_dir = g_strdup_printf ("%s-XXXXXX", id);
  mountpoint = g_mkdtemp_full (g_build_filename (cache_dir_path, mnt_dir, NULL), 0755);
  if (mountpoint == NULL)
    {
      glnx_set_error_from_errno (error);
      return NULL;
    }

  return g_steal_pointer (&mountpoint);
}

static gboolean
flatpak_dir_revokefs_fuse_unmount (OstreeRepo **repo,
                                  GLnxLockFile *lockfile,
                                  const gchar *mnt_dir,
                                  GError **error)
{
  g_autoptr(GSubprocess) fusermount = NULL;

  /* Clear references to child_repo as not to leave any open fds. This is needed for
   * a clean umount operation.
   */
  g_clear_pointer (repo, g_object_unref);
  glnx_release_lock_file (lockfile);

  fusermount = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE,
                                 error,
                                 "fusermount", "-u", "-z", mnt_dir,
                                 NULL);
  if (g_subprocess_wait_check (fusermount, NULL, error))
    {
      g_autoptr(GFile) mnt_dir_file = g_file_new_for_path (mnt_dir);
      g_autoptr(GError) tmp_error = NULL;

      if (!flatpak_rm_rf (mnt_dir_file, NULL, &tmp_error))
        g_warning ("Unable to remove mountpoint directory %s: %s", mnt_dir, tmp_error->message);

      return TRUE;
    }

  return FALSE;
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
flatpak_dir_system_helper_call (FlatpakDir         *self,
                                const gchar        *method_name,
                                GVariant           *parameters,
                                const GVariantType *reply_type,
                                GUnixFDList       **out_fd_list,
                                GCancellable       *cancellable,
                                GError            **error)
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

  g_info ("Calling system helper: %s", method_name);
  res = g_dbus_connection_call_with_unix_fd_list_sync (self->system_helper_bus,
                                                       FLATPAK_SYSTEM_HELPER_BUS_NAME,
                                                       FLATPAK_SYSTEM_HELPER_PATH,
                                                       FLATPAK_SYSTEM_HELPER_INTERFACE,
                                                       method_name,
                                                       parameters,
                                                       reply_type,
                                                       G_DBUS_CALL_FLAGS_NONE, G_MAXINT,
                                                       NULL, out_fd_list,
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
                                       const gchar *const *arg_previous_ids,
                                       const gchar        *arg_installation,
                                       GCancellable       *cancellable,
                                       GError            **error)
{
  const char *empty[] = { NULL };

  if (arg_subpaths == NULL)
    arg_subpaths = empty;
  if (arg_previous_ids == NULL)
    arg_previous_ids = empty;

  if (flatpak_dir_get_no_interaction (self))
    arg_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_NO_INTERACTION;

  g_autoptr(GVariant) ret =
    flatpak_dir_system_helper_call (self, "Deploy",
                                    g_variant_new ("(^ayuss^as^ass)",
                                                   arg_repo_path,
                                                   arg_flags,
                                                   arg_ref,
                                                   arg_origin,
                                                   arg_subpaths,
                                                   arg_previous_ids,
                                                   arg_installation),
                                    G_VARIANT_TYPE ("()"), NULL,
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
                                    G_VARIANT_TYPE ("()"), NULL,
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
                                    G_VARIANT_TYPE ("()"), NULL,
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
                                    G_VARIANT_TYPE ("(s)"), NULL,
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
                                    G_VARIANT_TYPE ("()"), NULL,
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
                                    G_VARIANT_TYPE ("()"), NULL,
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
                                    G_VARIANT_TYPE ("()"), NULL,
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
                                    G_VARIANT_TYPE ("()"), NULL,
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
                                    G_VARIANT_TYPE ("()"), NULL,
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
                                    G_VARIANT_TYPE ("()"), NULL,
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
                                    G_VARIANT_TYPE ("()"), NULL,
                                    cancellable, error);
  return ret != NULL;
}

static gboolean
flatpak_dir_system_helper_call_cancel_pull (FlatpakDir    *self,
                                            guint          arg_flags,
                                            const gchar   *arg_installation,
                                            const gchar   *arg_src_dir,
                                            GCancellable  *cancellable,
                                            GError       **error)
{
  if (flatpak_dir_get_no_interaction (self))
    arg_flags |= FLATPAK_HELPER_CANCEL_PULL_FLAGS_NO_INTERACTION;

  g_info ("Calling system helper: CancelPull");

  g_autoptr(GVariant) ret =
    flatpak_dir_system_helper_call (self, "CancelPull",
                                    g_variant_new ("(uss)",
                                                   arg_flags,
                                                   arg_installation,
                                                   arg_src_dir),
                                    NULL, NULL,
                                    cancellable, error);

   return ret != NULL;
}

static gboolean
flatpak_dir_system_helper_call_get_revokefs_fd (FlatpakDir   *self,
                                                guint         arg_flags,
                                                const gchar  *arg_installation,
                                                gint         *out_socket,
                                                gchar       **out_src_dir,
                                                GCancellable *cancellable,
                                                GError      **error)
{
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  gint fd = -1;
  gint fd_index = -1;

  if (flatpak_dir_get_no_interaction (self))
    arg_flags |= FLATPAK_HELPER_GET_REVOKEFS_FD_FLAGS_NO_INTERACTION;

  g_info ("Calling system helper: GetRevokefsFd");

  g_autoptr(GVariant) ret =
    flatpak_dir_system_helper_call (self, "GetRevokefsFd",
                                    g_variant_new ("(us)",
                                                   arg_flags,
                                                   arg_installation),
                                    G_VARIANT_TYPE ("(hs)"),
                                    &out_fd_list,
                                    cancellable, error);

  if (ret == NULL)
    return FALSE;

  g_variant_get (ret, "(hs)", &fd_index, out_src_dir);
  fd  = g_unix_fd_list_get (out_fd_list, fd_index, error);
  if (fd == -1)
    return FALSE;

  *out_socket = fd;

  return TRUE;
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
                                    G_VARIANT_TYPE ("()"), NULL,
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
                                    G_VARIANT_TYPE ("()"), NULL,
                                    cancellable, error);
  return ret != NULL;
}

static void
flatpak_dir_finalize (GObject *object)
{
  FlatpakDir *self = FLATPAK_DIR (object);

  g_clear_object (&self->repo);
  g_clear_object (&self->cache_dir);
  g_clear_object (&self->basedir);
  g_clear_pointer (&self->extra_data, dir_extra_data_free);

  if (self->system_helper_bus != (gpointer) 1)
    g_clear_object (&self->system_helper_bus);

  g_clear_pointer (&self->http_session, flatpak_http_session_free);
  g_clear_pointer (&self->summary_cache, g_hash_table_unref);
  g_clear_pointer (&self->remote_filters, g_hash_table_unref);
  g_clear_pointer (&self->masked, g_regex_unref);
  g_clear_pointer (&self->pinned, g_regex_unref);

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
  if (id != NULL && g_strcmp0 (id, SYSTEM_DIR_DEFAULT_ID) != 0)
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

  return (const char *) name;
}

char *
flatpak_dir_get_display_name (FlatpakDir *self)
{
  if (self->user)
    return g_strdup (_("User installation"));

  if (self->extra_data != NULL && g_strcmp0 (self->extra_data->id, SYSTEM_DIR_DEFAULT_ID) != 0)
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
flatpak_dir_load_deployed (FlatpakDir        *self,
                           FlatpakDecomposed *ref,
                           const char        *checksum,
                           GCancellable      *cancellable,
                           GError           **error)
{
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_autofree char *metadata_contents = NULL;
  FlatpakDeploy *deploy;
  gsize metadata_size;

  deploy_dir = flatpak_dir_get_if_deployed (self, ref, checksum, cancellable);
  if (deploy_dir == NULL)
    {
      if (checksum == NULL)
        g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                     _("%s not installed"), flatpak_decomposed_get_ref (ref));
      else
        g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                     _("%s (commit %s) not installed"), flatpak_decomposed_get_ref (ref), checksum);
      return NULL;
    }

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return NULL;

  metadata = g_file_get_child (deploy_dir, "metadata");
  if (!g_file_load_contents (metadata, cancellable, &metadata_contents, &metadata_size, NULL, error))
    return NULL;

  metakey = g_key_file_new ();
  if (!g_key_file_load_from_data (metakey, metadata_contents, metadata_size, 0, error))
    return NULL;

  deploy = flatpak_deploy_new (deploy_dir, ref, metakey, self->repo);

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
  if (flatpak_decomposed_is_app (ref))
    {
      g_autofree char *id = flatpak_decomposed_dup_id (ref);

      /* Only load system overrides for system installed apps */
      if (!self->user)
        {
          deploy->system_app_overrides = flatpak_load_override_file (id, FALSE, error);
          if (deploy->system_app_overrides == NULL)
            return NULL;
        }

      /* Always load user overrides */
      deploy->user_app_overrides = flatpak_load_override_file (id, TRUE, error);
      if (deploy->user_app_overrides == NULL)
        return NULL;
    }

  return deploy;
}

GFile *
flatpak_dir_get_deploy_dir (FlatpakDir *self,
                            FlatpakDecomposed *ref)
{
  return g_file_resolve_relative_path (self->basedir, flatpak_decomposed_get_ref (ref));
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

GFile *
flatpak_dir_get_sideload_repos_dir (FlatpakDir *self)
{
  return g_file_get_child (self->basedir, SIDELOAD_REPOS_DIR_NAME);
}

GFile *
flatpak_dir_get_runtime_sideload_repos_dir (FlatpakDir *self)
{
  g_autoptr(GFile) base = g_file_new_for_path (get_run_dir_location ());
  return g_file_get_child (base, SIDELOAD_REPOS_DIR_NAME);
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
flatpak_deploy_data_get_origin (GBytes *deploy_data)
{
  VarDeployDataRef ref = var_deploy_data_from_bytes (deploy_data);
  return var_deploy_data_get_origin (ref);
}

const char *
flatpak_deploy_data_get_commit (GBytes *deploy_data)
{
  VarDeployDataRef ref = var_deploy_data_from_bytes (deploy_data);
  return var_deploy_data_get_commit (ref);
}

gint32
flatpak_deploy_data_get_version (GBytes *deploy_data)
{
  VarDeployDataRef ref = var_deploy_data_from_bytes (deploy_data);
  VarMetadataRef metadata = var_deploy_data_get_metadata (ref);

  return var_metadata_lookup_int32 (metadata, "deploy-version", 0);
}

/* Note: This will return 0 if this is unset, which happens on deloy data updates, so ensure we handle that in all callers */
guint64
flatpak_deploy_data_get_timestamp (GBytes *deploy_data)
{
  VarDeployDataRef ref = var_deploy_data_from_bytes (deploy_data);
  VarMetadataRef metadata = var_deploy_data_get_metadata (ref);

  return var_metadata_lookup_uint64 (metadata, "timestamp", 0);
}

static const char *
flatpak_deploy_data_get_string (GBytes *deploy_data, const char *key)
{
  VarDeployDataRef ref = var_deploy_data_from_bytes (deploy_data);
  VarMetadataRef metadata = var_deploy_data_get_metadata (ref);

  return var_metadata_lookup_string (metadata, key, NULL);
}

static const char *
flatpak_deploy_data_get_localed_string (GBytes *deploy_data, const char *key)
{
  VarDeployDataRef ref = var_deploy_data_from_bytes (deploy_data);
  VarMetadataRef metadata = var_deploy_data_get_metadata (ref);
  const char * const * languages = g_get_language_names ();
  int i;

  for (i = 0; languages[i]; ++i)
    {
      g_autofree char *localed_key = NULL;
      VarVariantRef value_v;

      if (strcmp (languages[i], "C") == 0)
        localed_key = g_strdup (key);
      else
        localed_key = g_strdup_printf ("%s@%s", key, languages[i]);

      if (var_metadata_lookup (metadata, localed_key, NULL,  &value_v) &&
          var_variant_is_type (value_v, G_VARIANT_TYPE_STRING))
        return var_variant_get_string (value_v);
    }

  return NULL;
}

const char *
flatpak_deploy_data_get_alt_id (GBytes *deploy_data)
{
  return flatpak_deploy_data_get_string (deploy_data, "alt-id");
}

const char *
flatpak_deploy_data_get_eol (GBytes *deploy_data)
{
  return flatpak_deploy_data_get_string (deploy_data, "eol");
}

const char *
flatpak_deploy_data_get_eol_rebase (GBytes *deploy_data)
{
  return flatpak_deploy_data_get_string (deploy_data, "eolr");
}

/*<private>
 * flatpak_deploy_data_get_previous_ids:
 *
 * Returns: (array length=length zero-terminated=1) (transfer container): an array of constant strings
 **/
const char **
flatpak_deploy_data_get_previous_ids (GBytes *deploy_data, gsize *length)
{
  VarDeployDataRef ref = var_deploy_data_from_bytes (deploy_data);
  VarMetadataRef metadata = var_deploy_data_get_metadata (ref);
  VarVariantRef previous_ids_v;

  if (var_metadata_lookup (metadata, "previous-ids", NULL,  &previous_ids_v))
    return var_arrayofstring_to_strv (var_arrayofstring_from_variant (previous_ids_v), length);

  if (length != NULL)
    *length = 0;

  return NULL;
}

const char *
flatpak_deploy_data_get_runtime (GBytes *deploy_data)
{
  return flatpak_deploy_data_get_string (deploy_data, "runtime");
}

const char *
flatpak_deploy_data_get_extension_of (GBytes *deploy_data)
{
  return flatpak_deploy_data_get_string (deploy_data, "extension-of");
}

const char *
flatpak_deploy_data_get_appdata_name (GBytes *deploy_data)
{
  return flatpak_deploy_data_get_localed_string (deploy_data, "appdata-name");
}

const char *
flatpak_deploy_data_get_appdata_summary (GBytes *deploy_data)
{
  return flatpak_deploy_data_get_localed_string (deploy_data, "appdata-summary");
}

const char *
flatpak_deploy_data_get_appdata_version (GBytes *deploy_data)
{
  return flatpak_deploy_data_get_string (deploy_data, "appdata-version");
}

const char *
flatpak_deploy_data_get_appdata_license (GBytes *deploy_data)
{
  return flatpak_deploy_data_get_string (deploy_data, "appdata-license");
}

const char *
flatpak_deploy_data_get_appdata_content_rating_type (GBytes *deploy_data)
{
  VarDeployDataRef ref = var_deploy_data_from_bytes (deploy_data);
  VarMetadataRef metadata = var_deploy_data_get_metadata (ref);
  VarVariantRef rating_v;

  if (var_metadata_lookup (metadata, "appdata-content-rating", NULL,  &rating_v))
    {
      VarContentRatingRef rating = var_content_rating_from_variant (rating_v);
      return var_content_rating_get_rating_type (rating);
    }

  return NULL;
}

GHashTable *  /* (transfer container) (nullable) */
flatpak_deploy_data_get_appdata_content_rating (GBytes *deploy_data)
{
  VarDeployDataRef ref = var_deploy_data_from_bytes (deploy_data);
  VarMetadataRef metadata = var_deploy_data_get_metadata (ref);
  VarVariantRef rating_v;
  g_autoptr(GHashTable) content_rating = NULL;

  if (var_metadata_lookup (metadata, "appdata-content-rating", NULL,  &rating_v))
    {
      VarContentRatingRef rating = var_content_rating_from_variant (rating_v);
      VarRatingsRef ratings = var_content_rating_get_ratings (rating);
      gsize len, i;

      content_rating = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);

      len = var_ratings_get_length (ratings);
      for (i = 0; i < len; i++)
        {
          VarRatingsEntryRef entry = var_ratings_get_at (ratings, i);

          g_hash_table_insert (content_rating,
                               (gpointer) g_intern_string (var_ratings_entry_get_key (entry)),
                               (gpointer) g_intern_string (var_ratings_entry_get_value (entry)));
        }
    }

  return g_steal_pointer (&content_rating);
}

/*<private>
 * flatpak_deploy_data_get_subpaths:
 *
 * Returns: (array zero-terminated=1) (transfer container): an array of constant strings
 **/
const char **
flatpak_deploy_data_get_subpaths (GBytes *deploy_data)
{
  VarDeployDataRef ref = var_deploy_data_from_bytes (deploy_data);
  return var_arrayofstring_to_strv (var_deploy_data_get_subpaths (ref), NULL);
}

gboolean
flatpak_deploy_data_has_subpaths (GBytes *deploy_data)
{
  VarDeployDataRef ref = var_deploy_data_from_bytes (deploy_data);
  VarArrayofstringRef subpaths = var_deploy_data_get_subpaths (ref);

  return var_arrayofstring_get_length (subpaths) != 0;
}

guint64
flatpak_deploy_data_get_installed_size (GBytes *deploy_data)
{
  VarDeployDataRef ref = var_deploy_data_from_bytes (deploy_data);
  return var_deploy_data_get_installed_size (ref);
}

static char *
read_appdata_xml_from_deploy_dir (GFile *deploy_dir, const char *id)
{
  g_autoptr(GFile) appdata_file = NULL;
  g_autofree char *appdata_name = NULL;
  g_autoptr(GFileInputStream) appdata_in = NULL;
  gsize size;

  appdata_file = flatpak_build_file (deploy_dir, "files/share/swcatalog/xml/flatpak.xml.gz",  NULL);
  if (!g_file_test (g_file_peek_path (appdata_file), G_FILE_TEST_EXISTS))
    {
      g_clear_object (&appdata_file);
      appdata_name = g_strconcat (id, ".xml.gz", NULL);
      appdata_file = flatpak_build_file (deploy_dir, "files/share/app-info/xmls", appdata_name, NULL);
    }

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
add_locale_metadata_string (GVariantDict *metadata_dict,
                            const char   *keyname,
                            GHashTable   *values)
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

    g_variant_dict_insert_value (metadata_dict, key,
                                 g_variant_new_string (value));
  }
}

/* Convert @content_rating_type and @content_rating to a floating #GVariant of
 * type `(sa{ss})`. */
static GVariant *
appdata_content_rating_to_variant (const char *content_rating_type,
                                   GHashTable *content_rating)
{
  g_autoptr(GVariantBuilder) builder = g_variant_builder_new (G_VARIANT_TYPE ("(sa{ss})"));
  GHashTableIter iter;
  gpointer key, value;

  g_variant_builder_add (builder, "s", content_rating_type);
  g_variant_builder_open (builder, G_VARIANT_TYPE ("a{ss}"));

  g_hash_table_iter_init (&iter, content_rating);

  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *id = key, *val = value;
      g_variant_builder_add (builder, "{ss}", id, val);
    }

  g_variant_builder_close (builder);

  return g_variant_builder_end (builder);
}

static void
add_appdata_to_deploy_data (GVariantDict *metadata_dict,
                            GFile        *deploy_dir,
                            const char   *id)
{
  g_autofree char *appdata_xml = NULL;
  g_autoptr(GHashTable) names = NULL;
  g_autoptr(GHashTable) comments = NULL;
  g_autofree char *version = NULL;
  g_autofree char *license = NULL;
  g_autofree char *content_rating_type = NULL;
  g_autoptr(GHashTable) content_rating = NULL;

  appdata_xml = read_appdata_xml_from_deploy_dir (deploy_dir, id);
  if (appdata_xml == NULL)
    return;

  if (flatpak_parse_appdata (appdata_xml, id, &names, &comments, &version, &license,
                             &content_rating_type, &content_rating))
    {
      add_locale_metadata_string (metadata_dict, "appdata-name", names);
      add_locale_metadata_string (metadata_dict, "appdata-summary", comments);
      if (version)
        g_variant_dict_insert_value (metadata_dict, "appdata-version",
                                     g_variant_new_string (version));
      if (license)
        g_variant_dict_insert_value (metadata_dict, "appdata-license",
                                     g_variant_new_string (license));
      if (content_rating_type != NULL && content_rating != NULL)
        g_variant_dict_insert_value (metadata_dict, "appdata-content-rating",
                                     appdata_content_rating_to_variant (content_rating_type, content_rating));
    }
}

static void
add_commit_metadata_to_deploy_data (GVariantDict *metadata_dict,
                                    GVariant     *commit_metadata)
{
  const char *alt_id = NULL;
  const char *eol = NULL;
  const char *eol_rebase = NULL;

  g_variant_lookup (commit_metadata, "xa.alt-id", "&s", &alt_id);
  g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE, "&s", &eol);
  g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE_REBASE, "&s", &eol_rebase);

  if (alt_id)
    g_variant_dict_insert_value (metadata_dict, "alt-id",
                                 g_variant_new_string (alt_id));
  if (eol)
    g_variant_dict_insert_value (metadata_dict, "eol",
                                 g_variant_new_string (eol));
  if (eol_rebase)
    g_variant_dict_insert_value (metadata_dict, "eolr",
                                 g_variant_new_string (eol_rebase));
}

static void
add_metadata_to_deploy_data (GVariantDict *metadata_dict,
                             GKeyFile     *keyfile)
{
  g_autofree char *application_runtime = NULL;
  g_autofree char *extension_of = NULL;

  application_runtime = g_key_file_get_string (keyfile,
                                               FLATPAK_METADATA_GROUP_APPLICATION,
                                               FLATPAK_METADATA_KEY_RUNTIME, NULL);
  extension_of = g_key_file_get_string (keyfile,
                                        FLATPAK_METADATA_GROUP_EXTENSION_OF,
                                        FLATPAK_METADATA_KEY_REF, NULL);

  if (application_runtime)
    g_variant_dict_insert_value (metadata_dict, "runtime",
                                 g_variant_new_string (application_runtime));
  if (extension_of)
    g_variant_dict_insert_value (metadata_dict, "extension-of",
                                 g_variant_new_string (extension_of));
}

static GBytes *
flatpak_dir_new_deploy_data (FlatpakDir         *self,
                             GFile              *deploy_dir,
                             GVariant           *commit_data,
                             GVariant           *commit_metadata,
                             GKeyFile           *metadata,
                             const char         *id,
                             const char         *origin,
                             const char         *commit,
                             char              **subpaths,
                             guint64             installed_size,
                             const char * const *previous_ids)
{
  char *empty_subpaths[] = {NULL};
  g_auto(GVariantDict) metadata_dict = FLATPAK_VARIANT_DICT_INITIALIZER;
  g_autoptr(GVariant) res = NULL;

  g_variant_dict_init (&metadata_dict, NULL);
  g_variant_dict_insert_value (&metadata_dict, "deploy-version",
                               g_variant_new_int32 (FLATPAK_DEPLOY_VERSION_CURRENT));
  g_variant_dict_insert_value (&metadata_dict, "timestamp",
                               g_variant_new_uint64 (ostree_commit_get_timestamp (commit_data)));

  if (previous_ids)
    g_variant_dict_insert_value (&metadata_dict, "previous-ids",
                                 g_variant_new_strv (previous_ids, -1));

  add_commit_metadata_to_deploy_data (&metadata_dict, commit_metadata);
  add_metadata_to_deploy_data (&metadata_dict, metadata);
  add_appdata_to_deploy_data (&metadata_dict, deploy_dir, id);

  res = g_variant_ref_sink (g_variant_new ("(ss^ast@a{sv})",
                                           origin,
                                           commit,
                                           subpaths ? subpaths : empty_subpaths,
                                           GUINT64_TO_BE (installed_size),
                                           g_variant_dict_end (&metadata_dict)));
  return g_variant_get_data_as_bytes (res);
}

static GBytes *
upgrade_deploy_data (GBytes             *deploy_data,
                     GFile              *deploy_dir,
                     FlatpakDecomposed  *ref,
                     OstreeRepo         *repo,
                     GCancellable       *cancellable,
                     GError            **error)
{
  VarDeployDataRef deploy_ref = var_deploy_data_from_bytes (deploy_data);
  g_autoptr(GVariant) metadata = g_variant_ref_sink (var_metadata_peek_as_gvariant (var_deploy_data_get_metadata (deploy_ref)));
  g_auto(GVariantDict) metadata_dict = FLATPAK_VARIANT_DICT_INITIALIZER;
  g_autofree const char **subpaths = NULL;
  g_autoptr(GVariant) res = NULL;
  int i, n, old_version;

  g_variant_dict_init (&metadata_dict, NULL);
  g_variant_dict_insert_value (&metadata_dict, "deploy-version",
                               g_variant_new_int32 (FLATPAK_DEPLOY_VERSION_CURRENT));

  /* Copy all metadata except version from old */
  n = g_variant_n_children (metadata);
  for (i = 0; i < n; i++)
    {
      const char *key;
      g_autoptr(GVariant) value = NULL;

      g_variant_get_child (metadata, i, "{&s@v}", &key, &value);
      if (strcmp (key, "deploy-version") == 0)
        continue;
      g_variant_dict_insert_value (&metadata_dict, key, value);
    }

  old_version = flatpak_deploy_data_get_version (deploy_data);
  if (old_version < 1)
    {
      g_autofree char *id = flatpak_decomposed_dup_id (ref);
      add_appdata_to_deploy_data (&metadata_dict, deploy_dir, id);
    }

  if (old_version < 3)
    {
      /* We don't know what timestamp to use here, use 0 and special case that for update checks */
      g_variant_dict_insert_value (&metadata_dict, "timestamp",
                                   g_variant_new_uint64 (0));
    }

  /* Deploy versions older than 4 might have some of the below fields, but it's
   * not guaranteed if the deploy was first created with an old Flatpak version
   */
  if (old_version < 4)
    {
      const char *commit;
      g_autoptr(GVariant) commit_data = NULL;
      g_autoptr(GVariant) commit_metadata = NULL;
      g_autoptr(GKeyFile) keyfile = NULL;
      g_autoptr(GFile) metadata_file = NULL;
      g_autofree char *metadata_contents = NULL;
      gsize metadata_size = 0;
      g_autofree char *id = flatpak_decomposed_dup_id (ref);

      /* Add fields from commit metadata to deploy */
      commit = flatpak_deploy_data_get_commit (deploy_data);
      if (!ostree_repo_load_commit (repo, commit, &commit_data, NULL, error))
        return NULL;
      commit_metadata = g_variant_get_child_value (commit_data, 0);
      add_commit_metadata_to_deploy_data (&metadata_dict, commit_metadata);

      /* Add fields from metadata file to deploy */
      keyfile = g_key_file_new ();
      metadata_file = g_file_resolve_relative_path (deploy_dir, "metadata");
      if (!g_file_load_contents (metadata_file, cancellable,
                                 &metadata_contents, &metadata_size, NULL, error))
        return NULL;
      if (!g_key_file_load_from_data (keyfile, metadata_contents, metadata_size, 0, error))
        return NULL;
      add_metadata_to_deploy_data (&metadata_dict, keyfile);

      /* Add fields from appdata to deploy, since appdata-content-rating wasn't
       * added when upgrading from version 2 as it should have been
       */
      if (old_version >= 1)
        add_appdata_to_deploy_data (&metadata_dict, deploy_dir, id);
    }

  subpaths = flatpak_deploy_data_get_subpaths (deploy_data);
  res = g_variant_ref_sink (g_variant_new ("(ss^ast@a{sv})",
                                           flatpak_deploy_data_get_origin (deploy_data),
                                           flatpak_deploy_data_get_commit (deploy_data),
                                           subpaths,
                                           GUINT64_TO_BE (flatpak_deploy_data_get_installed_size (deploy_data)),
                                           g_variant_dict_end (&metadata_dict)));
  return g_variant_get_data_as_bytes (res);
}

GBytes *
flatpak_dir_get_deploy_data (FlatpakDir        *self,
                             FlatpakDecomposed *ref,
                             int                required_version,
                             GCancellable      *cancellable,
                             GError           **error)
{
  g_autoptr(GFile) deploy_dir = NULL;

  deploy_dir = flatpak_dir_get_if_deployed (self, ref, NULL, cancellable);
  if (deploy_dir == NULL)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   _("%s not installed"), flatpak_decomposed_get_ref (ref));
      return NULL;
    }

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return NULL;

  return flatpak_load_deploy_data (deploy_dir,
                                   ref,
                                   self->repo,
                                   required_version,
                                   cancellable,
                                   error);
}

char *
flatpak_dir_get_origin (FlatpakDir        *self,
                        FlatpakDecomposed *ref,
                        GCancellable      *cancellable,
                        GError           **error)
{
  g_autoptr(GBytes) deploy_data = NULL;

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

gboolean
flatpak_dir_migrate_config (FlatpakDir   *self,
                            gboolean     *changed,
                            GCancellable *cancellable,
                            GError      **error)
{
  g_auto(GStrv) remotes = NULL;
  g_autoptr(GKeyFile) config = NULL;
  int i;

  if (changed != NULL)
    *changed = FALSE;

  /* Only do anything if it exists */
  if (!flatpak_dir_maybe_ensure_repo (self, NULL, NULL))
    return TRUE;

  remotes = flatpak_dir_list_remotes (self, cancellable, NULL);
  if (remotes == NULL)
    return TRUE;

  /* Enable gpg-verify-summary for all remotes with a collection id *and* gpg-verify set, because
   * we want to use summary verification, but older versions of collection-id didn't work with it */
  for (i = 0; remotes != NULL && remotes[i] != NULL; i++)
    {
      g_autofree char *remote_collection_id = NULL;
      const char *remote = remotes[i];
      gboolean gpg_verify_summary;
      gboolean gpg_verify;

      if (flatpak_dir_get_remote_disabled (self, remote))
        continue;

      remote_collection_id = flatpak_dir_get_remote_collection_id (self, remotes[i]);
      if (remote_collection_id == NULL)
        continue;

      if (!ostree_repo_remote_get_gpg_verify_summary (self->repo, remote, &gpg_verify_summary, NULL))
        continue;

      if (!ostree_repo_remote_get_gpg_verify (self->repo, remote, &gpg_verify, NULL))
        continue;

      if (gpg_verify && !gpg_verify_summary)
        {
          g_autofree char *group = g_strdup_printf ("remote \"%s\"", remote);
          if (config == NULL)
            config = ostree_repo_copy_config (flatpak_dir_get_repo (self));

          g_info ("Migrating remote '%s' to gpg-verify-summary", remote);
          g_key_file_set_boolean (config, group, "gpg-verify-summary", TRUE);
        }
    }

  if (config != NULL)
    {
      if (flatpak_dir_use_system_helper (self, NULL))
        {
          g_autoptr(GError) local_error = NULL;
          const char *installation = flatpak_dir_get_id (self);

          if (!flatpak_dir_system_helper_call_ensure_repo (self,
                                                           FLATPAK_HELPER_ENSURE_REPO_FLAGS_NONE,
                                                           installation ? installation : "",
                                                           NULL, &local_error))
            g_info ("Failed to migrate system config: %s", local_error->message);
        }
      else
        {
          if (!ostree_repo_write_config (self->repo, config, error))
            return FALSE;
        }

      if (changed != NULL)
        *changed = TRUE;
    }

  return TRUE;
}

/* Warning: This is not threadsafe, don't use in libflatpak */
gboolean
flatpak_dir_recreate_repo (FlatpakDir   *self,
                           GCancellable *cancellable,
                           GError      **error)
{
  gboolean res;
  OstreeRepo *old_repo = g_steal_pointer (&self->repo);

  /* This is also set by ensure repo, so clear it too */
  g_clear_object (&self->cache_dir);

  res = flatpak_dir_ensure_repo (self, cancellable, error);
  g_clear_object (&old_repo);

  G_LOCK (config_cache);

  g_clear_pointer (&self->masked, g_regex_unref);
  g_clear_pointer (&self->pinned, g_regex_unref);

  G_UNLOCK (config_cache);

  return res;
}

static void
copy_remote_config (GKeyFile *config,
                    GKeyFile *group_config,
                    const char *remote_name)
{
  g_auto(GStrv) keys = NULL;
  g_autofree char *group = g_strdup_printf ("remote \"%s\"", remote_name);
  int i;

  g_key_file_remove_group (config, group, NULL);

  keys = g_key_file_get_keys (group_config, group, NULL, NULL);
  if (keys == NULL)
    return;

  for (i = 0; keys[i] != NULL; i++)
    {
      g_autofree gchar *value = g_key_file_get_value (group_config, group, keys[i], NULL);
      if (value &&
          /* Canonicalize empty filter to unset */
          (strcmp (keys[i], "xa.filter") != 0 ||
           *value != 0))
        g_key_file_set_value (config, group, keys[i], value);
    }
}

static GHashTable *
_flatpak_dir_find_new_flatpakrepos (FlatpakDir *self, OstreeRepo *repo)
{
  g_autoptr(GHashTable) flatpakrepos = NULL;
  g_autoptr(GFile) conf_dir = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GError) my_error = NULL;
  g_autofree char *config_dir = NULL;
  g_auto(GStrv) remotes = NULL;
  g_auto(GStrv) applied_remotes = NULL;

  g_assert (repo != NULL);

  /* Predefined remotes only applies for the default system installation */
  if (self->user ||
      (self->extra_data != NULL &&
       strcmp (self->extra_data->id, SYSTEM_DIR_DEFAULT_ID) != 0))
    return NULL;

  config_dir = g_strdup_printf ("%s/%s",
                                get_config_dir_location (),
                                SYSCONF_REMOTES_DIR);
  conf_dir = g_file_new_for_path (config_dir);
  dir_enum = g_file_enumerate_children (conf_dir,
                                        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                        G_FILE_QUERY_INFO_NONE,
                                        NULL, &my_error);
  if (my_error != NULL)
    return NULL;

  remotes = ostree_repo_remote_list (repo, NULL);
  applied_remotes = g_key_file_get_string_list (ostree_repo_get_config (repo),
                                                "core", "xa.applied-remotes", NULL, NULL);

  while (TRUE)
    {
      GFileInfo *file_info;
      GFile *path;
      const char *name;
      guint32 type;

      if (!g_file_enumerator_iterate (dir_enum, &file_info, &path,
                                      NULL, &my_error))
        {
          g_info ("Unexpected error reading file in %s: %s",
                  config_dir, my_error->message);
          break;
        }

      if (file_info == NULL)
        break;

      name = g_file_info_get_name (file_info);
      type = g_file_info_get_file_type (file_info);

      if (type == G_FILE_TYPE_REGULAR && g_str_has_suffix (name, SYSCONF_REMOTES_FILE_EXT))
        {
          g_autofree char *remote_name = g_strndup (name, strlen (name) - strlen (SYSCONF_REMOTES_FILE_EXT));

          if (remotes && g_strv_contains ((const char * const *)remotes, remote_name))
            continue;

          if (applied_remotes && g_strv_contains ((const char * const *)applied_remotes, remote_name))
            continue;

          if (flatpakrepos == NULL)
            flatpakrepos = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

          g_hash_table_insert (flatpakrepos, g_steal_pointer (&remote_name), g_file_enumerator_get_child (dir_enum, file_info));
        }
    }

  return g_steal_pointer (&flatpakrepos);
}

static gboolean
apply_new_flatpakrepo (const char *remote_name,
                       GFile      *file,
                       OstreeRepo *repo,
                       GError    **error)
{
  g_autoptr(GBytes) gpg_data = NULL;
  g_autoptr(GKeyFile) group_config = NULL;
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GKeyFile) new_config = NULL;
  g_auto(GStrv) old_applied_remotes = NULL;
  g_autoptr(GPtrArray) new_applied_remotes = NULL;
  int i;

  if (!g_key_file_load_from_file (keyfile, flatpak_file_get_path_cached (file), 0, &local_error))
    {
      flatpak_fail (error, _("Can't load file %s: %s\n"), flatpak_file_get_path_cached (file), local_error->message);
      return FALSE;
    }

  group_config = flatpak_parse_repofile (remote_name, FALSE, keyfile, &gpg_data, NULL, &local_error);
  if (group_config == NULL)
    {
      flatpak_fail (error, _("Error parsing system flatpakrepo file for %s: %s"), remote_name, local_error->message);
      return FALSE;
    }

  new_config = ostree_repo_copy_config (repo);

  old_applied_remotes = g_key_file_get_string_list (new_config, "core", "xa.applied-remotes", NULL, NULL);

  copy_remote_config (new_config, group_config, remote_name);

  new_applied_remotes = g_ptr_array_new_with_free_func (g_free);
  for (i = 0; old_applied_remotes != NULL && old_applied_remotes[i] != NULL; i++)
    g_ptr_array_add (new_applied_remotes, g_strdup (old_applied_remotes[i]));

  g_ptr_array_add (new_applied_remotes, g_strdup (remote_name));

  g_key_file_set_string_list (new_config, "core", "xa.applied-remotes",
                              (const char * const *) new_applied_remotes->pdata, new_applied_remotes->len);

  if (!ostree_repo_write_config (repo, new_config, error))
    return FALSE;

  if (!ostree_repo_reload_config (repo, NULL, error))
    return FALSE;

  if (gpg_data != NULL)
    {
      g_autoptr(GInputStream) input_stream = g_memory_input_stream_new_from_bytes (gpg_data);
      guint imported = 0;

      if (!ostree_repo_remote_gpg_import (repo, remote_name, input_stream,
                                          NULL, &imported, NULL, error))
        return FALSE;

      g_info ("Imported %u GPG key%s to remote \"%s\"", imported, (imported == 1) ? "" : "s", remote_name);
    }

  return TRUE;
}

static gboolean
system_helper_maybe_ensure_repo (FlatpakDir *self,
                                 FlatpakHelperEnsureRepoFlags flags,
                                 gboolean allow_empty,
                                 GCancellable *cancellable,
                                 GError **error)
{
  g_autoptr(GError) local_error = NULL;
  const char *installation = flatpak_dir_get_id (self);

  if (!flatpak_dir_system_helper_call_ensure_repo (self,
                                                   flags,
                                                   installation ? installation : "",
                                                   cancellable, &local_error))
    {
      if (allow_empty)
        return TRUE;

      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  return TRUE;
}

static gboolean
ensure_repo_opened (OstreeRepo *repo,
                    GCancellable *cancellable,
                    GError **error)
{
  if (!ostree_repo_open (repo, cancellable, error))
    {
      g_autofree char *repopath = NULL;

      repopath = g_file_get_path (ostree_repo_get_path (repo));
      g_prefix_error (error, _("While opening repository %s: "), repopath);
      return FALSE;
    }

  return TRUE;
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
  g_autoptr(GFile) cache_dir = NULL;
  g_autoptr(GHashTable) flatpakrepos = NULL;
  FlatpakHelperEnsureRepoFlags ensure_flags = FLATPAK_HELPER_ENSURE_REPO_FLAGS_NONE;

  if (self->repo != NULL)
    return TRUE;

  /* Don't trigger polkit prompts if we are just doing this opportunistically */
  if (allow_empty)
    ensure_flags |= FLATPAK_HELPER_ENSURE_REPO_FLAGS_NO_INTERACTION;

  if (!g_file_query_exists (self->basedir, cancellable))
    {
      if (flatpak_dir_use_system_helper (self, NULL))
        {
          if (!system_helper_maybe_ensure_repo (self, ensure_flags, allow_empty, cancellable, error))
            return FALSE;
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

  repo = ostree_repo_new (repodir);

  if (!g_file_query_exists (repodir, cancellable))
    {
      /* We always use bare-user-only these days, except old installations
         that still user bare-user */
      OstreeRepoMode mode = OSTREE_REPO_MODE_BARE_USER_ONLY;

      if (flatpak_dir_use_system_helper (self, NULL))
        {
          if (!system_helper_maybe_ensure_repo (self, ensure_flags, allow_empty, cancellable, error))
            return FALSE;

          if (!ensure_repo_opened (repo, cancellable, error))
            return FALSE;
        }
      else
        {
          if (!ostree_repo_create (repo, mode, cancellable, &my_error))
            {
              const char *repo_path = flatpak_file_get_path_cached (repodir);

              flatpak_rm_rf (repodir, cancellable, NULL);

              if (allow_empty)
                return TRUE;

              /* As of 2022, the error message from libostree is not the most helpful:
               * Creating repo: mkdirat: Permission denied
               * If the repository path is in the error message, assume this
               * has been fixed. If not, add it. */
              if (strstr (my_error->message, repo_path) != NULL)
                g_propagate_error (error, g_steal_pointer (&my_error));
              else
                g_set_error (error, my_error->domain, my_error->code,
                             "Unable to create repository at %s (%s)",
                             repo_path, my_error->message);

              return FALSE;
            }

          /* Create .changed file early to avoid polling non-existing file in monitor */
          if (!flatpak_dir_mark_changed (self, &my_error))
            {
              g_warning ("Error marking directory as changed: %s", my_error->message);
              g_clear_error (&my_error);
            }
        }
    }
  else
    {
      if (!ensure_repo_opened (repo, cancellable, error))
        return FALSE;
    }

  /* In the system-helper case we're directly using the global repo, and we can't write any
   * caches for summaries there, so we need to set a custom dir for this. Note, as per #3303
   * this has to be called after ostree_repo_open() in order to the custom cachedir being
   * overridden if the system dir is writable (like in the testsuite).
   */
  if (flatpak_dir_use_system_helper (self, NULL))
    {
      g_autofree char *cache_path = NULL;

      cache_dir = flatpak_ensure_user_cache_dir_location (error);
      if (cache_dir == NULL)
        return FALSE;

      cache_path = g_file_get_path (cache_dir);
      if (!ostree_repo_set_cache_dir (repo,
                                      AT_FDCWD, cache_path,
                                      cancellable, error))
        return FALSE;
    }

  /* Earlier flatpak used to reset min-free-space-percent to 0 every time, but now we
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

  flatpakrepos = _flatpak_dir_find_new_flatpakrepos (self, repo);
  if (flatpakrepos)
    {
      if (flatpak_dir_use_system_helper (self, NULL))
        {
          if (!system_helper_maybe_ensure_repo (self, ensure_flags, allow_empty, cancellable, error))
            return FALSE;

          if (!ostree_repo_reload_config (repo, cancellable, error))
            return FALSE;
        }
      else
        {
          GLNX_HASH_TABLE_FOREACH_KV (flatpakrepos, const char *, remote_name, GFile *, file)
            {
              if (!apply_new_flatpakrepo (remote_name, file, repo, error))
                return FALSE;
            }
        }
    }


  if (cache_dir == NULL)
    cache_dir = g_file_get_child (repodir, "tmp/cache");

  /* Make sure we didn't reenter weirdly */
  g_assert (self->repo == NULL);
  self->repo = g_object_ref (repo);
  self->cache_dir = g_object_ref (cache_dir);

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

static gboolean
_flatpak_dir_reload_config (FlatpakDir   *self,
                            GCancellable *cancellable,
                            GError      **error)
{
  if (self->repo)
    {
      if (!ostree_repo_reload_config (self->repo, cancellable, error))
        return FALSE;
    }

  /* Clear cached stuff from repo config */
  G_LOCK (config_cache);

  g_clear_pointer (&self->masked, g_regex_unref);
  g_clear_pointer (&self->pinned, g_regex_unref);

  G_UNLOCK (config_cache);
  return TRUE;
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

GPtrArray *
flatpak_dir_get_config_patterns (FlatpakDir *dir, const char *key)
{
  g_autoptr(GPtrArray) patterns = NULL;
  g_autofree char *key_value = NULL;
  int i;

  patterns = g_ptr_array_new_with_free_func (g_free);

  key_value = flatpak_dir_get_config (dir, key, NULL);
  if (key_value)
    {
      g_auto(GStrv) oldv = g_strsplit (key_value, ";", -1);

      for (i = 0; oldv[i] != NULL; i++)
        {
          const char *old = oldv[i];

          if (*old != 0 && !flatpak_g_ptr_array_contains_string (patterns, old))
            g_ptr_array_add (patterns, g_strdup (old));
        }
    }

  return g_steal_pointer (&patterns);
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

  if (!_flatpak_dir_reload_config (self, NULL, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_dir_config_append_pattern (FlatpakDir *self,
                                   const char *key,
                                   const char *pattern,
                                   gboolean    runtime_only,
                                   gboolean   *out_already_present,
                                   GError    **error)
{
  g_autoptr(GPtrArray) patterns = flatpak_dir_get_config_patterns (self, key);
  g_autofree char *regexp;
  gboolean already_present;
  g_autofree char *merged_patterns = NULL;

  regexp = flatpak_filter_glob_to_regexp (pattern, runtime_only, error);
  if (regexp == NULL)
    return FALSE;

  if (!(already_present = flatpak_g_ptr_array_contains_string (patterns, pattern)))
    g_ptr_array_add (patterns, g_strdup (pattern));

  if (out_already_present)
    *out_already_present = already_present;

  g_ptr_array_sort (patterns, flatpak_strcmp0_ptr);

  g_ptr_array_add (patterns, NULL);
  merged_patterns = g_strjoinv (";", (char **)patterns->pdata);

  return flatpak_dir_set_config (self, key, merged_patterns, error);
}

gboolean
flatpak_dir_config_remove_pattern (FlatpakDir *self,
                                   const char *key,
                                   const char *pattern,
                                   GError    **error)
{
  g_autoptr(GPtrArray) patterns = flatpak_dir_get_config_patterns (self, key);
  g_autofree char *merged_patterns = NULL;
  int j;

  for (j = 0; j < patterns->len; j++)
    {
      if (strcmp (g_ptr_array_index (patterns, j), pattern) == 0)
        break;
    }

  if (j == patterns->len)
    return flatpak_fail (error, _("No current %s pattern matching %s"), key, pattern);
  else
    g_ptr_array_remove_index (patterns, j);

  g_ptr_array_add (patterns, NULL);
  merged_patterns = g_strjoinv (";", (char **)patterns->pdata);

  return flatpak_dir_set_config (self, key, merged_patterns, error);
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

#define SECS_PER_MINUTE (60)
#define SECS_PER_HOUR   (60 * SECS_PER_MINUTE)
#define SECS_PER_DAY    (24 * SECS_PER_HOUR)

/* This looks for old temporary files created by previous versions of
   flatpak_dir_deploy_appstream(). These are all either directories
   starting with a dot, or symlinks starting with a dot. Such temp
   files if found can be from a concurrent deploy, so we only remove
   any such files older than a day to avoid races.
*/
static void
remove_old_appstream_tmpdirs (GFile *dir)
{
  g_auto(GLnxDirFdIterator) dir_iter = { 0 };
  time_t now = time (NULL);

  if (!glnx_dirfd_iterator_init_at (AT_FDCWD, flatpak_file_get_path_cached (dir),
                                    FALSE, &dir_iter, NULL))
    return;

  while (TRUE)
    {
      struct stat stbuf;
      struct dirent *dent;
      g_autoptr(GFile) tmp = NULL;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dir_iter, &dent, NULL, NULL))
        break;

      if (dent == NULL)
        break;

      /* We ignore non-dotfiles and .timestamps as they are not tempfiles */
      if (dent->d_name[0] != '.' ||
          strcmp (dent->d_name, ".timestamp") == 0)
        continue;

      /* Check for right types and names */
      if (dent->d_type == DT_DIR)
        {
          if (strlen (dent->d_name) != 72 ||
              dent->d_name[65] != '-')
            continue;
        }
      else if (dent->d_type == DT_LNK)
        {
          if (!g_str_has_prefix (dent->d_name, ".active-"))
            continue;
        }
      else
        continue;

      /* Check that the file is at least a day old to avoid races */
      if (!glnx_fstatat (dir_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW, NULL))
        continue;

      if (stbuf.st_mtime >= now ||
          now - stbuf.st_mtime < SECS_PER_DAY)
        continue;

      tmp = g_file_get_child (dir, dent->d_name);

      /* We ignore errors here, no need to worry anyone */
      g_info ("Deleting stale appstream deploy tmpdir %s", flatpak_file_get_path_cached (tmp));
      (void)flatpak_rm_rf (tmp, NULL, NULL);
    }
}

/* Like the function above, this looks for old temporary directories created by
 * previous versions of flatpak_dir_deploy().
 * These are all directories starting with a dot. Such directories can be from a
 * concurrent deploy, so we only remove directories older than a day to avoid
 * races.
*/
static void
remove_old_deploy_tmpdirs (GFile *dir)
{
  g_auto(GLnxDirFdIterator) dir_iter = { 0 };
  time_t now = time (NULL);

  if (!glnx_dirfd_iterator_init_at (AT_FDCWD, flatpak_file_get_path_cached (dir),
                                    FALSE, &dir_iter, NULL))
    return;

  while (TRUE)
    {
      struct stat stbuf;
      struct dirent *dent;
      g_autoptr(GFile) tmp = NULL;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dir_iter, &dent, NULL, NULL))
        break;

      if (dent == NULL)
        break;

      /* We ignore non-dotfiles and .timestamps as they are not tempfiles */
      if (dent->d_name[0] != '.' ||
          strcmp (dent->d_name, ".timestamp") == 0)
        continue;

      /* Check for right types and names. The format we’re looking for is:
       * .[0-9a-f]{64}-[0-9A-Z]{6} */
      if (dent->d_type == DT_DIR)
        {
          if (strlen (dent->d_name) != 72 ||
              dent->d_name[65] != '-')
            continue;
        }
      else
        continue;

      /* Check that the file is at least a day old to avoid races */
      if (!glnx_fstatat (dir_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW, NULL))
        continue;

      if (stbuf.st_mtime >= now ||
          now - stbuf.st_mtime < SECS_PER_DAY)
        continue;

      tmp = g_file_get_child (dir, dent->d_name);

      /* We ignore errors here, no need to worry anyone */
      g_info ("Deleting stale deploy tmpdir %s", flatpak_file_get_path_cached (tmp));
      (void)flatpak_rm_rf (tmp, NULL, NULL);
    }
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
  const char *old_dir = NULL;
  g_autofree char *new_checksum = NULL;
  g_autoptr(GFile) active_link = NULL;
  g_autofree char *branch = NULL;
  g_autoptr(GFile) old_checkout_dir = NULL;
  g_autoptr(GFile) active_tmp_link = NULL;
  g_autoptr(GError) tmp_error = NULL;
  g_autofree char *new_dir = NULL;
  OstreeRepoCheckoutAtOptions options = { 0, };
  glnx_autofd int dfd = -1;
  g_autoptr(GFileInfo) file_info = NULL;
  g_autofree char *tmpname = g_strdup (".active-XXXXXX");
  g_auto(GLnxLockFile) lock = { 0, };
  gboolean do_compress = FALSE;
  gboolean do_uncompress = TRUE;
  g_autofree char *filter_checksum = NULL;
  g_autoptr(GRegex) allow_refs = NULL;
  g_autoptr(GRegex) deny_refs = NULL;
  g_autofree char *subset = NULL;
  g_auto(GLnxTmpDir) tmpdir = { 0, };
  g_autoptr(FlatpakTempDir) tmplink = NULL;

  /* Keep a shared repo lock to avoid prunes removing objects we're relying on
   * while we do the checkout. This could happen if the ref changes after we
   * read its current value for the checkout. */
  if (!flatpak_dir_repo_lock (self, &lock, LOCK_SH, cancellable, error))
    return FALSE;

  if (!flatpak_dir_lookup_remote_filter (self, remote, TRUE, &filter_checksum, &allow_refs, &deny_refs, error))
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

  old_dir = NULL;
  file_info = g_file_query_info (active_link, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, NULL);
  if (file_info != NULL)
    old_dir =  g_file_info_get_symlink_target (file_info);

  subset = flatpak_dir_get_remote_subset (self, remote);

  if (subset)
    branch = g_strdup_printf ("appstream2/%s-%s", subset, arch);
  else
    branch = g_strdup_printf ("appstream2/%s", arch);

  if (!flatpak_repo_resolve_rev (self->repo, NULL, remote, branch, TRUE,
                                 &new_checksum, cancellable, error))
    return FALSE;

  if (new_checksum == NULL && subset == NULL)
    {
      /* Fall back to old branch (only exist on non-subsets) */
      g_clear_pointer (&branch, g_free);
      branch = g_strdup_printf ("appstream/%s", arch);
      if (!flatpak_repo_resolve_rev (self->repo, NULL, remote, branch, TRUE,
                                     &new_checksum, cancellable, error))
        return FALSE;
      do_compress = FALSE;
      do_uncompress = TRUE;
    }
  else
    {
      do_compress = TRUE;
      do_uncompress = FALSE;
    }

  if (new_checksum == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("No appstream commit to deploy"));

  if (filter_checksum)
    new_dir = g_strconcat (new_checksum, "-", filter_checksum, NULL);
  else
    new_dir = g_strdup (new_checksum);

  real_checkout_dir = g_file_get_child (arch_dir, new_dir);
  checkout_exists = g_file_query_exists (real_checkout_dir, NULL);

  if (old_dir != NULL && new_dir != NULL &&
      strcmp (old_dir, new_dir) == 0 &&
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
    g_autofree char *template = g_strdup_printf (".%s-XXXXXX", new_dir);
    g_autoptr(GFile) tmp_dir_template = g_file_get_child (arch_dir, template);

    if (!glnx_mkdtempat (AT_FDCWD, flatpak_file_get_path_cached (tmp_dir_template), 0755,
                         &tmpdir, error))
      return FALSE;
  }

  checkout_dir = g_file_new_for_path (tmpdir.path);

  options.mode = OSTREE_REPO_CHECKOUT_MODE_USER;
  options.overwrite_mode = OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES;
  options.enable_fsync = FALSE; /* We checkout to a temp dir and sync before moving it in place */
  options.bareuseronly_dirs = TRUE; /* https://github.com/ostreedev/ostree/pull/927 */

  if (!ostree_repo_checkout_at (self->repo, &options,
                                AT_FDCWD, tmpdir.path, new_checksum,
                                cancellable, error))
    return FALSE;

  /* Old appstream format don't have uncompressed file, so we uncompress it */
  if (do_uncompress)
    {
      g_autoptr(GFile) appstream_xml = g_file_get_child (checkout_dir, "appstream.xml");
      g_autoptr(GFile) appstream_gz_xml = g_file_get_child (checkout_dir, "appstream.xml.gz");
      g_autoptr(GOutputStream) out2 = NULL;
      g_autoptr(GFileOutputStream) out = NULL;
      g_autoptr(GFileInputStream) in = NULL;

      in = g_file_read (appstream_gz_xml, NULL, NULL);
      if (in)
        {
          g_autoptr(GZlibDecompressor) decompressor = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
          out = g_file_replace (appstream_xml, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION,
                                NULL, error);
          if (out == NULL)
            return FALSE;

          out2 = g_converter_output_stream_new (G_OUTPUT_STREAM (out), G_CONVERTER (decompressor));
          if (g_output_stream_splice (out2, G_INPUT_STREAM (in), G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                      NULL, error) < 0)
            return FALSE;
        }
    }

  if (deny_refs)
    {
      g_autoptr(GFile) appstream_xml = g_file_get_child (checkout_dir, "appstream.xml");
      g_autoptr(GFileInputStream) in = NULL;

      /* We need some ref filtering, so parse the xml */

      in = g_file_read (appstream_xml, NULL, NULL);
      if (in)
        {
          g_autoptr(FlatpakXml) appstream = NULL;
          g_autoptr(GBytes) content = NULL;

          appstream = flatpak_xml_parse (G_INPUT_STREAM (in), FALSE, cancellable, error);
          if (appstream == NULL)
            return FALSE;

          flatpak_appstream_xml_filter (appstream, allow_refs, deny_refs);

          if (!flatpak_appstream_xml_root_to_data (appstream, &content, NULL, error))
            return FALSE;

          if (!g_file_replace_contents  (appstream_xml,
                                         g_bytes_get_data (content, NULL), g_bytes_get_size (content),
                                         NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, NULL,
                                         cancellable, error))
            return FALSE;
        }

      do_compress = TRUE; /* We need to recompress this */
    }

  /* New appstream format don't have compressed file, so we compress it */
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

  if (!g_file_make_symbolic_link (active_tmp_link, new_dir, cancellable, error))
    return FALSE;

   /* This is a link, not a dir, but it will remove the same way on destroy */
  tmplink = g_object_ref (active_tmp_link);

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

  /* Don't delete tmpdir now that it's moved */
  glnx_tmpdir_unset (&tmpdir);

  if (syncfs (dfd) != 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  if (!flatpak_file_rename (active_tmp_link,
                            active_link,
                            cancellable, error))
    return FALSE;

  /* Don't delete tmplink now that it's moved */
  g_object_unref (g_steal_pointer (&tmplink));

  if (old_dir != NULL &&
      g_strcmp0 (old_dir, new_dir) != 0)
    {
      old_checkout_dir = g_file_get_child (arch_dir, old_dir);
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

  /* There used to be an bug here where temporary files where not removed, which could use
   * quite a lot of space over time, so we check for these and remove them. */
  remove_old_appstream_tmpdirs (arch_dir);

  if (out_changed)
    *out_changed = TRUE;

  return TRUE;
}

static gboolean repo_get_remote_collection_id (OstreeRepo *repo,
                                               const char *remote_name,
                                               char      **collection_id_out,
                                               GError    **error);


gboolean
flatpak_dir_find_latest_rev (FlatpakDir               *self,
                             FlatpakRemoteState       *state,
                             const char               *ref,
                             const char               *checksum_or_latest,
                             char                    **out_rev,
                             guint64                  *out_timestamp,
                             GFile                   **out_sideload_path,
                             GCancellable             *cancellable,
                             GError                  **error)
{
  g_autofree char *latest_rev = NULL;

  g_return_val_if_fail (out_rev != NULL, FALSE);

  if (!flatpak_remote_state_lookup_ref (state, ref, &latest_rev, out_timestamp, NULL, out_sideload_path, error))
    return FALSE;
  if (latest_rev == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_REF_NOT_FOUND,
                               _("Couldn't find latest checksum for ref %s in remote %s"),
                               ref, state->remote_name);

  if (out_rev != NULL)
    *out_rev = g_steal_pointer (&latest_rev);

  return TRUE;
}

static gboolean
get_mtime (GFile        *file,
           GTimeVal     *result,
           GCancellable *cancellable,
           GError      **error)
{
  g_autoptr(GFileInfo) info = g_file_query_info (file,
                                                 G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                                 G_FILE_QUERY_INFO_NONE,
                                                 cancellable, error);
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

  return get_mtime (src, &src_mtime, cancellable, NULL) &&
         get_mtime (dest, &dest_mtime, cancellable, NULL) &&
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

  ensure_http_session (self);

  if (!ostree_repo_remote_get_url (self->repo,
                                   remote,
                                   &oci_uri,
                                   error))
    return NULL;

  if (!flatpak_oci_index_ensure_cached (self->http_session, oci_uri,
                                        index_cache, index_uri_out,
                                        cancellable, &local_error))
    {
      if (!g_error_matches (local_error, FLATPAK_HTTP_ERROR, FLATPAK_HTTP_ERROR_NOT_CHANGED))
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
                                  FlatpakProgress     *progress,
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

  ensure_http_session (self);

  appstream = flatpak_oci_index_make_appstream (self->http_session,
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
                              FlatpakProgress     *progress,
                              GCancellable        *cancellable,
                              GError             **error)
{
  g_autofree char *new_branch = NULL;
  g_autofree char *old_branch = NULL;
  const char *used_branch = NULL;
  g_autofree char *new_checksum = NULL;
  g_autoptr(GError) first_error = NULL;
  g_autoptr(GError) second_error = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;
  g_autofree char *appstream_commit = NULL;
  g_autofree char *subset = NULL;
  g_autoptr(GFile) appstream_sideload_path = NULL;
  const char *installation;
  gboolean is_oci;

  if (out_changed)
    *out_changed = FALSE;

  if (arch == NULL)
    arch = flatpak_get_arch ();

  subset = flatpak_dir_get_remote_subset (self, remote);

  if (subset)
    {
      new_branch = g_strdup_printf ("appstream2/%s-%s", subset, arch);
      old_branch = g_strdup_printf ("appstream/%s-%s", subset, arch);
    }
  else
    {
      new_branch = g_strdup_printf ("appstream2/%s", arch);
      old_branch = g_strdup_printf ("appstream/%s", arch);
    }

  is_oci = flatpak_dir_get_remote_oci (self, remote);

  state = flatpak_dir_get_remote_state_optional (self, remote, FALSE, cancellable, error);
  if (state == NULL)
    return FALSE;

  used_branch = new_branch;
  if (!is_oci)
    {
      if (!flatpak_dir_find_latest_rev (self, state, used_branch, NULL, &appstream_commit, NULL, &appstream_sideload_path, cancellable, &first_error))
        {
          used_branch = old_branch;
          if (!flatpak_dir_find_latest_rev (self, state, used_branch, NULL, &appstream_commit, NULL, &appstream_sideload_path, cancellable, &second_error))
            {
              g_prefix_error (&first_error, "Error updating appstream2: ");
              g_prefix_error (&second_error, "Error updating appstream: ");
              g_propagate_prefixed_error (error, g_steal_pointer (&second_error), "%s; ", first_error->message);
              return FALSE;
            }
        }
    }

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
      else if (!gpg_verify_summary || !gpg_verify)
        {
          /* The remote is not gpg verified, so we don't want to allow installation via
             a download in the home directory, as there is no way to verify you're not
             injecting anything into the remote. However, in the case of a remote
             configured to a local filesystem we can just let the system helper do
             the installation, as it can then avoid network i/o and be certain the
             data comes from the right place.  */
          if (!g_str_has_prefix (url, "file:"))
            return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("Can't pull from untrusted non-gpg verified remote"));
        }
      else
        {
          g_autoptr(OstreeRepo) child_repo = flatpak_dir_create_system_child_repo (self, &child_repo_lock, NULL, error);
          if (child_repo == NULL)
            return FALSE;

          if (!flatpak_dir_pull (self, state, used_branch, appstream_commit, NULL, appstream_sideload_path, NULL, NULL,
                                 child_repo, FLATPAK_PULL_FLAGS_NONE, 0,
                                 progress, cancellable, error))
            {
              g_prefix_error (&first_error, "Error updating appstream: ");
              return FALSE;
            }

          if (!flatpak_repo_resolve_rev (child_repo, NULL, remote, used_branch, TRUE,
                                         &new_checksum, cancellable, error))
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


  if (!flatpak_dir_pull (self, state, used_branch, appstream_commit, NULL, appstream_sideload_path, NULL, NULL, NULL,
                         FLATPAK_PULL_FLAGS_NONE, OSTREE_REPO_PULL_FLAGS_NONE, progress,
                         cancellable, error))
    {
      g_prefix_error (&first_error, "Error updating appstream: ");
      return FALSE;
    }

  if (!flatpak_repo_resolve_rev (self->repo, NULL, remote, used_branch, TRUE,
                                 &new_checksum, cancellable, error))
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
                         FlatpakRemoteState  *state,
                         const char          *ref_to_fetch,
                         const char          *token,
                         const gchar * const *dirs_to_pull,
                         const char          *current_local_checksum,
                         gboolean             force_disable_deltas,
                         OstreeRepoPullFlags  flags,
                         FlatpakProgress     *progress)
{
  guint32 update_interval = 0;
  GVariantBuilder hdr_builder;

  if (state->summary_bytes && state->summary_sig_bytes)
    {
      g_variant_builder_add (builder, "{s@v}", "summary-bytes",
                             g_variant_new_variant (g_variant_new_from_bytes (G_VARIANT_TYPE ("ay"), state->summary_bytes, TRUE)));
      g_variant_builder_add (builder, "{s@v}", "summary-sig-bytes",
                             g_variant_new_variant (g_variant_new_from_bytes (G_VARIANT_TYPE ("ay"), state->summary_sig_bytes, TRUE)));
    }

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
  if (token)
    {
      g_autofree char *bearer_token = g_strdup_printf ("Bearer %s", token);
      g_variant_builder_add (&hdr_builder, "(ss)", "Authorization", bearer_token);
    }
  if (current_local_checksum)
    g_variant_builder_add (&hdr_builder, "(ss)", "Flatpak-Upgrade-From", current_local_checksum);
  g_variant_builder_add (builder, "{s@v}", "http-headers",
                         g_variant_new_variant (g_variant_builder_end (&hdr_builder)));
  g_variant_builder_add (builder, "{s@v}", "append-user-agent",
                         g_variant_new_variant (g_variant_new_string ("flatpak/" PACKAGE_VERSION)));

  update_interval = flatpak_progress_get_update_interval (progress);

  g_variant_builder_add (builder, "{s@v}", "update-frequency",
                         g_variant_new_variant (g_variant_new_uint32 (update_interval)));
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
           FlatpakRemoteState                   *state,
           const char                          **dirs_to_pull,
           const char                           *ref_to_fetch,
           const char                           *rev_to_fetch,
           GFile                                *sideload_repo,
           const char                           *token,
           FlatpakPullFlags                      flatpak_flags,
           OstreeRepoPullFlags                   flags,
           FlatpakProgress                      *progress,
           GCancellable                         *cancellable,
           GError                              **error)
{
  gboolean force_disable_deltas = (flatpak_flags & FLATPAK_PULL_FLAGS_NO_STATIC_DELTAS) != 0;
  g_autofree char *current_checksum = NULL;
  g_autoptr(GVariant) old_commit = NULL;
  g_autoptr(GVariant) new_commit = NULL;
  const char *revs_to_fetch[2];
  g_autoptr(GError) dummy_error = NULL;
  GVariantBuilder builder;
  g_autoptr(GVariant) options = NULL;
  const char *refs_to_fetch[2];
  g_autofree char *sideload_url = NULL;

  g_return_val_if_fail (ref_to_fetch != NULL, FALSE);
  g_return_val_if_fail (rev_to_fetch != NULL, FALSE);

  /* The ostree fetcher asserts if error is NULL */
  if (error == NULL)
    error = &dummy_error;

  /* We always want this on for every type of pull */
  flags |= OSTREE_REPO_PULL_FLAGS_BAREUSERONLY_FILES;

  if (!flatpak_repo_resolve_rev (self, NULL, state->remote_name, ref_to_fetch, TRUE,
                                 &current_checksum, cancellable, error))
    return FALSE;

  if (current_checksum != NULL &&
      !ostree_repo_load_commit (self, current_checksum, &old_commit, NULL, error))
    return FALSE;

  /* Pull options */
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  get_common_pull_options (&builder, state, ref_to_fetch, token, dirs_to_pull, current_checksum,
                           force_disable_deltas, flags, progress);

  if (sideload_repo)
    {
      GVariantBuilder colref_builder;

      sideload_url = g_file_get_uri (sideload_repo);

      g_info ("Sideloading %s from %s in pull", ref_to_fetch, sideload_url);

      g_assert (state->collection_id != NULL);

      g_variant_builder_init (&colref_builder, G_VARIANT_TYPE ("a(sss)"));
      g_variant_builder_add (&colref_builder, "(sss)", state->collection_id, ref_to_fetch, rev_to_fetch);

      g_variant_builder_add (&builder, "{s@v}", "collection-refs",
                             g_variant_new_variant (g_variant_builder_end (&colref_builder)));
      g_variant_builder_add (&builder, "{s@v}", "override-remote-name",
                             g_variant_new_variant (g_variant_new_string (state->remote_name)));
    }
  else
    {
      refs_to_fetch[0] = ref_to_fetch;
      refs_to_fetch[1] = NULL;
      g_variant_builder_add (&builder, "{s@v}", "refs",
                             g_variant_new_variant (g_variant_new_strv ((const char * const *) refs_to_fetch, -1)));

      revs_to_fetch[0] = rev_to_fetch;
      revs_to_fetch[1] = NULL;
      g_variant_builder_add (&builder, "{s@v}", "override-commit-ids",
                             g_variant_new_variant (g_variant_new_strv ((const char * const *) revs_to_fetch, -1)));


      if (state->sideload_repos->len > 0)
        {
          GVariantBuilder localcache_repos_builder;

          g_variant_builder_init (&localcache_repos_builder, G_VARIANT_TYPE ("as"));
          for (int i = 0; i < state->sideload_repos->len; i++)
            {
              FlatpakSideloadState *ss = g_ptr_array_index (state->sideload_repos, i);
              GFile *sideload_path = ostree_repo_get_path (ss->repo);

              g_variant_builder_add (&localcache_repos_builder, "s",
                                     flatpak_file_get_path_cached (sideload_path));
            }
          g_variant_builder_add (&builder, "{s@v}", "localcache-repos",
                                 g_variant_new_variant (g_variant_builder_end (&localcache_repos_builder)));
        }
    }

  options = g_variant_ref_sink (g_variant_builder_end (&builder));

  {
    g_auto(FlatpakMainContext) context = FLATKPAK_MAIN_CONTEXT_INIT;
    flatpak_progress_init_main_context (progress, &context);

    if (!ostree_repo_pull_with_options (self,
                                        sideload_url ? sideload_url : state->remote_name,
                                        options, context.ostree_progress, cancellable, error))
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
ensure_http_session (FlatpakDir *self)
{
  if (g_once_init_enter (&self->http_session))
    {
      FlatpakHttpSession *http_session;

      http_session = flatpak_create_http_session (PACKAGE_STRING);

      g_once_init_leave (&self->http_session, http_session);
    }
}

static void
extra_data_progress_report (guint64  downloaded_bytes,
                            gpointer user_data)
{
  FlatpakProgress *progress = FLATPAK_PROGRESS (user_data);

  flatpak_progress_update_extra_data (progress, downloaded_bytes);
}

static void
compute_extra_data_download_size (GVariant *commitv,
                                  guint64 *out_n_extra_data,
                                  guint64 *out_total_download_size)
{
  guint64 i;
  guint64 n_extra_data = 0;
  guint64 total_download_size = 0;
  g_autoptr(GVariant) extra_data_sources = NULL;

  extra_data_sources = flatpak_commit_get_extra_data_sources (commitv, NULL);
  if (extra_data_sources != NULL)
    {
      n_extra_data = g_variant_n_children (extra_data_sources);
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

  *out_n_extra_data = n_extra_data;
  *out_total_download_size = total_download_size;
}

static gboolean
flatpak_dir_setup_extra_data (FlatpakDir                           *self,
                              FlatpakRemoteState                   *state,
                              OstreeRepo                           *repo,
                              const char                           *ref,
                              const char                           *rev,
                              GFile                                *sideload_repo,
                              const char                           *token,
                              FlatpakPullFlags                      flatpak_flags,
                              FlatpakProgress                      *progress,
                              GCancellable                         *cancellable,
                              GError                              **error)
{
  guint64 n_extra_data = 0;
  guint64 total_download_size = 0;

  /* ostree-metadata and appstreams never have extra data, so ignore those */
  if (g_str_has_prefix (ref, "app/") || g_str_has_prefix (ref, "runtime/"))
    {
      g_autofree char *summary_checksum = NULL;
      GVariant *summary;

      /* Version 1 added extra data details, so we can rely on it
       * either being in the sparse cache or no extra data.  However,
       * it only applies to the commit the summary contains, so verify
       * that too.
       */
      summary = get_summary_for_ref (state, ref);
      if (summary != NULL &&
          flatpak_summary_lookup_ref (summary, NULL, ref, &summary_checksum, NULL) &&
          g_strcmp0 (rev, summary_checksum) == 0 &&
          flatpak_remote_state_get_cache_version (state) >= 1)
        {
          VarMetadataRef metadata;
          VarVariantRef res;

          if (flatpak_remote_state_lookup_sparse_cache (state, ref, &metadata, NULL) &&
              var_metadata_lookup (metadata, FLATPAK_SPARSE_CACHE_KEY_EXTRA_DATA_SIZE, NULL, &res) &&
              var_variant_is_type (res, VAR_EXTRA_DATA_SIZE_TYPEFORMAT))
            {
              VarExtraDataSizeRef eds = var_extra_data_size_from_variant (res);
              n_extra_data = var_extra_data_size_get_n_extra_data (eds);
              total_download_size = var_extra_data_size_get_total_size (eds);
            }
        }
      else
        {
          /* No summary/cache or old cache version, download commit and get size from there */
          g_autoptr(GVariant) commitv = flatpak_remote_state_load_ref_commit (state, self, ref, rev, token, NULL, cancellable, error);
          if (commitv == NULL)
            return FALSE;

          compute_extra_data_download_size (commitv, &n_extra_data, &total_download_size);
        }
    }

  if (n_extra_data > 0 &&
      (flatpak_flags & FLATPAK_PULL_FLAGS_DOWNLOAD_EXTRA_DATA) == 0)
    return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("Extra data not supported for non-gpg-verified local system installs"));

  flatpak_progress_init_extra_data (progress, n_extra_data, total_download_size);

  return TRUE;
}

static gboolean
flatpak_dir_pull_extra_data (FlatpakDir          *self,
                             OstreeRepo          *repo,
                             const char          *repository,
                             const char          *ref,
                             const char          *rev,
                             FlatpakPullFlags     flatpak_flags,
                             FlatpakProgress     *progress,
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
  flatpak_progress_start_extra_data (progress);

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
          flatpak_progress_reset_extra_data (progress);
          return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Unsupported extra data uri %s"), extra_data_uri);
        }

      /* TODO: Download to disk to support resumed downloads on error */

      extra_local_file = flatpak_build_file (base_dir, "extra-data", extra_data_sha256, extra_data_name, NULL);
      if (g_file_query_exists (extra_local_file, cancellable))
        {
          g_info ("Loading extra-data from local file %s", flatpak_file_get_path_cached (extra_local_file));
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
          ensure_http_session (self);
          bytes = flatpak_load_uri (self->http_session, extra_data_uri, 0, NULL,
                                    extra_data_progress_report, progress, NULL,
                                    cancellable, error);
        }

      if (bytes == NULL)
        {
          flatpak_progress_reset_extra_data (progress);
          g_prefix_error (error, _("While downloading %s: "), extra_data_uri);
          return FALSE;
        }

      if (g_bytes_get_size (bytes) != download_size)
        {
          flatpak_progress_reset_extra_data (progress);
          return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Wrong size for extra data %s"), extra_data_uri);
        }

      flatpak_progress_complete_extra_data_download (progress, download_size);

      sha256 = g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, bytes);
      if (strcmp (sha256, extra_data_sha256) != 0)
        {
          flatpak_progress_reset_extra_data (progress);
          return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid checksum for extra data %s"), extra_data_uri);
        }

      g_variant_builder_add (extra_data_builder,
                             "(^ay@ay)",
                             extra_data_name,
                             g_variant_new_from_bytes (G_VARIANT_TYPE ("ay"), bytes, TRUE));
    }

  extra_data = g_variant_ref_sink (g_variant_builder_end (extra_data_builder));

  flatpak_progress_reset_extra_data (progress);

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

static void
oci_pull_progress_cb (guint64 total_size, guint64 pulled_size,
                      guint32 n_layers, guint32 pulled_layers,
                      gpointer data)
{
  FlatpakProgress *progress = data;

  flatpak_progress_update_oci_pull (progress, total_size, pulled_size, n_layers, pulled_layers);
}

static gboolean
flatpak_dir_mirror_oci (FlatpakDir          *self,
                        FlatpakOciRegistry  *dst_registry,
                        FlatpakRemoteState  *state,
                        const char          *ref,
                        const char          *opt_rev,
                        const char          *skip_if_current_is,
                        const char          *token,
                        FlatpakProgress     *progress,
                        GCancellable        *cancellable,
                        GError             **error)
{
  g_autoptr(FlatpakOciRegistry) registry = NULL;
  g_autofree char *oci_digest = NULL;
  g_autofree char *latest_rev = NULL;
  VarRefInfoRef latest_rev_info;
  VarMetadataRef metadata;
  const char *oci_repository = NULL;
  const char *delta_url = NULL;
  const char *rev;
  gboolean res;

  /* We use the summary so that we can reuse any cached json */
  if (!flatpak_remote_state_lookup_ref (state, ref, &latest_rev, NULL, &latest_rev_info, NULL, error))
    return FALSE;
  if (latest_rev == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_REF_NOT_FOUND,
                               _("Couldn't find latest checksum for ref %s in remote %s"),
                               ref, state->remote_name);

  rev = opt_rev != NULL ? opt_rev : latest_rev;

  if (skip_if_current_is != NULL && strcmp (rev, skip_if_current_is) == 0)
    {
      return flatpak_fail_error (error, FLATPAK_ERROR_ALREADY_INSTALLED,
                                 _("%s commit %s already installed"),
                                 ref, rev);
    }

  metadata = var_ref_info_get_metadata (latest_rev_info);
  oci_repository = var_metadata_lookup_string (metadata, "xa.oci-repository", NULL);
  delta_url = var_metadata_lookup_string (metadata, "xa.delta-url", NULL);

  oci_digest = g_strconcat ("sha256:", rev, NULL);

  registry = flatpak_remote_state_new_oci_registry (state, token, cancellable, error);
  if (registry == NULL)
    return FALSE;

  flatpak_progress_start_oci_pull (progress);

  g_info ("Mirroring OCI image %s", oci_digest);

  res = flatpak_mirror_image_from_oci (dst_registry, registry, oci_repository, oci_digest, state->remote_name, ref, delta_url, self->repo, oci_pull_progress_cb,
                                       progress, cancellable, error);

  if (!res)
    return FALSE;

  return TRUE;
}

static gboolean
flatpak_dir_pull_oci (FlatpakDir          *self,
                      FlatpakRemoteState  *state,
                      const char          *ref,
                      const char          *opt_rev,
                      OstreeRepo          *repo,
                      FlatpakPullFlags     flatpak_flags,
                      OstreeRepoPullFlags  flags,
                      const char          *token,
                      FlatpakProgress     *progress,
                      GCancellable        *cancellable,
                      GError             **error)
{
  g_autoptr(FlatpakOciRegistry) registry = NULL;
  g_autoptr(FlatpakOciVersioned) versioned = NULL;
  g_autoptr(FlatpakOciImage) image_config = NULL;
  const char *oci_repository = NULL;
  const char *delta_url = NULL;
  g_autofree char *oci_digest = NULL;
  g_autofree char *checksum = NULL;
  VarRefInfoRef latest_rev_info;
  g_autofree char *latest_alt_commit = NULL;
  VarMetadataRef metadata;
  g_autofree char *latest_rev = NULL;
  G_GNUC_UNUSED g_autofree char *latest_commit =
    flatpak_dir_read_latest (self, state->remote_name, ref, &latest_alt_commit, cancellable, NULL);
  g_autofree char *name = NULL;

  /* We use the summary so that we can reuse any cached json */
  if (!flatpak_remote_state_lookup_ref (state, ref, &latest_rev, NULL, &latest_rev_info, NULL, error))
    return FALSE;
  if (latest_rev == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_REF_NOT_FOUND,
                               _("Couldn't find latest checksum for ref %s in remote %s"),
                               ref, state->remote_name);

  metadata = var_ref_info_get_metadata (latest_rev_info);
  oci_repository = var_metadata_lookup_string (metadata, "xa.oci-repository", NULL);
  delta_url = var_metadata_lookup_string (metadata, "xa.delta-url", NULL);

  oci_digest = g_strconcat ("sha256:", opt_rev != NULL ? opt_rev : latest_rev, NULL);

  /* Short circuit if we've already got this commit */
  if (latest_alt_commit != NULL && strcmp (oci_digest + strlen ("sha256:"), latest_alt_commit) == 0)
    return TRUE;

  registry = flatpak_remote_state_new_oci_registry (state, token, cancellable, error);
  if (registry == NULL)
    return FALSE;

  versioned = flatpak_oci_registry_load_versioned (registry, oci_repository, oci_digest,
                                                   NULL, NULL, cancellable, error);
  if (versioned == NULL)
    return FALSE;

  if (!FLATPAK_IS_OCI_MANIFEST (versioned))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Image is not a manifest"));

  image_config = flatpak_oci_registry_load_image_config (registry, oci_repository,
                                                         FLATPAK_OCI_MANIFEST (versioned)->config.digest,
                                                         (const char **)FLATPAK_OCI_MANIFEST (versioned)->config.urls,
                                                         NULL, cancellable, error);
  if (image_config == NULL)
    return FALSE;

  if (repo == NULL)
    repo = self->repo;

  flatpak_progress_start_oci_pull (progress);

  g_info ("Pulling OCI image %s", oci_digest);

  checksum = flatpak_pull_from_oci (repo, registry, oci_repository, oci_digest, delta_url, FLATPAK_OCI_MANIFEST (versioned), image_config,
                                    state->remote_name, ref, flatpak_flags, oci_pull_progress_cb, progress, cancellable, error);

  if (checksum == NULL)
    return FALSE;

  g_info ("Imported OCI image as checksum %s", checksum);

  if (repo == self->repo)
    name = flatpak_dir_get_name (self);
  else
    {
      GFile *file = ostree_repo_get_path (repo);
      name = g_file_get_path (file);
    }

  (flatpak_dir_log) (self, __FILE__, __LINE__, __FUNCTION__, name,
                     "pull oci", flatpak_oci_registry_get_uri (registry), ref, NULL, NULL, NULL,
                     "Pulled %s from %s", ref, flatpak_oci_registry_get_uri (registry));

  return TRUE;
}

gboolean
flatpak_dir_pull (FlatpakDir                           *self,
                  FlatpakRemoteState                   *state,
                  const char                           *ref,
                  const char                           *opt_rev,
                  const char                          **subpaths,
                  GFile                                *sideload_repo,
                  GBytes                               *require_metadata,
                  const char                           *token,
                  OstreeRepo                           *repo,
                  FlatpakPullFlags                      flatpak_flags,
                  OstreeRepoPullFlags                   flags,
                  FlatpakProgress                      *progress,
                  GCancellable                         *cancellable,
                  GError                              **error)
{
  gboolean ret = FALSE;
  gboolean have_commit = FALSE;
  g_autofree char *rev = NULL;
  g_autofree char *url = NULL;
  g_autoptr(GPtrArray) subdirs_arg = NULL;
  g_auto(GLnxLockFile) lock = { 0, };
  g_autofree char *name = NULL;
  g_autofree char *current_checksum = NULL;

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
    return flatpak_dir_pull_oci (self, state, ref, opt_rev, repo, flatpak_flags,
                                 flags, token, progress, cancellable, error);

  if (!ostree_repo_remote_get_url (self->repo,
                                   state->remote_name,
                                   &url,
                                   error))
    return FALSE;

  if (*url == 0)
    return TRUE; /* Empty url, silently disables updates */

  /* We get the rev ahead of time so that we know it for looking up e.g. extra-data
     and to make sure we're atomically using a single rev if we happen to do multiple
     pulls (e.g. with subpaths) */
  if (opt_rev != NULL)
    {
      rev = g_strdup (opt_rev);
    }
  else if (!flatpak_remote_state_lookup_ref (state, ref, &rev, NULL, NULL, NULL, error))
    {
      g_assert (error == NULL || *error != NULL);
      return FALSE;
    }

  g_info ("%s: Using commit %s for pull of ref %s from remote %s%s%s",
          G_STRFUNC, rev, ref, state->remote_name,
          sideload_repo ? "sideloaded from " : "",
          sideload_repo ? flatpak_file_get_path_cached (sideload_repo) : ""
          );

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

  /* Setup extra data information before starting to pull, so we can have precise
   * progress reports */
  if (!flatpak_dir_setup_extra_data (self, state, repo,
                                     ref, rev, sideload_repo, token,
                                     flatpak_flags,
                                     progress,
                                     cancellable,
                                     error))
    goto out;

  /* Work around a libostree bug where the pull may succeed but the pulled
   * commit will be incomplete by preemptively marking the commit partial.
   * Note this has to be done before ostree_repo_prepare_transaction() so we
   * aren't checking the staging dir for the commit.
   * https://github.com/flatpak/flatpak/issues/3479
   * https://github.com/ostreedev/ostree/pull/2549
   */
  {
    g_autoptr(GError) local_error = NULL;

    if (!ostree_repo_has_object (repo, OSTREE_OBJECT_TYPE_COMMIT, rev, &have_commit, NULL, &local_error))
      g_warning ("Encountered error checking for commit object %s: %s", rev, local_error->message);
    else if (!have_commit &&
             !ostree_repo_mark_commit_partial (repo, rev, TRUE, &local_error))
      g_warning ("Encountered error marking commit partial: %s: %s", rev, local_error->message);
  }

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    goto out;

  flatpak_repo_resolve_rev (repo, NULL, state->remote_name, ref, TRUE,
                            &current_checksum, NULL, NULL);

  if (!repo_pull (repo, state,
                  subdirs_arg ? (const char **) subdirs_arg->pdata : NULL,
                  ref, rev, sideload_repo, token, flatpak_flags, flags,
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
    {
      ostree_repo_abort_transaction (repo, cancellable, NULL);
      g_assert (error == NULL || *error != NULL);
    }

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
                           FlatpakProgress     *progress,
                           GCancellable        *cancellable,
                           GError             **error)
{
  /* The latter flag was introduced in https://github.com/ostreedev/ostree/pull/926 */
  const OstreeRepoPullFlags flags = OSTREE_REPO_PULL_FLAGS_UNTRUSTED | OSTREE_REPO_PULL_FLAGS_BAREUSERONLY_FILES;
  GVariantBuilder builder;
  g_autoptr(GVariant) options = NULL;
  gboolean res;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  const char *refs[2] = { NULL, NULL };
  const char *commits[2] = { NULL, NULL };
  g_autoptr(GError) dummy_error = NULL;
  g_auto(FlatpakMainContext) context = FLATKPAK_MAIN_CONTEXT_INIT;

  /* The ostree fetcher asserts if error is NULL */
  if (error == NULL)
    error = &dummy_error;

  refs[0] = ref;
  commits[0] = checksum;

  g_variant_builder_add (&builder, "{s@v}", "refs",
                         g_variant_new_variant (g_variant_new_strv ((const char * const *) refs, -1)));
  g_variant_builder_add (&builder, "{s@v}", "override-commit-ids",
                         g_variant_new_variant (g_variant_new_strv ((const char * const *) commits, -1)));

  g_variant_builder_add (&builder, "{s@v}", "flags",
                         g_variant_new_variant (g_variant_new_int32 (flags)));
  g_variant_builder_add (&builder, "{s@v}", "override-remote-name",
                         g_variant_new_variant (g_variant_new_string (remote_name)));
  g_variant_builder_add (&builder, "{s@v}", "gpg-verify",
                         g_variant_new_variant (g_variant_new_boolean (TRUE)));
  g_variant_builder_add (&builder, "{s@v}", "gpg-verify-summary",
                         g_variant_new_variant (g_variant_new_boolean (FALSE)));
  g_variant_builder_add (&builder, "{s@v}", "inherit-transaction",
                         g_variant_new_variant (g_variant_new_boolean (TRUE)));
  g_variant_builder_add (&builder, "{s@v}", "update-frequency",
                         g_variant_new_variant (g_variant_new_uint32 (FLATPAK_DEFAULT_UPDATE_INTERVAL_MS)));

  if (dirs_to_pull)
    {
      g_variant_builder_add (&builder, "{s@v}", "subdirs",
                             g_variant_new_variant (g_variant_new_strv ((const char * const *) dirs_to_pull, -1)));
      g_variant_builder_add (&builder, "{s@v}", "disable-static-deltas",
                             g_variant_new_variant (g_variant_new_boolean (TRUE)));
    }

  options = g_variant_ref_sink (g_variant_builder_end (&builder));

  flatpak_progress_init_main_context (progress, &context);
  res = ostree_repo_pull_with_options (repo, url, options,
                                       context.ostree_progress, cancellable, error);
  if (!res)
    translate_ostree_repo_pull_errors (error);

  return res;
}

gboolean
flatpak_dir_pull_untrusted_local (FlatpakDir          *self,
                                  const char          *src_path,
                                  const char          *remote_name,
                                  const char          *ref,
                                  const char         **subpaths,
                                  FlatpakProgress     *progress,
                                  GCancellable        *cancellable,
                                  GError             **error)
{
  g_autoptr(GFile) path_file = g_file_new_for_path (src_path);
  g_autofree char *url = g_file_get_uri (path_file);
  g_autofree char *checksum = NULL;
  g_autofree char *current_checksum = NULL;
  gboolean gpg_verify_summary;
  gboolean gpg_verify;
  g_autoptr(OstreeGpgVerifyResult) gpg_result = NULL;
  g_autoptr(GVariant) old_commit = NULL;
  g_autoptr(OstreeRepo) src_repo = NULL;
  g_autoptr(GVariant) new_commit = NULL;
  g_autoptr(GVariant) new_commit_metadata = NULL;
  g_autoptr(GVariant) extra_data_sources = NULL;
  g_autoptr(GPtrArray) subdirs_arg = NULL;
  g_auto(GLnxLockFile) lock = { 0, };
  gboolean ret = FALSE;
  g_autofree const char **ref_bindings = NULL;

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

  if (!ostree_repo_remote_get_gpg_verify (self->repo, remote_name,
                                          &gpg_verify, error))
    return FALSE;

  /* This was verified in the client, but lets do it here too */
  if (!gpg_verify_summary || !gpg_verify)
    return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("Can't pull from untrusted non-gpg verified remote"));

  if (!flatpak_repo_resolve_rev (self->repo, NULL, remote_name, ref, TRUE,
                                 &current_checksum, NULL, error))
    return FALSE;

  if (current_checksum != NULL &&
      !ostree_repo_load_commit (self->repo, current_checksum, &old_commit, NULL, error))
    return FALSE;

  src_repo = ostree_repo_new (path_file);
  if (!ostree_repo_open (src_repo, cancellable, error))
    return FALSE;

  if (!flatpak_repo_resolve_rev (src_repo, NULL, remote_name, ref, FALSE, &checksum, NULL, error))
    return FALSE;

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

  /* Here we check that there is actually a ref binding, otherwise we
     could allow installing a ref as another app, because both would
     pass gpg validation. Note that ostree pull actually also verifies
     the ref-bindings, but only if they exist. We could do only the
     ref-binding existence check, but if we got something weird might as
     well stop handling it early. */

  new_commit_metadata = g_variant_get_child_value (new_commit, 0);
  if (!g_variant_lookup (new_commit_metadata, OSTREE_COMMIT_META_KEY_REF_BINDING, "^a&s", &ref_bindings))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Commit for ‘%s’ has no ref binding"),  ref);

  if (!g_strv_contains ((const char *const *) ref_bindings, ref))
    {
      g_autofree char *as_string = g_strjoinv (", ", (char **)ref_bindings);
      return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Commit for ‘%s’ is not in expected bound refs: %s"),  ref, as_string);
    }

  if (old_commit)
    {
      guint64 old_timestamp;
      guint64 new_timestamp;

      old_timestamp = ostree_commit_get_timestamp (old_commit);
      new_timestamp = ostree_commit_get_timestamp (new_commit);

      if (new_timestamp < old_timestamp)
        return flatpak_fail_error (error, FLATPAK_ERROR_DOWNGRADE, "Not allowed to downgrade %s (old_commit: %s/%" G_GINT64_FORMAT " new_commit: %s/%" G_GINT64_FORMAT ")",
                                   ref, current_checksum, old_timestamp, checksum, new_timestamp);
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

  return ret;
}

FlatpakDecomposed *
flatpak_dir_current_ref (FlatpakDir   *self,
                         const char   *name,
                         GCancellable *cancellable)
{
  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) dir = NULL;
  g_autoptr(GFile) current_link = NULL;
  g_autoptr(GFileInfo) file_info = NULL;
  FlatpakDecomposed *decomposed;
  char *ref;

  base = g_file_get_child (flatpak_dir_get_path (self), "app");
  dir = g_file_get_child (base, name);

  current_link = g_file_get_child (dir, "current");

  file_info = g_file_query_info (current_link, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, NULL);
  if (file_info == NULL)
    return NULL;

  ref = g_strconcat ("app/", name, "/", g_file_info_get_symlink_target (file_info), NULL);
  decomposed = flatpak_decomposed_new_from_ref_take (ref, NULL);
  if (decomposed == NULL)
    g_free (ref);

  return decomposed;
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
  g_autoptr(GPtrArray) refs = NULL;
  g_autoptr(FlatpakDecomposed) current_ref = NULL;
  FlatpakDecomposed *other_ref = NULL;

  current_ref = flatpak_dir_current_ref (self, name, cancellable);
  if (current_ref)
    {
      refs = flatpak_dir_list_refs_for_name (self, FLATPAK_KINDS_APP, name, cancellable, NULL);
      if (refs)
        {
          for (int i = 0; i < refs->len; i++)
            {
              FlatpakDecomposed *ref = g_ptr_array_index (refs, i);
              if (!flatpak_decomposed_equal (ref, current_ref))
                {
                  other_ref = ref;
                  break;
                }
            }
        }
    }

  base = g_file_get_child (flatpak_dir_get_path (self), "app");
  dir = g_file_get_child (base, name);

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
flatpak_dir_make_current_ref (FlatpakDir        *self,
                              FlatpakDecomposed *ref,
                              GCancellable      *cancellable,
                              GError           **error)
{
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) dir = NULL;
  g_autoptr(GFile) current_link = NULL;
  g_autofree char *id = NULL;
  const char *rest;

  if (!flatpak_decomposed_is_app (ref))
    return flatpak_fail (error, _("Only applications can be made current"));

  base = g_file_get_child (flatpak_dir_get_path (self), flatpak_decomposed_get_kind_str (ref));

  id = flatpak_decomposed_dup_id (ref);
  dir = g_file_get_child (base, id);

  current_link = g_file_get_child (dir, "current");

  if (!g_file_delete (current_link, cancellable, &local_error) &&
      !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  rest = flatpak_decomposed_peek_arch (ref, NULL);
  if (!g_file_make_symbolic_link (current_link, rest, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
_flatpak_dir_list_refs_for_name (FlatpakDir   *self,
                                 GFile        *base_dir,
                                 FlatpakKinds kind,
                                 const char   *name,
                                 GPtrArray    *refs,
                                 GCancellable *cancellable,
                                 GError      **error)
{
  g_autoptr(GFile) dir = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  GError *temp_error = NULL;

  g_assert (kind == FLATPAK_KINDS_RUNTIME || kind == FLATPAK_KINDS_APP);

  dir = g_file_get_child (base_dir, name);

  if (!g_file_query_exists (dir, cancellable))
    return TRUE;

  dir_enum = g_file_enumerate_children (dir, OSTREE_GIO_FAST_QUERYINFO,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, error);
  if (!dir_enum)
    return FALSE;

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
        return FALSE;

      while ((child_info2 = g_file_enumerator_next_file (dir_enum2, cancellable, &temp_error)))
        {
          const char *branch = g_file_info_get_name (child_info2);

          if (g_file_info_get_file_type (child_info2) == G_FILE_TYPE_DIRECTORY)
            {
              g_autoptr(GFile) deploy = flatpak_build_file (child, branch, "active/deploy", NULL);

              if (g_file_query_exists (deploy, NULL))
                {
                  FlatpakDecomposed *ref = flatpak_decomposed_new_from_parts (kind, name, arch, branch, NULL);
                  if (ref)
                    g_ptr_array_add (refs, ref);
                }
            }

          g_clear_object (&child_info2);
        }

      if (temp_error != NULL)
        {
          g_propagate_error (error, temp_error);
          return FALSE;
        }

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      return FALSE;
    }

  return TRUE;
}

GPtrArray *
flatpak_dir_list_refs_for_name (FlatpakDir   *self,
                                FlatpakKinds kinds,
                                const char   *name,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_autoptr(GPtrArray) refs = NULL;

  refs = g_ptr_array_new_with_free_func ((GDestroyNotify)flatpak_decomposed_unref);

  if ((kinds & FLATPAK_KINDS_APP) != 0)
    {
      g_autoptr(GFile) base = g_file_get_child (flatpak_dir_get_path (self), "app");

      if (!_flatpak_dir_list_refs_for_name (self, base, FLATPAK_KINDS_APP, name, refs, cancellable, error))
        return NULL;
    }

  if ((kinds & FLATPAK_KINDS_RUNTIME) != 0)
    {
      g_autoptr(GFile) base = g_file_get_child (flatpak_dir_get_path (self), "runtime");

      if (!_flatpak_dir_list_refs_for_name (self, base, FLATPAK_KINDS_RUNTIME, name, refs, cancellable, error))
        return NULL;
    }

  g_ptr_array_sort (refs, (GCompareFunc)flatpak_decomposed_strcmp_p);

  return g_steal_pointer (&refs);
}

GPtrArray *
flatpak_dir_list_refs (FlatpakDir   *self,
                       FlatpakKinds kinds,
                       GCancellable *cancellable,
                       GError      **error)
{
  g_autoptr(GPtrArray) refs = NULL;

  refs = g_ptr_array_new_with_free_func ((GDestroyNotify)flatpak_decomposed_unref);

  if (kinds & FLATPAK_KINDS_APP)
    {
      g_autoptr(GFile) base = NULL;
      g_autoptr(GFileEnumerator) dir_enum = NULL;
      g_autoptr(GFileInfo) child_info = NULL;
      GError *temp_error = NULL;

      base = g_file_get_child (flatpak_dir_get_path (self), "app");

      if (g_file_query_exists (base, cancellable))
        {
          dir_enum = g_file_enumerate_children (base, OSTREE_GIO_FAST_QUERYINFO,
                                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                cancellable, error);
          if (!dir_enum)
            return NULL;

          while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)))
            {
              const char *name = g_file_info_get_name (child_info);

              if (g_file_info_get_file_type (child_info) != G_FILE_TYPE_DIRECTORY)
                {
                  g_clear_object (&child_info);
                  continue;
                }

              if (!_flatpak_dir_list_refs_for_name (self, base, FLATPAK_KINDS_APP, name, refs, cancellable, error))
                return NULL;

              g_clear_object (&child_info);
            }

          if (temp_error != NULL)
            {
              g_propagate_error (error, temp_error);
              return NULL;
            }
        }
    }

  if (kinds & FLATPAK_KINDS_RUNTIME)
    {
      g_autoptr(GFile) base = NULL;
      g_autoptr(GFileEnumerator) dir_enum = NULL;
      g_autoptr(GFileInfo) child_info = NULL;
      GError *temp_error = NULL;

      base = g_file_get_child (flatpak_dir_get_path (self), "runtime");

      if (g_file_query_exists (base, cancellable))
        {
          dir_enum = g_file_enumerate_children (base, OSTREE_GIO_FAST_QUERYINFO,
                                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                cancellable, error);
          if (!dir_enum)
            return NULL;

          while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)))
            {
              const char *name = g_file_info_get_name (child_info);

              if (g_file_info_get_file_type (child_info) != G_FILE_TYPE_DIRECTORY)
                {
                  g_clear_object (&child_info);
                  continue;
                }

              if (!_flatpak_dir_list_refs_for_name (self, base, FLATPAK_KINDS_RUNTIME, name, refs, cancellable, error))
                return NULL;

              g_clear_object (&child_info);
            }

          if (temp_error != NULL)
            {
              g_propagate_error (error, temp_error);
              return NULL;
            }
        }
    }

  g_ptr_array_sort (refs, (GCompareFunc)flatpak_decomposed_strcmp_p);

  return g_steal_pointer (&refs);
}

gboolean
flatpak_dir_is_runtime_extension (FlatpakDir        *self,
                                  FlatpakDecomposed *ref)
{
  g_autoptr(GBytes) ext_deploy_data = NULL;

  if (!flatpak_decomposed_is_runtime (ref))
    return FALSE;

  /* deploy v4 guarantees extension-of info */
  ext_deploy_data = flatpak_dir_get_deploy_data (self, ref, 4, NULL, NULL);
  if (ext_deploy_data && flatpak_deploy_data_get_extension_of (ext_deploy_data) != NULL)
    return TRUE;

  return FALSE;
}

static GHashTable *
flatpak_dir_get_runtime_app_map (FlatpakDir        *self,
                                 GCancellable      *cancellable,
                                 GError           **error)
{
  g_autoptr(GHashTable) runtime_app_map = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash,
                                                                 (GEqualFunc)flatpak_decomposed_equal,
                                                                 (GDestroyNotify)flatpak_decomposed_unref,
                                                                 (GDestroyNotify)g_ptr_array_unref);
  g_autoptr(GPtrArray) app_refs = NULL;

  app_refs = flatpak_dir_list_refs (self, FLATPAK_KINDS_APP, cancellable, error);
  if (app_refs == NULL)
    return NULL;

  for (guint i = 0; i < app_refs->len; i++)
    {
      FlatpakDecomposed *app_ref = g_ptr_array_index (app_refs, i);
      /* deploy v4 guarantees runtime info */
      g_autoptr(GBytes) app_deploy_data = flatpak_dir_get_deploy_data (self, app_ref, 4, NULL, NULL);
      g_autoptr(FlatpakDecomposed) runtime_decomposed = NULL;
      g_autoptr(GPtrArray) runtime_apps = NULL;
      const char *runtime_pref;

      if (app_deploy_data == NULL)
        continue;

      runtime_pref = flatpak_deploy_data_get_runtime (app_deploy_data);
      runtime_decomposed = flatpak_decomposed_new_from_pref (FLATPAK_KINDS_RUNTIME, runtime_pref, error);
      if (runtime_decomposed == NULL)
        return NULL;

      runtime_apps = g_hash_table_lookup (runtime_app_map, runtime_decomposed);
      if (runtime_apps == NULL)
        {
          runtime_apps = g_ptr_array_new_with_free_func ((GDestroyNotify)flatpak_decomposed_unref);
          g_hash_table_insert (runtime_app_map, flatpak_decomposed_ref (runtime_decomposed), g_ptr_array_ref (runtime_apps));
        }
      else
        g_ptr_array_ref (runtime_apps);

      g_ptr_array_add (runtime_apps, flatpak_decomposed_ref (app_ref));
    }

  return g_steal_pointer (&runtime_app_map);
}

GPtrArray *
flatpak_dir_list_app_refs_with_runtime (FlatpakDir         *self,
                                        GHashTable        **runtime_app_map,
                                        FlatpakDecomposed  *runtime_ref,
                                        GCancellable       *cancellable,
                                        GError            **error)
{
  GPtrArray *apps;

  g_assert (runtime_app_map != NULL);

  if (*runtime_app_map == NULL)
    *runtime_app_map = flatpak_dir_get_runtime_app_map (self, cancellable, error);

  if (*runtime_app_map == NULL)
    return NULL;

  apps = g_hash_table_lookup (*runtime_app_map, runtime_ref);
  if (apps == NULL) /* unused runtime */
    return g_ptr_array_new_with_free_func ((GDestroyNotify)flatpak_decomposed_unref);

  return g_ptr_array_ref (apps);
}

static GHashTable *
flatpak_dir_get_extension_app_map (FlatpakDir    *self,
                                   GHashTable    *runtime_app_map,
                                   GCancellable  *cancellable,
                                   GError       **error)
{
  g_autoptr(GHashTable) extension_app_map = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash,
                                                                   (GEqualFunc)flatpak_decomposed_equal,
                                                                   (GDestroyNotify)flatpak_decomposed_unref,
                                                                   (GDestroyNotify)g_ptr_array_unref);
  g_autoptr(GPtrArray) all_refs = NULL;

  g_assert (runtime_app_map != NULL);

  all_refs = flatpak_dir_list_refs (self, FLATPAK_KINDS_RUNTIME | FLATPAK_KINDS_APP, NULL, NULL);
  for (guint i = 0; all_refs != NULL && i < all_refs->len; i++)
    {
      FlatpakDecomposed *ref = g_ptr_array_index (all_refs, i);
      g_autoptr(GPtrArray) related = NULL;
      GPtrArray *runtime_apps = NULL;

      if (flatpak_decomposed_id_is_subref (ref))
        continue;

      if (flatpak_decomposed_is_runtime (ref))
        {
          runtime_apps = g_hash_table_lookup (runtime_app_map, ref);
          if (runtime_apps == NULL)
            continue;
        }

      related = flatpak_dir_find_local_related (self, ref, NULL, TRUE, cancellable, error);
      if (related == NULL)
        return NULL;

      for (guint j = 0; j < related->len; j++)
        {
          FlatpakRelated *rel = g_ptr_array_index (related, j);
          g_autoptr(GPtrArray) extension_apps = g_hash_table_lookup (extension_app_map, rel->ref);
          if (extension_apps == NULL)
            {
              extension_apps = g_ptr_array_new_with_free_func ((GDestroyNotify)flatpak_decomposed_unref);
              g_hash_table_insert (extension_app_map, flatpak_decomposed_ref (rel->ref), g_ptr_array_ref (extension_apps));
            }
          else
            g_ptr_array_ref (extension_apps);

          if (flatpak_decomposed_is_runtime (ref))
            {
              g_assert (runtime_apps);
              for (guint k = 0; runtime_apps && k < runtime_apps->len; k++)
                g_ptr_array_add (extension_apps, flatpak_decomposed_ref (g_ptr_array_index (runtime_apps, k)));
            }
          else
            g_ptr_array_add (extension_apps, flatpak_decomposed_ref (ref));
        }
    }

  return g_steal_pointer (&extension_app_map);
}

GPtrArray *
flatpak_dir_list_app_refs_with_runtime_extension (FlatpakDir        *self,
                                                  GHashTable        **runtime_app_map,
                                                  GHashTable        **extension_app_map,
                                                  FlatpakDecomposed  *runtime_ext_ref,
                                                  GCancellable       *cancellable,
                                                  GError            **error)
{
  GPtrArray *apps;

  g_assert (runtime_app_map != NULL);
  g_assert (extension_app_map != NULL);

  if (*runtime_app_map == NULL)
    *runtime_app_map = flatpak_dir_get_runtime_app_map (self, cancellable, error);

  if (*runtime_app_map == NULL)
    return NULL;

  if (*extension_app_map == NULL)
    *extension_app_map = flatpak_dir_get_extension_app_map (self, *runtime_app_map, cancellable, error);

  if (*extension_app_map == NULL)
    return NULL;

  apps = g_hash_table_lookup (*extension_app_map, runtime_ext_ref);
  if (apps == NULL) /* unused extension */
    return g_ptr_array_new_with_free_func ((GDestroyNotify)flatpak_decomposed_unref);

  return g_ptr_array_ref (apps);
}

GVariant *
flatpak_dir_read_latest_commit (FlatpakDir        *self,
                                const char        *remote,
                                FlatpakDecomposed *ref,
                                char             **out_checksum,
                                GCancellable      *cancellable,
                                GError           **error)
{
  g_autofree char *res = NULL;
  g_autoptr(GVariant) commit_data = NULL;

  if (!flatpak_repo_resolve_rev (self->repo, NULL, remote, flatpak_decomposed_get_ref (ref), FALSE,
                                 &res, cancellable, error))
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
  g_autofree char *alt_id = NULL;
  g_autofree char *res = NULL;

  if (!flatpak_repo_resolve_rev (self->repo, NULL, remote, ref, FALSE,
                                 &res, cancellable, error))
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
flatpak_dir_read_active (FlatpakDir        *self,
                         FlatpakDecomposed *ref,
                         GCancellable      *cancellable)
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
flatpak_dir_set_active (FlatpakDir        *self,
                        FlatpakDecomposed *ref,
                        const char        *active_id,
                        GCancellable      *cancellable,
                        GError           **error)
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

  g_info ("running triggers from %s", triggerspath);

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
          /* We need to canonicalize the basedir, because if has a symlink
             somewhere the bind mount will be on the target of that, not
             at that exact path. */
          g_autofree char *basedir_orig = g_file_get_path (self->basedir);
          g_autofree char *basedir = realpath (basedir_orig, NULL);
          g_autoptr(FlatpakBwrap) bwrap = NULL;
          g_autofree char *commandline = NULL;

          g_info ("running trigger %s", name);

          bwrap = flatpak_bwrap_new (NULL);

#ifndef DISABLE_SANDBOXED_TRIGGERS
          flatpak_bwrap_add_arg (bwrap, flatpak_get_bwrap ());
          flatpak_bwrap_add_args (bwrap,
                                  "--unshare-ipc",
                                  "--unshare-net",
                                  "--unshare-pid",
                                  "--ro-bind", "/", "/",
                                  "--proc", "/proc",
                                  "--dev", "/dev",
                                  "--bind", basedir, basedir,
                                  NULL);
#endif
          flatpak_bwrap_add_args (bwrap,
                                  flatpak_file_get_path_cached (child),
                                  basedir,
                                  NULL);
          flatpak_bwrap_finish (bwrap);

          commandline = flatpak_quote_argv ((const char **) bwrap->argv->pdata, -1);
          g_info ("Running '%s'", commandline);

          /* We use LEAVE_DESCRIPTORS_OPEN to work around dead-lock, see flatpak_close_fds_workaround */
          if (!g_spawn_sync ("/",
                             (char **) bwrap->argv->pdata,
                             NULL,
                             G_SPAWN_SEARCH_PATH | G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                             flatpak_bwrap_child_setup_cb, bwrap->fds,
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

          if (mime_node->type != XML_ELEMENT_NODE)
            continue;

          if (strcmp ((char *) mime_node->name, "mime-type") != 0)
            return FALSE;

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
export_desktop_file (const char         *app,
                     const char         *branch,
                     const char         *arch,
                     GKeyFile           *metadata,
                     const char * const *previous_ids,
                     int                 parent_fd,
                     const char         *name,
                     struct stat        *stat_buf,
                     char              **target,
                     GCancellable       *cancellable,
                     GError            **error)
{
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
  g_autofree char *escaped_app = maybe_quote (app);
  g_autofree char *escaped_branch = maybe_quote (branch);
  g_autofree char *escaped_arch = maybe_quote (arch);
  int i;
  const char *flatpak;

  if (!flatpak_openat_noatime (parent_fd, name, &desktop_fd, cancellable, error))
    return FALSE;

  if (!read_fd (desktop_fd, stat_buf, &data, &data_len, error))
    return FALSE;

  keyfile = g_key_file_new ();
  if (!g_key_file_load_from_data (keyfile, data, data_len, G_KEY_FILE_KEEP_TRANSLATIONS, error))
    return FALSE;

  if (g_str_has_suffix (name, ".service"))
    {
      g_autofree gchar *dbus_name = NULL;
      g_autofree gchar *expected_dbus_name = g_strndup (name, strlen (name) - strlen (".service"));

      dbus_name = g_key_file_get_string (keyfile, "D-BUS Service", "Name", NULL);

      if (dbus_name == NULL || strcmp (dbus_name, expected_dbus_name) != 0)
        {
          return flatpak_fail_error (error, FLATPAK_ERROR_EXPORT_FAILED,
                                     _("D-Bus service file '%s' has wrong name"), name);
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

      /* If the app has been renamed, add its old .desktop filename to
       * X-Flatpak-RenamedFrom in the new .desktop file, taking care not to
       * introduce duplicates.
       */
      if (previous_ids != NULL)
        {
          const char *X_FLATPAK_RENAMED_FROM = "X-Flatpak-RenamedFrom";
          g_auto(GStrv) renamed_from = g_key_file_get_string_list (keyfile,
                                                                   G_KEY_FILE_DESKTOP_GROUP,
                                                                   X_FLATPAK_RENAMED_FROM,
                                                                   NULL, NULL);
          g_autoptr(GPtrArray) merged = g_ptr_array_new_with_free_func (g_free);
          g_autoptr(GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
          const char *new_suffix;

          for (i = 0; renamed_from != NULL && renamed_from[i] != NULL; i++)
            {
              if (!g_hash_table_contains (seen, renamed_from[i]))
                {
                  gchar *copy = g_strdup (renamed_from[i]);
                  g_hash_table_insert (seen, copy, copy);
                  g_ptr_array_add (merged, g_steal_pointer (&copy));
                }
            }

          /* If an app was renamed from com.example.Foo to net.example.Bar, and
           * the new version exports net.example.Bar-suffix.desktop, we assume the
           * old version exported com.example.Foo-suffix.desktop.
           *
           * This assertion is true because
           * flatpak_name_matches_one_wildcard_prefix() is called on all
           * exported files before we get here.
           */
          g_assert (g_str_has_prefix (name, app));
          /* ".desktop" for the "main" desktop file; something like
           * "-suffix.desktop" for extra ones.
           */
          new_suffix = name + strlen (app);

          for (i = 0; previous_ids[i] != NULL; i++)
            {
              g_autofree gchar *previous_desktop = g_strconcat (previous_ids[i], new_suffix, NULL);
              if (!g_hash_table_contains (seen, previous_desktop))
                {
                  g_hash_table_insert (seen, previous_desktop, previous_desktop);
                  g_ptr_array_add (merged, g_steal_pointer (&previous_desktop));
                }
            }

          if (merged->len > 0)
            {
              g_ptr_array_add (merged, NULL);
              g_key_file_set_string_list (keyfile,
                                          G_KEY_FILE_DESKTOP_GROUP,
                                          X_FLATPAK_RENAMED_FROM,
                                          (const char * const *) merged->pdata,
                                          merged->len - 1);
            }
        }
    }

  groups = g_key_file_get_groups (keyfile, NULL);

  for (i = 0; groups[i] != NULL; i++)
    {
      g_autoptr(GString) new_exec = NULL;
      g_auto(GStrv) flatpak_run_opts = g_key_file_get_string_list (keyfile, groups[i], "X-Flatpak-RunOptions", NULL, NULL);
      g_autofree char *flatpak_run_args = format_flatpak_run_args_from_run_opts (flatpak_run_opts);

      g_key_file_remove_key (keyfile, groups[i], "X-Flatpak-RunOptions", NULL);
      g_key_file_remove_key (keyfile, groups[i], "TryExec", NULL);

      /* Remove this to make sure nothing tries to execute it outside the sandbox*/
      g_key_file_remove_key (keyfile, groups[i], "X-GNOME-Bugzilla-ExtraInfoScript", NULL);

      new_exec = g_string_new ("");
      if ((flatpak = g_getenv ("FLATPAK_BINARY")) == NULL)
        flatpak = FLATPAK_BINDIR "/flatpak";

      g_string_append_printf (new_exec,
                              "%s run --branch=%s --arch=%s",
                              flatpak,
                              escaped_branch,
                              escaped_arch);

      if (flatpak_run_args != NULL)
        g_string_append_printf (new_exec, "%s", flatpak_run_args);

      old_exec = g_key_file_get_string (keyfile, groups[i], "Exec", NULL);
      if (old_exec && g_shell_parse_argv (old_exec, &old_argc, &old_argv, NULL) && old_argc >= 1)
        {
          int j;
          g_autofree char *command = maybe_quote (old_argv[0]);

          g_string_append_printf (new_exec, " --command=%s", command);

          for (j = 1; j < old_argc; j++)
            {
              if (strcasecmp (old_argv[j], "%f") == 0 ||
                  strcasecmp (old_argv[j], "%u") == 0)
                {
                  g_string_append (new_exec, " --file-forwarding");
                  break;
                }
            }

          g_string_append (new_exec, " ");
          g_string_append (new_exec, escaped_app);

          for (j = 1; j < old_argc; j++)
            {
              g_autofree char *arg = maybe_quote (old_argv[j]);

              if (strcasecmp (arg, "%f") == 0)
                g_string_append_printf (new_exec, " @@ %s @@", arg);
              else if (strcasecmp (arg, "%u") == 0)
                g_string_append_printf (new_exec, " @@u %s @@", arg);
              else if (g_str_has_prefix (arg, "@@"))
                {
                  flatpak_fail_error (error, FLATPAK_ERROR_EXPORT_FAILED,
                                     _("Invalid Exec argument %s"), arg);
                  return FALSE;
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
    return FALSE;

  if (!flatpak_open_in_tmpdir_at (parent_fd, 0755, tmpfile_name, &out_stream, cancellable, error))
    return FALSE;

  if (!g_output_stream_write_all (out_stream, new_data, new_data_len, NULL, cancellable, error))
    return FALSE;

  if (!g_output_stream_close (out_stream, cancellable, error))
    return FALSE;

  if (target)
    *target = g_steal_pointer (&tmpfile_name);

  return TRUE;
}

static gboolean
rewrite_export_dir (const char         *app,
                    const char         *branch,
                    const char         *arch,
                    GKeyFile           *metadata,
                    const char * const *previous_ids,
                    FlatpakContext     *context,
                    int                 source_parent_fd,
                    const char         *source_name,
                    const char         *source_path,
                    GCancellable       *cancellable,
                    GError            **error)
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

          if (!rewrite_export_dir (app, branch, arch, metadata, previous_ids, context,
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
              if (!export_desktop_file (app, branch, arch, metadata, previous_ids,
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
flatpak_rewrite_export_dir (const char         *app,
                            const char         *branch,
                            const char         *arch,
                            GKeyFile           *metadata,
                            const char * const *previous_ids,
                            GFile              *source,
                            GCancellable       *cancellable,
                            GError            **error)
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
  if (!rewrite_export_dir (app, branch, arch, metadata, previous_ids, context,
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
          g_autofree char *symlink_name = g_strdup (".export-symlink-XXXXXX");
          g_autofree gchar *target = NULL;

          target = g_build_filename (source_symlink_prefix, dent->d_name, NULL);

          for (int count = 0; count < 100; count++)
            {
              glnx_gen_temp_name (symlink_name);

              if (symlinkat (target, destination_dfd, symlink_name) != 0)
                {
                  if (errno == EEXIST)
                    continue;

                  glnx_set_error_from_errno (error);
                  goto out;
                }

              if (renameat (destination_dfd, symlink_name, destination_dfd, dent->d_name) != 0)
                {
                  glnx_set_error_from_errno (error);
                  goto out;
                }

              break;
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
    "share/metainfo",                      "../..",
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
  g_autoptr(FlatpakDecomposed) current_ref = NULL;
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

  g_info ("extracting extra data to %s", flatpak_file_get_path_cached (extradir));

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
        return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA,
                                   _("Extra data %s missing in detached metadata"),
                                   extra_data_source_name);
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
  g_autofree char *runtime_pref = NULL;
  g_autoptr(FlatpakDecomposed) runtime_ref = NULL;
  g_autoptr(FlatpakDeploy) runtime_deploy = NULL;
  g_autoptr(FlatpakBwrap) bwrap = NULL;
  g_autoptr(GFile) app_files = NULL;
  g_autoptr(GFile) apply_extra_file = NULL;
  g_autoptr(GFile) app_export_file = NULL;
  g_autoptr(GFile) extra_export_file = NULL;
  g_autoptr(GFile) extra_files = NULL;
  g_autoptr(GFile) runtime_files = NULL;
  g_autoptr(FlatpakContext) app_context = NULL;
  g_auto(GStrv) minimal_envp = NULL;
  g_autofree char *runtime_arch = NULL;
  int exit_status;
  const char *group = FLATPAK_METADATA_GROUP_APPLICATION;
  g_autoptr(GError) local_error = NULL;
  FlatpakRunFlags run_flags;

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

  runtime_pref = g_key_file_get_string (metakey, group,
                                        FLATPAK_METADATA_KEY_RUNTIME, error);
  if (runtime_pref == NULL)
    runtime_pref = g_key_file_get_string (metakey, FLATPAK_METADATA_GROUP_EXTENSION_OF,
                                          FLATPAK_METADATA_KEY_RUNTIME, NULL);
  if (runtime_pref == NULL)
    return FALSE;

  runtime_ref = flatpak_decomposed_new_from_pref (FLATPAK_KINDS_RUNTIME, runtime_pref, error);
  if (runtime_ref == NULL)
    return FALSE;
  runtime_arch = flatpak_decomposed_dup_arch (runtime_ref);

  if (!g_key_file_get_boolean (metakey, FLATPAK_METADATA_GROUP_EXTRA_DATA,
                               FLATPAK_METADATA_KEY_NO_RUNTIME, NULL))
    {
      /* We pass in self here so that we ensure that we find the runtime in case it only
         exists in this installation (which might be custom) */
      runtime_deploy = flatpak_find_deploy_for_ref (flatpak_decomposed_get_ref (runtime_ref), NULL, self, cancellable, error);
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

  /* Might need multiarch in apply_extra (see e.g. #3742).
   * Should be pretty safe in this limited context */
  run_flags = (FLATPAK_RUN_FLAG_MULTIARCH |
               FLATPAK_RUN_FLAG_NO_SESSION_HELPER |
               FLATPAK_RUN_FLAG_NO_PROC |
               FLATPAK_RUN_FLAG_NO_SESSION_BUS_PROXY |
               FLATPAK_RUN_FLAG_NO_SYSTEM_BUS_PROXY |
               FLATPAK_RUN_FLAG_NO_A11Y_BUS_PROXY);

  if (!flatpak_run_setup_base_argv (bwrap, runtime_files, NULL, runtime_arch,
                                    run_flags, error))
    return FALSE;

  app_context = flatpak_context_new ();

  if (!flatpak_run_add_environment_args (bwrap, NULL, run_flags, id,
                                         app_context, NULL, NULL, -1,
                                         NULL, cancellable, error))
    return FALSE;

  flatpak_bwrap_populate_runtime_dir (bwrap, NULL);

  flatpak_bwrap_envp_to_args (bwrap);

  flatpak_bwrap_add_arg (bwrap, "/app/bin/apply_extra");

  flatpak_bwrap_finish (bwrap);

  g_info ("Running /app/bin/apply_extra ");

  /* We run the sandbox without caps, but it can still create files owned by itself with
   * arbitrary permissions, including setuid myself. This is extra risky in the case where
   * this runs as root in the system helper case. We canonicalize the permissions at the
   * end, but to avoid non-canonical permissions leaking out before then we make the
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
                                         getuid () == 0 ? 0 : -1,
                                         getuid () == 0 ? 0 : -1,
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

/* Check the user’s parental controls allow installation of @ref by looking at
 * its cached @deploy_data, which contains its content rating as extracted from
 * its AppData when it was originally downloaded. That’s compared to the
 * parental controls policy loaded from the #MctManager.
 *
 * If @ref should not be installed, an error is returned. */
static gboolean
flatpak_dir_check_parental_controls (FlatpakDir    *self,
                                     const char    *ref,
                                     GBytes        *deploy_data,
                                     GCancellable  *cancellable,
                                     GError       **error)
{
#ifdef HAVE_LIBMALCONTENT
#ifdef USE_SYSTEM_HELPER
  g_autoptr(GError) local_error = NULL;
  const char *on_session = g_getenv ("FLATPAK_SYSTEM_HELPER_ON_SESSION");
  g_autoptr(GDBusConnection) dbus_connection = NULL;
  g_autoptr(MctManager) manager = NULL;
  g_autoptr(MctAppFilter) app_filter = NULL;
  const char *content_rating_type;
  g_autoptr(GHashTable) content_rating = NULL;
  g_autoptr(AutoPolkitAuthority) authority = NULL;
  g_autoptr(AutoPolkitSubject) subject = NULL;
  gint subject_uid;
  g_autoptr(AutoPolkitAuthorizationResult) result = NULL;
  gboolean authorized;
  gboolean repo_installation_allowed, app_is_appropriate;
  PolkitCheckAuthorizationFlags polkit_flags;
  MctGetAppFilterFlags manager_flags;

  /* Assume that root is allowed to install any ref and shouldn't have any
   * parental controls restrictions applied to them. Note that this branch
   * must not be taken if this code is running within the system-helper, as that
   * runs as root but on behalf of another process. If running within the
   * system-helper, self->source_pid is non-zero. */
  if (self->source_pid == 0 && getuid () == 0)
    {
      g_info ("Skipping parental controls check for %s due to running as root", ref);
      return TRUE;
    }

  /* The ostree-metadata and appstream/ branches should not have any parental
   * controls restrictions. Similarly, for the moment, there is no point in
   * restricting runtimes. */
  if (!g_str_has_prefix (ref, "app/"))
    return TRUE;

  g_info ("Getting parental controls details for %s from %s",
           ref, flatpak_deploy_data_get_origin (deploy_data));

  if (on_session != NULL)
    {
      /* FIXME: Instead of skipping the parental controls check in the test
       * environment, make a mock service for it.
       * https://github.com/flatpak/flatpak/issues/2993 */
      g_info ("Skipping parental controls check for %s since the "
              "system bus is unavailable in the test environment", ref);
      return TRUE;
    }

  dbus_connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, cancellable, &local_error);
  if (dbus_connection == NULL)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  if (self->user || self->source_pid == 0)
    subject = polkit_unix_process_new_for_owner (getpid (), 0, getuid ());
  else
    subject = polkit_unix_process_new_for_owner (self->source_pid, 0, -1);

  /* Get the parental controls for the invoking user. */
  subject_uid = polkit_unix_process_get_uid (POLKIT_UNIX_PROCESS (subject));
  if (subject_uid == -1)
    {
      g_set_error_literal (error, G_DBUS_ERROR, G_DBUS_ERROR_AUTH_FAILED,
                           "Failed to get subject UID");
      return FALSE;
    }

  manager = mct_manager_new (dbus_connection);
  manager_flags = MCT_GET_APP_FILTER_FLAGS_NONE;
  if (!flatpak_dir_get_no_interaction (self))
    manager_flags |= MCT_GET_APP_FILTER_FLAGS_INTERACTIVE;
  app_filter = mct_manager_get_app_filter (manager, subject_uid,
                                           manager_flags,
                                           cancellable, &local_error);
  if (g_error_matches (local_error, MCT_APP_FILTER_ERROR, MCT_APP_FILTER_ERROR_DISABLED))
    {
      g_info ("Skipping parental controls check for %s since parental "
              "controls are disabled globally", ref);
      return TRUE;
    }
  else if (g_error_matches (local_error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
           g_error_matches (local_error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER))
    {
      g_info ("Skipping parental controls check for %s since a required "
              "service was not found", ref);
      return TRUE;
    }
  else if (local_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  /* Check the content rating against the parental controls. If the app is
   * allowed to be installed, return so immediately. */
  repo_installation_allowed = ((self->user && mct_app_filter_is_user_installation_allowed (app_filter)) ||
                               (!self->user && mct_app_filter_is_system_installation_allowed (app_filter)));

  content_rating_type = flatpak_deploy_data_get_appdata_content_rating_type (deploy_data);
  content_rating = flatpak_deploy_data_get_appdata_content_rating (deploy_data);
  app_is_appropriate = flatpak_oars_check_rating (content_rating, content_rating_type,
                                                  app_filter);

  if (repo_installation_allowed && app_is_appropriate)
    {
      g_info ("Parental controls policy satisfied for %s", ref);
      return TRUE;
    }

  /* Otherwise, check polkit to see if the admin is going to allow the user to
   * override their parental controls policy. We can’t pass any details to this
   * polkit check, since it could be run by the user or by the system helper,
   * and non-root users can’t pass details to polkit checks. */
  authority = polkit_authority_get_sync (NULL, error);
  if (authority == NULL)
    return FALSE;

  polkit_flags = POLKIT_CHECK_AUTHORIZATION_FLAGS_NONE;
  if (!flatpak_dir_get_no_interaction (self))
    polkit_flags |= POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION;
  result = polkit_authority_check_authorization_sync (authority, subject,
                                                      "org.freedesktop.Flatpak.override-parental-controls",
                                                      NULL,
                                                      polkit_flags,
                                                      cancellable, error);
  if (result == NULL)
    return FALSE;

  authorized = polkit_authorization_result_get_is_authorized (result);

  if (!authorized)
    return flatpak_fail_error (error, FLATPAK_ERROR_PERMISSION_DENIED,
                               /* Translators: The placeholder is for an app ref. */
                               _("Installing %s is not allowed by the policy set by your administrator"),
                               ref);

  g_info ("Parental controls policy overridden by polkit for %s", ref);
#endif  /* USE_SYSTEM_HELPER */
#endif  /* HAVE_LIBMALCONTENT */

  return TRUE;
}

/* We create a deploy ref for the currently deployed version of all refs to avoid
   deployed commits being pruned when e.g. we pull --no-deploy. */
static gboolean
flatpak_dir_update_deploy_ref (FlatpakDir *self,
                               const char *ref,
                               const char *checksum,
                               GError    **error)
{
  g_autofree char *deploy_ref = g_strconcat ("deploy/", ref, NULL);

  if (!ostree_repo_set_ref_immediate (self->repo, NULL, deploy_ref, checksum, NULL, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_dir_deploy (FlatpakDir          *self,
                    const char          *origin,
                    FlatpakDecomposed   *ref,
                    const char          *checksum_or_latest,
                    const char * const * subpaths,
                    const char * const * previous_ids,
                    GCancellable        *cancellable,
                    GError             **error)
{
  g_autofree char *resolved_ref = NULL;
  g_autofree char *ref_id = NULL;
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) deploy_base = NULL;
  glnx_autofd int deploy_base_dfd = -1;
  g_autoptr(GFile) checkoutdir = NULL;
  g_autoptr(GFile) bindir = NULL;
  g_autofree char *checkoutdirpath = NULL;
  const char *checkoutdir_basename;
  g_autoptr(GFile) real_checkoutdir = NULL;
  g_autoptr(GFile) dotref = NULL;
  g_autoptr(GFile) files_etc = NULL;
  g_autoptr(GFile) deploy_data_file = NULL;
  g_autoptr(GVariant) commit_data = NULL;
  g_autoptr(GBytes) deploy_data = NULL;
  g_autoptr(GFile) export = NULL;
  g_autoptr(GFile) extradir = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  guint64 installed_size = 0;
  OstreeRepoCheckoutAtOptions options = { 0, };
  const char *checksum;
  glnx_autofd int checkoutdir_dfd = -1;
  const char *xa_ref = NULL;
  g_autofree char *checkout_basename = NULL;
  gboolean created_extra_data = FALSE;
  g_autoptr(GVariant) commit_metadata = NULL;
  g_auto(GLnxLockFile) lock = { 0, };
  g_autoptr(GFile) metadata_file = NULL;
  g_autofree char *metadata_contents = NULL;
  gsize metadata_size = 0;
  const char *flatpak;
  g_auto(GLnxTmpDir) tmp_dir_handle = { 0, };

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  ref_id = flatpak_decomposed_dup_id (ref);

  /* Keep a shared repo lock to avoid prunes removing objects we're relying on
   * while we do the checkout. This could happen if the ref changes after we
   * read its current value for the checkout. */
  if (!flatpak_dir_repo_lock (self, &lock, LOCK_SH, cancellable, error))
    return FALSE;

  deploy_base = flatpak_dir_get_deploy_dir (self, ref);

  if (!glnx_opendirat (AT_FDCWD, flatpak_file_get_path_cached (deploy_base), TRUE, &deploy_base_dfd, error))
    return FALSE;

  /* There used to be a bug here where temporary files beneath @deploy_base were not removed,
   * which could use quite a lot of space over time, so we check for these and remove them.
   * We only do so for the current app to avoid every deploy operation iterating over
   * every app directory and all their immediate descendents. That would be a bit much I/O. */
  remove_old_deploy_tmpdirs (deploy_base);

  if (checksum_or_latest == NULL)
    {
      g_info ("No checksum specified, getting tip of %s from origin %s", flatpak_decomposed_get_ref (ref), origin);

      resolved_ref = flatpak_dir_read_latest (self, origin, flatpak_decomposed_get_ref (ref), NULL, cancellable, error);
      if (resolved_ref == NULL)
        {
          g_prefix_error (error, _("While trying to resolve ref %s: "), flatpak_decomposed_get_ref (ref));
          return FALSE;
        }

      checksum = resolved_ref;
      g_info ("tip resolved to: %s", checksum);
    }
  else
    {
      checksum = checksum_or_latest;
      g_info ("Looking for checksum %s in local repo", checksum);
      if (!ostree_repo_read_commit (self->repo, checksum, NULL, NULL, cancellable, NULL))
        return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("%s is not available"), flatpak_decomposed_get_ref (ref));
    }

  if (!ostree_repo_load_commit (self->repo, checksum, &commit_data, NULL, error))
    return FALSE;

  commit_metadata = g_variant_get_child_value (commit_data, 0);
  checkout_basename = flatpak_dir_get_deploy_subdir (self, checksum, subpaths);

  real_checkoutdir = g_file_get_child (deploy_base, checkout_basename);
  if (g_file_query_exists (real_checkoutdir, cancellable))
    return flatpak_fail_error (error, FLATPAK_ERROR_ALREADY_INSTALLED,
                               _("%s commit %s already installed"), flatpak_decomposed_get_ref (ref), checksum);

  g_autofree char *template = g_strdup_printf (".%s-XXXXXX", checkout_basename);

  if (!glnx_mkdtempat (deploy_base_dfd, template, 0755, &tmp_dir_handle, NULL))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Can't create deploy directory"));
      return FALSE;
    }

  checkoutdir = g_file_get_child (deploy_base, tmp_dir_handle.path);

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
  checkoutdir_basename = tmp_dir_handle.path;  /* so checkoutdirpath = deploy_base_dfd / checkoutdir_basename */

  if (subpaths == NULL || *subpaths == NULL)
    {
      if (!ostree_repo_checkout_at (self->repo, &options,
                                    deploy_base_dfd, checkoutdir_basename,
                                    checksum,
                                    cancellable, error))
        {
          g_prefix_error (error, _("While trying to checkout %s into %s: "), checksum, checkoutdirpath);
          return FALSE;
        }
    }
  else
    {
      g_autoptr(GFile) files = g_file_get_child (checkoutdir, "files");
      int i;

      if (!g_file_make_directory_with_parents (files, cancellable, error))
        return FALSE;

      options.subpath = "metadata";

      if (!ostree_repo_checkout_at (self->repo, &options,
                                    deploy_base_dfd, checkoutdir_basename,
                                    checksum,
                                    cancellable, error))
        {
          g_prefix_error (error, _("While trying to checkout metadata subpath: "));
          return FALSE;
        }

      for (i = 0; subpaths[i] != NULL; i++)
        {
          g_autofree char *subpath = g_build_filename ("files", subpaths[i], NULL);
          g_autofree char *dstpath = g_build_filename (checkoutdirpath, "/files", subpaths[i], NULL);
          g_autofree char *dstpath_parent = g_path_get_dirname (dstpath);
          g_autofree char *dstpath_relative_to_deploy_base = g_build_filename (checkoutdir_basename, "/files", subpaths[i], NULL);
          g_autoptr(GFile) child = NULL;

          child = g_file_resolve_relative_path (root, subpath);

          if (!g_file_query_exists (child, cancellable))
            {
              g_info ("subpath %s not in tree", subpaths[i]);
              continue;
            }

          if (g_mkdir_with_parents (dstpath_parent, 0755))
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }

          options.subpath = subpath;
          if (!ostree_repo_checkout_at (self->repo, &options,
                                        deploy_base_dfd, dstpath_relative_to_deploy_base,
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
          FlatpakDecomposed *checkout_ref = ref;
          g_autoptr(FlatpakDecomposed) commit_ref = NULL;

          commit_ref = flatpak_decomposed_new_from_ref (xa_ref, error);
          if (commit_ref == NULL)
            {
              g_prefix_error (error, _("Invalid commit ref %s: "), xa_ref);
              return FALSE;
            }

          /* Fatal if kind/name/arch don't match. Warn for branch mismatch. */
          if (!flatpak_decomposed_equal_except_branch (checkout_ref, commit_ref))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           _("Deployed ref %s does not match commit (%s)"),
                           flatpak_decomposed_get_ref (ref), xa_ref);
              return FALSE;
            }

          if (strcmp (flatpak_decomposed_get_branch (checkout_ref), flatpak_decomposed_get_branch (commit_ref)) != 0)
            g_warning (_("Deployed ref %s branch does not match commit (%s)"),
                       flatpak_decomposed_get_ref (ref), xa_ref);
        }
      else if (strcmp (flatpak_decomposed_get_ref (ref), xa_ref) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                       _("Deployed ref %s does not match commit (%s)"), flatpak_decomposed_get_ref (ref), xa_ref);
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

      if (!flatpak_check_required_version (flatpak_decomposed_get_ref (ref), keyfile, error))
        return FALSE;
    }

  /* Check the metadata in the commit to make sure it matches the actual
   * deployed metadata, in case we relied on the one in the commit for
   * a decision
   */
  if (!validate_commit_metadata (commit_data, flatpak_decomposed_get_ref (ref),
                                 metadata_contents, metadata_size, error))
    return FALSE;

  dotref = g_file_resolve_relative_path (checkoutdir, "files/.ref");
  if (!g_file_replace_contents (dotref, "", 0, NULL, FALSE,
                                G_FILE_CREATE_REPLACE_DESTINATION, NULL, cancellable, error))
    return FALSE;

  export = g_file_get_child (checkoutdir, "export");

  /* Never export any binaries bundled with the app */
  bindir = g_file_get_child (export, "bin");
  if (!flatpak_rm_rf (bindir, cancellable, error))
    return FALSE;

  if (flatpak_decomposed_is_runtime (ref))
    {
      /* Ensure that various files exist as regular files in /usr/etc, as we
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
            return FALSE;

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
      g_autofree char *ref_arch = flatpak_decomposed_dup_arch (ref);
      g_autofree char *ref_branch = flatpak_decomposed_dup_branch (ref);
      g_autoptr(GFile) wrapper = g_file_get_child (bindir, ref_id);
      g_autofree char *escaped_app = maybe_quote (ref_id);
      g_autofree char *escaped_branch = maybe_quote (ref_branch);
      g_autofree char *escaped_arch = maybe_quote (ref_arch);
      g_autofree char *bin_data = NULL;
      int r;

      if (!flatpak_mkdir_p (bindir, cancellable, error))
        return FALSE;

      if (!flatpak_rewrite_export_dir (ref_id, ref_branch, ref_arch,
                                       keyfile, previous_ids, export,
                                       cancellable,
                                       error))
        return FALSE;
      if ((flatpak = g_getenv ("FLATPAK_BINARY")) == NULL)
        flatpak = FLATPAK_BINDIR "/flatpak";

      bin_data = g_strdup_printf ("#!/bin/sh\nexec %s run --branch=%s --arch=%s %s \"$@\"\n",
                                  flatpak, escaped_branch, escaped_arch, escaped_app);
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
                                             commit_data,
                                             commit_metadata,
                                             keyfile,
                                             ref_id,
                                             origin,
                                             checksum,
                                             (char **) subpaths,
                                             installed_size,
                                             previous_ids);

  /* Check the app is actually allowed to be used by this user. This can block
   * on getting authorisation. */
  if (!flatpak_dir_check_parental_controls (self, flatpak_decomposed_get_ref (ref), deploy_data, cancellable, error))
    return FALSE;

  deploy_data_file = g_file_get_child (checkoutdir, "deploy");
  if (!flatpak_bytes_save (deploy_data_file, deploy_data, cancellable, error))
    return FALSE;

  if (!glnx_opendirat (deploy_base_dfd, checkoutdir_basename, TRUE, &checkoutdir_dfd, error))
    return FALSE;

  if (syncfs (checkoutdir_dfd) != 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  if (!g_file_move (checkoutdir, real_checkoutdir, G_FILE_COPY_NO_FALLBACK_FOR_MOVE,
                    cancellable, NULL, NULL, error))
    return FALSE;

  glnx_tmpdir_unset (&tmp_dir_handle);

  if (!flatpak_dir_set_active (self, ref, checkout_basename, cancellable, error))
    return FALSE;

  if (!flatpak_dir_update_deploy_ref (self, flatpak_decomposed_get_ref (ref), checksum, error))
    return FALSE;

  return TRUE;
}

/* -origin remotes are deleted when the last ref referring to it is undeployed */
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
flatpak_dir_deploy_install (FlatpakDir        *self,
                            FlatpakDecomposed *ref,
                            const char        *origin,
                            const char       **subpaths,
                            const char       **previous_ids,
                            gboolean           reinstall,
                            gboolean           pin_on_deploy,
                            GCancellable      *cancellable,
                            GError           **error)
{
  g_auto(GLnxLockFile) lock = { 0, };
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) old_deploy_dir = NULL;
  gboolean created_deploy_base = FALSE;
  gboolean ret = FALSE;
  g_autoptr(GError) local_error = NULL;
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
          g_autoptr(GBytes) old_deploy = NULL;
          const char *old_origin;

          old_deploy = flatpak_load_deploy_data (old_deploy_dir, ref, self->repo, FLATPAK_DEPLOY_VERSION_ANY, cancellable, error);
          if (old_deploy == NULL)
            goto out;

          /* If the old install was from a different remote, remove the ref */
          old_origin = flatpak_deploy_data_get_origin (old_deploy);
          if (strcmp (old_origin, origin) != 0)
            remove_ref_from_remote = g_strdup (old_origin);

          g_info ("Removing old deployment for reinstall");
          if (!flatpak_dir_undeploy (self, ref, old_active,
                                     TRUE, FALSE,
                                     cancellable, error))
            goto out;
        }
      else
        {
          g_autofree char *id = flatpak_decomposed_dup_id (ref);
          g_autofree char *branch = flatpak_decomposed_dup_branch (ref);
          g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                       _("%s branch %s already installed"), id, branch);
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

  if (!flatpak_dir_deploy (self, origin, ref, NULL, (const char * const *) subpaths,
                           previous_ids, cancellable, error))
    goto out;

  if (flatpak_decomposed_is_app (ref))
    {
      g_autofree char *id = flatpak_decomposed_dup_id (ref);

      if (!flatpak_dir_make_current_ref (self, ref, cancellable, error))
        goto out;

      if (!flatpak_dir_update_exports (self, id, cancellable, error))
        goto out;
    }

  /* Remove old ref if the reinstalled was from a different remote */
  if (remove_ref_from_remote != NULL)
    {
      if (!flatpak_dir_remove_ref (self, remove_ref_from_remote, flatpak_decomposed_get_ref (ref), cancellable, error))
        goto out;

      flatpak_dir_prune_origin_remote (self, remove_ref_from_remote);
    }

  /* Release lock before doing possibly slow prune */
  glnx_release_lock_file (&lock);

  flatpak_dir_cleanup_removed (self, cancellable, NULL);

  if (!flatpak_dir_mark_changed (self, error))
    goto out;

  /* Pin runtimes that are installed explicitly rather than pulled as
   * dependencies so they are not automatically removed. */
  if (pin_on_deploy &&
      !flatpak_dir_config_append_pattern (self, "pinned",
                                          flatpak_decomposed_get_ref (ref),
                                          TRUE, NULL, error))
    goto out;

  ret = TRUE;

  commit = flatpak_dir_read_active (self, ref, cancellable);
  flatpak_dir_log (self, "deploy install", origin, flatpak_decomposed_get_ref (ref), commit, old_active, NULL,
                   "Installed %s from %s", flatpak_decomposed_get_ref (ref), origin);

out:
  if (created_deploy_base && !ret)
    flatpak_rm_rf (deploy_base, cancellable, NULL);

  return ret;
}


gboolean
flatpak_dir_deploy_update (FlatpakDir        *self,
                           FlatpakDecomposed *ref,
                           const char        *checksum_or_latest,
                           const char       **opt_subpaths,
                           const char       **opt_previous_ids,
                           GCancellable      *cancellable,
                           GError           **error)
{
  g_autoptr(GBytes) old_deploy_data = NULL;
  g_auto(GLnxLockFile) lock = { 0, };
  g_autofree const char **old_subpaths = NULL;
  g_autofree char *old_active = NULL;
  const char *old_origin;
  g_autofree char *commit = NULL;
  g_autofree const char **previous_ids = NULL;
  g_auto(GStrv) previous_ids_owned = NULL;

  if (!flatpak_dir_lock (self, &lock,
                         cancellable, error))
    return FALSE;

  old_deploy_data = flatpak_dir_get_deploy_data (self, ref,
                                                 FLATPAK_DEPLOY_VERSION_ANY,
                                                 cancellable, error);
  if (old_deploy_data == NULL)
    return FALSE;

  old_active = flatpak_dir_read_active (self, ref, cancellable);

  old_origin = flatpak_deploy_data_get_origin (old_deploy_data);
  old_subpaths = flatpak_deploy_data_get_subpaths (old_deploy_data);

  previous_ids = flatpak_deploy_data_get_previous_ids (old_deploy_data, NULL);
  if (opt_previous_ids)
    {
      previous_ids_owned = flatpak_strv_merge ((char **) previous_ids, (char **) opt_previous_ids);
      g_clear_pointer (&previous_ids, g_free);
    }
  else
    previous_ids_owned = g_strdupv ((char **) previous_ids);

  if (!flatpak_dir_deploy (self,
                           old_origin,
                           ref,
                           checksum_or_latest,
                           opt_subpaths ? opt_subpaths : old_subpaths,
                           (const char * const *) previous_ids_owned,
                           cancellable, error))
    return FALSE;

  if (old_active &&
      !flatpak_dir_undeploy (self, ref, old_active,
                             TRUE, FALSE,
                             cancellable, error))
    return FALSE;

  if (flatpak_decomposed_is_app (ref))
    {
      g_autofree char *id = flatpak_decomposed_dup_id (ref);

      if (!flatpak_dir_update_exports (self, id, cancellable, error))
        return FALSE;
    }

  /* Release lock before doing possibly slow prune */
  glnx_release_lock_file (&lock);

  if (!flatpak_dir_mark_changed (self, error))
    return FALSE;

  flatpak_dir_cleanup_removed (self, cancellable, NULL);

  commit = flatpak_dir_read_active (self, ref, cancellable);
  flatpak_dir_log (self, "deploy update", old_origin, flatpak_decomposed_get_ref (ref), commit, old_active, NULL,
                   "Updated %s from %s", flatpak_decomposed_get_ref (ref), old_origin);

  return TRUE;
}

static void
rewrite_one_dynamic_launcher (const char *portal_desktop_dir,
                              const char *portal_icon_dir,
                              const char *desktop_name,
                              const char *old_app_id,
                              const char *new_app_id)
{
  g_autoptr(GKeyFile) old_key_file = NULL;
  g_autoptr(GKeyFile) new_key_file = NULL;
  g_autoptr(GFile) link_file = NULL;
  g_autoptr(GFile) new_link_file = NULL;
  g_autoptr(GString) data_string = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autofree char *old_data = NULL;
  g_autofree char *desktop_path = NULL;
  g_autofree char *new_desktop_path = NULL;
  g_autofree char *icon_path = NULL;
  g_autofree char *relative_path = NULL;
  g_autofree char *new_desktop = NULL;
  const gchar *desktop_suffix;

  g_assert (g_str_has_suffix (desktop_name, ".desktop"));
  g_assert (g_str_has_prefix (desktop_name, old_app_id));

  desktop_path = g_build_filename (portal_desktop_dir,
                                   desktop_name, NULL);
  old_key_file = g_key_file_new ();
  if (!g_key_file_load_from_file (old_key_file, desktop_path,
                                  G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                  &local_error))
    {
      g_warning ("Error encountered loading key file %s: %s", desktop_path, local_error->message);
      return;
    }
  if (!g_key_file_has_key (old_key_file, G_KEY_FILE_DESKTOP_GROUP, "X-Flatpak", NULL))
    {
      g_info ("Ignoring non-Flatpak dynamic launcher: %s", desktop_path);
      return;
    }

  /* Fix paths in desktop file with a find-and-replace. The portal handled
   * quoting the app ID in the Exec line for us.
   */
  old_data = g_key_file_to_data (old_key_file, NULL, NULL);
  data_string = g_string_new ((const char *)old_data);
  g_string_replace (data_string, old_app_id, new_app_id, 0);
  new_key_file = g_key_file_new ();
  if (!g_key_file_load_from_data (new_key_file, data_string->str, -1,
                                  G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                  &local_error))
    {
      g_warning ("Cannot load desktop file %s after rewrite: %s", desktop_path, local_error->message);
      g_warning ("Key file contents:\n%s\n", (const char *)data_string->str);
      return;
    }

  /* Write it out at the new path */
  desktop_suffix = desktop_name + strlen (old_app_id);
  new_desktop = g_strconcat (new_app_id, desktop_suffix, NULL);
  new_desktop_path = g_build_filename (portal_desktop_dir, new_desktop, NULL);
  if (!g_key_file_save_to_file (new_key_file, new_desktop_path, &local_error))
    {
      g_warning ("Couldn't rewrite desktop file from %s to %s: %s",
                 desktop_path, new_desktop_path, local_error->message);
      return;
    }

  /* Fix symlink */
  link_file = g_file_new_build_filename (g_get_user_data_dir (), "applications", desktop_name, NULL);
  relative_path = g_build_filename ("..", "xdg-desktop-portal", "applications", new_desktop, NULL);
  if (!g_file_delete (link_file, NULL, &local_error) &&
      !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_info ("Unable to delete desktop file link %s: %s", desktop_name, local_error->message);
      g_clear_error (&local_error);
    }

  new_link_file = g_file_new_build_filename (g_get_user_data_dir (), "applications", new_desktop, NULL);
  if (!g_file_make_symbolic_link (new_link_file, relative_path, NULL, &local_error))
    {
      g_warning ("Unable to rename desktop file link %s -> %s: %s",
                 desktop_name, new_desktop, local_error->message);
      return;
    }

  /* Delete the old desktop file */
  unlink (desktop_path);

  /* And rename the icon */
  icon_path = g_key_file_get_string (old_key_file, G_KEY_FILE_DESKTOP_GROUP, "Icon", NULL);
  if (g_str_has_prefix (icon_path, portal_icon_dir))
    {
      g_autoptr(GFile) icon_file = NULL;
      g_autofree char *icon_basename = NULL;
      g_autofree char *new_icon = NULL;
      gchar *icon_suffix;

      icon_file = g_file_new_for_path (icon_path);
      icon_basename = g_path_get_basename (icon_path);
      if (g_str_has_prefix (icon_basename, old_app_id))
        {
          icon_suffix = icon_basename + strlen (old_app_id);
          new_icon = g_strconcat (new_app_id, icon_suffix, NULL);
          if (!g_file_set_display_name (icon_file, new_icon, NULL, &local_error))
            {
              g_warning ("Unable to rename icon file %s -> %s: %s", icon_basename, new_icon,
                         local_error->message);
              g_clear_error (&local_error);
            }
        }
    }
}

static void
rewrite_dynamic_launchers (FlatpakDecomposed   *ref,
                           const char * const * previous_ids)

{
  g_autoptr(GFile) portal_desktop_dir = NULL;
  g_autofree char *portal_icon_path = NULL;
  g_autofree char *app_id = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GError) local_error = NULL;

  if (!flatpak_decomposed_is_app (ref))
    return;

  app_id = flatpak_decomposed_dup_id (ref);

  /* Rename any dynamic launchers written by xdg-desktop-portal. The
   * portal has its own code for renaming launchers on session start but we
   * need to do it here as well so the launchers are correct in both cases:
   * (1) the app rename transaction is being executed by the same user that
   * has the launchers, or (2) the app is installed system-wide and another
   * user has launchers.
   */
  if (previous_ids != NULL)
    {
      portal_desktop_dir = g_file_new_build_filename (g_get_user_data_dir (),
                                                      "xdg-desktop-portal",
                                                      "applications", NULL);
      portal_icon_path = g_build_filename (g_get_user_data_dir (),
                                           "xdg-desktop-portal", "icons", NULL);
      dir_enum = g_file_enumerate_children (portal_desktop_dir,
                                            G_FILE_ATTRIBUTE_STANDARD_NAME,
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            NULL, &local_error);
    }
  if (dir_enum == NULL)
    {
      if (local_error && !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_warning ("Failed to enumerate portal desktop dir %s: %s",
                     flatpak_file_get_path_cached (portal_desktop_dir),
                     local_error->message);
        }
      g_clear_error (&local_error);
    }
  else
    {
      g_autoptr(GFileInfo) child_info = NULL;
      g_auto(GStrv) previous_ids_sorted = NULL;

      /* Sort by decreasing length so we get the longest prefix below */
      previous_ids_sorted = flatpak_strv_sort_by_length (previous_ids);

      while ((child_info = g_file_enumerator_next_file (dir_enum, NULL, &local_error)) != NULL)
        {
          const char *desktop_name;
          int i;

          desktop_name = g_file_info_get_name (child_info);
          if (!g_str_has_suffix (desktop_name, ".desktop"))
            continue;

          for (i = 0; previous_ids_sorted[i] != NULL; i++)
            {
              if (g_str_has_prefix (desktop_name, previous_ids_sorted[i]))
                {
                  rewrite_one_dynamic_launcher (flatpak_file_get_path_cached (portal_desktop_dir),
                                                portal_icon_path, desktop_name,
                                                previous_ids_sorted[i], app_id);
                  break;
                }
            }
        }
      if (local_error)
        {
          g_warning ("Failed to enumerate portal desktop dir %s: %s",
                     flatpak_file_get_path_cached (portal_desktop_dir),
                     local_error->message);
        }
    }
}

static FlatpakOciRegistry *
flatpak_dir_create_system_child_oci_registry (FlatpakDir   *self,
                                              GLnxLockFile *file_lock,
                                              const char   *token,
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

  flatpak_oci_registry_set_token (new_registry, token);

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
  repo = ostree_repo_new (repo_dir);

  if (!ostree_repo_open (repo, NULL, error))
    return NULL;

  /* We don't need to sync the child repos, they are never used for stable storage, and we
     verify + fsync when importing to stable storage */
  ostree_repo_set_disable_fsync (repo, TRUE);

  g_autoptr(GFile) user_cache_dir = flatpak_ensure_user_cache_dir_location (error);
  if (user_cache_dir == NULL)
    return FALSE;

  if (!ostree_repo_set_cache_dir (repo, AT_FDCWD,
                                  flatpak_file_get_path_cached (user_cache_dir),
                                  NULL, error))
    return FALSE;

  /* Create a commitpartial in the child repo if needed to ensure we download everything, because
     any commitpartial state in the parent will not otherwise be inherited */
  if (optional_commit)
    {
      g_autofree char *commitpartial_basename = g_strconcat (optional_commit, ".commitpartial", NULL);
      g_autoptr(GFile) orig_commitpartial =
        flatpak_build_file (ostree_repo_get_path (self->repo),
                            "state", commitpartial_basename, NULL);
      if (g_file_query_exists (orig_commitpartial, NULL))
        {
          g_autoptr(GFile) commitpartial =
            flatpak_build_file (ostree_repo_get_path (repo),
                                "state", commitpartial_basename, NULL);
          g_file_replace_contents (commitpartial, "", 0, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, NULL);
        }
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

static gboolean
flatpak_dir_setup_revokefs_fuse_mount (FlatpakDir    *self,
                                       FlatpakDecomposed *ref,
                                       const gchar   *installation,
                                       gchar        **out_src_dir,
                                       gchar        **out_mnt_dir,
                                       GCancellable  *cancellable)
{
  g_autoptr (GError) local_error = NULL;
  g_autofree gchar *src_dir_tmp = NULL;
  g_autofree gchar *mnt_dir_tmp = NULL;
  gint socket = -1;
  gboolean res = FALSE;
  const char *revokefs_fuse_bin = LIBEXECDIR "/revokefs-fuse";

  if (g_getenv ("FLATPAK_REVOKEFS_FUSE"))
    revokefs_fuse_bin = g_getenv ("FLATPAK_REVOKEFS_FUSE");

  if (!flatpak_dir_system_helper_call_get_revokefs_fd (self,
                                                       FLATPAK_HELPER_GET_REVOKEFS_FD_FLAGS_NONE,
                                                       installation ? installation : "",
                                                       &socket,
                                                       &src_dir_tmp,
                                                       cancellable,
                                                       &local_error))
    {
      if (g_error_matches (local_error, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED))
        g_info ("revokefs-fuse not supported on your installation: %s", local_error->message);
      else
        g_warning ("Failed to get revokefs-fuse socket from system-helper: %s", local_error->message);

      goto out;
    }
  else
    {
      g_autoptr(GSubprocess) revokefs_fuse = NULL;
      g_autoptr(GSubprocessLauncher) launcher = NULL;
      g_autofree gchar *client_uid = NULL;

      mnt_dir_tmp = flatpak_dir_revokefs_fuse_create_mountpoint (ref, &local_error);
      if (mnt_dir_tmp == NULL)
        {
          g_warning ("Failed to create a mountpoint for revokefs-fuse: %s", local_error->message);
          close (socket);
          goto out;
        }

      client_uid = g_strdup_printf ("uid=%d", getuid ());
      launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
      g_subprocess_launcher_take_fd (launcher, socket, 3);
      revokefs_fuse = g_subprocess_launcher_spawn (launcher,
                                                   &local_error,
                                                   revokefs_fuse_bin, "-o", client_uid, "--socket=3",
                                                   src_dir_tmp, mnt_dir_tmp, NULL);
      if (revokefs_fuse == NULL ||
          !g_subprocess_wait_check (revokefs_fuse, NULL, &local_error))
        {
          g_warning ("Error spawning revokefs-fuse: %s", local_error->message);
          close (socket);
          goto out;
        }
    }

  res = TRUE;

out:
  /* It is unconventional to steal these values on error. However, it depends on where
   * this function failed. If we are able to spawn the revokefs backend (src_dir_tmp
   * is non-NULL) but failed to create mountpoint or spawning revokefs-fuse here,
   * we  still need the src_dir_tmp value to cleanup the revokefs backend properly
   * through the system-helper's CancelPull(). Hence, always stealing values can tell
   * the caller under what circumstances this function failed and cleanup accordingly. */
  *out_mnt_dir = g_steal_pointer (&mnt_dir_tmp);
  *out_src_dir = g_steal_pointer (&src_dir_tmp);

  return res;
}

static void
flatpak_dir_unmount_and_cancel_pull (FlatpakDir    *self,
                                     guint          arg_flags,
                                     GCancellable  *cancellable,
                                     OstreeRepo   **repo,
                                     GLnxLockFile  *lockfile,
                                     const char    *mnt_dir,
                                     const char    *src_dir)
{
  const char *installation = flatpak_dir_get_id (self);
  g_autoptr(GError) error = NULL;

  if (mnt_dir &&
      !flatpak_dir_revokefs_fuse_unmount (repo, lockfile, mnt_dir, &error))
    g_warning ("Could not unmount revokefs-fuse filesystem at %s: %s", mnt_dir, error->message);

  g_clear_error (&error);

  if (src_dir &&
      !flatpak_dir_system_helper_call_cancel_pull (self,
                                                   arg_flags,
                                                   installation ? installation : "",
                                                   src_dir, cancellable, &error))
    g_warning ("Error cancelling ongoing pull at %s: %s", src_dir, error->message);
}

gboolean
flatpak_dir_install (FlatpakDir          *self,
                     gboolean             no_pull,
                     gboolean             no_deploy,
                     gboolean             no_static_deltas,
                     gboolean             reinstall,
                     gboolean             app_hint,
                     gboolean             pin_on_deploy,
                     FlatpakRemoteState  *state,
                     FlatpakDecomposed   *ref,
                     const char          *opt_commit,
                     const char         **opt_subpaths,
                     const char         **opt_previous_ids,
                     GFile               *sideload_repo,
                     GBytes              *require_metadata,
                     const char          *token,
                     FlatpakProgress     *progress,
                     GCancellable        *cancellable,
                     GError             **error)
{
  FlatpakPullFlags flatpak_flags;

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
      gboolean is_revokefs_pull = FALSE;

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

          registry = flatpak_dir_create_system_child_oci_registry (self, &child_repo_lock, token, error);
          if (registry == NULL)
            return FALSE;

          registry_file = g_file_new_for_uri (flatpak_oci_registry_get_uri (registry));

          child_repo_path = g_file_get_path (registry_file);

          if (!flatpak_dir_mirror_oci (self, registry, state, flatpak_decomposed_get_ref (ref), opt_commit, NULL, token, progress, cancellable, error))
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
            return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("Can't pull from untrusted non-gpg verified remote"));
        }
      else
        {
          /* For system pulls, the pull has to be made in a child repo first,
             which is then pulled into the system's one. The pull from child
             repo into the system repo can occur in one of the two following ways:
                1) Hard-link the child repo into system's one.
                2) Copy and verify each object from the child repo to the system's one.

             2) poses the problem of using double disk-space which might fail the
             installation of very big applications. For e.g. at endless, the encyclopedia app
             is about ~6GB, hence ~12GB of free disk-space is required to get it installed.

             For 1), we need to make sure that we address all the security concerns that
             might escalate during the pull from a remote into child repo and subsequently,
             hard-linking it into the (root-owned)system repo. This is taken care of by a
             special FUSE process(revokefs-fuse) which guards all the writes made to the
             child repo and ensures that no file descriptors remain open to the child repo
             before the hard-linkable pull is made into the system's repo.
             More details about the security issues dealt here are present at
             https://github.com/flatpak/flatpak/wiki/Noncopying-system-app-installation

             In case we fail to apply pull approach 1), the pull automatically fallbacks to use 2). */
          g_autofree gchar *src_dir = NULL;
          g_autofree gchar *mnt_dir = NULL;
          g_autoptr(GError) local_error = NULL;

          if (!flatpak_dir_setup_revokefs_fuse_mount (self,
                                                      ref,
                                                      installation,
                                                      &src_dir, &mnt_dir,
                                                      cancellable))
            {
              flatpak_dir_unmount_and_cancel_pull (self, FLATPAK_HELPER_CANCEL_PULL_FLAGS_NONE,
                                                   cancellable,
                                                   &child_repo, &child_repo_lock,
                                                   mnt_dir, src_dir);
            }
          else
            {
              g_autofree gchar *repo_basename = NULL;
              g_autoptr(GFile) mnt_dir_file = NULL;

              mnt_dir_file = g_file_new_for_path (mnt_dir);
              child_repo = flatpak_dir_create_child_repo (self, mnt_dir_file, &child_repo_lock, opt_commit, &local_error);
              if (child_repo == NULL)
                {
                  g_warning ("Cannot create repo on revokefs mountpoint %s: %s", mnt_dir, local_error->message);
                  flatpak_dir_unmount_and_cancel_pull (self,
                                                       FLATPAK_HELPER_CANCEL_PULL_FLAGS_NONE,
                                                       cancellable,
                                                       &child_repo, &child_repo_lock,
                                                       mnt_dir, src_dir);
                  g_clear_error (&local_error);
                }
              else
                {
                  repo_basename = g_file_get_basename (ostree_repo_get_path (child_repo));
                  child_repo_path = g_build_filename (src_dir, repo_basename, NULL);
                  is_revokefs_pull = TRUE;
                }
            }

          /* Fallback if revokefs-fuse setup does not succeed. This makes the pull
           * temporarily use double disk-space. */
          if (!is_revokefs_pull)
            {
             /* We're pulling from a remote source, we do the network mirroring pull as a
                user and hand back the resulting data to the system-helper, that trusts us
                due to the GPG signatures in the repo */
              child_repo = flatpak_dir_create_system_child_repo (self, &child_repo_lock, NULL, error);
              if (child_repo == NULL)
                return FALSE;
              else
                child_repo_path = g_file_get_path (ostree_repo_get_path (child_repo));
            }

          flatpak_flags |= FLATPAK_PULL_FLAGS_SIDELOAD_EXTRA_DATA;

          if (!flatpak_dir_pull (self, state, flatpak_decomposed_get_ref (ref), opt_commit, subpaths, sideload_repo, require_metadata, token,
                                 child_repo,
                                 flatpak_flags,
                                 0,
                                 progress, cancellable, error))
            {
              if (is_revokefs_pull)
                {
                  flatpak_dir_unmount_and_cancel_pull (self,
                                                       FLATPAK_HELPER_CANCEL_PULL_FLAGS_PRESERVE_PULL,
                                                       cancellable,
                                                       &child_repo, &child_repo_lock,
                                                       mnt_dir, src_dir);
                }

              return FALSE;
            }

          g_assert (child_repo_path != NULL);

          if (is_revokefs_pull &&
              !flatpak_dir_revokefs_fuse_unmount (&child_repo, &child_repo_lock, mnt_dir, &local_error))
            {
              g_propagate_prefixed_error (error, g_steal_pointer (&local_error),
                      _("Could not unmount revokefs-fuse filesystem at %s: "), mnt_dir);

              if (src_dir &&
                  !flatpak_dir_system_helper_call_cancel_pull (self,
                                                               FLATPAK_HELPER_CANCEL_PULL_FLAGS_PRESERVE_PULL,
                                                               installation ? installation : "",
                                                               src_dir, cancellable, &local_error))
                g_warning ("Error cancelling ongoing pull at %s: %s", src_dir, local_error->message);
              return FALSE;
            }
        }

      if (no_deploy)
        helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY;

      if (reinstall)
        helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL;

      if (app_hint)
        helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_APP_HINT;

      if (pin_on_deploy)
        helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE_PINNED;

      helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_INSTALL_HINT;

      if (!flatpak_dir_system_helper_call_deploy (self,
                                                  child_repo_path ? child_repo_path : "",
                                                  helper_flags, flatpak_decomposed_get_ref (ref), state->remote_name,
                                                  (const char * const *) subpaths,
                                                  (const char * const *) opt_previous_ids,
                                                  installation ? installation : "",
                                                  cancellable,
                                                  error))
        return FALSE;

      if (child_repo_path && !is_revokefs_pull)
        (void) glnx_shutil_rm_rf_at (AT_FDCWD, child_repo_path, NULL, NULL);

      /* In case the app is being renamed, rewrite any launchers made by
       * xdg-desktop-portal. This has to be done as the user so can't be in the
       * system helper.
       */
      if (opt_previous_ids)
        rewrite_dynamic_launchers (ref, opt_previous_ids);

      return TRUE;
    }

  if (!no_pull)
    {
      if (!flatpak_dir_pull (self, state, flatpak_decomposed_get_ref (ref), opt_commit, opt_subpaths, sideload_repo, require_metadata, token, NULL,
                             flatpak_flags, OSTREE_REPO_PULL_FLAGS_NONE,
                             progress, cancellable, error))
        return FALSE;
    }

  if (!no_deploy)
    {
      if (!flatpak_dir_deploy_install (self, ref, state->remote_name, opt_subpaths,
                                       opt_previous_ids, reinstall, pin_on_deploy,
                                       cancellable, error))
        return FALSE;

      /* In case the app is being renamed, rewrite any launchers made by
       * xdg-desktop-portal.
       */
      if (opt_previous_ids)
        rewrite_dynamic_launchers (ref, opt_previous_ids);
    }

  return TRUE;
}

char *
flatpak_dir_ensure_bundle_remote (FlatpakDir         *self,
                                  GFile              *file,
                                  GBytes             *extra_gpg_data,
                                  FlatpakDecomposed **out_ref,
                                  char              **out_checksum,
                                  char              **out_metadata,
                                  gboolean           *out_created_remote,
                                  GCancellable       *cancellable,
                                  GError            **error)
{
  g_autoptr(FlatpakDecomposed) ref = NULL;
  gboolean created_remote = FALSE;
  g_autoptr(GBytes) deploy_data = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *origin = NULL;
  g_autofree char *fp_metadata = NULL;
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

  /* If we rely on metadata (to e.g. print permissions), check it exists before creating the remote */
  if (out_metadata && fp_metadata == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, "No metadata in bundle header");
      return NULL;
    }

  gpg_data = extra_gpg_data ? extra_gpg_data : included_gpg_data;

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
      g_autofree char *id = flatpak_decomposed_dup_id (ref);
      /* Add a remote for later updates */
      basename = g_file_get_basename (file);
      remote = flatpak_dir_create_origin_remote (self,
                                                 origin,
                                                 id,
                                                 basename,
                                                 flatpak_decomposed_get_ref (ref),
                                                 gpg_data,
                                                 collection_id,
                                                 &created_remote,
                                                 cancellable,
                                                 error);
      if (remote == NULL)
        return NULL;
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
flatpak_dir_install_bundle (FlatpakDir         *self,
                            GFile              *file,
                            const char         *remote,
                            FlatpakDecomposed **out_ref,
                            GCancellable       *cancellable,
                            GError            **error)
{
  g_autofree char *ref_str = NULL;
  g_autoptr(FlatpakDecomposed) ref = NULL;
  g_autoptr(GBytes) deploy_data = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *origin = NULL;
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
                                                          &ref_str,
                                                          cancellable,
                                                          error))
        return FALSE;


      ref = flatpak_decomposed_new_from_ref (ref_str, error);
      if (ref == NULL)
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

  deploy_data = flatpak_dir_get_deploy_data (self, ref, FLATPAK_DEPLOY_VERSION_ANY, cancellable, NULL);
  if (deploy_data != NULL)
    {
      if (strcmp (flatpak_deploy_data_get_commit (deploy_data), to_checksum) == 0)
        {
          g_autofree char *id = flatpak_decomposed_dup_id (ref);
          g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                       _("This version of %s is already installed"), id);
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
                                 flatpak_decomposed_get_ref (ref),
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
      if (!flatpak_dir_deploy_update (self, ref, NULL, NULL, NULL, cancellable, error))
        return FALSE;
    }
  else
    {
      if (!flatpak_dir_deploy_install (self, ref, remote, NULL, NULL, FALSE, FALSE, cancellable, error))
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
flatpak_dir_needs_update_for_commit_and_subpaths (FlatpakDir        *self,
                                                  const char        *remote,
                                                  FlatpakDecomposed *ref,
                                                  const char        *target_commit,
                                                  const char       **opt_subpaths)
{
  g_autoptr(GBytes) deploy_data = NULL;
  g_autofree const char **old_subpaths = NULL;
  const char **subpaths;
  g_autofree char *url = NULL;
  const char *installed_commit;
  const char *installed_alt_id;
  const char *extension_of;

  g_assert (target_commit != NULL);

  /* Never update from disabled remotes */
  if (!ostree_repo_remote_get_url (self->repo, remote, &url, NULL))
    return FALSE;

  if (*url == 0)
    return FALSE;

  /* deploy v4 guarantees alt-id/extension-of info */
  deploy_data = flatpak_dir_get_deploy_data (self, ref, 4, NULL, NULL);
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

  /* If masked, don't update */
  if (flatpak_dir_ref_is_masked (self, flatpak_decomposed_get_ref (ref)))
    return FALSE;

  extension_of = flatpak_deploy_data_get_extension_of (deploy_data);
  /* If the main ref is masked, don't update extensions of it (like .Locale or .Debug) */
  if (extension_of && flatpak_dir_ref_is_masked (self, extension_of))
    return FALSE;

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

/* This is called by the old-school non-transaction flatpak_installation_update, so doesn't do a lot. */
char *
flatpak_dir_check_for_update (FlatpakDir               *self,
                              FlatpakRemoteState       *state,
                              FlatpakDecomposed        *ref,
                              const char               *checksum_or_latest,
                              const char              **opt_subpaths,
                              gboolean                  no_pull,
                              GCancellable             *cancellable,
                              GError                  **error)
{
  g_autofree char *latest_rev = NULL;
  const char *target_rev = NULL;

  if (no_pull)
    {
      if (!flatpak_repo_resolve_rev (self->repo, NULL, state->remote_name,
                                     flatpak_decomposed_get_ref (ref), FALSE, &latest_rev, NULL, NULL))
        {
          g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
                       _("%s already installed"), flatpak_decomposed_get_ref (ref));
          return NULL; /* No update, because nothing to update to */
        }
    }
  else
    {
      if (!flatpak_dir_find_latest_rev (self, state, flatpak_decomposed_get_ref (ref), checksum_or_latest, &latest_rev,
                                        NULL, NULL, cancellable, error))
        return NULL;
    }

  if (checksum_or_latest != NULL)
    target_rev = checksum_or_latest;
  else
    target_rev = latest_rev;

  if (flatpak_dir_needs_update_for_commit_and_subpaths (self, state->remote_name, ref, target_rev, opt_subpaths))
    return g_strdup (target_rev);

  g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED,
               _("%s commit %s already installed"), flatpak_decomposed_get_ref (ref), target_rev);
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
                    FlatpakDecomposed                    *ref,
                    const char                           *commit,
                    const char                          **opt_subpaths,
                    const char                          **opt_previous_ids,
                    GFile                                *sideload_repo,
                    GBytes                               *require_metadata,
                    const char                           *token,
                    FlatpakProgress                      *progress,
                    GCancellable                         *cancellable,
                    GError                              **error)
{
  g_autoptr(GBytes) deploy_data = NULL;
  const char **subpaths = NULL;
  const char *empty_subpaths[] = {NULL};
  g_autofree char *url = NULL;
  FlatpakPullFlags flatpak_flags;
  g_autofree const char **old_subpaths = NULL;
  gboolean is_oci;

  /* This is calculated in check_for_update */
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
  else if (old_subpaths)
    subpaths = old_subpaths;
  else
    subpaths = empty_subpaths;

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
      gboolean gpg_verify_summary;
      gboolean gpg_verify;
      gboolean is_revokefs_pull = FALSE;

      if (allow_downgrade)
        return flatpak_fail_error (error, FLATPAK_ERROR_DOWNGRADE,
                                   _("Can't update to a specific commit without root permissions"));

      helper_flags = FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE;

      if (!ostree_repo_remote_get_gpg_verify_summary (self->repo, state->remote_name,
                                                      &gpg_verify_summary, error))
        return FALSE;

      if (!ostree_repo_remote_get_gpg_verify (self->repo, state->remote_name,
                                              &gpg_verify, error))
        return FALSE;

      if (no_pull)
        {
          /* Nothing to do here */
        }
      else if (is_oci)
        {
          g_autoptr(FlatpakOciRegistry) registry = NULL;
          g_autoptr(GFile) registry_file = NULL;

          registry = flatpak_dir_create_system_child_oci_registry (self, &child_repo_lock, token, error);
          if (registry == NULL)
            return FALSE;

          registry_file = g_file_new_for_uri (flatpak_oci_registry_get_uri (registry));

          child_repo_path = g_file_get_path (registry_file);

          if (!flatpak_dir_mirror_oci (self, registry, state, flatpak_decomposed_get_ref (ref),
                                       commit, NULL, token, progress, cancellable, error))
            return FALSE;
        }
      else if (!gpg_verify_summary || !gpg_verify)
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
          /* First try to update using revokefs-fuse codepath. If it fails, try to update using a
           * temporary child-repo. Read flatpak_dir_install for more details on using revokefs-fuse */
          g_autofree gchar *src_dir = NULL;
          g_autofree gchar *mnt_dir = NULL;
          g_autoptr(GError) local_error = NULL;

          if (!flatpak_dir_setup_revokefs_fuse_mount (self,
                                                      ref,
                                                      installation,
                                                      &src_dir, &mnt_dir,
                                                      cancellable))
            {
              flatpak_dir_unmount_and_cancel_pull (self, FLATPAK_HELPER_CANCEL_PULL_FLAGS_NONE,
                                                   cancellable,
                                                   &child_repo, &child_repo_lock,
                                                   mnt_dir, src_dir);
            }
          else
            {
              g_autofree gchar *repo_basename = NULL;
              g_autoptr(GFile) mnt_dir_file = NULL;

              mnt_dir_file = g_file_new_for_path (mnt_dir);
              child_repo = flatpak_dir_create_child_repo (self, mnt_dir_file, &child_repo_lock, commit, &local_error);
              if (child_repo == NULL)
                {
                  g_warning ("Cannot create repo on revokefs mountpoint %s: %s", mnt_dir, local_error->message);
                  flatpak_dir_unmount_and_cancel_pull (self,
                                                       FLATPAK_HELPER_CANCEL_PULL_FLAGS_NONE,
                                                       cancellable,
                                                       &child_repo, &child_repo_lock,
                                                       mnt_dir, src_dir);
                  g_clear_error (&local_error);
                }
              else
                {
                  repo_basename = g_file_get_basename (ostree_repo_get_path (child_repo));
                  child_repo_path = g_build_filename (src_dir, repo_basename, NULL);
                  is_revokefs_pull = TRUE;
                }
            }

          /* Fallback if revokefs-fuse setup does not succeed. This makes the pull
           * temporarily use double disk-space. */
          if (!is_revokefs_pull)
            {
              /* We're pulling from a remote source, we do the network mirroring pull as a
                 user and hand back the resulting data to the system-helper, that trusts us
                 due to the GPG signatures in the repo */

              child_repo = flatpak_dir_create_system_child_repo (self, &child_repo_lock, commit, error);
              if (child_repo == NULL)
                return FALSE;
              else
                child_repo_path = g_file_get_path (ostree_repo_get_path (child_repo));
            }

          flatpak_flags |= FLATPAK_PULL_FLAGS_SIDELOAD_EXTRA_DATA;
          if (!flatpak_dir_pull (self, state, flatpak_decomposed_get_ref (ref),
                                 commit, subpaths, sideload_repo, require_metadata, token,
                                 child_repo,
                                 flatpak_flags, 0,
                                 progress, cancellable, error))
            {
              if (is_revokefs_pull)
                {
                  flatpak_dir_unmount_and_cancel_pull (self,
                                                       FLATPAK_HELPER_CANCEL_PULL_FLAGS_PRESERVE_PULL,
                                                       cancellable,
                                                       &child_repo, &child_repo_lock,
                                                       mnt_dir, src_dir);
                }

              return FALSE;
            }

          g_assert (child_repo_path != NULL);

          if (is_revokefs_pull &&
              !flatpak_dir_revokefs_fuse_unmount (&child_repo, &child_repo_lock, mnt_dir, &local_error))
            {
              g_warning ("Could not unmount revokefs-fuse filesystem at %s: %s", mnt_dir, local_error->message);
              flatpak_dir_unmount_and_cancel_pull (self,
                                                   FLATPAK_HELPER_CANCEL_PULL_FLAGS_PRESERVE_PULL,
                                                   cancellable,
                                                   &child_repo, &child_repo_lock,
                                                   mnt_dir, src_dir);
              return FALSE;
            }
        }

      if (no_deploy)
        helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY;

      if (app_hint)
        helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_APP_HINT;

      if (install_hint)
        helper_flags |= FLATPAK_HELPER_DEPLOY_FLAGS_INSTALL_HINT;

      if (!flatpak_dir_system_helper_call_deploy (self,
                                                  child_repo_path ? child_repo_path : "",
                                                  helper_flags, flatpak_decomposed_get_ref (ref), state->remote_name,
                                                  subpaths, opt_previous_ids,
                                                  installation ? installation : "",
                                                  cancellable,
                                                  error))
        return FALSE;

      if (child_repo_path && !is_revokefs_pull)
        (void) glnx_shutil_rm_rf_at (AT_FDCWD, child_repo_path, NULL, NULL);

      /* In case the app is being renamed, rewrite any launchers made by
       * xdg-desktop-portal. This has to be done as the user so can't be in the
       * system helper.
       */
      if (opt_previous_ids)
        rewrite_dynamic_launchers (ref, opt_previous_ids);

      return TRUE;
    }

  if (!no_pull)
    {
      if (!flatpak_dir_pull (self, state, flatpak_decomposed_get_ref (ref),
                             commit, subpaths, sideload_repo, require_metadata, token,
                             NULL, flatpak_flags, OSTREE_REPO_PULL_FLAGS_NONE,
                             progress, cancellable, error))
        return FALSE;

      /* Take this opportunity to clean up refs/mirrors/ since a prune will happen
       * after this update operation. See
       * https://github.com/flatpak/flatpak/issues/3222
       * Note: For the system-helper case we do this in handle_deploy()
       */
      if (!flatpak_dir_delete_mirror_refs (self, FALSE, cancellable, error))
        return FALSE;
    }

  if (!no_deploy)
    {
      if (!flatpak_dir_deploy_update (self, ref,
                                      /* We don't know the local commit id in the OCI case, and
                                         we only support one version anyway */
                                      is_oci ? NULL : commit,
                                      subpaths, opt_previous_ids,
                                      cancellable, error))
        return FALSE;

      /* In case the app is being renamed, rewrite any launchers made by
       * xdg-desktop-portal.
       */
      if (opt_previous_ids)
        rewrite_dynamic_launchers (ref, opt_previous_ids);
    }

  return TRUE;
}

gboolean
flatpak_dir_uninstall (FlatpakDir                 *self,
                       FlatpakDecomposed          *ref,
                       FlatpakHelperUninstallFlags flags,
                       GCancellable               *cancellable,
                       GError                    **error)
{
  const char *repository;
  g_autoptr(FlatpakDecomposed) current_ref = NULL;
  gboolean was_deployed;
  g_autofree char *name = NULL;
  g_autofree char *old_active = NULL;
  g_auto(GLnxLockFile) lock = { 0, };
  g_autoptr(GBytes) deploy_data = NULL;
  gboolean keep_ref = flags & FLATPAK_HELPER_UNINSTALL_FLAGS_KEEP_REF;
  gboolean force_remove = flags & FLATPAK_HELPER_UNINSTALL_FLAGS_FORCE_REMOVE;

  name = flatpak_decomposed_dup_id (ref);

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      const char *installation = flatpak_dir_get_id (self);

      if (!flatpak_dir_system_helper_call_uninstall (self,
                                                     flags, flatpak_decomposed_get_ref (ref),
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

  /* Note: the origin remote usually exists but it's not guaranteed (the user
   * could have run remote-delete --force) */
  repository = flatpak_deploy_data_get_origin (deploy_data);
  if (repository == NULL)
    return FALSE;

  if (flatpak_decomposed_is_runtime (ref) && !force_remove)
    {
      g_autoptr(GHashTable) runtime_app_map = NULL;
      g_autoptr(GPtrArray) blocking = NULL;

      /* Look for apps that need this runtime */
      blocking = flatpak_dir_list_app_refs_with_runtime (self, &runtime_app_map, ref, cancellable, error);
      if (blocking == NULL)
        return FALSE;

      if (blocking->len > 0)
        {
          g_autoptr(GString) joined = g_string_new ("");
          for (int i = 0; i < blocking->len; i++)
            {
              FlatpakDecomposed *blocking_ref = g_ptr_array_index (blocking, i);
              g_autofree char *id = flatpak_decomposed_dup_id (blocking_ref);
              if (i != 0)
                g_string_append (joined, ", ");
              g_string_append (joined, id);
            }

          return flatpak_fail_error (error, FLATPAK_ERROR_RUNTIME_USED,
                                     _("Can't remove %s, it is needed for: %s"), flatpak_decomposed_get_pref (ref), joined->str);
        }
    }

  old_active = g_strdup (flatpak_deploy_data_get_commit (deploy_data));

  g_info ("dropping active ref");
  if (!flatpak_dir_set_active (self, ref, NULL, cancellable, error))
    return FALSE;

  if (flatpak_decomposed_is_app (ref))
    {
      current_ref = flatpak_dir_current_ref (self, name, cancellable);
      if (current_ref != NULL &&
          flatpak_decomposed_equal (ref, current_ref))
        {
          g_info ("dropping current ref");
          if (!flatpak_dir_drop_current_ref (self, name, cancellable, error))
            return FALSE;
        }
    }

  if (!flatpak_dir_update_deploy_ref (self, flatpak_decomposed_get_ref (ref), NULL, error))
    return FALSE;

  if (!flatpak_dir_undeploy_all (self, ref, force_remove, &was_deployed, cancellable, error))
    return FALSE;

  if (!keep_ref &&
      !flatpak_dir_remove_ref (self, repository, flatpak_decomposed_get_ref (ref), cancellable, error))
    return FALSE;

  /* Take this opportunity to clean up refs/mirrors/ since a prune will happen
   * after this uninstall operation. See
   * https://github.com/flatpak/flatpak/issues/3222
   */
  if (!flatpak_dir_delete_mirror_refs (self, FALSE, cancellable, error))
    return FALSE;

  if (flatpak_decomposed_is_app (ref) &&
      !flatpak_dir_update_exports (self, name, cancellable, error))
    return FALSE;

  glnx_release_lock_file (&lock);

  flatpak_dir_prune_origin_remote (self, repository);

  flatpak_dir_cleanup_removed (self, cancellable, NULL);

  if (!flatpak_dir_mark_changed (self, error))
    return FALSE;

  if (!was_deployed)
    {
      const char *branch = flatpak_decomposed_get_branch (ref);
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   _("%s branch %s is not installed"), name, branch);
      return FALSE;
    }

  flatpak_dir_log (self, "uninstall", NULL, flatpak_decomposed_get_ref (ref), NULL, old_active, NULL,
                   "Uninstalled %s", flatpak_decomposed_get_ref (ref));

  return TRUE;
}

gboolean
flatpak_dir_collect_deployed_refs (FlatpakDir   *self,
                                   const char   *type,
                                   const char   *name_prefix,
                                   const char   *arch,
                                   const char   *branch,
                                   GHashTable   *hash,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFile) dir = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  GError *temp_error = NULL;
  FlatpakKinds kind;

  if (strcmp (type, "app") == 0)
    kind = FLATPAK_KINDS_APP;
  else
    kind = FLATPAK_KINDS_RUNTIME;

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
          g_autoptr(GFile) child2 = g_file_get_child (child1, arch);
          g_autoptr(GFile) child3 = g_file_get_child (child2, branch);
          g_autoptr(GFile) active = g_file_get_child (child3, "active");

          if (g_file_query_exists (active, cancellable))
            {
              FlatpakDecomposed *ref = flatpak_decomposed_new_from_parts (kind, name, arch, branch, NULL);
              if (ref)
                g_hash_table_add (hash, ref);
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
flatpak_dir_list_deployed (FlatpakDir        *self,
                           FlatpakDecomposed *ref,
                           char            ***deployed_ids,
                           GCancellable      *cancellable,
                           GError           **error)
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
flatpak_dir_undeploy (FlatpakDir        *self,
                      FlatpakDecomposed *ref,
                      const char        *active_id,
                      gboolean           is_update,
                      gboolean           force_remove,
                      GCancellable      *cancellable,
                      GError           **error)
{
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) checkoutdir = NULL;
  g_autoptr(GFile) removed_subdir = NULL;
  g_autoptr(GFile) removed_dir = NULL;
  g_autofree char *id = NULL;
  g_autofree char *dirname = NULL;
  g_autofree char *current_active = NULL;
  g_autoptr(GFile) change_file = NULL;
  g_autoptr(GError) child_error = NULL;
  int i, retry;

  g_assert (ref != NULL);
  g_assert (active_id != NULL);

  deploy_base = flatpak_dir_get_deploy_dir (self, ref);

  checkoutdir = g_file_get_child (deploy_base, active_id);
  if (!g_file_query_exists (checkoutdir, cancellable))
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   _("%s commit %s not installed"),
                   flatpak_decomposed_get_ref (ref), active_id);
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

  id = flatpak_decomposed_dup_id (ref);
  dirname = g_strdup_printf ("%s-%s", id, active_id);

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
      g_autoptr(GError) tmp_error = NULL;

      if (!flatpak_rm_rf (removed_subdir, cancellable, &tmp_error))
        g_warning ("Unable to remove old checkout: %s", tmp_error->message);
    }

  return TRUE;
}

gboolean
flatpak_dir_undeploy_all (FlatpakDir        *self,
                          FlatpakDecomposed *ref,
                          gboolean           force_remove,
                          gboolean          *was_deployed_out,
                          GCancellable      *cancellable,
                          GError           **error)
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
      g_info ("undeploying %s", deployed[i]);
      if (!flatpak_dir_undeploy (self, ref, deployed[i], FALSE, force_remove, cancellable, error))
        return FALSE;
    }

  deploy_base = flatpak_dir_get_deploy_dir (self, ref);
  was_deployed = g_file_query_exists (deploy_base, cancellable);
  if (was_deployed)
    {
      g_info ("removing deploy base");
      if (!flatpak_rm_rf (deploy_base, cancellable, error))
        return FALSE;
    }

  g_info ("cleaning up empty directories");
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
flatpak_dir_remove_ref (FlatpakDir        *self,
                        const char        *remote_name,
                        const char        *ref, /* NOTE: Not necessarily a app/runtime ref */
                        GCancellable      *cancellable,
                        GError           **error)
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
          g_autoptr(GError) tmp_error = NULL;
          if (!flatpak_rm_rf (child, cancellable, &tmp_error))
            g_warning ("Unable to remove old checkout: %s", tmp_error->message);
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
          g_info ("Skipping prune due to in progress operation");
          return TRUE;
        }

      g_propagate_error (error, g_steal_pointer (&lock_error));
      return FALSE;
    }

  g_info ("Pruning repo");
  if (!ostree_repo_prune (self->repo,
                          OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY,
                          0,
                          &objects_total,
                          &objects_pruned,
                          &pruned_object_size_total,
                          cancellable, error))
    goto out;

  formatted_freed_size = g_format_size_full (pruned_object_size_total, 0);
  g_info ("Pruned %d/%d objects, size %s", objects_total, objects_pruned, formatted_freed_size);

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
                            gboolean      delete,
                            GCancellable *cancellable,
                            GError      **error)
{
  if (flatpak_dir_use_system_helper (self, NULL))
    {
      const char *installation = flatpak_dir_get_id (self);

      return flatpak_dir_system_helper_call_update_summary (self,
                                                            delete ? FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_DELETE
                                                                   : FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_NONE,
                                                            installation ? installation : "",
                                                            cancellable,
                                                            error);
    }

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return FALSE;

  if (delete)
    {
      g_autoptr(GError) local_error = NULL;
      g_autoptr(GFile) summary_file = NULL;

      g_info ("Deleting summary");

      summary_file = g_file_get_child (ostree_repo_get_path (self->repo), "summary");

      if (!g_file_delete (summary_file, cancellable, &local_error) &&
          !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
      return TRUE;
    }
  else
    {
      g_auto(GLnxLockFile) lock = { 0, };

      g_info ("Updating summary");

      /* Keep a shared repo lock to avoid prunes removing objects we're relying on
       * while generating the summary. */
      if (!flatpak_dir_repo_lock (self, &lock, LOCK_SH, cancellable, error))
        return FALSE;

      return ostree_repo_regenerate_summary (self->repo, NULL, cancellable, error);
    }
}

GFile *
flatpak_dir_get_if_deployed (FlatpakDir        *self,
                             FlatpakDecomposed *ref,
                             const char        *checksum,
                             GCancellable      *cancellable)
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
      g_autofree char *id = flatpak_decomposed_dup_id (ref);
      g_autofree char *dirname = NULL;

      dirname = g_strdup_printf ("%s-%s", id, checksum);
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

static void
remote_filter_free (RemoteFilter *remote_filter)
{
  g_free (remote_filter->checksum);
  g_object_unref (remote_filter->path);
  if (remote_filter->allow)
    g_regex_unref (remote_filter->allow);
  if (remote_filter->deny)
    g_regex_unref (remote_filter->deny);

  g_free (remote_filter);
}


static RemoteFilter *
remote_filter_load (GFile *path, GError **error)
{
  RemoteFilter *filter;
  g_autofree char *data = NULL;
  gsize data_size;
  GTimeVal mtime;
  g_autoptr(GRegex) allow_refs = NULL;
  g_autoptr(GRegex) deny_refs = NULL;

  /* Save mtime before loading to avoid races */
  if (!get_mtime (path, &mtime, NULL, error))
    {
      glnx_prefix_error (error, _("Failed to load filter '%s'"), flatpak_file_get_path_cached (path));
      return NULL;
    }

  if (!g_file_load_contents (path, NULL, &data, &data_size, NULL, error))
    {
      glnx_prefix_error (error, _("Failed to load filter '%s'"), flatpak_file_get_path_cached (path));
      return NULL;
    }

  if (!flatpak_parse_filters (data, &allow_refs, &deny_refs, error))
    {
      glnx_prefix_error (error, _("Failed to parse filter '%s'"), flatpak_file_get_path_cached (path));
      return NULL;
    }

  filter = g_new0 (RemoteFilter, 1);
  filter->checksum = g_compute_checksum_for_data (G_CHECKSUM_SHA256, (guchar *)data, data_size);
  filter->path = g_object_ref (path);
  filter->mtime = mtime;
  filter->last_mtime_check = g_get_monotonic_time ();
  filter->allow = g_steal_pointer (&allow_refs);
  filter->deny = g_steal_pointer (&deny_refs);

  return filter;
}

G_LOCK_DEFINE_STATIC (filters);

static gboolean
flatpak_dir_lookup_remote_filter (FlatpakDir *self,
                                  const char *name,
                                  gboolean    force_load,
                                  char      **checksum_out,
                                  GRegex    **allow_regex,
                                  GRegex    **deny_regex,
                                  GError **error)
{
  RemoteFilter *filter = NULL;
  g_autofree char *filter_path = NULL;
  gboolean handled_fallback = FALSE;
  g_autoptr(GFile) filter_file = NULL;

  if (checksum_out)
    *checksum_out = NULL;
  *allow_regex = NULL;
  *deny_regex = NULL;

  filter_path = flatpak_dir_get_remote_filter (self, name);

  if (filter_path == NULL)
    return TRUE;

  filter_file = g_file_new_for_path (filter_path);

  G_LOCK (filters);

  if (self->remote_filters == NULL)
    self->remote_filters = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) remote_filter_free);

  filter = g_hash_table_lookup (self->remote_filters, name);
  if (filter)
    {
      guint64 now = g_get_monotonic_time ();
      GTimeVal mtime;

      if (g_file_equal (filter->path, filter_file) != 0)
        filter = NULL; /* New path, reload */
      else if ((now - filter->last_mtime_check) > (1000 * (FILTER_MTIME_CHECK_TIMEOUT_MSEC)))
        {
          /* Fall back to backup copy if remote filter disappears */
          handled_fallback = TRUE;
          if (!g_file_query_exists (filter_file, NULL))
            {
              g_autofree char *basename = g_strconcat (name, ".filter", NULL);
              g_object_unref (filter_file);
              filter_file = flatpak_build_file (self->basedir, "repo", basename, NULL);
            }

          filter->last_mtime_check = now;
          if (!get_mtime (filter_file, &mtime, NULL, NULL) ||
              mtime.tv_sec != filter->mtime.tv_sec ||
              mtime.tv_usec != filter->mtime.tv_usec)
            filter = NULL; /* Different mtime, reload */
        }
    }

  if (filter)
    {
      if (checksum_out)
        *checksum_out = g_strdup (filter->checksum);
      if (filter->allow)
        *allow_regex = g_regex_ref (filter->allow);
      if (filter->deny)
        *deny_regex = g_regex_ref (filter->deny);
    }

  G_UNLOCK (filters);

  if (filter) /* This is outside the lock, but we already copied the returned data, and we're not dereferencing filter */
    return TRUE;

  /* Fall back to backup copy if remote filter disappears */
  if (!handled_fallback && !g_file_query_exists (filter_file, NULL))
    {
      g_autofree char *basename = g_strconcat (name, ".filter", NULL);
      g_object_unref (filter_file);
      filter_file = flatpak_build_file (self->basedir, "repo", basename, NULL);
    }

  filter = remote_filter_load (filter_file, error);
  if (filter == NULL)
    return FALSE;

  if (checksum_out)
    *checksum_out = g_strdup (filter->checksum);
  if (filter->allow)
    *allow_regex = g_regex_ref (filter->allow);
  if (filter->deny)
    *deny_regex = g_regex_ref (filter->deny);

  G_LOCK (filters);
  g_hash_table_replace (self->remote_filters, g_strdup (name), filter);
  G_UNLOCK (filters);

  return TRUE;
}

G_LOCK_DEFINE_STATIC (cache);

/* FIXME: Move all this caching into libostree. */
static void
cached_summary_free (CachedSummary *summary)
{
  g_bytes_unref (summary->bytes);
  if (summary->bytes_sig)
    g_bytes_unref (summary->bytes_sig);
  g_free (summary->name);
  g_free (summary->url);
  g_free (summary);
}

static CachedSummary *
cached_summary_new (GBytes     *bytes,
                    GBytes     *bytes_sig,
                    const char *name,
                    const char *url)
{
  CachedSummary *summary = g_new0 (CachedSummary, 1);

  summary->bytes = g_bytes_ref (bytes);
  if (bytes_sig)
    summary->bytes_sig = g_bytes_ref (bytes_sig);
  summary->url = g_strdup (url);
  summary->name = g_strdup (name);
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
      if ((now - summary->time) / G_USEC_PER_SEC < SUMMARY_CACHE_TIMEOUT_SEC &&
          strcmp (url, summary->url) == 0)
        {
          /* g_info ("Using cached summary for remote %s", name); */
          *bytes_out = g_bytes_ref (summary->bytes);
          if (bytes_sig_out)
            {
              if (summary->bytes_sig)
                *bytes_sig_out = g_bytes_ref (summary->bytes_sig);
              else
                *bytes_sig_out = NULL;
            }
          res = TRUE;

          /* Bump the cache expiry time */
          summary->time = now;
        }
      else
        {
          /* Timed out or URL has changed; remove the entry */
          g_hash_table_remove (self->summary_cache, name);
          res = FALSE;
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
  g_hash_table_replace (self->summary_cache, summary->name, summary);

  G_UNLOCK (cache);
}

gboolean
flatpak_dir_remote_make_oci_summary (FlatpakDir   *self,
                                     const char   *remote,
                                     gboolean      only_cached,
                                     GBytes      **out_summary,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  g_autoptr(GVariant) summary = NULL;
  g_autoptr(GFile) index_cache = NULL;
  g_autofree char *index_uri = NULL;
  g_autoptr(GFile) summary_cache = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GMappedFile) mfile = NULL;
  g_autoptr(GBytes) cache_bytes = NULL;
  g_autoptr(GBytes) summary_bytes = NULL;

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      const char *installation = flatpak_dir_get_id (self);
      FlatpakHelperGenerateOciSummaryFlags flags = FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_NONE;

      if (only_cached)
        flags |= FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_ONLY_CACHED;

      if (!flatpak_dir_system_helper_call_generate_oci_summary (self,
                                                                flags,
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
      index_cache = flatpak_dir_update_oci_index (self, remote, &index_uri, cancellable, error);
      if (index_cache == NULL)
        return FALSE;

      summary_cache = flatpak_dir_get_oci_summary_location (self, remote, error);
      if (summary_cache == NULL)
        return FALSE;

      if (!only_cached && !check_destination_mtime (index_cache, summary_cache, cancellable))
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
        {
          if (only_cached)
            {
              g_clear_error (error);
              g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_CACHED,
                           _("No oci summary cached for remote '%s'"), remote);
            }

          return FALSE;
        }

      cache_bytes = g_mapped_file_get_bytes (mfile);
      *out_summary = g_steal_pointer (&cache_bytes);
    }

  return TRUE;
}

typedef struct {
  char *filename;
  gint64 mtime;
} CachedSummaryData;

static void
cached_summary_data_free (CachedSummaryData *data)
{
  g_free (data->filename);
  g_free (data);
}

static gboolean
flatpak_dir_gc_cached_digested_summaries (FlatpakDir   *self,
                                          const char   *remote_name,
                                          const char   *dont_prune_file,
                                          GCancellable *cancellable,
                                          GError      **error)
{
  g_autoptr(GHashTable) cached_data_for_arch = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)cached_summary_data_free);
  g_autoptr(GFile) cache_dir = flatpak_build_file (self->cache_dir, "summaries", NULL);
  g_auto(GLnxDirFdIterator) iter = {0};
  struct dirent *dent;
  g_autoptr(GError) local_error = NULL;

  if (!glnx_dirfd_iterator_init_at (AT_FDCWD, flatpak_file_get_path_cached (cache_dir), FALSE, &iter, &local_error))
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return TRUE;

      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  g_autofree char *prefix = g_strconcat (remote_name, "-", NULL);

  while (TRUE)
    {
      struct stat stbuf;
      const char *arch_start, *arch_end;
      g_autofree char *arch = NULL;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&iter, &dent, cancellable, error))
        return FALSE;

      if (dent == NULL)
        break;

      /* Cached are regular file named "${remote-name}-${arch}-${sha256}.sub", ignore anything else */
      if (dent->d_type != DT_REG ||
          !g_str_has_prefix (dent->d_name, prefix) ||
          !g_str_has_suffix (dent->d_name, ".sub"))
        continue;

      arch_start = dent->d_name + strlen (prefix);
      arch_end = strchr (arch_start, '-');
      if (arch_end == NULL)
        continue;

      /* Keep the latest subsummary for each remote-name + arch so we can use it for deltas */
      if (fstatat (iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }

      arch = g_strndup (arch_start, arch_end - arch_start);

      CachedSummaryData *old_data = g_hash_table_lookup (cached_data_for_arch, arch);
      if (old_data == NULL || stbuf.st_mtime > old_data->mtime)
        {
          CachedSummaryData *new_data;

          if (old_data &&
              strcmp (dont_prune_file, old_data->filename) != 0 &&
              unlinkat (iter.fd, old_data->filename, 0) != 0)
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }

          new_data = g_new0 (CachedSummaryData, 1);
          new_data->filename = g_strdup (dent->d_name);
          new_data->mtime = stbuf.st_mtime;
          g_hash_table_insert (cached_data_for_arch, g_steal_pointer (&arch), new_data);
        }
      else /* stbuf.st_mtime <= old_data->mtime */
        {
          if (stbuf.st_mtime < old_data->mtime &&
              strcmp (dont_prune_file, dent->d_name) != 0 &&
              unlinkat (iter.fd, dent->d_name, 0) != 0)
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }
        }
    }

  return TRUE;
}

static gboolean
_flatpak_dir_remote_clear_cached_summary (FlatpakDir   *self,
                                          const char   *remote,
                                          const char   *extension,
                                          GCancellable *cancellable,
                                          GError      **error)
{
  g_autoptr(GFile) cache_dir = flatpak_build_file (self->cache_dir, "summaries", NULL);
  g_autofree char *filename = g_strconcat (remote, extension, NULL);
  g_autoptr(GFile) file = flatpak_build_file (cache_dir, filename, NULL);
  g_autoptr(GError) local_error = NULL;

  if (!g_file_delete (file, NULL, &local_error) &&
      !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  return TRUE;
}

static gboolean
flatpak_dir_remote_clear_cached_summary (FlatpakDir   *self,
                                         const char   *remote,
                                         GCancellable *cancellable,
                                         GError      **error)
{
  g_info ("Clearing cached summaries for remote %s", remote);
  if (!_flatpak_dir_remote_clear_cached_summary (self, remote, NULL, cancellable, error))
    return FALSE;
  if (!_flatpak_dir_remote_clear_cached_summary (self, remote, ".sig", cancellable, error))
    return FALSE;
  if (!_flatpak_dir_remote_clear_cached_summary (self, remote, ".idx", cancellable, error))
    return FALSE;
  if (!_flatpak_dir_remote_clear_cached_summary (self, remote, ".idx.sig", cancellable, error))
    return FALSE;
  return TRUE;
}


static gboolean
flatpak_dir_remote_save_cached_summary (FlatpakDir   *self,
                                        const char   *basename,
                                        const char   *main_ext,
                                        const char   *sig_ext,
                                        GBytes       *main,
                                        GBytes       *sig,
                                        GCancellable *cancellable,
                                        GError      **error)
{
  g_autofree char *main_file_name = g_strconcat (basename, main_ext, NULL);
  g_autofree char *sig_file_name = g_strconcat (basename, sig_ext, NULL);
  g_autoptr(GFile) cache_dir = flatpak_build_file (self->cache_dir, "summaries", NULL);
  g_autoptr(GFile) main_cache_file = flatpak_build_file (cache_dir, main_file_name, NULL);
  g_autoptr(GFile) sig_cache_file = flatpak_build_file (cache_dir, sig_file_name, NULL);
  g_autoptr(GError) local_error = NULL;

  if (!flatpak_mkdir_p (cache_dir, cancellable, error))
    return FALSE;

  if (!g_file_replace_contents (main_cache_file, g_bytes_get_data (main, NULL), g_bytes_get_size (main), NULL, FALSE,
                                G_FILE_CREATE_REPLACE_DESTINATION, NULL, cancellable, error))
    return FALSE;

  if (sig_ext)
    {
      if (sig)
        {
          if (!g_file_replace_contents (sig_cache_file, g_bytes_get_data (sig, NULL), g_bytes_get_size (sig), NULL, FALSE,
                                        G_FILE_CREATE_REPLACE_DESTINATION, NULL, cancellable, error))
            return FALSE;
        }
      else
        {
          if (!g_file_delete (sig_cache_file, NULL, &local_error) &&
              !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }
        }
    }

  return TRUE;
}

static gboolean
flatpak_dir_remote_load_cached_summary (FlatpakDir   *self,
                                        const char   *basename,
                                        const char   *checksum,
                                        const char   *main_ext,
                                        const char   *sig_ext,
                                        GBytes      **out_main,
                                        GBytes      **out_sig,
                                        GCancellable *cancellable,
                                        GError      **error)
{
  g_autofree char *main_file_name = g_strconcat (basename, main_ext, NULL);
  g_autofree char *sig_file_name = g_strconcat (basename, sig_ext, NULL);
  g_autoptr(GFile) main_cache_file = flatpak_build_file (self->cache_dir, "summaries", main_file_name, NULL);
  g_autoptr(GFile) sig_cache_file = flatpak_build_file (self->cache_dir, "summaries", sig_file_name, NULL);
  g_autoptr(GMappedFile) mfile = NULL;
  g_autoptr(GMappedFile) sig_mfile = NULL;
  g_autoptr(GBytes) mfile_bytes = NULL;
  g_autofree char *sha256 = NULL;

  mfile = g_mapped_file_new (flatpak_file_get_path_cached (main_cache_file), FALSE, NULL);
  if (mfile == NULL)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_CACHED,
                   _("No cached summary for remote '%s'"), basename);
      return FALSE;
    }

  if (out_sig)
    sig_mfile = g_mapped_file_new (flatpak_file_get_path_cached (sig_cache_file), FALSE, NULL);

  mfile_bytes = g_mapped_file_get_bytes (mfile);

  /* The checksum would've already been verified before the file was written,
   * but check again in case something went wrong during disk I/O. This is
   * especially important since the variant-schema-compiler code assumes the
   * GVariant data is well formed and asserts otherwise.
   */
  if (checksum != NULL)
    {
      sha256 = g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, mfile_bytes);
      if (strcmp (sha256, checksum) != 0)
        {
          g_autoptr(GError) local_error = NULL;

          if (!g_file_delete (main_cache_file, NULL, &local_error) &&
              !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_autofree char *path = g_file_get_path (main_cache_file);
              g_info ("Unable to delete file %s: %s", path, local_error->message);
              g_clear_error (&local_error);
            }

          if (sig_ext)
            {
              if (!g_file_delete (sig_cache_file, NULL, &local_error) &&
                  !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
                {
                  g_autofree char *path = g_file_get_path (sig_cache_file);
                  g_info ("Unable to delete file %s: %s", path, local_error->message);
                  g_clear_error (&local_error);
                }
            }

          return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA,
                                     _("Invalid checksum for indexed summary %s read from %s"),
                                     checksum, flatpak_file_get_path_cached (main_cache_file));
        }
    }

  *out_main = g_steal_pointer (&mfile_bytes);
  if (sig_mfile)
    *out_sig = g_mapped_file_get_bytes (sig_mfile);

  return TRUE;
}

static gboolean
flatpak_dir_remote_fetch_summary (FlatpakDir   *self,
                                  const char   *name_or_uri,
                                  gboolean      only_cached,
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

  /* Seems ostree asserts if this is NULL */
  if (error == NULL)
    error = &local_error;

  if (flatpak_dir_get_remote_oci (self, name_or_uri))
    {
      if (!flatpak_dir_remote_make_oci_summary (self, name_or_uri,
                                                only_cached,
                                                &summary,
                                                cancellable,
                                                error))
        return FALSE;
    }
  else
    {
      if (only_cached)
        {
          if (!flatpak_dir_remote_load_cached_summary (self, name_or_uri, NULL, NULL, ".sig",
                                                       &summary, &summary_sig, cancellable, error))
            return FALSE;
          g_info ("Loaded summary from cache for remote ‘%s’", name_or_uri);
        }
      else
        {
          g_info ("Fetching summary file for remote ‘%s’", name_or_uri);
          if (!ostree_repo_remote_fetch_summary (self->repo, name_or_uri,
                                                 &summary, &summary_sig,
                                                 cancellable,
                                                 error))
            return FALSE;
        }
    }

  if (summary == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Remote listing for %s not available; server has no summary file. Check the URL passed to remote-add was valid."), name_or_uri);

  if (!is_local && !only_cached)
    {
      g_autofree char *cache_key = g_strconcat ("summary-", name_or_uri, NULL);
      flatpak_dir_cache_summary (self, summary, summary_sig, cache_key, url);
    }

  *out_summary = g_steal_pointer (&summary);
  if (out_summary_sig)
    *out_summary_sig = g_steal_pointer (&summary_sig);

  return TRUE;
}

static gboolean
remote_verify_signature (OstreeRepo *repo,
                         const char *remote_name,
                         GBytes *data,
                         GBytes *sig_file,
                         GCancellable *cancellable,
                         GError **error)
{
  g_autoptr(GVariant) signatures_variant = NULL;
  g_autoptr(GVariant) signaturedata = NULL;
  g_autoptr (GBytes) signatures = NULL;
  g_autoptr(GByteArray) buffer = NULL;
  g_autoptr(OstreeGpgVerifyResult) verify_result = NULL;
  GVariantIter iter;
  GVariant *child;

  signatures_variant = g_variant_new_from_bytes (OSTREE_SUMMARY_SIG_GVARIANT_FORMAT,
                                                 sig_file, FALSE);
  signaturedata = g_variant_lookup_value (signatures_variant, "ostree.gpgsigs", G_VARIANT_TYPE ("aay"));
  if (signaturedata == NULL)
    {
      g_set_error_literal (error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_NO_SIGNATURE,
                           "GPG verification enabled, but no signatures found (use gpg-verify=false in remote config to disable)");
      return FALSE;
    }

  buffer = g_byte_array_new ();
  g_variant_iter_init (&iter, signaturedata);
  while ((child = g_variant_iter_next_value (&iter)) != NULL)
    {
      g_byte_array_append (buffer,
                           g_variant_get_data (child),
                           g_variant_get_size (child));
      g_variant_unref (child);
    }
  signatures = g_byte_array_free_to_bytes (g_steal_pointer (&buffer));

  verify_result = ostree_repo_gpg_verify_data (repo,
                                               remote_name,
                                               data,
                                               signatures,
                                               NULL, NULL,
                                               cancellable, error);
  if (!ostree_gpg_verify_result_require_valid_signature (verify_result, error))
    return FALSE;

  return TRUE;
}

static GBytes *
load_uri_with_fallback (FlatpakHttpSession    *http_session,
                        const char            *uri,
                        const char            *uri2,
                        FlatpakHTTPFlags       flags,
                        const char            *token,
                        GCancellable          *cancellable,
                        GError               **error)
{
  g_autoptr(GError) local_error = NULL;
  GBytes *res;

  res = flatpak_load_uri (http_session, uri, flags, token,
                          NULL, NULL, NULL,
                          cancellable, &local_error);
  if (res)
    return res;

  if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  return flatpak_load_uri (http_session, uri2, flags, token,
                           NULL, NULL, NULL,
                           cancellable, error);
}

static gboolean
flatpak_dir_remote_fetch_summary_index (FlatpakDir   *self,
                                        const char   *name_or_uri,
                                        gboolean      only_cached,
                                        GBytes      **out_index,
                                        GBytes      **out_index_sig,
                                        GCancellable *cancellable,
                                        GError      **error)
{
  g_autofree char *url = NULL;
  gboolean is_local;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GError) cache_error = NULL;
  g_autoptr(GBytes) cached_index = NULL;
  g_autoptr(GBytes) cached_index_sig = NULL;
  g_autoptr(GBytes) index = NULL;
  g_autoptr(GBytes) index_sig = NULL;
  gboolean gpg_verify_summary;

  ensure_http_session (self);

  if (!ostree_repo_remote_get_url (self->repo, name_or_uri, &url, error))
    return FALSE;

  if (!ostree_repo_remote_get_gpg_verify_summary (self->repo, name_or_uri, &gpg_verify_summary, NULL))
    return FALSE;

  if (!g_str_has_prefix (name_or_uri, "file:") && flatpak_dir_get_remote_disabled (self, name_or_uri))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Can't fetch summary from disabled remote ‘%s’", name_or_uri);
      return FALSE;
    }

  if (flatpak_dir_get_remote_oci (self, name_or_uri))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No index in OCI remote ‘%s’", name_or_uri);
      return FALSE;
    }

  is_local = g_str_has_prefix (url, "file:");

  /* Seems ostree asserts if this is NULL */
  if (error == NULL)
    error = &local_error;

  flatpak_dir_remote_load_cached_summary (self, name_or_uri, NULL, ".idx", ".idx.sig",
                                          &cached_index, &cached_index_sig, cancellable, &cache_error);

  if (only_cached)
    {
      if (cached_index == NULL)
        {
          g_propagate_error (error, g_steal_pointer (&cache_error));
          return FALSE;
        }
      g_info ("Loaded summary index from cache for remote ‘%s’", name_or_uri);

      index = g_steal_pointer (&cached_index);
      if (gpg_verify_summary)
        index_sig = g_steal_pointer (&cached_index_sig);
    }
  else
    {
      g_autofree char *index_url = g_build_filename (url, "summary.idx", NULL);
      g_autoptr(GBytes) dl_index = NULL;
      gboolean used_download = FALSE;

      g_info ("Fetching summary index file for remote ‘%s’", name_or_uri);

      dl_index = flatpak_load_uri (self->http_session, index_url, 0, NULL,
                                   NULL, NULL, NULL,
                                   cancellable, error);
      if (dl_index == NULL)
        return FALSE;

      /* If the downloaded index is the same as the cached one we need not re-download or
       * re-verify, just use the cache (which we verified before) */
      if (cached_index != NULL && g_bytes_equal (cached_index, dl_index))
        {
          index = g_steal_pointer (&cached_index);
          if (gpg_verify_summary)
            index_sig = g_steal_pointer (&cached_index_sig);
        }
      else
        {
          index = g_steal_pointer (&dl_index);
          used_download = TRUE;
        }

      if (gpg_verify_summary && index_sig == NULL)
        {
          g_autofree char *index_digest = g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, index);
          g_autofree char *index_sig_filename = g_strconcat (index_digest, ".idx.sig", NULL);
          g_autofree char *index_sig_url = g_build_filename (url, "summaries", index_sig_filename, NULL);
          g_autofree char *index_sig_url2 = g_build_filename (url, "summary.idx.sig", NULL);
          g_autoptr(GError) dl_sig_error = NULL;
          g_autoptr (GBytes) dl_index_sig = NULL;

          dl_index_sig = load_uri_with_fallback (self->http_session, index_sig_url, index_sig_url2, 0, NULL,
                                                 cancellable, &dl_sig_error);
          if (dl_index_sig == NULL)
            {
              if (g_error_matches (dl_sig_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
                g_set_error (error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_NO_SIGNATURE,
                             "GPG verification enabled, but no summary signatures found (use gpg-verify-summary=false in remote config to disable)");
              else
                g_propagate_error (error, g_steal_pointer (&dl_sig_error));

              return FALSE;
            }

          if (!remote_verify_signature (self->repo, name_or_uri,
                                        index, dl_index_sig,
                                        cancellable, error))
            return FALSE;

          index_sig = g_steal_pointer (&dl_index_sig);
          used_download = TRUE;
        }

      g_assert (index != NULL);
      if (gpg_verify_summary)
        g_assert (index_sig != NULL);

      /* Update cache on disk if we downloaded anything, but never cache for file: repos */
      if (used_download && !is_local &&
          !flatpak_dir_remote_save_cached_summary (self, name_or_uri, ".idx", ".idx.sig",
                                                   index, index_sig, cancellable, error))
        return FALSE;
    }

  /* Cache in memory */
  if (!is_local && !only_cached)
    {
      g_autofree char *cache_key = g_strconcat ("index-", name_or_uri, NULL);
      flatpak_dir_cache_summary (self, index, index_sig, cache_key, url);
    }

  *out_index = g_steal_pointer (&index);
  if (out_index_sig)
    *out_index_sig = g_steal_pointer (&index_sig);

  return TRUE;
}

static gboolean
flatpak_dir_remote_fetch_indexed_summary (FlatpakDir   *self,
                                          const char   *name_or_uri,
                                          const char   *arch,
                                          GVariant     *subsummary_info_v,
                                          gboolean      only_cached,
                                          GBytes      **out_summary,
                                          GCancellable *cancellable,
                                          GError      **error)
{
  g_autofree char *url = NULL;
  gboolean is_local;
  g_autoptr(GError) cache_error = NULL;
  g_autoptr(GBytes) summary_z = NULL;
  g_autoptr(GBytes) summary = NULL;
  g_autofree char *sha256 = NULL;
  VarSubsummaryRef subsummary_info;
  gsize checksum_bytes_len;
  const guchar *checksum_bytes;
  g_autofree char *checksum = NULL;
  g_autofree char *cache_name = NULL;

  ensure_http_session (self);

  if (!ostree_repo_remote_get_url (self->repo, name_or_uri, &url, error))
    return FALSE;

  if (!g_str_has_prefix (name_or_uri, "file:") && flatpak_dir_get_remote_disabled (self, name_or_uri))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Can't fetch summary from disabled remote ‘%s’", name_or_uri);
      return FALSE;
    }

  subsummary_info = var_subsummary_from_gvariant (subsummary_info_v);
  checksum_bytes = var_subsummary_peek_checksum (subsummary_info, &checksum_bytes_len);
  g_assert (checksum_bytes_len == OSTREE_SHA256_DIGEST_LEN); /* We verified this when scanning index */
  checksum = ostree_checksum_from_bytes (checksum_bytes);

  is_local = g_str_has_prefix (url, "file:");

  /* No in-memory caching for local files */
  if (!is_local)
    {
      if (flatpak_dir_lookup_cached_summary (self, out_summary, NULL, checksum, url))
        return TRUE;
    }

  cache_name = g_strconcat (name_or_uri, "-", arch, "-", checksum, NULL);

  /* First look for an on-disk cache */
  if (!flatpak_dir_remote_load_cached_summary (self, cache_name, checksum, ".sub", NULL,
                                               &summary, NULL, cancellable, &cache_error))
    {
      g_autofree char *old_checksum = NULL;
      g_autoptr(GBytes) old_summary = NULL;

      /* Else fetch it */
      if (only_cached)
        {
          g_propagate_error (error, g_steal_pointer (&cache_error));
          return FALSE;
        }

      /* Warn if the on-disk cache is corrupt; perhaps the write was interrupted? */
      if (g_error_matches (cache_error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_DATA))
        g_warning ("%s", cache_error->message);

      /* Look for first applicable deltas */
      VarArrayofChecksumRef history = var_subsummary_get_history (subsummary_info);
      gsize history_len = var_arrayof_checksum_get_length (history);
      for (gsize i = 0; i < history_len; i++)
        {
          VarChecksumRef old = var_arrayof_checksum_get_at (history, i);
          g_autofree char *old_cache_name = NULL;

          if (var_checksum_get_length (old) != OSTREE_SHA256_DIGEST_LEN)
            continue;

          old_checksum = ostree_checksum_from_bytes (var_checksum_peek (old));
          old_cache_name = g_strconcat (name_or_uri, "-", arch, "-", old_checksum, NULL);
          if (flatpak_dir_remote_load_cached_summary (self, old_cache_name, old_checksum, ".sub", NULL,
                                                      &old_summary, NULL, cancellable, NULL))
            break;
        }

      if (old_summary)
        {
          g_autoptr(GError) delta_error = NULL;

          g_autofree char *delta_filename = g_strconcat (old_checksum, "-", checksum, ".delta", NULL);
          g_autofree char *delta_url = g_build_filename (url, "summaries", delta_filename, NULL);

          g_info ("Fetching indexed summary delta %s for remote ‘%s’", delta_filename, name_or_uri);

          g_autoptr(GBytes) delta = flatpak_load_uri (self->http_session, delta_url, 0, NULL,
                                                      NULL, NULL, NULL,
                                                      cancellable, &delta_error);
          if (delta == NULL)
            g_info ("Failed to load delta, falling back: %s", delta_error->message);
          else
            {
              g_autoptr(GBytes) applied = flatpak_summary_apply_diff (old_summary, delta, &delta_error);

              if (applied == NULL)
                g_warning ("Failed to apply delta, falling back: %s", delta_error->message);
              else
                {
                  sha256 = g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, applied);
                  if (strcmp (sha256, checksum) != 0)
                    g_warning ("Applying delta gave wrong checksum, falling back");
                  else
                    summary = g_steal_pointer (&applied);
                }
            }
        }

      if (summary == NULL)
        {
          g_autofree char *filename = g_strconcat (checksum, ".gz", NULL);
          g_info ("Fetching indexed summary file %s for remote ‘%s’", filename, name_or_uri);
          g_autofree char *subsummary_url = g_build_filename (url, "summaries", filename, NULL);
          summary_z = flatpak_load_uri (self->http_session, subsummary_url, 0, NULL,
                                        NULL, NULL, NULL,
                                        cancellable, error);
          if (summary_z == NULL)
            return FALSE;

          summary = flatpak_zlib_decompress_bytes (summary_z, error);
          if (summary == NULL)
            return FALSE;

          g_free (sha256);
          sha256 = g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, summary);
          if (strcmp (sha256, checksum) != 0)
            return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid checksum for indexed summary %s for remote '%s'"), checksum, name_or_uri);
        }

      /* Save to disk */
      if (!is_local)
        {
          if (!flatpak_dir_remote_save_cached_summary (self, cache_name, ".sub", NULL,
                                                       summary, NULL,
                                                       cancellable, error))
            return FALSE;

          if (!flatpak_dir_gc_cached_digested_summaries (self, name_or_uri, cache_name,
                                                         cancellable, error))
            return FALSE;
        }
    }
  else
    g_info ("Loaded indexed summary file %s from cache for remote ‘%s’", checksum, name_or_uri);

  /* Cache in memory */
  if (!is_local && !only_cached)
    flatpak_dir_cache_summary (self, summary, NULL, checksum, url);

  *out_summary = g_steal_pointer (&summary);

  return TRUE;
}

static FlatpakRemoteState *
_flatpak_dir_get_remote_state (FlatpakDir   *self,
                               const char   *remote_or_uri,
                               gboolean      optional,
                               gboolean      local_only,
                               gboolean      only_cached,
                               gboolean      opt_summary_is_index,
                               GBytes       *opt_summary,
                               GBytes       *opt_summary_sig,
                               GCancellable *cancellable,
                               GError      **error)
{
  g_autoptr(FlatpakRemoteState) state = flatpak_remote_state_new ();
  g_autoptr(GPtrArray) sideload_paths = NULL;
  g_autofree char *url = NULL;
  g_autoptr(GError) my_error = NULL;
  gboolean is_local;
  gboolean got_summary = FALSE;
  const char *arch = flatpak_get_default_arch ();
  g_autoptr(GBytes) index_bytes = NULL;
  g_autoptr(GBytes) index_sig_bytes = NULL;
  g_autoptr(GBytes) summary_bytes = NULL;
  g_autoptr(GBytes) summary_sig_bytes = NULL;

  if (error == NULL)
    error = &my_error;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return NULL;

  state->remote_name = g_strdup (remote_or_uri);
  state->is_file_uri = is_local = g_str_has_prefix (remote_or_uri, "file:");
  if (!is_local)
    {
      if (!flatpak_dir_has_remote (self, remote_or_uri, error))
        return NULL;
      if (!repo_get_remote_collection_id (self->repo, remote_or_uri, &state->collection_id, error))
        return NULL;
      if (!flatpak_dir_lookup_remote_filter (self, remote_or_uri, FALSE, NULL, &state->allow_refs, &state->deny_refs, error))
        return NULL;
      if (!ostree_repo_remote_get_url (self->repo, remote_or_uri, &url, error))
        return NULL;

      state->default_token_type = flatpak_dir_get_remote_default_token_type (self, remote_or_uri);
    }

  sideload_paths = flatpak_dir_get_sideload_repo_paths (self);
  for (int i = 0; i < sideload_paths->len; i++)
    flatpak_remote_state_add_sideload_repo (state, g_ptr_array_index (sideload_paths, i));

  if (local_only)
    {
      flatpak_fail (&state->summary_fetch_error, "Internal error, local_only state");
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
        }

      if (opt_summary_is_index)
        {
          if (opt_summary_sig)
            index_sig_bytes = g_bytes_ref (opt_summary_sig);
          index_bytes = g_bytes_ref (opt_summary);
        }
      else
        {
          if (opt_summary_sig)
            summary_sig_bytes = g_bytes_ref (opt_summary_sig);
          summary_bytes = g_bytes_ref (opt_summary);
        }

      got_summary = TRUE;
    }

  /* First try the memory cache. Note: No in-memory caching for local files. */
  if (!is_local)
    {
      if (!got_summary)
        {
          g_autofree char *index_cache_key = g_strconcat ("index-", remote_or_uri, NULL);
          if (flatpak_dir_lookup_cached_summary (self, &index_bytes, &index_sig_bytes, index_cache_key, url))
            got_summary = TRUE;
        }

      if (!got_summary)
        {
          g_autofree char *summary_cache_key = g_strconcat ("summary-", remote_or_uri, NULL);
          if (flatpak_dir_lookup_cached_summary (self, &summary_bytes, &summary_sig_bytes, summary_cache_key, url))
            got_summary = TRUE;
        }
    }

  /* Then look for an indexed summary on disk/network */
  if (!got_summary)
    {
      g_autoptr(GError) local_error = NULL;

      if (flatpak_dir_remote_fetch_summary_index (self, remote_or_uri, only_cached, &index_bytes, &index_sig_bytes,
                                                  cancellable, &local_error))
        {
          got_summary = TRUE;
        }
      else
        {
          if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
              !g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_CACHED))
            {
              /* We got an error other than not-found, assume we're indexed but there is some network error */
              got_summary = TRUE;
              if (optional && !g_cancellable_is_cancelled (cancellable))
                {
                  g_info ("Failed to download optional summary index: %s", local_error->message);
                  state->summary_fetch_error = g_steal_pointer (&local_error);
                }
              else
                {
                  g_propagate_error (error, g_steal_pointer (&local_error));
                  return NULL;
                }
            }
        }
    }

  if (!got_summary)
    {
      /* No index, fall back to full summary */
      g_autoptr(GError) local_error = NULL;

      if (flatpak_dir_remote_fetch_summary (self, remote_or_uri, only_cached, &summary_bytes, &summary_sig_bytes,
                                            cancellable, &local_error))
        {
          got_summary = TRUE;
        }
      else
        {
          if (optional && !g_cancellable_is_cancelled (cancellable))
            {
              g_info ("Failed to download optional summary: %s", local_error->message);
              state->summary_fetch_error = g_steal_pointer (&local_error);
            }
          else
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return NULL;
            }
        }
    }

  if (index_bytes)
    {
      state->index = g_variant_ref_sink (g_variant_new_from_bytes (FLATPAK_SUMMARY_INDEX_GVARIANT_FORMAT,
                                                                   index_bytes, FALSE));
      state->index_sig_bytes = g_steal_pointer (&index_sig_bytes);
    }
  else if (summary_bytes)
    {
      state->summary = g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT,
                                                                     summary_bytes, FALSE));
      state->summary_bytes = g_steal_pointer (&summary_bytes);
      state->summary_sig_bytes = g_steal_pointer (&summary_sig_bytes);
    }

  if (state->index)
    {
      g_autofree char *require_subset = flatpak_dir_get_remote_subset (self, state->remote_name);
      VarSummaryIndexRef index = var_summary_index_from_gvariant (state->index);
      VarSummaryIndexSubsummariesRef subsummaries = var_summary_index_get_subsummaries (index);
      gsize n_subsummaries = var_summary_index_subsummaries_get_length (subsummaries);

      state->index_ht = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);

      for (gsize i = 0; i < n_subsummaries; i++)
        {
          VarSummaryIndexSubsummariesEntryRef entry = var_summary_index_subsummaries_get_at (subsummaries, i);
          const char *name = var_summary_index_subsummaries_entry_get_key (entry);
          VarSubsummaryRef subsummary = var_summary_index_subsummaries_entry_get_value (entry);
          gsize checksum_bytes_len;
          const char *dash, *subsummary_arch;

          dash = strchr (name, '-');
          subsummary_arch = dash == NULL ? name : dash + 1;

          if (dash == NULL) /* No subset */
            {
              if (require_subset != NULL)
                continue;
            }
          else /* Subset */
            {
              if (require_subset == NULL)
                continue;
              else
                {
                  g_autofree char *subset = g_strndup (name, dash - name);
                  if (strcmp (require_subset, subset) != 0)
                    continue;
                }
            }

          var_subsummary_peek_checksum (subsummary, &checksum_bytes_len);
          if (G_UNLIKELY (checksum_bytes_len != OSTREE_SHA256_DIGEST_LEN))
            {
              g_info ("Invalid checksum for digested summary, not using cache");
              continue;
            }

          g_hash_table_insert (state->index_ht, g_strdup (subsummary_arch), var_subsummary_to_owned_gvariant (subsummary, state->index));
        }

      /* Always load default (or specified) arch subsummary. Further arches can be manually loaded with flatpak_remote_state_ensure_subsummary. */
      if (opt_summary == NULL &&
          !flatpak_remote_state_ensure_subsummary (state, self, arch, only_cached, cancellable, error))
        return NULL;
    }

  if (state->collection_id != NULL &&
      state->summary != NULL &&
      !_validate_summary_for_collection_id (state->summary, state->collection_id, error))
    return NULL;

  if (flatpak_dir_get_remote_oci (self, remote_or_uri))
    {
      state->default_token_type = 1;
    }

  if (state->summary != NULL || state->index != NULL) /* In the optional case we might not have a summary */
    {
      VarMetadataRef meta = flatpak_remote_state_get_main_metadata (state);
      VarVariantRef res;

      if (var_metadata_lookup (meta, "xa.default-token-type", NULL, &res) &&
          var_variant_is_type (res, G_VARIANT_TYPE_INT32))
        state->default_token_type = GINT32_FROM_LE (var_variant_get_int32 (res));
    }

  return g_steal_pointer (&state);
}

FlatpakRemoteState *
flatpak_dir_get_remote_state (FlatpakDir   *self,
                              const char   *remote,
                              gboolean      only_cached,
                              GCancellable *cancellable,
                              GError      **error)
{
  return _flatpak_dir_get_remote_state (self, remote, FALSE, FALSE, only_cached, FALSE, NULL, NULL, cancellable, error);
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
  return _flatpak_dir_get_remote_state (self, remote, FALSE, FALSE, FALSE, FALSE, opt_summary, opt_summary_sig, cancellable, error);
}

FlatpakRemoteState *
flatpak_dir_get_remote_state_for_index (FlatpakDir   *self,
                                        const char   *remote,
                                        GBytes       *opt_index,
                                        GBytes       *opt_index_sig,
                                        GCancellable *cancellable,
                                        GError      **error)
{
  return _flatpak_dir_get_remote_state (self, remote, FALSE, FALSE, FALSE, TRUE, opt_index, opt_index_sig, cancellable, error);
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
                                       gboolean      only_cached,
                                       GCancellable *cancellable,
                                       GError      **error)
{
  return _flatpak_dir_get_remote_state (self, remote, TRUE, FALSE, only_cached, FALSE, NULL, NULL, cancellable, error);
}


/* This doesn't do any i/o at all, just keeps track of the local details like
   remote and collection-id. Useful when doing no-pull operations */
FlatpakRemoteState *
flatpak_dir_get_remote_state_local_only (FlatpakDir   *self,
                                         const char   *remote,
                                         GCancellable *cancellable,
                                         GError      **error)
{
  return _flatpak_dir_get_remote_state (self, remote, TRUE, TRUE, FALSE, FALSE, NULL, NULL, cancellable, error);
}

static void
populate_hash_table_from_refs_map (GHashTable         *ret_all_refs,
                                   GHashTable         *ref_timestamps,
                                   VarRefMapRef        ref_map,
                                   const char         *opt_collection_id,
                                   FlatpakRemoteState *state)
{
  gsize len, i;

  len = var_ref_map_get_length (ref_map);
  for (i = 0; i < len; i++)
    {
      VarRefMapEntryRef entry = var_ref_map_get_at (ref_map, i);
      const char *ref_name = var_ref_map_entry_get_ref (entry);
      const guint8 *csum_bytes;
      gsize csum_len;
      VarRefInfoRef info;
      guint64 *new_timestamp = NULL;
      g_autoptr(FlatpakDecomposed) decomposed = NULL;

      if (!flatpak_remote_state_allow_ref (state, ref_name))
        continue;

      info = var_ref_map_entry_get_info (entry);

      csum_bytes = var_ref_info_peek_checksum (info, &csum_len);
      if (csum_len != OSTREE_SHA256_DIGEST_LEN)
        continue;

      decomposed = flatpak_decomposed_new_from_col_ref (ref_name, opt_collection_id, NULL);
      if (decomposed == NULL)
        continue;

      if (ref_timestamps)
        {
          guint64 timestamp = get_timestamp_from_ref_info (info);
          gpointer value;

          if (g_hash_table_lookup_extended (ref_timestamps, ref_name, NULL, &value))
            {
              guint64 *old_timestamp = value;
              if (*old_timestamp >= timestamp)
                continue; /* New timestamp is older, skip this commit */
            }

          new_timestamp = g_memdup2 (&timestamp, sizeof (guint64));
        }

      g_hash_table_replace (ret_all_refs, g_steal_pointer (&decomposed), ostree_checksum_from_bytes (csum_bytes));
      if (new_timestamp)
        g_hash_table_replace (ref_timestamps, g_strdup (ref_name), new_timestamp);
    }
}


/* This tries to list all available remote refs but also tries to keep
 * working when offline, so it looks in sideloaded repos. Also it uses
 * in-memory cached summaries which ostree doesn't. */
gboolean
flatpak_dir_list_all_remote_refs (FlatpakDir         *self,
                                  FlatpakRemoteState *state,
                                  GHashTable        **out_all_refs,
                                  GCancellable       *cancellable,
                                  GError            **error)
{
  g_autoptr(GHashTable) ret_all_refs = NULL;
  VarSummaryRef summary;
  VarMetadataRef exts;
  VarRefMapRef ref_map;
  VarVariantRef v;

  /* This is  ref->commit */
  ret_all_refs = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash, (GEqualFunc)flatpak_decomposed_equal, (GDestroyNotify)flatpak_decomposed_unref, g_free);

  if (state->index != NULL)
    {
      /* We're online, so report only the refs from the summary */
      GLNX_HASH_TABLE_FOREACH_KV (state->subsummaries, const char *, arch, GVariant *, subsummary)
        {
          summary = var_summary_from_gvariant (subsummary);
          ref_map = var_summary_get_ref_map (summary);

          /* NOTE: collection id is NULL here not state->collection_id, see the
           * note on flatpak_decomposed_get_collection_id()
           */
          populate_hash_table_from_refs_map (ret_all_refs, NULL, ref_map, NULL /* collection id */, state);
        }
    }
  else if (state->summary != NULL)
    {
      /* We're online, so report only the refs from the summary */
      const char *main_collection_id = NULL;

      summary = var_summary_from_gvariant (state->summary);

      exts = var_summary_get_metadata (summary);

      if (state->is_file_uri)
        {
          /* This is a local repo, generally this means we gave a file: uri to a sideload repo so
           * we can enumerate it. We special case this by also adding all the collection_ref maps,
           * with collection_id set on the decomposed refs and setting the right collection id for
           * the main ref_map.
           */
          main_collection_id = var_metadata_lookup_string (exts, "ostree.summary.collection-id", NULL);
          if (var_metadata_lookup (exts, "ostree.summary.collection-map", NULL, &v))
            {
              VarCollectionMapRef map = var_collection_map_from_variant (v);

              gsize len = var_collection_map_get_length (map);
              for (gsize i = 0; i < len; i++)
                {
                  VarCollectionMapEntryRef entry = var_collection_map_get_at (map, i);
                  const char *collection_id = var_collection_map_entry_get_key (entry);
                  ref_map = var_collection_map_entry_get_value (entry);

                  populate_hash_table_from_refs_map (ret_all_refs, NULL, ref_map, collection_id, state);
                }
            }
        }

      /* refs that match the main collection-id,
         NOTE: We only set collection id if this is a file: uri remote */
      ref_map = var_summary_get_ref_map (summary);
      populate_hash_table_from_refs_map (ret_all_refs, NULL, ref_map, main_collection_id, state);
    }
  else if (state->collection_id)
    {
      g_autoptr(GHashTable) ref_mtimes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

      /* No main summary, add just all sideloded refs, with the latest version of each checksum */

      for (int i = 0; i < state->sideload_repos->len; i++)
        {
          FlatpakSideloadState *ss = g_ptr_array_index (state->sideload_repos, i);

          summary = var_summary_from_gvariant (ss->summary);
          exts = var_summary_get_metadata (summary);

          if (var_metadata_lookup (exts, "ostree.summary.collection-map", NULL, &v))
            {
              VarCollectionMapRef map = var_collection_map_from_variant (v);

              if (var_collection_map_lookup (map, state->collection_id, NULL, &ref_map))
                populate_hash_table_from_refs_map (ret_all_refs, ref_mtimes, ref_map, NULL, state);
            }
        }
    }

  /* If no sideloaded refs, might as well return the summary error if set */
  if (g_hash_table_size (ret_all_refs) == 0 &&
      !flatpak_remote_state_ensure_summary (state, error))
    return FALSE;

  *out_all_refs = g_steal_pointer (&ret_all_refs);

  return TRUE;
}

static GPtrArray *
find_matching_refs (GHashTable           *refs,
                    const char           *opt_name,
                    const char           *opt_branch,
                    const char           *opt_default_branch,
                    const char          **valid_arches, /* NULL => any arch */
                    const char           *opt_default_arch,
                    FlatpakKinds          kinds,
                    FindMatchingRefsFlags flags,
                    GError              **error)
{
  g_autoptr(GPtrArray) matched_refs = NULL;
  g_autoptr(GError) local_error = NULL;
  gboolean found_exact_name_match = FALSE;
  gboolean found_default_branch_match = FALSE;
  gboolean found_default_arch_match = FALSE;

  if (opt_name && !(flags & FIND_MATCHING_REFS_FLAGS_FUZZY) &&
      !flatpak_is_valid_name (opt_name, -1, &local_error))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("'%s' is not a valid name: %s"), opt_name, local_error->message);
      return NULL;
    }

  if (opt_branch && !flatpak_is_valid_branch (opt_branch, -1, &local_error))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("'%s' is not a valid branch name: %s"), opt_branch, local_error->message);
      return NULL;
    }

  matched_refs = g_ptr_array_new_with_free_func ((GDestroyNotify)flatpak_decomposed_unref);

  GLNX_HASH_TABLE_FOREACH (refs, FlatpakDecomposed *, ref)
    {
      if ((flatpak_decomposed_get_kinds (ref) & kinds) == 0)
        continue;

      if (opt_name)
        {
          if ((flags & FIND_MATCHING_REFS_FLAGS_FUZZY) && !flatpak_decomposed_id_is_subref (ref))
            {
              if (!flatpak_decomposed_is_id_fuzzy (ref, opt_name))
                continue;
            }
          else
            {
              if (!flatpak_decomposed_is_id (ref, opt_name))
                continue;
           }
        }

      if (valid_arches != NULL &&
          !flatpak_decomposed_is_arches (ref, -1, valid_arches))
        continue;

      if (opt_branch != NULL && !flatpak_decomposed_is_branch (ref, opt_branch))
        continue;

      if (opt_name != NULL && flatpak_decomposed_is_id (ref, opt_name))
        found_exact_name_match = TRUE;

      if (opt_default_arch != NULL && flatpak_decomposed_is_arch (ref, opt_default_arch))
        found_default_arch_match = TRUE;

      if (opt_default_branch != NULL && flatpak_decomposed_is_branch (ref, opt_default_branch))
        found_default_branch_match = TRUE;

      g_ptr_array_add (matched_refs, flatpak_decomposed_ref (ref));
    }

  /* Don't show fuzzy matches if we found at least one exact name match, and
   * enforce the default arch/branch */
  if (found_exact_name_match || found_default_arch_match || found_default_branch_match)
    {
      guint i;

      /* Walk through the array backwards so we can safely remove */
      for (i = matched_refs->len; i > 0; i--)
        {
          FlatpakDecomposed *matched_ref = g_ptr_array_index (matched_refs, i - 1);

          if (found_exact_name_match && !flatpak_decomposed_is_id (matched_ref, opt_name))
            g_ptr_array_remove_index (matched_refs, i - 1);
          else if (found_default_arch_match && !flatpak_decomposed_is_arch (matched_ref, opt_default_arch))
            g_ptr_array_remove_index (matched_refs, i - 1);
          else if (found_default_branch_match && !flatpak_decomposed_is_branch (matched_ref, opt_default_branch))
            g_ptr_array_remove_index (matched_refs, i - 1);
        }
    }

  return g_steal_pointer (&matched_refs);
}

static GPtrArray *
get_refs_for_arch (GPtrArray *refs, const char *arch)
{
  g_autoptr(GPtrArray) arched_refs = g_ptr_array_new_with_free_func ((GDestroyNotify)flatpak_decomposed_unref);

  for (int i = 0; i < refs->len; i++)
    {
      FlatpakDecomposed *ref = g_ptr_array_index (refs, i);
      if (flatpak_decomposed_is_arch (ref, arch))
        g_ptr_array_add (arched_refs, flatpak_decomposed_ref (ref));
    }

  return g_steal_pointer (&arched_refs);
}

static gpointer
fail_multiple_refs (GError    **error,
                    const char *name,
                    GPtrArray  *refs)
{
  g_autoptr(GString) err = g_string_new ("");

  g_string_printf (err, _("Multiple branches available for %s, you must specify one of: "), name);
  g_ptr_array_sort (refs, flatpak_strcmp0_ptr);

  for (int i = 0; i < refs->len; i++)
    {
      FlatpakDecomposed *ref = g_ptr_array_index (refs, i);
      if (i != 0)
        g_string_append (err, ", ");

      g_string_append (err, flatpak_decomposed_get_pref (ref));
    }
  flatpak_fail (error, "%s", err->str);

  return NULL;
}

static FlatpakDecomposed *
find_matching_ref (GHashTable  *refs,
                   const char  *name,
                   const char  *opt_branch,
                   const char  *opt_default_branch,
                   const char **valid_arches, /* NULL => any arch */
                   const char  *opt_default_arch,
                   FlatpakKinds kinds,
                   GError     **error)
{
  g_autoptr(GPtrArray) matched_refs = NULL;

  matched_refs = find_matching_refs (refs,
                                     name,
                                     opt_branch,
                                     opt_default_branch,
                                     valid_arches,
                                     opt_default_arch,
                                     kinds,
                                     FIND_MATCHING_REFS_FLAGS_NONE,
                                     error);
  if (matched_refs == NULL)
    return NULL;

  if (valid_arches != NULL)
    {
      /* Filter by valid, arch. We stop at the first arch (in prio order) that has a match */
      for (int i = 0; valid_arches[i] != NULL; i++)
        {
          const char *arch = valid_arches[i];

          g_autoptr(GPtrArray) arched_refs = get_refs_for_arch (matched_refs, arch);

          if (arched_refs->len == 1)
            return flatpak_decomposed_ref (g_ptr_array_index (arched_refs, 0));

          if (arched_refs->len > 1)
            return fail_multiple_refs (error, name, arched_refs);
        }
    }
  else
    {
      if (matched_refs->len == 1)
        return flatpak_decomposed_ref (g_ptr_array_index (matched_refs, 0));

      if (matched_refs->len > 1)
        return fail_multiple_refs (error, name, matched_refs);
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

/* This tries to find all available refs based on the specified name/branch/arch
 * triplet from  a remote. If arch is not specified, matches only on compatible arches.
*/
GPtrArray *
flatpak_dir_find_remote_refs (FlatpakDir           *self,
                              FlatpakRemoteState   *state,
                              const char           *name,
                              const char           *opt_branch,
                              const char           *opt_default_branch,
                              const char           *opt_arch,
                              const char           *opt_default_arch,
                              FlatpakKinds          kinds,
                              FindMatchingRefsFlags flags,
                              GCancellable         *cancellable,
                              GError              **error)
{
  g_autoptr(GHashTable) remote_refs = NULL;
  g_autoptr(GPtrArray) matched_refs = NULL;
  const char **valid_arches = flatpak_get_arches ();
  const char *opt_arches[] = {opt_arch, NULL};

  if (opt_arch != NULL)
    valid_arches = opt_arches;

  if (!flatpak_dir_list_all_remote_refs (self, state,
                                         &remote_refs, cancellable, error))
    return NULL;

  matched_refs = find_matching_refs (remote_refs,
                                     name,
                                     opt_branch,
                                     opt_default_branch,
                                     valid_arches,
                                     opt_default_arch,
                                     kinds,
                                     flags,
                                     error);
  if (matched_refs == NULL)
    return NULL;

  /* If we can't match anything and we had an error downloading (offline?), report that as its more helpful */
  if (matched_refs->len == 0 && state->summary_fetch_error)
    {
      g_propagate_error (error, g_error_copy (state->summary_fetch_error));
      return NULL;
    }

  return g_steal_pointer (&matched_refs);
}

static FlatpakDecomposed *
find_ref_for_refs_set (GHashTable   *refs,
                       const char   *name,
                       const char   *opt_branch,
                       const char   *opt_default_branch,
                       const char   *opt_arch,
                       FlatpakKinds  kinds,
                       GError      **error)
{
  const char **valid_arches = flatpak_get_arches ();
  const char *opt_arches[] = {opt_arch, NULL};

  if (opt_arch != NULL)
    valid_arches = opt_arches;

  g_autoptr(GError) my_error = NULL;
  g_autoptr(FlatpakDecomposed) ref = find_matching_ref (refs,
                                                        name,
                                                        opt_branch,
                                                        opt_default_branch,
                                                        valid_arches,
                                                        NULL,
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

/* This tries to find a single ref based on the specfied name/branch/arch
 * triplet from  a remote. If arch is not specified, matches only on compatible arches.
*/
FlatpakDecomposed *
flatpak_dir_find_remote_ref (FlatpakDir   *self,
                             FlatpakRemoteState *state,
                             const char   *name,
                             const char   *opt_branch,
                             const char   *opt_default_branch,
                             const char   *opt_arch,
                             FlatpakKinds  kinds,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(FlatpakDecomposed) remote_ref = NULL;
  g_autoptr(GHashTable) remote_refs = NULL;
  g_autoptr(GError) my_error = NULL;

  /* Avoid work if the entire ref was specified */
  if (opt_branch != NULL && opt_arch != NULL && (kinds == FLATPAK_KINDS_APP || kinds == FLATPAK_KINDS_RUNTIME))
    return flatpak_decomposed_new_from_parts (kinds, name, opt_arch, opt_branch, error);

  if (!flatpak_dir_list_all_remote_refs (self, state,
                                         &remote_refs, cancellable, error))
    return NULL;

  remote_ref = find_ref_for_refs_set (remote_refs, name, opt_branch,
                                      opt_default_branch, opt_arch,
                                      kinds,  &my_error);
  if (!remote_ref)
    {
      if (g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       _("Error searching remote %s: %s"),
                       state->remote_name,
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

static GHashTable *
refspecs_decompose_steal (GHashTable *refspecs)
{
  g_autoptr(GHashTable) refs = NULL;
  GHashTableIter iter;
  gpointer key, value;

  refs = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash, (GEqualFunc)flatpak_decomposed_equal,
                                (GDestroyNotify)flatpak_decomposed_unref, g_free);

  g_hash_table_iter_init (&iter, refspecs);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      char *checksum = value;
      char *refspec = key;
      FlatpakDecomposed *decomposed;

      g_hash_table_iter_steal (&iter);

      decomposed = flatpak_decomposed_new_from_refspec_take (refspec, NULL);
      if (decomposed)
        {
          g_hash_table_insert (refs, decomposed, checksum);
        }
      else
        {
          g_free (checksum);
          g_free (refspec);
        }
    }

  return g_steal_pointer (&refs);
}

GPtrArray *
flatpak_dir_find_local_refs (FlatpakDir           *self,
                             const char           *remote,
                             const char           *name,
                             const char           *opt_branch,
                             const char           *opt_default_branch,
                             const char           *opt_arch,
                             const char           *opt_default_arch,
                             FlatpakKinds          kinds,
                             FindMatchingRefsFlags flags,
                             GCancellable         *cancellable,
                             GError              **error)
{
  g_autoptr(GHashTable) local_refs = NULL;
  g_autoptr(GHashTable) local_refspecs = NULL;
  g_autoptr(GError) my_error = NULL;
  g_autofree char *refspec_prefix = g_strconcat (remote, ":.", NULL);
  g_autoptr(GPtrArray) matched_refs = NULL;
  const char **valid_arches = flatpak_get_arches ();
  const char *opt_arches[] = {opt_arch, NULL};

  if (opt_arch != NULL)
    valid_arches = opt_arches;

  if (!flatpak_dir_ensure_repo (self, NULL, error))
    return NULL;

  if (!ostree_repo_list_refs (self->repo,
                              refspec_prefix,
                              &local_refspecs, cancellable, error))
    return NULL;

  local_refs = refspecs_decompose_steal (local_refspecs);

  matched_refs = find_matching_refs (local_refs,
                                     name,
                                     opt_branch,
                                     opt_default_branch,
                                     valid_arches,
                                     opt_default_arch,
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

  return g_steal_pointer (&matched_refs);
}

static GHashTable *
flatpak_dir_get_all_installed_refs (FlatpakDir  *self,
                                    FlatpakKinds kinds,
                                    GError     **error)
{
  g_autoptr(GHashTable) local_refs = NULL;

  if (!flatpak_dir_maybe_ensure_repo (self, NULL, error))
    return NULL;

  local_refs = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash, (GEqualFunc)flatpak_decomposed_equal, (GDestroyNotify)flatpak_decomposed_unref, NULL);
  if (kinds & FLATPAK_KINDS_APP)
    {
      g_autoptr(GPtrArray) app_refs = flatpak_dir_list_refs (self, FLATPAK_KINDS_APP, NULL, error);
      if (app_refs == NULL)
        return NULL;

      for (int i = 0; i < app_refs->len; i++)
        {
          FlatpakDecomposed *app_ref = g_ptr_array_index (app_refs, i);
          g_hash_table_add (local_refs, flatpak_decomposed_ref (app_ref));
        }
    }
  if (kinds & FLATPAK_KINDS_RUNTIME)
    {
      g_autoptr(GPtrArray) runtime_refs = flatpak_dir_list_refs (self, FLATPAK_KINDS_RUNTIME, NULL, error);
      if (runtime_refs == NULL)
        return NULL;

      for (int i = 0; i < runtime_refs->len; i++)
        {
          FlatpakDecomposed *runtime_ref = g_ptr_array_index (runtime_refs, i);
          g_hash_table_add (local_refs, flatpak_decomposed_ref (runtime_ref));
        }
    }

  return g_steal_pointer (&local_refs);
}

/* This tries to find a all installed refs based on the specfied name/branch/arch
 * triplet. Matches on all arches.
*/
GPtrArray *
flatpak_dir_find_installed_refs (FlatpakDir           *self,
                                 const char           *opt_name,
                                 const char           *opt_branch,
                                 const char           *opt_arch,
                                 FlatpakKinds          kinds,
                                 FindMatchingRefsFlags flags,
                                 GError              **error)
{
  g_autoptr(GHashTable) local_refs = NULL;
  g_autoptr(GPtrArray) matched_refs = NULL;
  const char **valid_arches = NULL; /* List all installed arches if unspecified */
  const char *opt_arches[] = {opt_arch, NULL};

  if (opt_arch != NULL)
    valid_arches = opt_arches;

  local_refs = flatpak_dir_get_all_installed_refs (self, kinds, error);
  if (local_refs == NULL)
    return NULL;

  matched_refs = find_matching_refs (local_refs,
                                     opt_name,
                                     opt_branch,
                                     NULL, /* default branch */
                                     valid_arches,
                                     NULL, /* default arch */
                                     kinds,
                                     flags,
                                     error);
  if (matched_refs == NULL)
    return NULL;

  return g_steal_pointer (&matched_refs);
}

/* This tries to find a single ref based on the specfied name/branch/arch
 * triplet. This matches on all (installed) arches, but defaults to the primary
 * arch if that is installed. Otherwise, ambiguity is an error.
*/
FlatpakDecomposed *
flatpak_dir_find_installed_ref (FlatpakDir   *self,
                                const char   *opt_name,
                                const char   *opt_branch,
                                const char   *opt_arch,
                                FlatpakKinds  kinds,
                                GError      **error)
{
  g_autoptr(FlatpakDecomposed) local_ref = NULL;
  g_autoptr(GHashTable) local_refs = NULL;
  g_autoptr(GError) my_error = NULL;
  const char **valid_arches = NULL; /* All are valid unless specified in opt_arch */
  const char *default_arch = flatpak_get_arch ();
  const char *opt_arches[] = {opt_arch, NULL};

  if (opt_arch != NULL)
    valid_arches = opt_arches;

  local_refs = flatpak_dir_get_all_installed_refs (self, kinds, error);
  if (local_refs == NULL)
    return NULL;

  local_ref = find_matching_ref (local_refs, opt_name, opt_branch, NULL,
                                 valid_arches, default_arch,
                                 kinds, &my_error);
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
      return g_steal_pointer (&local_ref);
    }

  g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
               _("%s/%s/%s not installed"),
               opt_name ? opt_name : "*unspecified*",
               opt_arch ? opt_arch : "*unspecified*",
               opt_branch ? opt_branch : "*unspecified*");
  return NULL;
}

/* Given a list of decomposed refs in local_refspecs, remove any refs that have already
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
  g_autoptr(GPtrArray) undeployed_refs = g_ptr_array_new_full (local_refspecs->len, (GDestroyNotify)flatpak_decomposed_unref);
  gsize i;

  for (i = 0; i < local_refspecs->len; ++i)
    {
      FlatpakDecomposed *ref = g_ptr_array_index (local_refspecs, i);
      g_autoptr(GBytes) deploy_data = NULL;

      deploy_data = flatpak_dir_get_deploy_data (self, ref, FLATPAK_DEPLOY_VERSION_ANY, NULL, NULL);

      if (!deploy_data)
        g_ptr_array_add (undeployed_refs, flatpak_decomposed_ref (ref));
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
 * Returns: %TRUE if cleaning up the refs succeeded, %FALSE otherwise
 */
gboolean
flatpak_dir_cleanup_undeployed_refs (FlatpakDir   *self,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  g_autoptr(GHashTable) local_refspecs = NULL;
  g_autoptr(GHashTable) local_refs = NULL;
  g_autoptr(GPtrArray)  local_flatpak_refspecs = NULL;
  g_autoptr(GPtrArray) undeployed_refs = NULL;
  gsize i = 0;

  if (!ostree_repo_list_refs (self->repo, NULL, &local_refspecs, cancellable, error))
    return FALSE;

  local_refs = refspecs_decompose_steal (local_refspecs);

  local_flatpak_refspecs = find_matching_refs (local_refs,
                                               NULL, NULL, NULL, NULL, NULL,
                                               FLATPAK_KINDS_APP |
                                               FLATPAK_KINDS_RUNTIME,
                                               0, error);

  if (!local_flatpak_refspecs)
    return FALSE;

  undeployed_refs = filter_out_deployed_refs (self, local_flatpak_refspecs, error);

  if (!undeployed_refs)
    return FALSE;

  for (; i < undeployed_refs->len; ++i)
    {
      FlatpakDecomposed *ref = g_ptr_array_index (undeployed_refs, i);
      g_autofree gchar *remote = flatpak_decomposed_dup_remote (ref);

      if (!flatpak_dir_remove_ref (self, remote, flatpak_decomposed_get_ref (ref), cancellable, error))
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
  g_autoptr(DirExtraData) extra_data = dir_extra_data_new (SYSTEM_DIR_DEFAULT_ID,
                                                           SYSTEM_DIR_DEFAULT_DISPLAY_NAME,
                                                           SYSTEM_DIR_DEFAULT_PRIORITY,
                                                           SYSTEM_DIR_DEFAULT_STORAGE_TYPE);
  return flatpak_dir_new_full (path, FALSE, extra_data);
}

/* This figures out if it is a user or system dir automatically */
FlatpakDir *
flatpak_dir_get_by_path (GFile *path)
{
  GPtrArray *locations = flatpak_get_system_base_dir_locations (NULL, NULL);
  int i;

  if (locations)
    {
      for (i = 0; i < locations->len; i++)
        {
          GFile *system_path = g_ptr_array_index (locations, i);

          if (g_file_equal (system_path, path))
            {
              DirExtraData *extra_data = g_object_get_data (G_OBJECT (path), "extra-data");
              return flatpak_dir_new_full (path, FALSE, extra_data);
            }
        }
    }

  /* If its not configured as a system installation it will not have
     an installation id and we can't use the system helper, so assume
     user (and fail later with permission issues if its not owned by
     the caller) */

  return flatpak_dir_new (path, TRUE);
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

  if (id == NULL || g_strcmp0 (id, SYSTEM_DIR_DEFAULT_ID) == 0)
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

char **
flatpak_dir_list_remote_config_keys (FlatpakDir *self,
                                     const char *remote_name)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_keys (config, group, NULL, NULL);

  return NULL;
}

GPtrArray *
flatpak_dir_get_sideload_repo_paths (FlatpakDir *self)
{
  g_autoptr(GFile) sideload_repos_dir = flatpak_dir_get_sideload_repos_dir (self);
  g_autoptr(GFile) runtime_sideload_repos_dir = flatpak_dir_get_runtime_sideload_repos_dir (self);
  g_autoptr(GPtrArray) res = g_ptr_array_new_with_free_func (g_object_unref);

  add_sideload_subdirs (res, sideload_repos_dir, TRUE);
  add_sideload_subdirs (res, runtime_sideload_repos_dir, TRUE);

  return g_steal_pointer (&res);
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

static const char *
canonical_filter (const char *filter)
{
  /* Canonicalize "no filter", to NULL (empty means the same) */
  if (filter && *filter == 0)
    return NULL;
  return filter;
}

gboolean
flatpak_dir_compare_remote_filter (FlatpakDir *self,
                                   const char *remote_name,
                                   const char *filter)
{
  g_autofree char *current_filter = flatpak_dir_get_remote_filter (self, remote_name);

  return g_strcmp0 (current_filter, canonical_filter (filter)) == 0;
}

/* returns the canonical form */
char *
flatpak_dir_get_remote_filter (FlatpakDir *self,
                               const char *remote_name)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_autofree char *group = get_group (remote_name);

  if (config)
    {
      g_autofree char *filter = g_key_file_get_string (config, group, "xa.filter", NULL);

      if (filter && *filter != 0)
        return g_steal_pointer (&filter);
    }

  return NULL;
}

char *
flatpak_dir_get_remote_comment (FlatpakDir *self,
                                const char *remote_name)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_string (config, group, "xa.comment", NULL);

  return NULL;
}

char *
flatpak_dir_get_remote_description (FlatpakDir *self,
                                    const char *remote_name)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_string (config, group, "xa.description", NULL);

  return NULL;
}

char *
flatpak_dir_get_remote_homepage (FlatpakDir *self,
                                 const char *remote_name)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_string (config, group, "xa.homepage", NULL);

  return NULL;
}

char *
flatpak_dir_get_remote_icon (FlatpakDir *self,
                             const char *remote_name)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return g_key_file_get_string (config, group, "xa.icon", NULL);

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

gint32
flatpak_dir_get_remote_default_token_type (FlatpakDir *self,
                                           const char *remote_name)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_autofree char *group = get_group (remote_name);

  if (config)
    return (gint32)g_key_file_get_integer (config, group, "xa.default-token-type", NULL);

  return 0;
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

char *
flatpak_dir_get_remote_subset (FlatpakDir *self,
                               const char *remote_name)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_autofree char *group = get_group (remote_name);
  g_autofree char *subset = NULL;

  if (config == NULL)
    return NULL;

  subset = g_key_file_get_string (config, group, "xa.subset", NULL);
  if (subset == NULL || *subset == 0)
    return NULL;

  return g_steal_pointer (&subset);
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

static char *
flatpak_dir_get_remote_install_authenticator_name (FlatpakDir *self,
                                                   const char *remote_name)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_autofree char *group = get_group (remote_name);

  if (config == NULL ||
      !g_key_file_get_boolean (config, group, "xa.authenticator-install", NULL))
    return NULL;

  return g_key_file_get_string (config, group, "xa.authenticator-name", NULL);
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
      FlatpakDecomposed *ref = key;
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
                       gboolean    gpg_verify)
{
  g_autofree char *real_url = NULL;
  g_autofree char *real_main_ref = NULL;
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

      if (origin_remote_matches (repo, name, url, main_ref, gpg_verify))
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
  g_key_file_set_string (*new_config, group, "gpg-verify-summary", gpg_verify ? "true" : "false");
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
                                  gboolean     *changed_config,
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

  if (new_config && !_flatpak_dir_reload_config (self, cancellable, error))
    return FALSE;

  if (changed_config)
    *changed_config = (new_config != NULL);

  return g_steal_pointer (&remote);
}

static gboolean
parse_ref_file (GKeyFile *keyfile,
                char    **name_out,
                char    **branch_out,
                char    **url_out,
                GBytes  **gpg_data_out,
                gboolean *is_runtime_out,
                char    **collection_id_out,
                GError  **error)
{
  g_autofree char *url = NULL;
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

  /* We have a hierarchy of keys for setting the collection ID, which all have
   * the same effect. The only difference is which versions of Flatpak support
   * them, and therefore what P2P implementation is enabled by them:
   * DeploySideloadCollectionID: supported by Flatpak >= 1.12.8 (1.7.1
   *   introduced sideload support but this key was added late)
   * DeployCollectionID: supported by Flatpak >= 1.0.6
   * CollectionID: supported by Flatpak >= 0.9.8
   */
  collection_id = flatpak_keyfile_get_string_non_empty (keyfile, FLATPAK_REF_GROUP,
                                                        FLATPAK_REF_DEPLOY_SIDELOAD_COLLECTION_ID_KEY);

  if (collection_id == NULL)
    {
      collection_id = flatpak_keyfile_get_string_non_empty (keyfile, FLATPAK_REF_GROUP,
                                                            FLATPAK_REF_DEPLOY_COLLECTION_ID_KEY);
    }
  if (collection_id == NULL)
    {
      collection_id = flatpak_keyfile_get_string_non_empty (keyfile, FLATPAK_REF_GROUP,
                                                            FLATPAK_REF_COLLECTION_ID_KEY);
    }

  if (collection_id != NULL && gpg_data == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Collection ID requires GPG key to be provided"));

  *name_out = g_steal_pointer (&name);
  *branch_out = g_steal_pointer (&branch);
  *url_out = g_steal_pointer (&url);
  *gpg_data_out = g_steal_pointer (&gpg_data);
  *is_runtime_out = is_runtime;
  *collection_id_out = g_steal_pointer (&collection_id);

  return TRUE;
}

gboolean
flatpak_dir_create_remote_for_ref_file (FlatpakDir         *self,
                                        GKeyFile           *keyfile,
                                        const char         *default_arch,
                                        char              **remote_name_out,
                                        char              **collection_id_out,
                                        FlatpakDecomposed **ref_out,
                                        GError            **error)
{
  g_autoptr(GBytes) gpg_data = NULL;
  g_autofree char *name = NULL;
  g_autofree char *branch = NULL;
  g_autofree char *url = NULL;
  g_autofree char *remote = NULL;
  gboolean is_runtime = FALSE;
  g_autofree char *collection_id = NULL;
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(FlatpakDecomposed) ref = NULL;

  if (!parse_ref_file (keyfile, &name, &branch, &url, &gpg_data, &is_runtime, &collection_id, error))
    return FALSE;

  ref = flatpak_decomposed_new_from_parts (is_runtime ? FLATPAK_KINDS_RUNTIME : FLATPAK_KINDS_APP,
                                           name, default_arch, branch, error);
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
  remote = flatpak_dir_find_remote_by_uri (self, url);

  if (remote == NULL)
    {
      /* title is NULL because the title from the ref file is the title of the app not the remote */
      remote = flatpak_dir_create_origin_remote (self, url, name, NULL, flatpak_decomposed_get_ref (ref),
                                                 gpg_data, collection_id, NULL, NULL, error);
      if (remote == NULL)
        return FALSE;
    }

  if (collection_id_out != NULL)
    *collection_id_out = g_steal_pointer (&collection_id);

  *remote_name_out = g_steal_pointer (&remote);
  *ref_out = g_steal_pointer (&ref);
  return TRUE;
}

/* This tries to find a pre-configured remote for the specified uri.
 *
 *  We consider non-OCI URLs equal even if one lacks a trailing slash.
 */
char *
flatpak_dir_find_remote_by_uri (FlatpakDir *self,
                                const char *uri)
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

          if (!ostree_repo_remote_get_url (self->repo,
                                           remote,
                                           &remote_uri,
                                           NULL))
            continue;

          if (flatpak_uri_equal (uri, remote_uri))
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
flatpak_dir_list_dependency_remotes (FlatpakDir   *self,
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

      if (flatpak_dir_get_remote_nodeps (self, remote))
        continue;

      g_ptr_array_add (res, g_strdup (remote));
    }

  g_ptr_array_add (res, NULL);
  return (char **) g_ptr_array_free (g_steal_pointer (&res), FALSE);
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

          if (!g_str_has_prefix (refspec, prefix))
            continue;

          g_autoptr(FlatpakDecomposed) ref = flatpak_decomposed_new_from_refspec (refspec, NULL);
          if (ref == NULL)
            continue;

          g_autofree char *origin = flatpak_dir_get_origin (self, ref, cancellable, NULL);
          if (g_strcmp0 (origin, remote_name) == 0)
            return flatpak_fail_error (error, FLATPAK_ERROR_REMOTE_USED,
                                       _("Can't remove remote '%s' with installed ref %s (at least)"),
                                       remote_name, flatpak_decomposed_get_ref (ref));
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
  g_autofree gchar *filter_path = NULL;
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

      /* If we e.g. changed url or gpg config the cached summary may be invalid */
      if (!flatpak_dir_remote_clear_cached_summary (self, remote_name, cancellable, error))
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

  copy_remote_config (new_config, config, remote_name);

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
      g_info ("Imported %u GPG key%s to remote \"%s\"",
              imported, (imported == 1) ? "" : "s", remote_name);
    }

  filter_path = g_key_file_get_value (new_config, group, "xa.filter", NULL);
  if (filter_path && *filter_path && g_file_test (filter_path, G_FILE_TEST_EXISTS))
    {
      /* Make a backup filter copy in case it goes away later */
      g_autofree char *filter_name = g_strconcat (remote_name, ".filter", NULL);
      g_autoptr(GFile) filter_file = g_file_new_for_path (filter_path);
      g_autoptr(GFile) filter_copy = flatpak_build_file (self->basedir, "repo", filter_name, NULL);
      g_autoptr(GError) local_error = NULL;
      g_autofree char *backup_data = NULL;
      gsize backup_data_size;

      if (g_file_load_contents (filter_file, cancellable, &backup_data, &backup_data_size, NULL, &local_error))
        {
          g_autofree char *backup_data_copy =
            g_strdup_printf ("# backup copy of %s, do not edit!\n%s", filter_path, backup_data);

          if (!g_file_replace_contents (filter_copy, backup_data_copy, strlen (backup_data_copy),
                                        NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, NULL, cancellable, &local_error))
            g_info ("Failed to save backup copy of filter file %s: %s\n", filter_path, local_error->message);
        }
      else
        {
          g_info ("Failed to read filter %s file while making a backup copy: %s\n", filter_path, local_error->message);
        }
    }

  /* If we e.g. changed url or gpg config the cached summary may be invalid */
  if (!flatpak_dir_remote_clear_cached_summary (self, remote_name, cancellable, error))
    return FALSE;

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
      g_autoptr(GHashTable) decomposed_local_refs =
        g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash, (GEqualFunc)flatpak_decomposed_equal, (GDestroyNotify)flatpak_decomposed_unref, NULL);
      g_autoptr(GHashTable) local_refs = NULL;
      g_autoptr(FlatpakDecomposed) decomposed_main_ref = NULL;
      GHashTableIter hash_iter;
      gpointer key;
      g_autofree char *refspec_prefix = g_strconcat (state->remote_name, ":.", NULL);
      g_autofree char *remote_main_ref = NULL;

      /* For noenumerate remotes, only return data for already locally
       * available refs, or the ref set as xa.main-ref on the remote, or
       * extensions of that main ref */

      if (!ostree_repo_list_refs (self->repo, refspec_prefix, &local_refs,
                                  cancellable, error))
        return FALSE;

      g_hash_table_iter_init (&hash_iter, local_refs);
      while (g_hash_table_iter_next (&hash_iter, &key, NULL))
        {
          const char *refspec = key;
          g_autofree char *ref = NULL;
          g_autoptr(FlatpakDecomposed) d = NULL;

          if (!ostree_parse_refspec (refspec, NULL, &ref, error))
            return FALSE;

          d = flatpak_decomposed_new_from_ref (ref, NULL);
          if (d)
            g_hash_table_insert (decomposed_local_refs, g_steal_pointer (&d), NULL);
        }

      remote_main_ref = flatpak_dir_get_remote_main_ref (self, state->remote_name);
      if (remote_main_ref != NULL && *remote_main_ref != '\0')
        decomposed_main_ref = flatpak_decomposed_new_from_col_ref (remote_main_ref, state->collection_id, NULL);

      /* Then we remove all remote refs not in the local refs set, not the main
       * ref, and not an extension of the main ref */
      GLNX_HASH_TABLE_FOREACH_IT (*refs, it, FlatpakDecomposed *, d, void *, v)
        {
          if (g_hash_table_contains (decomposed_local_refs, d))
            continue;

          if (decomposed_main_ref != NULL)
            {
              g_autofree char *main_ref_id = NULL;
              g_autofree char *main_ref_prefix = NULL;

              if (flatpak_decomposed_equal (decomposed_main_ref, d))
                continue;

              main_ref_id = flatpak_decomposed_dup_id (decomposed_main_ref);
              main_ref_prefix = g_strconcat (main_ref_id, ".", NULL);
              if (flatpak_decomposed_id_has_prefix (d, main_ref_prefix))
                continue;
            }

          g_hash_table_iter_remove (&it);
      }
    }

  return TRUE;
}

static gboolean
strv_contains_prefix (const gchar * const *strv,
                      const gchar         *str)
{
  g_return_val_if_fail (strv != NULL, FALSE);
  g_return_val_if_fail (str != NULL, FALSE);

  for (; *strv != NULL; strv++)
    {
      if (g_str_has_prefix (str, *strv))
        return TRUE;
    }

  return FALSE;
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
    "xa.comment",
    "xa.description",
    "xa.homepage",
    "xa.icon",
    "xa.default-branch",
    "xa.gpg-keys",
    "xa.redirect-url",
    "xa.authenticator-name",
    "xa.authenticator-install",
    OSTREE_META_KEY_DEPLOY_COLLECTION_ID,
    "xa.deploy-collection-id", /* This is a new version only supported in post p2p flatpak (1.7) */
    NULL
  };
  static const char *const supported_param_prefixes[] = {
    "xa.authenticator-options.",
    NULL
  };
  g_autoptr(GPtrArray) updated_params = NULL;
  g_autoptr(GVariant) metadata = NULL;
  GVariantIter iter;
  g_autoptr(GBytes) gpg_keys = NULL;

  updated_params = g_ptr_array_new_with_free_func (g_free);

  if (!flatpak_remote_state_ensure_summary (remote_state, error))
    return FALSE;

  if (remote_state->index)
    metadata = g_variant_get_child_value (remote_state->index, 1);
  else
    metadata = g_variant_get_child_value (remote_state->summary, 1);

  g_variant_iter_init (&iter, metadata);
  if (g_variant_iter_n_children (&iter) > 0)
    {
      GVariant *value_var = NULL;
      char *key = NULL;

      while (g_variant_iter_next (&iter, "{sv}", &key, &value_var))
        {
          if (g_strv_contains (supported_params, key) ||
              strv_contains_prefix (supported_param_prefixes, key))
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
                      else if (strcmp (key, "xa.deploy-collection-id") == 0)
                        g_ptr_array_add (updated_params, g_strdup ("collection-id"));
                      else
                        g_ptr_array_add (updated_params, g_strdup (key));
                      g_ptr_array_add (updated_params, g_strdup (value));
                    }
                }
              else if (g_variant_is_of_type (value_var, G_VARIANT_TYPE_BOOLEAN))
                {
                  gboolean value = g_variant_get_boolean (value_var);
                  g_ptr_array_add (updated_params, g_strdup (key));
                  if (value)
                    g_ptr_array_add (updated_params, g_strdup ("true"));
                  else
                    g_ptr_array_add (updated_params, g_strdup ("false"));
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
                                         FlatpakRemoteState *optional_remote_state,
                                         gboolean     *updated_out,
                                         GCancellable *cancellable,
                                         GError      **error)
{
  gboolean is_oci;
  g_autoptr(FlatpakRemoteState) local_state = NULL;
  FlatpakRemoteState *state;
  gboolean has_changed = FALSE;

  /* Initialize if we exit early */
  if (updated_out)
    *updated_out = FALSE;

  if (flatpak_dir_get_remote_disabled (self, remote))
    return TRUE;

  is_oci = flatpak_dir_get_remote_oci (self, remote);
  if (is_oci)
    return TRUE;

  if (optional_remote_state)
    state = optional_remote_state;
  else
    {
      local_state = flatpak_dir_get_remote_state (self, remote, FALSE, cancellable, error);
      if (local_state == NULL)
        return FALSE;
      state = local_state;
    }

  if (flatpak_dir_use_system_helper (self, NULL))
    {
      gboolean gpg_verify_summary;
      gboolean gpg_verify;

      if (!ostree_repo_remote_get_gpg_verify_summary (self->repo, remote, &gpg_verify_summary, error))
        return FALSE;

      if (!ostree_repo_remote_get_gpg_verify (self->repo, remote, &gpg_verify, error))
        return FALSE;

      if (!gpg_verify_summary || !gpg_verify)
        {
          g_info ("Ignoring automatic updates for system-helper remotes without gpg signatures");
          return TRUE;
        }

      if ((state->summary != NULL && state->summary_sig_bytes == NULL) ||
          (state->index != NULL && state->index_sig_bytes == NULL))
        {
          g_info ("Can't update remote configuration as user, no GPG signature");
          return TRUE;
        }

      if (!flatpak_dir_update_remote_configuration_for_state (self, state, TRUE, &has_changed, cancellable, error))
        return FALSE;

      if (has_changed)
        {
          g_autoptr(GBytes) bytes = g_variant_get_data_as_bytes (state->index ? state->index : state->summary);
          GBytes *sig_bytes = state->index ? state->index_sig_bytes : state->summary_sig_bytes;
          glnx_autofd int summary_fd = -1;
          g_autofree char *summary_path = NULL;
          glnx_autofd int summary_sig_fd = -1;
          g_autofree char *summary_sig_path = NULL;
          const char *installation;
          FlatpakHelperUpdateRemoteFlags flags = 0;

          if (state->index)
            flags |= FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_SUMMARY_IS_INDEX;

          summary_fd = g_file_open_tmp ("remote-summary.XXXXXX", &summary_path, error);
          if (summary_fd == -1)
            return FALSE;
          if (glnx_loop_write (summary_fd, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes)) < 0)
            return glnx_throw_errno (error);

          if (sig_bytes != NULL)
            {
              summary_sig_fd = g_file_open_tmp ("remote-summary-sig.XXXXXX", &summary_sig_path, error);
              if (summary_sig_fd == -1)
                return FALSE;
              if (glnx_loop_write (summary_sig_fd, g_bytes_get_data (sig_bytes, NULL), g_bytes_get_size (sig_bytes)) < 0)
                return glnx_throw_errno (error);
            }

          installation = flatpak_dir_get_id (self);

          if (!flatpak_dir_system_helper_call_update_remote (self, flags, remote,
                                                             installation ? installation : "",
                                                             summary_path, summary_sig_path ? summary_sig_path : "",
                                                             cancellable, error))
            return FALSE;

          unlink (summary_path);
          if (summary_sig_path)
            unlink (summary_sig_path);


          if (!flatpak_dir_remote_clear_cached_summary (self, remote, cancellable, error))
            return FALSE;

        }

      if (updated_out)
        *updated_out = has_changed;

      return TRUE;
    }

  if (!flatpak_dir_update_remote_configuration_for_state (self, state, FALSE, &has_changed, cancellable, error))
    return FALSE;

  if (has_changed &&
      !flatpak_dir_remote_clear_cached_summary (self, remote, cancellable, error))
    return FALSE;

  if (updated_out)
    *updated_out = has_changed;

  return TRUE;
}

void
flatpak_related_free (FlatpakRelated *self)
{
  g_free (self->remote);
  flatpak_decomposed_unref (self->ref);
  g_free (self->commit);
  g_strfreev (self->subpaths);
  g_free (self);
}

static void
add_related (FlatpakDir        *self,
             GPtrArray         *related,
             const char        *remote,
             const char        *extension,
             FlatpakDecomposed *extension_ref,
             const char        *checksum,
             gboolean           no_autodownload,
             const char        *download_if,
             const char        *autoprune_unless,
             gboolean           autodelete,
             gboolean           locale_subset)
{
  g_autoptr(GBytes) deploy_data = NULL;
  g_autofree const char **old_subpaths = NULL;
  g_autofree const char *id = NULL;
  g_autofree const char *arch = NULL;
  g_autofree const char *branch = NULL;
  g_auto(GStrv) extra_subpaths = NULL;
  g_auto(GStrv) subpaths = NULL;
  FlatpakRelated *rel;
  gboolean download;
  gboolean delete = autodelete;
  gboolean auto_prune = FALSE;
  g_autoptr(GFile) unmaintained_path = NULL;

  deploy_data = flatpak_dir_get_deploy_data (self, extension_ref, FLATPAK_DEPLOY_VERSION_ANY, NULL, NULL);

  id = flatpak_decomposed_dup_id (extension_ref);
  arch = flatpak_decomposed_dup_arch (extension_ref);
  branch = flatpak_decomposed_dup_branch (extension_ref);

  if (deploy_data)
    {
      old_subpaths = flatpak_deploy_data_get_subpaths (deploy_data);
      /* If the extension is installed already, its origin overrides the remote
       * that would otherwise be used */
      remote = flatpak_deploy_data_get_origin (deploy_data);
    }

  /* Only respect no-autodownload/download-if for uninstalled refs, we
     always want to update if you manually installed something */
  download =
    flatpak_extension_matches_reason (id, download_if, !no_autodownload) ||
    deploy_data != NULL;

  if (!flatpak_extension_matches_reason (id, autoprune_unless, TRUE))
    auto_prune = TRUE;

  /* Don't download if there is an unmaintained extension already installed */
  unmaintained_path =
    flatpak_find_unmaintained_extension_dir_if_exists (id, arch, branch, NULL);
  if (unmaintained_path != NULL && deploy_data == NULL)
    {
      g_info ("Skipping related extension ‘%s’ because it is already "
              "installed as an unmaintained extension in ‘%s’.",
              id, flatpak_file_get_path_cached (unmaintained_path));
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
  rel->remote = g_strdup (remote);
  rel->ref = flatpak_decomposed_ref (extension_ref);
  rel->commit = g_strdup (checksum);
  rel->subpaths = g_steal_pointer (&subpaths);
  rel->download = download;
  rel->delete = delete;
  rel->auto_prune = auto_prune;

  g_ptr_array_add (related, rel);
}

static GRegex *
flatpak_dir_get_mask_regexp (FlatpakDir *self)
{
  GRegex *res = NULL;

  G_LOCK (config_cache);

  if (self->masked == NULL)
    {
      g_autofree char *masked = NULL;

      masked = flatpak_dir_get_config (self, "masked", NULL);
      if (masked)
        {
          g_auto(GStrv) patterns = g_strsplit (masked, ";", -1);
          g_autoptr(GString) deny_regexp = g_string_new ("^(");
          int i;

          for (i = 0; patterns[i] != NULL; i++)
            {
              const char *pattern = patterns[i];

              if (*pattern != 0)
                {
                  g_autofree char *regexp = NULL;

                  regexp = flatpak_filter_glob_to_regexp (pattern, FALSE, NULL);
                  if (regexp)
                    {
                      if (i != 0)
                        g_string_append (deny_regexp, "|");
                      g_string_append (deny_regexp, regexp);
                    }
                }
            }

          g_string_append (deny_regexp, ")$");
          self->masked = g_regex_new (deny_regexp->str, G_REGEX_DOLLAR_ENDONLY|G_REGEX_RAW|G_REGEX_OPTIMIZE, G_REGEX_MATCH_ANCHORED, NULL);
        }
    }

  if (self->masked)
    res = g_regex_ref (self->masked);

  G_UNLOCK (config_cache);

  return res;
}

gboolean
flatpak_dir_ref_is_masked (FlatpakDir *self,
                           const char *ref)
{
  g_autoptr(GRegex) masked = flatpak_dir_get_mask_regexp (self);

  return !flatpak_filters_allow_ref (NULL, masked, ref);
}

static GRegex *
flatpak_dir_get_pin_regexp (FlatpakDir *self)
{
  GRegex *res = NULL;

  G_LOCK (config_cache);

  if (self->pinned == NULL)
    {
      g_autofree char *pinned = NULL;

      pinned = flatpak_dir_get_config (self, "pinned", NULL);
      if (pinned)
        {
          g_auto(GStrv) patterns = g_strsplit (pinned, ";", -1);
          g_autoptr(GString) deny_regexp = g_string_new ("^(");
          int i;

          for (i = 0; patterns[i] != NULL; i++)
            {
              const char *pattern = patterns[i];

              if (*pattern != 0)
                {
                  g_autofree char *regexp = NULL;

                  regexp = flatpak_filter_glob_to_regexp (pattern,
                                                          TRUE, /* only match runtimes */
                                                          NULL);
                  if (regexp)
                    {
                      if (i != 0)
                        g_string_append (deny_regexp, "|");
                      g_string_append (deny_regexp, regexp);
                    }
                }
            }

          g_string_append (deny_regexp, ")$");
          self->pinned = g_regex_new (deny_regexp->str, G_REGEX_DOLLAR_ENDONLY|G_REGEX_RAW|G_REGEX_OPTIMIZE, G_REGEX_MATCH_ANCHORED, NULL);
        }
    }

  if (self->pinned)
    res = g_regex_ref (self->pinned);

  G_UNLOCK (config_cache);

  return res;
}

gboolean
flatpak_dir_ref_is_pinned (FlatpakDir *self,
                           const char *ref)
{
  g_autoptr(GRegex) pinned = flatpak_dir_get_pin_regexp (self);

  return !flatpak_filters_allow_ref (NULL, pinned, ref);
}

GPtrArray *
flatpak_dir_find_remote_related_for_metadata (FlatpakDir         *self,
                                              FlatpakRemoteState *state,
                                              FlatpakDecomposed  *ref,
                                              GKeyFile           *metakey,
                                              GCancellable       *cancellable,
                                              GError            **error)
{
  int i;
  g_autoptr(GPtrArray) related = g_ptr_array_new_with_free_func ((GDestroyNotify) flatpak_related_free);
  g_autofree char *url = NULL;
  g_auto(GStrv) groups = NULL;
  g_autoptr(GRegex) masked = NULL;
  g_autofree char *ref_arch = flatpak_decomposed_dup_arch (ref);
  g_autofree char *ref_branch = flatpak_decomposed_dup_branch (ref);

  if (!ostree_repo_remote_get_url (self->repo,
                                   state->remote_name,
                                   &url,
                                   error))
    return NULL;

  if (*url == 0)
    return g_steal_pointer (&related);  /* Empty url, silently disables updates */

  masked = flatpak_dir_get_mask_regexp (self);

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
                default_branches[0] = ref_branch;
              branches = default_branches;
            }

          for (branch_i = 0; branches[branch_i] != NULL; branch_i++)
            {
              g_autoptr(FlatpakDecomposed) extension_ref = NULL;
              g_autofree char *checksum = NULL;
              const char *branch = branches[branch_i];

              extension_ref = flatpak_decomposed_new_from_parts (FLATPAK_KINDS_RUNTIME,
                                                                 extension, ref_arch, branch, NULL);
              if (extension_ref == NULL)
                continue;

              if (flatpak_remote_state_lookup_ref (state, flatpak_decomposed_get_ref (extension_ref), &checksum, NULL, NULL, NULL, NULL))
                {
                  if (flatpak_filters_allow_ref (NULL, masked, flatpak_decomposed_get_ref (extension_ref)))
                    add_related (self, related, state->remote_name, extension, extension_ref, checksum,
                                 no_autodownload, download_if, autoprune_unless, autodelete, locale_subset);
                }
              else if (subdirectories)
                {
                  g_autoptr(GPtrArray) subref_refs = flatpak_remote_state_match_subrefs (state, extension_ref);
                  for (int j = 0; j < subref_refs->len; j++)
                    {
                      FlatpakDecomposed *subref_ref = g_ptr_array_index (subref_refs, j);
                      g_autofree char *subref_checksum = NULL;

                      if (flatpak_remote_state_lookup_ref (state, flatpak_decomposed_get_ref (subref_ref),
                                                           &subref_checksum, NULL, NULL, NULL, NULL) &&
                          flatpak_filters_allow_ref (NULL, masked,  flatpak_decomposed_get_ref (subref_ref)))
                        add_related (self, related, state->remote_name, extension, subref_ref, subref_checksum,
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
                                 FlatpakDecomposed  *ref,
                                 gboolean            use_installed_metadata,
                                 GCancellable       *cancellable,
                                 GError            **error)
{
  g_autofree char *metadata = NULL;
  g_autoptr(GKeyFile) metakey = g_key_file_new ();
  g_autoptr(GPtrArray) related = g_ptr_array_new_with_free_func ((GDestroyNotify) flatpak_related_free);
  g_autofree char *url = NULL;

  if (!flatpak_dir_ensure_repo (self, cancellable, error))
    return NULL;

  if (!ostree_repo_remote_get_url (self->repo,
                                   state->remote_name,
                                   &url,
                                   error))
    return NULL;

  if (*url == 0)
    return g_steal_pointer (&related);  /* Empty url, silently disables updates */

  if (use_installed_metadata)
    {
      g_autoptr(GFile) deploy_dir = NULL;
      g_autoptr(GBytes) deploy_data = NULL;
      g_autoptr(GFile) metadata_file = NULL;

      deploy_dir = flatpak_dir_get_if_deployed (self, ref, NULL, cancellable);
      if (deploy_dir == NULL)
        {
          g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                       _("%s not installed"), flatpak_decomposed_get_ref (ref));
          return NULL;
        }

      deploy_data = flatpak_load_deploy_data (deploy_dir, ref, self->repo, FLATPAK_DEPLOY_VERSION_ANY, cancellable, error);
      if (deploy_data == NULL)
        return NULL;

      metadata_file = g_file_get_child (deploy_dir, "metadata");
      if (!g_file_load_contents (metadata_file, cancellable, &metadata, NULL, NULL, NULL))
        {
          g_info ("No metadata in local deploy");
          /* No metadata => no related, but no error */
        }
    }
  else
    flatpak_remote_state_load_data (state, flatpak_decomposed_get_ref (ref),
                                    NULL, NULL, &metadata,
                                    NULL);

  if (metadata != NULL &&
      g_key_file_load_from_data (metakey, metadata, -1, 0, NULL))
    {
      g_ptr_array_unref (related);
      related = flatpak_dir_find_remote_related_for_metadata (self, state, ref, metakey, cancellable, error);
    }

  return g_steal_pointer (&related);
}

static GHashTable *
local_match_prefix (FlatpakDir        *self,
                    FlatpakDecomposed *extension_ref,
                    const char        *remote,
                    GHashTable        *decomposed_to_search)
{
  GHashTable *matches = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash,
                                               (GEqualFunc)flatpak_decomposed_equal,
                                               (GDestroyNotify)flatpak_decomposed_unref,
                                               NULL);
  g_autofree char *id = NULL;
  g_autofree char *arch = NULL;
  g_autofree char *branch = NULL;
  g_autofree char *id_prefix = NULL;

  id = flatpak_decomposed_dup_id (extension_ref);
  arch = flatpak_decomposed_dup_arch (extension_ref);
  branch = flatpak_decomposed_dup_branch (extension_ref);

  id_prefix = g_strconcat (id, ".", NULL);

  if (decomposed_to_search)
    {
      GHashTableIter hash_iter;
      gpointer key;

      g_hash_table_iter_init (&hash_iter, decomposed_to_search);
      while (g_hash_table_iter_next (&hash_iter, &key, NULL))
        {
          FlatpakDecomposed *to_test = key;

          if (flatpak_decomposed_get_kind (extension_ref) != flatpak_decomposed_get_kind (to_test))
            continue;

          /* Must match type, arch, branch */
          if (!flatpak_decomposed_is_arch (to_test, arch) ||
              !flatpak_decomposed_is_branch (to_test, branch))
            continue;

          /* But only prefix of id */
          if (!flatpak_decomposed_id_has_prefix (to_test, id_prefix))
            continue;

          g_hash_table_add (matches, flatpak_decomposed_ref (to_test));
        }
    }

  /* Also check deploys. In case remote-delete --force is run, we can end up
   * with a deploy without a corresponding ref in the repo. */
  flatpak_dir_collect_deployed_refs (self, flatpak_decomposed_get_kind_str (extension_ref),
                                     id_prefix, arch, branch, matches, NULL, NULL);

  return matches;
}

/* Finds all the locally installed ref related to ref, if remote_name is set it is limited to refs from that remote */
GPtrArray *
flatpak_dir_find_local_related_for_metadata (FlatpakDir        *self,
                                             FlatpakDecomposed *ref,
                                             const char        *remote_name, /* nullable */
                                             GKeyFile          *metakey,
                                             GCancellable      *cancellable,
                                             GError           **error)
{
  int i;
  g_autoptr(GPtrArray) related = g_ptr_array_new_with_free_func ((GDestroyNotify) flatpak_related_free);
  g_autoptr(GHashTable) all_decomposed_for_remote = NULL;
  g_auto(GStrv) groups = NULL;
  g_autofree char *ref_arch = flatpak_decomposed_dup_arch (ref);
  g_autofree char *ref_branch = flatpak_decomposed_dup_branch (ref);

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
                default_branches[0] = ref_branch;
              branches = default_branches;
            }

          for (branch_i = 0; branches[branch_i] != NULL; branch_i++)
            {
              g_autoptr(FlatpakDecomposed) extension_ref = NULL;
              g_autofree char *checksum = NULL;
              g_autoptr(GBytes) deploy_data = NULL;
              const char *branch = branches[branch_i];

              extension_ref = flatpak_decomposed_new_from_parts (FLATPAK_KINDS_RUNTIME,
                                                                 extension, ref_arch, branch, NULL);
              if (extension_ref == NULL)
                continue;

              if (remote_name != NULL &&
                  flatpak_repo_resolve_rev (self->repo,
                                            NULL,
                                            remote_name,
                                            flatpak_decomposed_get_ref (extension_ref),
                                            FALSE,
                                            &checksum,
                                            NULL,
                                            NULL))
                {
                  add_related (self, related, remote_name, extension, extension_ref,
                               checksum, no_autodownload, download_if, autoprune_unless, autodelete, locale_subset);
                }
              else if ((deploy_data = flatpak_dir_get_deploy_data (self, extension_ref,
                                                                   FLATPAK_DEPLOY_VERSION_ANY,
                                                                   NULL, NULL)) != NULL &&
                       (remote_name == NULL || g_strcmp0 (flatpak_deploy_data_get_origin (deploy_data), remote_name) == 0))
                {
                  /* Here we're including extensions that are deployed but might
                   * not have a ref in the repo, as happens with remote-delete
                   * --force
                   */
                  checksum = g_strdup (flatpak_deploy_data_get_commit (deploy_data));
                  add_related (self, related,
                               flatpak_deploy_data_get_origin (deploy_data),
                               extension, extension_ref, checksum,
                               no_autodownload, download_if, autoprune_unless, autodelete, locale_subset);
                }
              else if (subdirectories)
                {
                  g_autoptr(GHashTable) matches = NULL;

                  if (!all_decomposed_for_remote)
                    {
                      g_autoptr(GHashTable) refs = NULL;
                      g_autofree char *list_prefix = NULL;
                      if (remote_name != NULL)
                        list_prefix = g_strdup_printf ("%s:", remote_name);

                      if (ostree_repo_list_refs (self->repo, list_prefix, &refs, NULL, NULL))
                        {
                          GHashTableIter iter;
                          gpointer key;

                          all_decomposed_for_remote = g_hash_table_new_full (
                            (GHashFunc)flatpak_decomposed_hash,
                            (GEqualFunc)flatpak_decomposed_equal,
                            (GDestroyNotify)flatpak_decomposed_unref,
                            NULL);

                          g_hash_table_iter_init (&iter, refs);
                          while (g_hash_table_iter_next (&iter, &key, NULL))
                            {
                              const char *refspec = key;
                              g_autoptr(FlatpakDecomposed) decomposed = NULL;

                              decomposed = flatpak_decomposed_new_from_refspec (refspec, NULL);
                              if (decomposed != NULL)
                                g_hash_table_add (all_decomposed_for_remote, g_steal_pointer (&decomposed));
                            }
                        }
                    }

                  matches = local_match_prefix (self, extension_ref, remote_name, all_decomposed_for_remote);
                  GLNX_HASH_TABLE_FOREACH (matches, FlatpakDecomposed *, match)
                    {
                      g_autofree char *match_checksum = NULL;
                      g_autoptr(GBytes) match_deploy_data = NULL;

                      if (remote_name != NULL &&
                          flatpak_repo_resolve_rev (self->repo,
                                                    NULL,
                                                    remote_name,
                                                    flatpak_decomposed_get_ref (match),
                                                    FALSE,
                                                    &match_checksum,
                                                    NULL,
                                                    NULL))
                        {
                          add_related (self, related, remote_name, extension, match, match_checksum,
                                       no_autodownload, download_if, autoprune_unless, autodelete, locale_subset);
                        }
                      else if ((match_deploy_data = flatpak_dir_get_deploy_data (self, match,
                                                                                 FLATPAK_DEPLOY_VERSION_ANY,
                                                                                 NULL, NULL)) != NULL &&
                               (remote_name == NULL || g_strcmp0 (flatpak_deploy_data_get_origin (match_deploy_data), remote_name) == 0))
                        {
                          /* Here again we're including extensions that are deployed but might
                           * not have a ref in the repo
                           */
                          match_checksum = g_strdup (flatpak_deploy_data_get_commit (match_deploy_data));
                          add_related (self, related,
                                       flatpak_deploy_data_get_origin (match_deploy_data),
                                       extension, match, match_checksum,
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
flatpak_dir_find_local_related (FlatpakDir        *self,
                                FlatpakDecomposed *ref,
                                const char        *remote_name,
                                gboolean           deployed,
                                GCancellable      *cancellable,
                                GError           **error)
{
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(GBytes) deploy_data = NULL;
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
                       _("%s not installed"), flatpak_decomposed_get_ref (ref));
          return NULL;
        }

      deploy_data = flatpak_load_deploy_data (deploy_dir, ref, self->repo, FLATPAK_DEPLOY_VERSION_ANY, cancellable, error);
      if (deploy_data == NULL)
        return NULL;

      if (flatpak_deploy_data_get_extension_of (deploy_data) == NULL)
        {
          metadata = g_file_get_child (deploy_dir, "metadata");
          if (!g_file_load_contents (metadata, cancellable, &metadata_contents, NULL, NULL, NULL))
            {
              g_info ("No metadata in local deploy");
              /* No metadata => no related, but no error */
            }
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
            g_info ("No xa.metadata in local commit %s ref %s", checksum, flatpak_decomposed_get_ref (ref));
        }
    }

  if (metadata_contents &&
      g_key_file_load_from_data (metakey, metadata_contents, -1, 0, NULL))
    related = flatpak_dir_find_local_related_for_metadata (self, ref, remote_name, metakey, cancellable, error);
  else
    related = g_ptr_array_new_with_free_func ((GDestroyNotify) flatpak_related_free);

  return g_steal_pointer (&related);
}

FlatpakDecomposed *
flatpak_dir_get_remote_auto_install_authenticator_ref (FlatpakDir         *self,
                                                        const char         *remote_name)
{
  g_autofree char *authenticator_name = NULL;
  g_autoptr(FlatpakDecomposed) ref = NULL;

  authenticator_name = flatpak_dir_get_remote_install_authenticator_name (self, remote_name);
  if (authenticator_name != NULL)
    {
      g_autoptr(GError) local_error = NULL;
      ref = flatpak_decomposed_new_from_parts (FLATPAK_KINDS_APP, authenticator_name, flatpak_get_arch (), "autoinstall", &local_error);
      if (ref == NULL)
        g_info ("Invalid authenticator ref: %s\n", local_error->message);
    }

  return g_steal_pointer (&ref);
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

      const char * const *categories = flatpak_get_locale_categories ();

      for (j = 0; categories[j]; j++)
        {
          g_autofree char *prefix = g_strdup_printf ("%s=", categories[j]);
          if (g_str_has_prefix (strv[i], prefix))
            {
              locale = strv[i] + strlen (prefix);
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

static char **
flatpak_dir_get_config_strv (FlatpakDir *self, char *key)
{
  GKeyFile *config = flatpak_dir_get_repo_config (self);
  g_auto(GStrv) lang = NULL;

  if (config)
    {
      if (g_key_file_has_key (config, "core", key, NULL))
        {
          lang = g_key_file_get_string_list (config, "core", key, NULL, NULL);
          return g_steal_pointer (&lang);
        }
    }
  return NULL;
}

static const GPtrArray *
get_system_locales (FlatpakDir *self)
{
  static GPtrArray *cached = NULL;

  if (g_once_init_enter (&cached))
    {
      GPtrArray *langs = g_ptr_array_new_with_free_func (g_free);
      g_autoptr(GDBusProxy) localed_proxy = NULL;
      g_autoptr(GDBusProxy) accounts_proxy = NULL;

      /* Get the system default locales */
      localed_proxy = get_localed_dbus_proxy ();
      if (localed_proxy != NULL)
        get_locale_langs_from_localed_dbus (localed_proxy, langs);

      /* Now add the user account locales from AccountsService. If accounts_proxy is
       * not NULL, it means that AccountsService exists */
      accounts_proxy = get_accounts_dbus_proxy ();
      if (accounts_proxy != NULL)
        get_locale_langs_from_accounts_dbus (accounts_proxy, langs);

      g_ptr_array_add (langs, NULL);

      g_once_init_leave (&cached, langs);
    }

  return (const GPtrArray *)cached;
}

char **
flatpak_dir_get_default_locales (FlatpakDir *self)
{
  g_auto(GStrv) extra_languages = NULL;
  const GPtrArray *langs;

  extra_languages = flatpak_dir_get_config_strv (self, "xa.extra-languages");

  if (flatpak_dir_is_user (self))
    {
      g_auto(GStrv) locale_langs = flatpak_get_current_locale_langs ();

      return sort_strv (flatpak_strv_merge (extra_languages, locale_langs));
    }

  /* Then get the system default locales */
  langs = get_system_locales (self);

  return sort_strv (flatpak_strv_merge (extra_languages, (char **) langs->pdata));
}

char **
flatpak_dir_get_default_locale_languages (FlatpakDir *self)
{
  g_auto(GStrv) extra_languages = NULL;
  const GPtrArray *langs;
  int i;

  extra_languages = flatpak_dir_get_config_strv (self, "xa.extra-languages");
  for (i = 0; extra_languages != NULL && extra_languages[i] != NULL; i++)
    {
      /* Strip the locale, modifier or codeset, if present. */
      gchar *match = strpbrk (extra_languages[i], "._@");
      if (match != NULL)
        *match = '\0';
    }

  if (flatpak_dir_is_user (self))
    {
      g_auto(GStrv) locale_langs = flatpak_get_current_locale_langs ();

      return sort_strv (flatpak_strv_merge (extra_languages, locale_langs));
    }

  /* Then get the system default locales */
  langs = get_system_locales (self);

  return sort_strv (flatpak_strv_merge (extra_languages, (char **) langs->pdata));
}

char **
flatpak_dir_get_locales (FlatpakDir *self)
{
  char **langs = NULL;

  /* Fetch the list of languages specified by xa.languages - if this key is empty,
   * this would mean that all languages are accepted. You can read the man for the
   * flatpak-config section for more info.
   */
  langs = flatpak_dir_get_config_strv (self, "xa.languages");
  if (langs)
    return sort_strv (langs);

  return flatpak_dir_get_default_locales (self);
}


char **
flatpak_dir_get_locale_languages (FlatpakDir *self)
{
  char **langs = NULL;

  /* Fetch the list of languages specified by xa.languages - if this key is empty,
   * this would mean that all languages are accepted. You can read the man for the
   * flatpak-config section for more info.
   */
  langs = flatpak_dir_get_config_strv (self, "xa.languages");
  if (langs)
    return sort_strv (langs);

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

void
flatpak_dir_set_source_pid (FlatpakDir *self,
                            pid_t       pid)
{
  self->source_pid = pid;
}

pid_t
flatpak_dir_get_source_pid (FlatpakDir *self)
{
  return self->source_pid;
}

static void
  (flatpak_dir_log) (FlatpakDir * self,
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
  g_vsnprintf (message + len, sizeof (message) - len, format, args);
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

/* Delete refs that are in refs/mirrors/ rather than refs/remotes/ to prevent
 * disk space from leaking. See https://github.com/flatpak/flatpak/issues/3222
 * The caller is responsible for ensuring that @dir has a repo, and for pruning
 * the repo after calling this function to actually free the disk space.
 */
gboolean
flatpak_dir_delete_mirror_refs (FlatpakDir    *self,
                                gboolean       dry_run,
                                GCancellable  *cancellable,
                                GError       **error)
{
  g_autoptr(GHashTable) collection_refs = NULL;  /* (element-type OstreeCollectionRef utf8) */
  g_autoptr(GPtrArray) ignore_collections = g_ptr_array_new_with_free_func (g_free); /* (element-type utf8) */
  g_auto(GStrv) remotes = NULL;
  const char *repo_collection_id;
  OstreeRepo *repo;
  int i;

  /* Generally a flatpak repo should not have its own collection ID set, but
   * check just in case flatpak is being run on a server for some reason. When
   * a repo has a collection ID set, its own refs from refs/heads/ will be
   * listed by the ostree_repo_list_collection_refs() call below, and we need
   * to be sure not to delete them. There would be no reason to install from a
   * server to itself, so we don't expect refs matching repo_collection_id to
   * be in refs/mirrors/.
   */
  repo = flatpak_dir_get_repo (self);
  repo_collection_id = ostree_repo_get_collection_id (repo);
  if (repo_collection_id != NULL)
    g_ptr_array_add (ignore_collections, g_strdup (repo_collection_id));

  /* Check also for any disabled remotes and ignore any associated
   * collection-refs; in the case of Endless this would be the remote used for
   * OS updates which Flatpak shouldn't touch.
   */
  remotes = ostree_repo_remote_list (repo, NULL);
  for (i = 0; remotes != NULL && remotes[i] != NULL; i++)
    {
      g_autofree char *remote_collection_id = NULL;

      if (!flatpak_dir_get_remote_disabled (self, remotes[i]))
        continue;
      remote_collection_id = flatpak_dir_get_remote_collection_id (self, remotes[i]);
      if (remote_collection_id != NULL)
        g_ptr_array_add (ignore_collections, g_steal_pointer (&remote_collection_id));
    }
  g_ptr_array_add (ignore_collections, NULL);

  if (!ostree_repo_list_collection_refs (repo, NULL, &collection_refs,
                                         OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_REMOTES,
                                         cancellable, error))
    return FALSE;

  /* Now delete any collection-refs which are in refs/mirrors/, were created by
   * Flatpak, and don't belong to a disabled remote.
   */
  GLNX_HASH_TABLE_FOREACH (collection_refs, const OstreeCollectionRef *, c_r)
    {
      if (g_strv_contains ((const char * const *)ignore_collections->pdata, c_r->collection_id))
        {
          g_info ("Ignoring collection-ref (%s, %s) since its remote is disabled or it matches the repo collection ID",
                  c_r->collection_id, c_r->ref_name);
          continue;
        }

      /* Only delete refs which Flatpak created; the repo may have other
       * users. We could check only for refs that come from configured
       * remotes, but that would not cover the case of if a remote was
       * deleted.
       */
      if (flatpak_is_app_runtime_or_appstream_ref (c_r->ref_name) ||
          g_strcmp0 (c_r->ref_name, OSTREE_REPO_METADATA_REF) == 0)
        {
          if (dry_run)
            g_print (_("Skipping deletion of mirror ref (%s, %s)…\n"), c_r->collection_id, c_r->ref_name);
          else
            {
              if (!ostree_repo_set_collection_ref_immediate (repo, c_r, NULL, cancellable, error))
                return FALSE;
            }
        }
    }

  return TRUE;
}


static gboolean
dir_get_metadata (FlatpakDir        *dir,
                  FlatpakDecomposed *ref,
                  GKeyFile         **out_metakey)
{
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_autofree char *metadata_contents = NULL;
  gsize metadata_size;

  deploy_dir = flatpak_dir_get_if_deployed (dir, ref, NULL, NULL);
  if (deploy_dir == NULL)
    return FALSE;

  metadata = g_file_get_child (deploy_dir, "metadata");
  if (!g_file_load_contents (metadata, NULL, &metadata_contents, &metadata_size, NULL, NULL))
    return FALSE;

  metakey = g_key_file_new ();
  if (!g_key_file_load_from_data (metakey, metadata_contents, metadata_size, 0, NULL))
    return FALSE;

  *out_metakey = g_steal_pointer (&metakey);

  return TRUE;
}

static gboolean
maybe_get_metakey (FlatpakDir        *dir,
                   FlatpakDir        *shadowing_dir,
                   FlatpakDecomposed *ref,
                   GHashTable        *metadata_injection,
                   GKeyFile         **out_metakey,
                   gboolean          *out_ref_is_shadowed)
{
  if (shadowing_dir &&
      dir_get_metadata (shadowing_dir, ref, out_metakey))
    {
      *out_ref_is_shadowed = TRUE;
      return TRUE;
    }

  if (metadata_injection != NULL)
    {
      GKeyFile *injected_metakey = g_hash_table_lookup (metadata_injection, flatpak_decomposed_get_ref (ref));
      if (injected_metakey != NULL)
        {
          *out_ref_is_shadowed = FALSE;
          *out_metakey = g_key_file_ref (injected_metakey);
          return TRUE;
        }
    }

  if (dir_get_metadata (dir, ref, out_metakey))
    {
      *out_ref_is_shadowed = FALSE;
      return TRUE;
    }

  return FALSE;
}

static void
queue_ref_for_analysis (FlatpakDecomposed *ref,
                        const char *arch,
                        GHashTable *analyzed_refs,
                        GQueue     *refs_to_analyze)
{
  if (arch != NULL && !flatpak_decomposed_is_arch (ref, arch))
    return;

  if (g_hash_table_lookup (analyzed_refs, ref) != NULL)
    return;

  g_hash_table_add (analyzed_refs, flatpak_decomposed_ref (ref));
  g_queue_push_tail (refs_to_analyze, ref); /* owned by analyzed_refs */
}

/* This traverses from all the "root" refs and into for any recursive dependencies in @self
 * that they use. In the regular case we just consider the @self installation,
 * but we can also handle the case where another directory "shadows" self. For example
 * we might be looking for used refs in the "system" dir, and the "user" dir is
 * shadowing it, meaning that if a ref is installed in the user dir it is considered used
 * from there instead of @self. So, analyzed refs from @shadowing_dir are *not* put
 * in @used_ref (although their dependencies may).
 *
 * Notes:
 *  The "root" refs come from @shadowing_dir if not %NULL and @self otherwise.
 *  refs_to_exclude, and metadata_injection both only affect @self, not @shadowing_dir
 */
static GHashTable *
find_used_refs (FlatpakDir         *self,
                FlatpakDir         *shadowing_dir, /* nullable */
                const char         *arch,
                GHashTable         *metadata_injection,
                GHashTable         *refs_to_exclude,
                GHashTable         *used_refs, /* This is filled in */
                GCancellable       *cancellable,
                GError            **error)
{
  g_autoptr(GPtrArray) root_app_refs = NULL;
  g_autoptr(GPtrArray) root_runtime_refs = NULL;
  g_autoptr(GHashTable) analyzed_refs = NULL;
  g_autoptr(GQueue) refs_to_analyze = NULL;
  FlatpakDir *root_ref_dir;
  FlatpakDecomposed *ref_to_analyze;

  refs_to_analyze = g_queue_new ();
  analyzed_refs = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash, (GEqualFunc)flatpak_decomposed_equal, (GDestroyNotify)flatpak_decomposed_unref, NULL);

  if (shadowing_dir)
    root_ref_dir = shadowing_dir;
  else
    root_ref_dir = self;

  root_app_refs = flatpak_dir_list_refs (root_ref_dir, FLATPAK_KINDS_APP, cancellable, error);
  if (root_app_refs == NULL)
    return NULL;

  for (int i = 0; i < root_app_refs->len; i++)
    {
      FlatpakDecomposed *root_app_ref = g_ptr_array_index (root_app_refs, i);
      queue_ref_for_analysis (root_app_ref, arch, analyzed_refs, refs_to_analyze);
    }

  root_runtime_refs = flatpak_dir_list_refs (root_ref_dir, FLATPAK_KINDS_RUNTIME, cancellable, error);
  if (root_runtime_refs == NULL)
    return NULL;

  for (int i = 0; i < root_runtime_refs->len; i++)
    {
      FlatpakDecomposed *root_runtime_ref = g_ptr_array_index (root_runtime_refs, i);
      /* Consider all shadow dir runtimes as roots because we don't really do full analysis for shadowing_dir.
       * For example a system installed app could end up using the user version of a runtime, which in turn
       * uses a system gl extension.
       *
       * However, for non-shadowed runtime refs, only pinned ones are roots */
      if (root_ref_dir == shadowing_dir ||
          flatpak_dir_ref_is_pinned (root_ref_dir, flatpak_decomposed_get_ref (root_runtime_ref)))
        queue_ref_for_analysis (root_runtime_ref, arch, analyzed_refs, refs_to_analyze);
    }

  /* Any injected refs are considered used, because this is used by transaction
   * to emulate installing a new ref, and we never want the new ref:s dependencies
   * seem ununsed. */
  if (metadata_injection)
    {
      GLNX_HASH_TABLE_FOREACH (metadata_injection, const char *, injected_ref)
        {
          g_autoptr(FlatpakDecomposed) injected = flatpak_decomposed_new_from_ref (injected_ref, NULL);
          if (injected)
            queue_ref_for_analysis (injected, arch, analyzed_refs, refs_to_analyze);
        }
    }

  while ((ref_to_analyze = g_queue_pop_head (refs_to_analyze)) != NULL)
    {
      g_autoptr(GKeyFile) metakey = NULL;
      gboolean ref_is_shadowed;
      gboolean is_app;
      g_autoptr(GPtrArray) related = NULL;
      g_autofree char *sdk = NULL;

      if (!maybe_get_metakey (self, shadowing_dir, ref_to_analyze, metadata_injection,
                              &metakey, &ref_is_shadowed))
        continue; /* Something used something we could not find, that is fine and happens for instance with sdk dependencies */

      if (!ref_is_shadowed)
        {
          /* Mark the analyzed ref used as it wasn't shadowed */
          if (!g_hash_table_contains (used_refs, ref_to_analyze))
            g_hash_table_add (used_refs, flatpak_decomposed_ref (ref_to_analyze));

          /* For excluded refs we mark them as used (above) so that they don't get listed as
           * unused, but we don't analyze them for any dependencies. Note that refs_to_exclude only
           * affects the base dir, so does not affect shadowed refs */
          if (refs_to_exclude != NULL && g_hash_table_contains (refs_to_exclude, ref_to_analyze))
            continue;
        }

      /************************************************
       * Find all dependencies and queue for analysis *
       ***********************************************/

      is_app = flatpak_decomposed_is_app (ref_to_analyze);

      /* App directly depends on its runtime */
      if (is_app)
        {
          g_autofree char *runtime = g_key_file_get_string (metakey, "Application", "runtime", NULL);
          if (runtime)
            {
              g_autoptr(FlatpakDecomposed) runtime_ref = flatpak_decomposed_new_from_pref (FLATPAK_KINDS_RUNTIME, runtime, NULL);
              if (runtime_ref && !flatpak_decomposed_equal (runtime_ref, ref_to_analyze))
                queue_ref_for_analysis (runtime_ref, arch, analyzed_refs, refs_to_analyze);
            }
        }

      /* Both apps and runtims directly depends on its sdk, to avoid suddenly uninstalling something you use to develop the app */
      sdk = g_key_file_get_string (metakey, is_app ? "Application" : "Runtime", "sdk", NULL);
      if (sdk)
        {
          g_autoptr(FlatpakDecomposed) sdk_ref = flatpak_decomposed_new_from_pref (FLATPAK_KINDS_RUNTIME, sdk, NULL);
          if (sdk_ref && !flatpak_decomposed_equal (sdk_ref, ref_to_analyze))
            queue_ref_for_analysis (sdk_ref, arch, analyzed_refs, refs_to_analyze);
        }

      /* Extensions with extra data, that are not specially marked NoRuntime needs the runtime at install.
       * Lets keep it around to not re-download it next update */
      if (!is_app &&
          g_key_file_has_group (metakey, "Extra Data") &&
          !g_key_file_get_boolean (metakey, "Extra Data", "NoRuntime", NULL))
        {
          g_autofree char *extension_runtime_ref = g_key_file_get_string (metakey, "ExtensionOf", "runtime", NULL);
          if (extension_runtime_ref != NULL)
            {
              g_autoptr(FlatpakDecomposed) d = flatpak_decomposed_new_from_ref (extension_runtime_ref, NULL);
              if (d)
                queue_ref_for_analysis (d, arch, analyzed_refs, refs_to_analyze);
            }
        }

      /* We pass NULL for remote-name here, because we want to consider related refs from all remotes */
      related = flatpak_dir_find_local_related_for_metadata (self, ref_to_analyze,
                                                             NULL, metakey, NULL, NULL);
      for (int i = 0; related != NULL && i < related->len; i++)
        {
          FlatpakRelated *rel = g_ptr_array_index (related, i);

          if (!rel->auto_prune)
            {
              queue_ref_for_analysis (rel->ref, arch, analyzed_refs, refs_to_analyze);
            }
        }
    }

  return g_steal_pointer (&used_refs);
}

/* See the documentation for
 * flatpak_installation_list_unused_refs_with_options().
 * The returned pointer array is transfer full. */
char **
flatpak_dir_list_unused_refs (FlatpakDir         *self,
                              const char         *arch,
                              GHashTable         *metadata_injection,
                              GHashTable         *eol_injection,
                              const char * const *refs_to_exclude,
                              gboolean            filter_by_eol,
                              GCancellable       *cancellable,
                              GError            **error)
{
  g_autoptr(GHashTable) used_refs = NULL;
  g_autoptr(GHashTable) excluded_refs_ht = NULL;
  g_autoptr(GPtrArray) refs =  NULL;
  g_autoptr(GPtrArray) runtime_refs = NULL;

  /* Convert refs_to_exclude to hashtable for fast repeated lookups */
  if (refs_to_exclude)
    {
      excluded_refs_ht = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash, (GEqualFunc)flatpak_decomposed_equal, (GDestroyNotify)flatpak_decomposed_unref, NULL);
      for (int i = 0; refs_to_exclude[i] != NULL; i++)
        {
          const char *ref_to_exclude = refs_to_exclude[i];
          g_autoptr(FlatpakDecomposed) d = flatpak_decomposed_new_from_ref (ref_to_exclude, NULL);
          if (d)
            g_hash_table_add (excluded_refs_ht, flatpak_decomposed_ref (d));
        }
    }

  used_refs = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash, (GEqualFunc)flatpak_decomposed_equal, (GDestroyNotify)flatpak_decomposed_unref, NULL);

  if (!find_used_refs (self, NULL, arch, metadata_injection, excluded_refs_ht,
                       used_refs, cancellable, error))
    return NULL;

  /* If @self is a system installation, also check the per-user installation
   * for any apps there using runtimes in the system installation or runtimes
   * there with sdks or extensions in the system installation. Only do so if
   * the per-user installation exists; it wouldn't make sense to create it here
   * if not.
   */
  if (!flatpak_dir_is_user (self))
    {
      g_autoptr(FlatpakDir) user_dir = flatpak_dir_get_user ();
      g_autoptr(GError) local_error = NULL;

      if (!find_used_refs (self, user_dir, arch, metadata_injection, excluded_refs_ht,
                           used_refs, cancellable, &local_error))
        {
          /* We may get permission denied if the process is sandboxed with
           * systemd's ProtectHome=
           */
          if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
              !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return NULL;
            }
        }
    }

  runtime_refs = flatpak_dir_list_refs (self, FLATPAK_KINDS_RUNTIME, cancellable, error);
  if (runtime_refs == NULL)
    return NULL;

  refs = g_ptr_array_new_with_free_func (g_free);

  for (int i = 0; i < runtime_refs->len; i++)
    {
      FlatpakDecomposed *ref = g_ptr_array_index (runtime_refs, i);

      if (g_hash_table_contains (used_refs, ref))
        continue;

      if (arch != NULL && !flatpak_decomposed_is_arch (ref, arch))
        continue;

      if (filter_by_eol)
        {
          gboolean is_eol = FALSE;

          if (eol_injection && g_hash_table_contains (eol_injection, flatpak_decomposed_get_ref (ref)))
            {
              is_eol = GPOINTER_TO_INT (g_hash_table_lookup (eol_injection, ref));
            }
          else
            {
              g_autoptr(GBytes) deploy_data = NULL;

              /* deploy v4 guarantees eol/eolr info */
              deploy_data = flatpak_dir_get_deploy_data (self, ref, 4,
                                                         cancellable, NULL);
              is_eol = deploy_data != NULL &&
                (flatpak_deploy_data_get_eol (deploy_data) != NULL ||
                 flatpak_deploy_data_get_eol_rebase (deploy_data));
            }

          if (!is_eol)
            continue;
        }

      g_ptr_array_add (refs, flatpak_decomposed_dup_ref (ref));
    }

  g_ptr_array_add (refs, NULL);
  return (char **)g_ptr_array_free (g_steal_pointer (&refs), FALSE);
}
