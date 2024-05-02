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

#pragma once

#include "libglnx.h"

#include <ostree.h>

#include "flatpak-ref-utils-private.h"
#include "flatpak-variant-private.h"

/**
 * FLATPAK_SUMMARY_INDEX_GVARIANT_FORMAT:
 *
 * dict
 *   s: subset name
 *  ->
 *   ay - checksum of subsummary
 *   aay - previous subsummary checksums
 *   a{sv} - per subset metadata
 * a{sv} - metadata

 */
#define FLATPAK_SUMMARY_INDEX_GVARIANT_STRING "(a{s(ayaaya{sv})}a{sv})"
#define FLATPAK_SUMMARY_INDEX_GVARIANT_FORMAT G_VARIANT_TYPE (FLATPAK_SUMMARY_INDEX_GVARIANT_STRING)

#define FLATPAK_REF_GROUP "Flatpak Ref"
#define FLATPAK_REF_VERSION_KEY "Version"
#define FLATPAK_REF_URL_KEY "Url"
#define FLATPAK_REF_RUNTIME_REPO_KEY "RuntimeRepo"
#define FLATPAK_REF_SUGGEST_REMOTE_NAME_KEY "SuggestRemoteName"
#define FLATPAK_REF_TITLE_KEY "Title"
#define FLATPAK_REF_GPGKEY_KEY "GPGKey"
#define FLATPAK_REF_IS_RUNTIME_KEY "IsRuntime"
#define FLATPAK_REF_NAME_KEY "Name"
#define FLATPAK_REF_BRANCH_KEY "Branch"
#define FLATPAK_REF_COLLECTION_ID_KEY "CollectionID"
#define FLATPAK_REF_DEPLOY_COLLECTION_ID_KEY "DeployCollectionID"
#define FLATPAK_REF_DEPLOY_SIDELOAD_COLLECTION_ID_KEY "DeploySideloadCollectionID"

#define FLATPAK_REPO_GROUP "Flatpak Repo"
#define FLATPAK_REPO_VERSION_KEY "Version"
#define FLATPAK_REPO_URL_KEY "Url"
#define FLATPAK_REPO_SUBSET_KEY "Subset"
#define FLATPAK_REPO_TITLE_KEY "Title"
#define FLATPAK_REPO_DEFAULT_BRANCH_KEY "DefaultBranch"
#define FLATPAK_REPO_GPGKEY_KEY "GPGKey"
#define FLATPAK_REPO_NODEPS_KEY "NoDeps"
#define FLATPAK_REPO_COMMENT_KEY "Comment"
#define FLATPAK_REPO_DESCRIPTION_KEY "Description"
#define FLATPAK_REPO_HOMEPAGE_KEY "Homepage"
#define FLATPAK_REPO_ICON_KEY "Icon"
#define FLATPAK_REPO_FILTER_KEY "Filter"
#define FLATPAK_REPO_AUTHENTICATOR_NAME_KEY "AuthenticatorName"
#define FLATPAK_REPO_AUTHENTICATOR_INSTALL_KEY "AuthenticatorInstall"

#define FLATPAK_REPO_COLLECTION_ID_KEY "CollectionID"
#define FLATPAK_REPO_DEPLOY_COLLECTION_ID_KEY "DeployCollectionID"
#define FLATPAK_REPO_DEPLOY_SIDELOAD_COLLECTION_ID_KEY "DeploySideloadCollectionID"

#define FLATPAK_SPARSE_CACHE_KEY_ENDOFLINE "eol"
#define FLATPAK_SPARSE_CACHE_KEY_ENDOFLINE_REBASE "eolr"
#define FLATPAK_SPARSE_CACHE_KEY_TOKEN_TYPE "tokt"
#define FLATPAK_SPARSE_CACHE_KEY_EXTRA_DATA_SIZE "eds"

#define FLATPAK_SUMMARY_HISTORY_LENGTH_DEFAULT 16

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
GVariant *flatpak_repo_load_summary (OstreeRepo *repo,
                                     GError    **error);
GVariant *flatpak_repo_load_summary_index (OstreeRepo *repo,
                                           GError    **error);
GVariant *flatpak_repo_load_digested_summary (OstreeRepo *repo,
                                              const char *digest,
                                              GError    **error);

GBytes *flatpak_summary_apply_diff (GBytes *old,
                                    GBytes *diff,
                                    GError **error);

typedef enum
{
  FLATPAK_REPO_UPDATE_FLAG_NONE = 0,
  FLATPAK_REPO_UPDATE_FLAG_DISABLE_INDEX = 1 << 0,
} FlatpakRepoUpdateFlags;

gboolean flatpak_repo_update (OstreeRepo            *repo,
                              FlatpakRepoUpdateFlags flags,
                              const char           **gpg_key_ids,
                              const char            *gpg_homedir,
                              GCancellable          *cancellable,
                              GError               **error);

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

gboolean   flatpak_repo_generate_appstream (OstreeRepo   *repo,
                                            const char  **gpg_key_ids,
                                            const char   *gpg_homedir,
                                            guint64       timestamp,
                                            GCancellable *cancellable,
                                            GError      **error);

gboolean flatpak_repo_resolve_rev (OstreeRepo    *repo,
                                   const char    *collection_id, /* nullable */
                                   const char    *remote_name, /* nullable */
                                   const char    *ref_name,
                                   gboolean       allow_noent,
                                   char         **out_rev,
                                   GCancellable  *cancellable,
                                   GError       **error);

gboolean flatpak_pull_from_bundle (OstreeRepo   *repo,
                                   GFile        *file,
                                   const char   *remote,
                                   const char   *ref,
                                   gboolean      require_gpg_signature,
                                   GCancellable *cancellable,
                                   GError      **error);

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
