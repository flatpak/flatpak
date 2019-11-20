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

#ifndef __FLATPAK_UTILS_H__
#define __FLATPAK_UTILS_H__

#include <string.h>

#include "libglnx/libglnx.h"
#include <flatpak-common-types-private.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include "flatpak-dbus-generated.h"
#include "flatpak-document-dbus-generated.h"
#include "flatpak-context-private.h"
#include "flatpak-error.h"
#include "flatpak-utils-http-private.h"
#include <ostree.h>
#include <json-glib/json-glib.h>

#define AUTOFS_SUPER_MAGIC 0x0187

#define FLATPAK_ANSI_ALT_SCREEN_ON "\x1b[?1049h"
#define FLATPAK_ANSI_ALT_SCREEN_OFF "\x1b[?1049l"
#define FLATPAK_ANSI_HIDE_CURSOR "\x1b[?25l"
#define FLATPAK_ANSI_SHOW_CURSOR "\x1b[?25h"
#define FLATPAK_ANSI_BOLD_ON "\x1b[1m"
#define FLATPAK_ANSI_BOLD_OFF "\x1b[22m"
#define FLATPAK_ANSI_FAINT_ON "\x1b[2m"
#define FLATPAK_ANSI_FAINT_OFF "\x1b[22m"
#define FLATPAK_ANSI_RED "\x1b[31m"
#define FLATPAK_ANSI_GREEN "\x1b[32m"
#define FLATPAK_ANSI_COLOR_RESET "\x1b[0m"

#define FLATPAK_ANSI_ROW_N "\x1b[%d;1H"
#define FLATPAK_ANSI_CLEAR "\x1b[0J"

void flatpak_get_window_size (int *rows,
                              int *cols);
gboolean flatpak_get_cursor_pos (int *row,
                                 int *col);
void flatpak_hide_cursor (void);
void flatpak_show_cursor (void);

void flatpak_enable_raw_mode (void);
void flatpak_disable_raw_mode (void);

/* https://bugzilla.gnome.org/show_bug.cgi?id=766370 */
#if !GLIB_CHECK_VERSION (2, 49, 3)
#define FLATPAK_VARIANT_BUILDER_INITIALIZER {{0, }}
#define FLATPAK_VARIANT_DICT_INITIALIZER {{0, }}
#else
#define FLATPAK_VARIANT_BUILDER_INITIALIZER {{{0, }}}
#define FLATPAK_VARIANT_DICT_INITIALIZER {{{0, }}}
#endif

/* https://github.com/GNOME/libglnx/pull/38
 * Note by using #define rather than wrapping via a static inline, we
 * don't have to re-define attributes like G_GNUC_PRINTF.
 */
#define flatpak_fail glnx_throw

gboolean flatpak_fail_error (GError     **error,
                             FlatpakError code,
                             const char  *fmt,
                             ...) G_GNUC_PRINTF (3, 4);

void flatpak_debug2 (const char *format,
                     ...) G_GNUC_PRINTF (1, 2);

gint flatpak_strcmp0_ptr (gconstpointer a,
                          gconstpointer b);

/* Sometimes this is /var/run which is a symlink, causing weird issues when we pass
 * it as a path into the sandbox */
char * flatpak_get_real_xdg_runtime_dir (void);

gboolean  flatpak_has_path_prefix (const char *str,
                                   const char *prefix);

const char * flatpak_path_match_prefix (const char *pattern,
                                        const char *path);

void     flatpak_disable_fancy_output (void);
void     flatpak_enable_fancy_output (void);
gboolean flatpak_fancy_output (void);

const char * flatpak_get_arch (void);
const char ** flatpak_get_arches (void);
gboolean flatpak_is_linux32_arch (const char *arch);

const char ** flatpak_get_gl_drivers (void);
gboolean flatpak_extension_matches_reason (const char *extension_id,
                                           const char *reason,
                                           gboolean    default_value);

const char * flatpak_get_bwrap (void);

char **flatpak_strv_merge (char   **strv1,
                           char   **strv2);
char **flatpak_subpaths_merge (char **subpaths1,
                               char **subpaths2);

