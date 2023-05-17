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
 */

#pragma once

#include "libglnx.h"
#include <ostree.h>

#include "flatpak-ref-utils-private.h"
#include "flatpak-variant-private.h"

#define FLATPAK_XA_CACHE_VERSION 2
/* version 1 added extra data download size */
/* version 2 added ot.ts timestamps (to new format) */

#define FLATPAK_XA_SUMMARY_VERSION 1
/* version 0/missing is standard ostree summary,
 * version 1 is compact format with inline cache and no deltas
 */

/* Thse are key names in the per-ref metadata in the summary */
#define OSTREE_COMMIT_TIMESTAMP "ostree.commit.timestamp"
#define OSTREE_COMMIT_TIMESTAMP2 "ot.ts" /* Shorter version of the above */

#define FLATPAK_SUMMARY_DIFF_HEADER "xadf"

#define FLATPAK_SUMMARY_HISTORY_LENGTH_DEFAULT 16

GVariant *flatpak_repo_load_summary (OstreeRepo *repo,
                                     GError    **error);
GVariant *flatpak_repo_load_summary_index (OstreeRepo *repo,
                                           GError    **error);
GVariant *flatpak_repo_load_digested_summary (OstreeRepo *repo,
                                              const char *digest,
                                              GError    **error);
GPtrArray *flatpak_summary_match_subrefs (GVariant   *summary,
                                          const char *collection_id,
                                          FlatpakDecomposed *ref);
gboolean flatpak_summary_lookup_ref (GVariant      *summary,
                                     const char    *collection_id,
                                     const char    *ref,
                                     char         **out_checksum,
                                     VarRefInfoRef *out_info);
gboolean flatpak_summary_find_ref_map (VarSummaryRef  summary,
                                       const char    *collection_id,
                                       VarRefMapRef  *refs_out);
gboolean flatpak_var_ref_map_lookup_ref (VarRefMapRef   ref_map,
                                         const char    *ref,
                                         VarRefInfoRef *out_info);

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
gboolean flatpak_repo_set_authenticator_name (OstreeRepo *repo,
                                              const char *authenticator_name,
                                              GError    **error);
gboolean flatpak_repo_set_authenticator_install (OstreeRepo *repo,
                                                 gboolean authenticator_install,
                                                 GError    **error);
gboolean flatpak_repo_set_authenticator_option (OstreeRepo *repo,
                                                const char *key,
                                                const char *value,
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
gboolean flatpak_repo_set_deploy_sideload_collection_id (OstreeRepo *repo,
                                                         gboolean    deploy_collection_id,
                                                         GError    **error);
gboolean flatpak_repo_set_summary_history_length (OstreeRepo *repo,
                                                  guint       length,
                                                  GError    **error);
guint    flatpak_repo_get_summary_history_length (OstreeRepo *repo);
gboolean flatpak_repo_set_gpg_keys (OstreeRepo *repo,
                                    GBytes     *bytes,
                                    GError    **error);

GBytes *flatpak_summary_apply_diff (GBytes *old,
                                    GBytes *diff,
                                    GError **error);

typedef enum {
  FLATPAK_REPO_UPDATE_FLAG_NONE = 0,
  FLATPAK_REPO_UPDATE_FLAG_DISABLE_INDEX = 1 << 0,
} FlatpakRepoUpdateFlags;

gboolean flatpak_repo_update (OstreeRepo            *repo,
                              FlatpakRepoUpdateFlags flags,
                              const char           **gpg_key_ids,
                              const char            *gpg_homedir,
                              GCancellable          *cancellable,
                              GError               **error);
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

GVariant *flatpak_bundle_load (GFile              *file,
                               char              **commit,
                               FlatpakDecomposed **ref,
                               char              **origin,
                               char              **runtime_repo,
                               char              **app_metadata,
                               guint64            *installed_size,
                               GBytes            **gpg_keys,
                               char              **collection_id,
                               GError            **error);

gboolean flatpak_pull_from_bundle (OstreeRepo   *repo,
                                   GFile        *file,
                                   const char   *remote,
                                   const char   *ref,
                                   gboolean      require_gpg_signature,
                                   GCancellable *cancellable,
                                   GError      **error);

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
      g_object_unref (repo);
    }
}

static inline FlatpakRepoTransaction *
flatpak_repo_transaction_start (OstreeRepo   *repo,
                                GCancellable *cancellable,
                                GError      **error)
{
  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return NULL;
  return (FlatpakRepoTransaction *) g_object_ref (repo);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakRepoTransaction, flatpak_repo_transaction_cleanup)

gboolean   flatpak_repo_generate_appstream (OstreeRepo   *repo,
                                            const char  **gpg_key_ids,
                                            const char   *gpg_homedir,
                                            guint64       timestamp,
                                            GCancellable *cancellable,
                                            GError      **error);

static inline void
flatpak_ostree_progress_finish (OstreeAsyncProgress *progress)
{
  if (progress != NULL)
    {
      ostree_async_progress_finish (progress);
      g_object_unref (progress);
    }
}

typedef OstreeAsyncProgress OstreeAsyncProgressFinish;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeAsyncProgressFinish, flatpak_ostree_progress_finish);

gboolean flatpak_repo_resolve_rev (OstreeRepo    *repo,
                                   const char    *collection_id, /* nullable */
                                   const char    *remote_name, /* nullable */
                                   const char    *ref_name,
                                   gboolean       allow_noent,
                                   char         **out_rev,
                                   GCancellable  *cancellable,
                                   GError       **error);
