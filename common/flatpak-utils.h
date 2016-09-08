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
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <libsoup/soup.h>
#include "flatpak-dbus.h"
#include "flatpak-dir.h"
#include <ostree.h>

typedef enum {
  FLATPAK_HOST_COMMAND_FLAGS_CLEAR_ENV = 1 << 0,
} FlatpakHostCommandFlags;


gboolean flatpak_fail (GError    **error,
                       const char *format,
                       ...);

gint flatpak_strcmp0_ptr (gconstpointer a,
                          gconstpointer b);

const char * flatpak_path_match_prefix (const char *pattern,
                                        const char *path);

gboolean flatpak_is_in_sandbox (void);

const char * flatpak_get_arch (void);
const char ** flatpak_get_arches (void);

const char * flatpak_get_bwrap (void);

char ** flatpak_get_current_locale_subpaths (void);

void flatpak_migrate_from_xdg_app (void);

GBytes * flatpak_read_stream (GInputStream *in,
                              gboolean      null_terminate,
                              GError      **error);

gboolean flatpak_variant_save (GFile        *dest,
                               GVariant     *variant,
                               GCancellable *cancellable,
                               GError      **error);
gboolean flatpak_variant_bsearch_str (GVariant   *array,
                                      const char *str,
                                      int        *out_pos);
GVariant *flatpak_repo_load_summary (OstreeRepo *repo,
                                     GError **error);
char **  flatpak_summary_match_subrefs (GVariant *summary,
                                        const char *ref);
gboolean flatpak_summary_lookup_ref (GVariant   *summary,
                                     const char *ref,
                                     char      **out_checksum);

gboolean flatpak_has_name_prefix (const char *string,
                                  const char *name);
gboolean flatpak_is_valid_name (const char *string);
gboolean flatpak_is_valid_branch (const char *string);

char **flatpak_decompose_ref (const char *ref,
                              GError    **error);

gboolean flatpak_split_partial_ref_arg (char *partial_ref,
                                        char **inout_arch,
                                        char **inout_branch,
                                        GError    **error);

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
GFile * flatpak_find_files_dir_for_ref (const char   *ref,
                                        GCancellable *cancellable,
                                        GError      **error);
FlatpakDeploy * flatpak_find_deploy_for_ref (const char   *ref,
                                             GCancellable *cancellable,
                                             GError      **error);
char ** flatpak_list_deployed_refs (const char   *type,
                                    const char   *name_prefix,
                                    const char   *branch,
                                    const char   *arch,
                                    GCancellable *cancellable,
                                    GError      **error);

gboolean flatpak_overlay_symlink_tree (GFile        *source,
                                       GFile        *destination,
                                       const char   *symlink_prefix,
                                       GCancellable *cancellable,
                                       GError      **error);
gboolean flatpak_remove_dangling_symlinks (GFile        *dir,
                                           GCancellable *cancellable,
                                           GError      **error);

void  flatpak_invocation_lookup_app_info (GDBusMethodInvocation *invocation,
                                          GCancellable          *cancellable,
                                          GAsyncReadyCallback    callback,
                                          gpointer               user_data);

GKeyFile *flatpak_invocation_lookup_app_info_finish (GDBusMethodInvocation *invocation,
                                                     GAsyncResult          *result,
                                                     GError               **error);

void  flatpak_connection_track_name_owners (GDBusConnection *connection);

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

gint flatpak_mkstempat (int    dir_fd,
                        gchar *tmpl,
                        int    flags,
                        int    mode);


typedef struct FlatpakTablePrinter FlatpakTablePrinter;

FlatpakTablePrinter *flatpak_table_printer_new (void);
void                flatpak_table_printer_free (FlatpakTablePrinter *printer);
void                flatpak_table_printer_add_column (FlatpakTablePrinter *printer,
                                                      const char          *text);
void                flatpak_table_printer_append_with_comma (FlatpakTablePrinter *printer,
                                                             const char          *text);
void                flatpak_table_printer_finish_row (FlatpakTablePrinter *printer);
void                flatpak_table_printer_print (FlatpakTablePrinter *printer);