char *flatpak_get_lang_from_locale (const char *locale);
char **flatpak_get_current_locale_langs (void);

gboolean flatpak_write_update_checksum (GOutputStream *out,
                                        gconstpointer  data,
                                        gsize          len,
                                        gsize         *out_bytes_written,
                                        GChecksum     *checksum,
                                        GCancellable  *cancellable,
                                        GError       **error);


gboolean flatpak_splice_update_checksum (GOutputStream         * out,
                                         GInputStream          *in,
                                         GChecksum             *checksum,
                                         FlatpakLoadUriProgress progress,
                                         gpointer progress_data,
                                         GCancellable          *cancellable,
                                         GError               **error);

GBytes * flatpak_read_stream (GInputStream * in,
                              gboolean null_terminate,
                              GError      **error);

gboolean flatpak_variant_save (GFile        *dest,
                               GVariant     *variant,
                               GCancellable *cancellable,
                               GError      **error);
gboolean flatpak_variant_bsearch_str (GVariant   *array,
                                      const char *str,
                                      int        *out_pos);
GVariant *flatpak_repo_load_summary (OstreeRepo *repo,
                                     GError    **error);
char **  flatpak_summary_match_subrefs (GVariant   *summary,
                                        const char *collection_id,
                                        const char *ref);
gboolean flatpak_summary_lookup_ref (GVariant   *summary,
                                     const char *collection_id,
                                     const char *ref,
                                     char      **out_checksum,
                                     GVariant  **out_variant);

gboolean flatpak_name_matches_one_wildcard_prefix (const char         *string,
                                                   const char * const *maybe_wildcard_prefixes,
                                                   gboolean            require_exact_match);

gboolean flatpak_get_allowed_exports (const char     *source_path,
                                      const char     *app_id,
                                      FlatpakContext *context,
                                      char         ***allowed_extensions_out,
                                      char         ***allowed_prefixes_out,
                                      gboolean       *require_exact_match_out);

gboolean flatpak_is_valid_name (const char *string,
                                GError    **error);
gboolean flatpak_is_valid_branch (const char *string,
                                  GError    **error);
gboolean flatpak_has_name_prefix (const char *string,
                                  const char *name);

char * flatpak_make_valid_id_prefix (const char *orig_id);
gboolean flatpak_id_has_subref_suffix (const char *id);

char **flatpak_decompose_ref (const char *ref,
                              GError    **error);

char * flatpak_filter_glob_to_regexp (const char *glob, GError **error);
gboolean flatpak_parse_filters (const char *data,
                                GRegex **allow_refs_out,
                                GRegex **deny_refs_out,
                                GError **error);
gboolean flatpak_filters_allow_ref (GRegex *allow_refs,
                                    GRegex *deny_refs,
                                    const char *ref);

FlatpakKinds flatpak_kinds_from_bools (gboolean app,
                                       gboolean runtime);

gboolean flatpak_split_partial_ref_arg (const char   *partial_ref,
                                        FlatpakKinds  default_kinds,
                                        const char   *default_arch,
                                        const char   *default_branch,
                                        FlatpakKinds *out_kinds,
                                        char        **out_id,
                                        char        **out_arch,
                                        char        **out_branch,
                                        GError      **error);
gboolean flatpak_split_partial_ref_arg_novalidate (const char   *partial_ref,
                                                   FlatpakKinds  default_kinds,
                                                   const char   *default_arch,
                                                   const char   *default_branch,
                                                   FlatpakKinds *out_kinds,
                                                   char        **out_id,
                                                   char        **out_arch,
                                                   char        **out_branch);

int flatpak_compare_ref (const char *ref1,
                         const char *ref2);

char * flatpak_compose_ref (gboolean    app,
                            const char *name,
                            const char *branch,
                            const char *arch,
                            GError    **error);

char * flatpak_build_untyped_ref (const char *runtime,
                                  const char *branch,
                                  const char *arch);
char * flatpak_build_runtime_ref (const char *runtime,
                                  const char *branch,
                                  const char *arch);
char * flatpak_build_app_ref (const char *app,
                              const char *branch,
                              const char *arch);
char * flatpak_find_current_ref (const char   *app_id,
                                 GCancellable *cancellable,
                                 GError      **error);
GFile *flatpak_find_deploy_dir_for_ref (const char   *ref,
                                        FlatpakDir  **dir_out,
                                        GCancellable *cancellable,
                                        GError      **error);
GFile * flatpak_find_files_dir_for_ref (const char   *ref,
                                        GCancellable *cancellable,
                                        GError      **error);
GFile * flatpak_find_unmaintained_extension_dir_if_exists (const char   *name,
                                                           const char   *arch,
                                                           const char   *branch,
                                                           GCancellable *cancellable);
FlatpakDeploy * flatpak_find_deploy_for_ref_in (GPtrArray    *dirs,
                                                const char   *ref,
                                                const char   *commit,
                                                GCancellable *cancellable,
                                                GError      **error);
FlatpakDeploy * flatpak_find_deploy_for_ref (const char   *ref,
                                             const char   *commit,
                                             GCancellable *cancellable,
                                             GError      **error);
char ** flatpak_list_deployed_refs (const char   *type,
                                    const char   *name_prefix,
                                    const char   *arch,
                                    const char   *branch,
                                    GCancellable *cancellable,
                                    GError      **error);
char ** flatpak_list_unmaintained_refs (const char   *name_prefix,
                                        const char   *branch,
                                        const char   *arch,
                                        GCancellable *cancellable,
                                        GError      **error);

gboolean flatpak_remove_dangling_symlinks (GFile        *dir,
                                           GCancellable *cancellable,
                                           GError      **error);

gboolean flatpak_utils_ascii_string_to_unsigned (const gchar *str,
                                                 guint        base,
                                                 guint64      min,
                                                 guint64      max,
                                                 guint64     *out_num,
                                                 GError     **error);


#if !GLIB_CHECK_VERSION (2, 40, 0)
static inline gboolean
g_key_file_save_to_file (GKeyFile    *key_file,
                         const gchar *filename,
                         GError     **error)
{
  gchar *contents;
  gboolean success;
  gsize length;

  contents = g_key_file_to_data (key_file, &length, NULL);
  success = g_file_set_contents (filename, contents, length, error);
  g_free (contents);

  return success;
}
#endif

#if !GLIB_CHECK_VERSION (2, 50, 0)
static inline gboolean
g_key_file_load_from_bytes (GKeyFile     *key_file,
                            GBytes       *bytes,
                            GKeyFileFlags flags,
                            GError      **error)
{
  const guchar *data;
  gsize size;

  data = g_bytes_get_data (bytes, &size);
  return g_key_file_load_from_data (key_file, (const gchar *) data, size, flags, error);
}
#endif


#if !GLIB_CHECK_VERSION (2, 56, 0)
GDateTime *flatpak_g_date_time_new_from_iso8601 (const gchar *text,
                                                 GTimeZone   *default_tz);

static inline GDateTime *
g_date_time_new_from_iso8601 (const gchar *text, GTimeZone *default_tz)
{
  return flatpak_g_date_time_new_from_iso8601 (text, default_tz);
}
#endif


#if !GLIB_CHECK_VERSION (2, 56, 0)
typedef void (* GClearHandleFunc) (guint handle_id);

static inline void
g_clear_handle_id (guint            *tag_ptr,
                   GClearHandleFunc  clear_func)
{
  guint _handle_id;

  _handle_id = *tag_ptr;
  if (_handle_id > 0)
    {
      *tag_ptr = 0;
      clear_func (_handle_id);
    }
}
#endif


#if !GLIB_CHECK_VERSION (2, 58, 0)
static inline gboolean
g_hash_table_steal_extended (GHashTable    *hash_table,
                             gconstpointer  lookup_key,
                             gpointer      *stolen_key,
                             gpointer      *stolen_value)
{
  if (g_hash_table_lookup_extended (hash_table, lookup_key, stolen_key, stolen_value))
    {
      g_hash_table_steal (hash_table, lookup_key);
      return TRUE;
    }
  else
      return FALSE;
}
#endif

gboolean flatpak_g_ptr_array_contains_string (GPtrArray  *array,
                                              const char *str);