gboolean flatpak_repo_set_title (OstreeRepo *repo,
                                 const char *title,
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

gboolean flatpak_mtree_create_root (OstreeRepo        *repo,
                                    OstreeMutableTree *mtree,
                                    GCancellable      *cancellable,
                                    GError           **error);

GVariant * flatpak_bundle_load (GFile   *file,
                                char   **commit,
                                char   **ref,
                                char   **origin,
                                guint64 *installed_size,
                                GBytes **gpg_keys,
                                GError **error);

gboolean flatpak_pull_from_bundle (OstreeRepo   *repo,
                                   GFile        *file,
                                   const char   *remote,
                                   const char   *ref,
                                   gboolean      require_gpg_signature,
                                   GCancellable *cancellable,
                                   GError      **error);


typedef struct
{
  char *id;
  char *installed_id;
  char *ref;
  char *directory;
  char *files_path;
  gboolean needs_tmpfs;
} FlatpakExtension;

void flatpak_extension_free (FlatpakExtension *extension);

GList *flatpak_list_extensions (GKeyFile   *metakey,
                                const char *arch,
                                const char *branch);

gboolean            flatpak_spawn (GFile       *dir,
                                   char       **output,
                                   GError     **error,
                                   const gchar *argv0,
                                   va_list      args);

gboolean            flatpak_spawnv (GFile                *dir,
                                    char                **output,
                                    GError              **error,
                                    const gchar * const  *argv);

const char *flatpak_file_get_path_cached (GFile *file);

gboolean flatpak_openat_noatime (int            dfd,
                                 const char    *name,
                                 int           *ret_fd,
                                 GCancellable  *cancellable,
                                 GError       **error);

typedef enum {
  FLATPAK_CP_FLAGS_NONE = 0,
  FLATPAK_CP_FLAGS_MERGE = 1<<0,
  FLATPAK_CP_FLAGS_NO_CHOWN = 1<<1,
  FLATPAK_CP_FLAGS_MOVE = 1<<2,
} FlatpakCpFlags;

gboolean   flatpak_cp_a (GFile         *src,
                         GFile         *dest,
                         FlatpakCpFlags flags,
                         GCancellable  *cancellable,
                         GError       **error);

gboolean flatpak_zero_mtime (int parent_dfd,
                             const char *rel_path,
                             GCancellable  *cancellable,
                             GError       **error);

gboolean flatpak_mkdir_p (GFile         *dir,
                          GCancellable  *cancellable,
                          GError       **error);

gboolean flatpak_rm_rf (GFile         *dir,
                        GCancellable  *cancellable,
                        GError       **error);

gboolean flatpak_file_rename (GFile *from,
                              GFile *to,
                              GCancellable  *cancellable,
                              GError       **error);

gboolean flatpak_open_in_tmpdir_at (int                tmpdir_fd,
                                    int                mode,
                                    char              *tmpl,
                                    GOutputStream    **out_stream,
                                    GCancellable      *cancellable,
                                    GError           **error);

#define flatpak_autorm_rf _GLIB_CLEANUP (g_autoptr_cleanup_generic_gfree)

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
flatpak_repo_transaction_start (OstreeRepo     *repo,
                                GCancellable   *cancellable,
                                GError        **error)
{
  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return NULL;
  return (FlatpakRepoTransaction *)repo;
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakRepoTransaction, flatpak_repo_transaction_cleanup)

#define AUTOLOCK(name) G_GNUC_UNUSED __attribute__((cleanup (flatpak_auto_unlock_helper))) GMutex * G_PASTE (auto_unlock, __LINE__) = flatpak_auto_lock_helper (&G_LOCK_NAME (name))

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeRepo, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeMutableTree, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeAsyncProgress, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeGpgVerifyResult, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeRepoCommitModifier, ostree_repo_commit_modifier_unref)

#ifndef SOUP_AUTOCLEANUPS_H
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SoupSession, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SoupMessage, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SoupRequest, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SoupURI, soup_uri_free)
#endif

#if !GLIB_CHECK_VERSION(2, 43, 4)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GUnixFDList, g_object_unref)
#endif

/* This uses a weird Auto prefix to avoid conflicts with later added autogenerated autoptr support, per:
 * https://git.gnome.org/browse/glib/commit/?id=1c6cd5f0a3104aa9b62c7f1d3086181f63e71b59
 */
typedef FlatpakSessionHelper AutoFlatpakSessionHelper;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoFlatpakSessionHelper, g_object_unref)

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
FlatpakXml *flatpak_xml_parse (GInputStream *in,
                               gboolean      compressed,
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
GBytes *flatpak_appstream_xml_root_to_data (FlatpakXml *appstream_root,
                                            GError    **error);
gboolean   flatpak_repo_generate_appstream (OstreeRepo   *repo,
                                            const char  **gpg_key_ids,
                                            const char   *gpg_homedir,
                                            GCancellable *cancellable,
                                            GError      **error);

gboolean flatpak_allocate_tmpdir (int           tmpdir_dfd,
                                  const char   *tmpdir_relpath,
                                  const char   *tmpdir_prefix,
                                  char        **tmpdir_name_out,
                                  int          *tmpdir_fd_out,
                                  GLnxLockFile *file_lock_out,
                                  gboolean     *reusing_dir_out,
                                  GCancellable *cancellable,
                                  GError      **error);


typedef struct {
  char *shell_cur;
  char *cur;
  char *prev;
  char *line;
  int point;
  char **argv;
  char **original_argv;
  int argc;
  int original_argc;
} FlatpakCompletion;

void flatpak_completion_debug (const gchar *format, ...);

FlatpakCompletion *flatpak_completion_new   (const char        *arg_line,
                                             const char        *arg_point,
                                             const char        *arg_cur);
void               flatpak_complete_word    (FlatpakCompletion *completion,
                                             char              *format,
                                             ...);
void               flatpak_complete_ref     (FlatpakCompletion *completion,
                                             OstreeRepo        *repo);
void               flatpak_complete_file    (FlatpakCompletion *completion);
void               flatpak_complete_dir     (FlatpakCompletion *completion);
void               flatpak_complete_options (FlatpakCompletion *completion,
                                             GOptionEntry      *entries);
void               flatpak_completion_free  (FlatpakCompletion *completion);

#endif /* __FLATPAK_UTILS_H__ */