/* Returns the first string in subset that is not in strv */
static inline const gchar *
g_strv_subset (const gchar * const *strv,
               const gchar * const *subset)
{
  int i;

  for (i = 0; subset[i]; i++)
    {
      const char *key;

      key = subset[i];
      if (!g_strv_contains (strv, key))
        return key;
    }

  return NULL;
}

static inline void
flatpak_auto_unlock_helper (GMutex **mutex)
{
  if (*mutex)
    g_mutex_unlock (*mutex);
}

static inline GMutex *
flatpak_auto_lock_helper (GMutex *mutex)
{
  if (mutex)
    g_mutex_lock (mutex);
  return mutex;
}

gboolean flatpak_switch_symlink_and_remove (const char *symlink_path,
                                            const char *target,
                                            GError    **error);

GKeyFile * flatpak_parse_repofile (const char   *remote_name,
                                   gboolean      from_ref,
                                   GKeyFile     *keyfile,
                                   GBytes      **gpg_data_out,
                                   GCancellable *cancellable,
                                   GError      **error);

gboolean flatpak_repo_set_title (OstreeRepo *repo,
                                 const char *title,
                                 GError    **error);
gboolean flatpak_repo_set_comment (OstreeRepo *repo,
                                   const char *comment,
                                   GError    **error);
gboolean flatpak_repo_set_description (OstreeRepo *repo,
                                       const char *description,
                                       GError    **error);
gboolean flatpak_repo_set_icon (OstreeRepo *repo,
                                const char *icon,
                                GError    **error);
gboolean flatpak_repo_set_homepage (OstreeRepo *repo,
                                    const char *homepage,
                                    GError    **error);
gboolean flatpak_repo_set_redirect_url (OstreeRepo *repo,
                                        const char *redirect_url,
                                        GError    **error);
gboolean flatpak_repo_set_default_branch (OstreeRepo *repo,
                                          const char *branch,
                                          GError    **error);
gboolean flatpak_repo_set_collection_id (OstreeRepo *repo,
                                         const char *collection_id,
                                         GError    **error);
gboolean flatpak_repo_set_deploy_collection_id (OstreeRepo *repo,
                                                gboolean    deploy_collection_id,
                                                GError    **error);
gboolean flatpak_repo_set_gpg_keys (OstreeRepo *repo,
                                    GBytes     *bytes,
                                    GError    **error);
gboolean flatpak_repo_update (OstreeRepo   *repo,
                              const char  **gpg_key_ids,
                              const char   *gpg_homedir,
                              GCancellable *cancellable,
                              GError      **error);
gboolean flatpak_repo_collect_sizes (OstreeRepo   *repo,
                                     GFile        *root,
                                     guint64      *installed_size,
                                     guint64      *download_size,
                                     GCancellable *cancellable,
                                     GError      **error);
GVariant *flatpak_commit_get_extra_data_sources (GVariant *commitv,
                                                 GError  **error);
GVariant *flatpak_repo_get_extra_data_sources (OstreeRepo   *repo,
                                               const char   *rev,
                                               GCancellable *cancellable,
                                               GError      **error);
void flatpak_repo_parse_extra_data_sources (GVariant      *extra_data_sources,
                                            int            index,
                                            const char   **name,
                                            guint64       *download_size,
                                            guint64       *installed_size,
                                            const guchar **sha256,
                                            const char   **uri);
gboolean flatpak_mtree_ensure_dir_metadata (OstreeRepo        *repo,
                                            OstreeMutableTree *mtree,
                                            GCancellable      *cancellable,
                                            GError           **error);
gboolean flatpak_mtree_create_symlink (OstreeRepo         *repo,
                                       OstreeMutableTree  *parent,
                                       const char         *name,
                                       const char         *target,
                                       GError            **error);
gboolean flatpak_mtree_add_file_from_bytes (OstreeRepo *repo,
                                            GBytes *bytes,
                                            OstreeMutableTree *parent,
                                            const char *filename,
                                            GCancellable *cancellable,
                                            GError      **error);
gboolean flatpak_mtree_create_dir (OstreeRepo         *repo,
                                   OstreeMutableTree  *parent,
                                   const char         *name,
                                   OstreeMutableTree **dir_out,
                                   GError            **error);


GVariant * flatpak_bundle_load (GFile   *file,
                                char   **commit,
                                char   **ref,
                                char   **origin,
                                char   **runtime_repo,
                                char   **app_metadata,
                                guint64 *installed_size,
                                GBytes **gpg_keys,
                                char   **collection_id,
                                GError **error);

gboolean flatpak_pull_from_bundle (OstreeRepo   *repo,
                                   GFile        *file,
                                   const char   *remote,
                                   const char   *ref,
                                   gboolean      require_gpg_signature,
                                   GCancellable *cancellable,
                                   GError      **error);

typedef void (*FlatpakOciPullProgress) (guint64  total_size,
                                        guint64  pulled_size,
                                        guint32  n_layers,
                                        guint32  pulled_layers,
                                        gpointer data);

char * flatpak_pull_from_oci (OstreeRepo            *repo,
                              FlatpakOciRegistry    *registry,
                              const char            *oci_repository,
                              const char            *digest,
                              FlatpakOciManifest    *manifest,
                              FlatpakOciImage       *image_config,
                              const char            *remote,
                              const char            *ref,
                              FlatpakOciPullProgress progress_cb,
                              gpointer               progress_data,
                              GCancellable          *cancellable,
                              GError               **error);

gboolean flatpak_mirror_image_from_oci (FlatpakOciRegistry    *dst_registry,
                                        FlatpakOciRegistry    *registry,
                                        const char            *oci_repository,
                                        const char            *digest,
                                        const char            *ref,
                                        FlatpakOciPullProgress progress_cb,
                                        gpointer               progress_data,
                                        GCancellable          *cancellable,
                                        GError               **error);

typedef struct
{
  char    *id;
  char    *installed_id;
  char    *commit;
  char    *ref;
  char    *directory;
  char    *files_path;
  char    *subdir_suffix;
  char    *add_ld_path;
  char   **merge_dirs;
  int      priority;
  gboolean needs_tmpfs;
  gboolean is_unmaintained;
} FlatpakExtension;

void flatpak_extension_free (FlatpakExtension *extension);

void flatpak_parse_extension_with_tag (const char *extension,
                                       char      **name,
                                       char      **tag);

GList *flatpak_list_extensions (GKeyFile   *metakey,
                                const char *arch,
                                const char *branch);

char * flatpak_quote_argv (const char *argv[],
                           gssize      len);
gboolean flatpak_file_arg_has_suffix (const char *arg,
                                      const char *suffix);

const char *flatpak_file_get_path_cached (GFile *file);

GFile *flatpak_build_file_va (GFile  *base,
                              va_list args);
GFile *flatpak_build_file (GFile *base,
                           ...) G_GNUC_NULL_TERMINATED;

gboolean flatpak_openat_noatime (int           dfd,
                                 const char   *name,
                                 int          *ret_fd,
                                 GCancellable *cancellable,
                                 GError      **error);

typedef enum {
  FLATPAK_CP_FLAGS_NONE = 0,
  FLATPAK_CP_FLAGS_MERGE = 1 << 0,
  FLATPAK_CP_FLAGS_NO_CHOWN = 1 << 1,
  FLATPAK_CP_FLAGS_MOVE = 1 << 2,
} FlatpakCpFlags;

gboolean   flatpak_cp_a (GFile         *src,
                         GFile         *dest,
                         FlatpakCpFlags flags,
                         GCancellable  *cancellable,
                         GError       **error);

gboolean flatpak_mkdir_p (GFile        *dir,
                          GCancellable *cancellable,
                          GError      **error);

gboolean flatpak_rm_rf (GFile        *dir,
                        GCancellable *cancellable,
                        GError      **error);

gboolean flatpak_canonicalize_permissions (int         parent_dfd,
                                           const char *rel_path,
                                           int         uid,
                                           int         gid,
                                           GError    **error);

gboolean flatpak_file_rename (GFile        *from,
                              GFile        *to,
                              GCancellable *cancellable,
                              GError      **error);

gboolean flatpak_open_in_tmpdir_at (int             tmpdir_fd,
                                    int             mode,
                                    char           *tmpl,
                                    GOutputStream **out_stream,
                                    GCancellable   *cancellable,
                                    GError        **error);

gboolean flatpak_buffer_to_sealed_memfd_or_tmpfile (GLnxTmpfile *tmpf,
                                                    const char  *name,
                                                    const char  *str,
                                                    size_t       len,
                                                    GError     **error);

static inline void
flatpak_temp_dir_destroy (void *p)
{
  GFile *dir = p;

  if (dir)
    {
      flatpak_rm_rf (dir, NULL, NULL);
      g_object_unref (dir);
    }
}

typedef GFile FlatpakTempDir;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakTempDir, flatpak_temp_dir_destroy)

typedef GMainContext GMainContextPopDefault;
static inline void
flatpak_main_context_pop_default_destroy (void *p)
{
  GMainContext *main_context = p;

  if (main_context)
    g_main_context_pop_thread_default (main_context);
}

static inline GMainContextPopDefault *
flatpak_main_context_new_default (void)
{
  GMainContext *main_context = g_main_context_new ();

  g_main_context_push_thread_default (main_context);
  return main_context;
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GMainContextPopDefault, flatpak_main_context_pop_default_destroy)

typedef OstreeRepo FlatpakRepoTransaction;

static inline void
flatpak_repo_transaction_cleanup (void *p)
{
  OstreeRepo *repo = p;

  if (repo)
    {
      g_autoptr(GError) error = NULL;
      if (!ostree_repo_abort_transaction (repo, NULL, &error))
        g_warning ("Error aborting ostree transaction: %s", error->message);
    }
}

static inline FlatpakRepoTransaction *
flatpak_repo_transaction_start (OstreeRepo   *repo,
                                GCancellable *cancellable,
                                GError      **error)
{
  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return NULL;
  return (FlatpakRepoTransaction *) repo;
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakRepoTransaction, flatpak_repo_transaction_cleanup)

#define AUTOLOCK(name) G_GNUC_UNUSED __attribute__((cleanup (flatpak_auto_unlock_helper))) GMutex * G_PASTE (auto_unlock, __LINE__) = flatpak_auto_lock_helper (&G_LOCK_NAME (name))

#if !defined(SOUP_AUTOCLEANUPS_H) && !defined(__SOUP_AUTOCLEANUPS_H__)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SoupSession, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SoupMessage, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SoupRequest, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SoupRequestHTTP, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SoupURI, soup_uri_free)
#endif

#if !JSON_CHECK_VERSION (1, 1, 2)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (JsonArray, json_array_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (JsonBuilder, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (JsonGenerator, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (JsonNode, json_node_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (JsonObject, json_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (JsonParser, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (JsonPath, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (JsonReader, g_object_unref)
#endif

#if !GLIB_CHECK_VERSION (2, 43, 4)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GUnixFDList, g_object_unref)
#endif

/* This uses a weird Auto prefix to avoid conflicts with later added autogenerated autoptr support, per:
 * https://git.gnome.org/browse/glib/commit/?id=1c6cd5f0a3104aa9b62c7f1d3086181f63e71b59
 */
typedef FlatpakSessionHelper AutoFlatpakSessionHelper;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoFlatpakSessionHelper, g_object_unref)

G_DEFINE_AUTOPTR_CLEANUP_FUNC (XdpDbusDocuments, g_object_unref)

typedef struct FlatpakXml FlatpakXml;

struct FlatpakXml
{
  gchar      *element_name; /* NULL == text */
  char      **attribute_names;
  char      **attribute_values;
  char       *text;
  FlatpakXml *parent;
  FlatpakXml *first_child;
  FlatpakXml *last_child;
  FlatpakXml *next_sibling;
};

FlatpakXml *flatpak_xml_new (const gchar *element_name);
FlatpakXml *flatpak_xml_new_text (const gchar *text);
void       flatpak_xml_add (FlatpakXml *parent,
                            FlatpakXml *node);
void       flatpak_xml_free (FlatpakXml *node);
FlatpakXml *flatpak_xml_parse (GInputStream * in,
                               gboolean compressed,
                               GCancellable *cancellable,
                               GError      **error);
void       flatpak_xml_to_string (FlatpakXml *node,
                                  GString    *res);
FlatpakXml *flatpak_xml_unlink (FlatpakXml *node,
                                FlatpakXml *prev_sibling);
FlatpakXml *flatpak_xml_find (FlatpakXml  *node,
                              const char  *type,
                              FlatpakXml **prev_child_out);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakXml, flatpak_xml_free);

FlatpakXml *flatpak_appstream_xml_new (void);
gboolean   flatpak_appstream_xml_migrate (FlatpakXml *source,
                                          FlatpakXml *dest,
                                          const char *ref,
                                          const char *id,
                                          GKeyFile   *metadata);
gboolean flatpak_appstream_xml_root_to_data (FlatpakXml *appstream_root,
                                             GBytes    **uncompressed,
                                             GBytes    **compressed,
                                             GError    **error);
gboolean   flatpak_repo_generate_appstream (OstreeRepo   *repo,
                                            const char  **gpg_key_ids,
                                            const char   *gpg_homedir,
                                            guint64       timestamp,
                                            GCancellable *cancellable,
                                            GError      **error);
void flatpak_appstream_xml_filter (FlatpakXml *appstream,
                                   GRegex *allow_refs,
                                   GRegex *deny_refs);


gboolean flatpak_allocate_tmpdir (int           tmpdir_dfd,
                                  const char   *tmpdir_relpath,
                                  const char   *tmpdir_prefix,
                                  char        **tmpdir_name_out,
                                  int          *tmpdir_fd_out,
                                  GLnxLockFile *file_lock_out,
                                  gboolean     *reusing_dir_out,
                                  GCancellable *cancellable,
                                  GError      **error);


gboolean flatpak_yes_no_prompt (gboolean    default_yes,
                                const char *prompt,
                                ...) G_GNUC_PRINTF (2, 3);

long flatpak_number_prompt (gboolean    default_yes,
                            int         min,
                            int         max,
                            const char *prompt,
                            ...) G_GNUC_PRINTF (4, 5);
int *flatpak_numbers_prompt (gboolean    default_yes,
                             int         min,
                             int         max,
                             const char *prompt,
                             ...) G_GNUC_PRINTF (4, 5);
int *flatpak_parse_numbers (const char *buf,
                            int         min,
                            int         max);

void flatpak_format_choices (const char **choices,
                             const char  *prompt,
                             ...) G_GNUC_PRINTF (2, 3);

typedef void (*FlatpakProgressCallback)(const char *status,
                                        guint       progress,
                                        gboolean    estimating,
                                        gpointer    user_data);

OstreeAsyncProgress *flatpak_progress_new (FlatpakProgressCallback progress,
                                           gpointer                progress_data);

OstreeAsyncProgress *flatpak_progress_chain (OstreeAsyncProgress *progress);

static inline void
flatpak_progress_unchain (OstreeAsyncProgress *chained_progress)
{
#if OSTREE_CHECK_VERSION (2019, 6)
  if (chained_progress != NULL)
    ostree_async_progress_finish (chained_progress);
#endif
}

typedef OstreeAsyncProgress FlatpakAsyncProgressChained;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakAsyncProgressChained, flatpak_progress_unchain);

void flatpak_log_dir_access (FlatpakDir *dir);

gboolean flatpak_check_required_version (const char *ref,
                                         GKeyFile   *metakey,
                                         GError    **error);

int flatpak_levenshtein_distance (const char *s,
                                  const char *t);

char *   flatpak_dconf_path_for_app_id (const char *app_id);
gboolean flatpak_dconf_path_is_similar (const char *path1,
                                        const char *path2);

gboolean flatpak_repo_resolve_rev (OstreeRepo    *repo,
                                   const char    *collection_id, /* nullable */
                                   const char    *remote_name, /* nullable */
                                   const char    *ref_name,
                                   gboolean       allow_noent,
                                   char         **out_rev,
                                   GCancellable  *cancellable,
                                   GError       **error);

#define FLATPAK_MESSAGE_ID "c7b39b1e006b464599465e105b361485"

#endif /* __FLATPAK_UTILS_H__ */
