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

#ifndef __FLATPAK_DIR_H__
#define __FLATPAK_DIR_H__

#include <ostree.h>

#include "libglnx/libglnx.h"
#include <flatpak-common-types.h>

#define FLATPAK_TYPE_DIR flatpak_dir_get_type ()
#define FLATPAK_DIR(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_DIR, FlatpakDir))
#define FLATPAK_IS_DIR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_DIR))

#define FLATPAK_TYPE_DEPLOY flatpak_deploy_get_type ()
#define FLATPAK_DEPLOY(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_DEPLOY, FlatpakDeploy))
#define FLATPAK_IS_DEPLOY(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_DEPLOY))

GType flatpak_dir_get_type (void);
GType flatpak_deploy_get_type (void);

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

#ifdef FLATPAK_ENABLE_P2P
#define FLATPAK_REF_COLLECTION_ID_KEY "CollectionID"
#endif  /* FLATPAK_ENABLE_P2P */

#define FLATPAK_REPO_GROUP "Flatpak Repo"
#define FLATPAK_REPO_VERSION_KEY "Version"
#define FLATPAK_REPO_URL_KEY "Url"
#define FLATPAK_REPO_TITLE_KEY "Title"
#define FLATPAK_REPO_DEFAULT_BRANCH_KEY "DefaultBranch"
#define FLATPAK_REPO_GPGKEY_KEY "GPGKey"
#define FLATPAK_REPO_NODEPS_KEY "NoDeps"

#ifdef FLATPAK_ENABLE_P2P
#define FLATPAK_REPO_COLLECTION_ID_KEY "CollectionID"
#endif  /* FLATPAK_ENABLE_P2P */

#define FLATPAK_DEFAULT_UPDATE_FREQUENCY 100
#define FLATPAK_CLI_UPDATE_FREQUENCY 300

typedef struct
{
  char           *collection_id;  /* (nullable) */
  char           *ref;
  char           *commit;
  char          **subpaths;
  gboolean        download;
  gboolean        delete;
} FlatpakRelated;

void         flatpak_related_free (FlatpakRelated *related);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakDir, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakDeploy, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakRelated, flatpak_related_free)

typedef enum {
  FLATPAK_HELPER_DEPLOY_FLAGS_NONE = 0,
  FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE = 1 << 0,
  FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY = 1 << 1,
  FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL = 1 << 2,
} FlatpakHelperDeployFlags;

#define FLATPAK_HELPER_DEPLOY_FLAGS_ALL (FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE|FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY|FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL)

typedef enum {
  FLATPAK_HELPER_UNINSTALL_FLAGS_NONE = 0,
  FLATPAK_HELPER_UNINSTALL_FLAGS_KEEP_REF = 1 << 0,
  FLATPAK_HELPER_UNINSTALL_FLAGS_FORCE_REMOVE = 1 << 1,
} FlatpakHelperUninstallFlags;

#define FLATPAK_HELPER_UNINSTALL_FLAGS_ALL (FLATPAK_HELPER_UNINSTALL_FLAGS_KEEP_REF | FLATPAK_HELPER_UNINSTALL_FLAGS_FORCE_REMOVE)

typedef enum {
  FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_NONE = 0,
  FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_FORCE_REMOVE = 1 << 0,
} FlatpakHelperConfigureRemoteFlags;

#define FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_ALL (FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_FORCE_REMOVE)

typedef enum {
  FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_NONE = 0,
} FlatpakHelperUpdateRemoteFlags;

#define FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_ALL (0)

typedef enum {
  FLATPAK_PULL_FLAGS_NONE = 0,
  FLATPAK_PULL_FLAGS_DOWNLOAD_EXTRA_DATA = 1 << 0,
  FLATPAK_PULL_FLAGS_SIDELOAD_EXTRA_DATA = 1 << 1,
  FLATPAK_PULL_FLAGS_ALLOW_DOWNGRADE = 1 << 2,
  FLATPAK_PULL_FLAGS_NO_STATIC_DELTAS = 1 << 3,
} FlatpakPullFlags;

typedef enum {
  FLATPAK_DIR_STORAGE_TYPE_DEFAULT = 0,
  FLATPAK_DIR_STORAGE_TYPE_HARD_DISK,
  FLATPAK_DIR_STORAGE_TYPE_SDCARD,
  FLATPAK_DIR_STORAGE_TYPE_MMC,
} FlatpakDirStorageType;

GQuark       flatpak_dir_error_quark (void);

#ifndef FLATPAK_ENABLE_P2P
/* Rather than putting #ifdefs around all the function arguments for result sets,
 * define away OstreeRepoFinderResult if we’re compiling without P2P support.
 * The surrounding code should always pass in NULL if P2P support is disabled. */
typedef void OstreeRepoFinderResult;
typedef void** OstreeRepoFinderResultv;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OstreeRepoFinderResult, void)
G_DEFINE_AUTO_CLEANUP_FREE_FUNC (OstreeRepoFinderResultv, void, NULL)
#endif  /* !FLATPAK_ENABLE_P2P */

/**
 * FLATPAK_DEPLOY_DATA_GVARIANT_FORMAT:
 *
 * s - origin
 * s - commit
 * as - subpaths
 * t - installed size
 * a{sv} - Metadata
 */
#define FLATPAK_DEPLOY_DATA_GVARIANT_STRING "(ssasta{sv})"
#define FLATPAK_DEPLOY_DATA_GVARIANT_FORMAT G_VARIANT_TYPE (FLATPAK_DEPLOY_DATA_GVARIANT_STRING)

GPtrArray * flatpak_get_system_base_dir_locations (GCancellable *cancellable,
                                                   GError      **error);
GFile *     flatpak_get_system_default_base_dir_location (void);
GFile *     flatpak_get_user_base_dir_location (void);

GKeyFile *     flatpak_load_override_keyfile (const char *app_id,
                                              gboolean    user,
                                              GError    **error);
FlatpakContext *flatpak_load_override_file (const char *app_id,
                                            gboolean    user,
                                            GError    **error);
gboolean       flatpak_save_override_keyfile (GKeyFile   *metakey,
                                              const char *app_id,
                                              gboolean    user,
                                              GError    **error);

const char *        flatpak_deploy_data_get_origin (GVariant *deploy_data);
const char *        flatpak_deploy_data_get_commit (GVariant *deploy_data);
const char **       flatpak_deploy_data_get_subpaths (GVariant *deploy_data);
guint64             flatpak_deploy_data_get_installed_size (GVariant *deploy_data);
const char *        flatpak_deploy_data_get_alt_id (GVariant *deploy_data);

GFile *        flatpak_deploy_get_dir (FlatpakDeploy *deploy);
GFile *        flatpak_deploy_get_files (FlatpakDeploy *deploy);
FlatpakContext *flatpak_deploy_get_overrides (FlatpakDeploy *deploy);
GKeyFile *     flatpak_deploy_get_metadata (FlatpakDeploy *deploy);

FlatpakDir *  flatpak_dir_new (GFile   *basedir,
                               gboolean user);
FlatpakDir *  flatpak_dir_clone (FlatpakDir *self);
FlatpakDir  *flatpak_dir_get_user (void);
FlatpakDir  *flatpak_dir_get_system_default (void);
GPtrArray   *flatpak_dir_get_system_list (GCancellable *cancellable,
                                          GError      **error);
FlatpakDir  *flatpak_dir_get_system_by_id (const char   *id,
                                           GCancellable *cancellable,
                                           GError      **error);
gboolean    flatpak_dir_is_user (FlatpakDir *self);
void        flatpak_dir_set_no_system_helper (FlatpakDir *self,
                                              gboolean    no_system_helper);
GFile *     flatpak_dir_get_path (FlatpakDir *self);
GFile *     flatpak_dir_get_changed_path (FlatpakDir *self);
const char *flatpak_dir_get_id (FlatpakDir *self);
const char *flatpak_dir_get_display_name (FlatpakDir *self);
char *      flatpak_dir_get_name (FlatpakDir *self);
gint        flatpak_dir_get_priority (FlatpakDir *self);
FlatpakDirStorageType flatpak_dir_get_storage_type (FlatpakDir *self);
GFile *     flatpak_dir_get_deploy_dir (FlatpakDir *self,
                                        const char *ref);
GFile *     flatpak_dir_get_unmaintained_extension_dir (FlatpakDir *self,
                                                        const char *name,
                                                        const char *arch,
                                                        const char *branch);
GVariant *  flatpak_dir_get_deploy_data (FlatpakDir   *dir,
                                         const char   *ref,
                                         GCancellable *cancellable,
                                         GError      **error);
char *      flatpak_dir_get_origin (FlatpakDir   *self,
                                    const char   *ref,
                                    GCancellable *cancellable,
                                    GError      **error);
char **     flatpak_dir_get_subpaths (FlatpakDir   *self,
                                      const char   *ref,
                                      GCancellable *cancellable,
                                      GError      **error);
GFile *     flatpak_dir_get_exports_dir (FlatpakDir *self);
GFile *     flatpak_dir_get_removed_dir (FlatpakDir *self);
GFile *     flatpak_dir_get_if_deployed (FlatpakDir   *self,
                                         const char   *ref,
                                         const char   *checksum,
                                         GCancellable *cancellable);
GFile *     flatpak_dir_get_unmaintained_extension_dir_if_exists (FlatpakDir *self,
                                                                  const char *name,
                                                                  const char *arch,
                                                                  const char *branch,
                                                                  GCancellable *cancellable);

gboolean    flatpak_dir_remote_has_ref (FlatpakDir   *self,
                                        const char   *remote,
                                        const char   *ref);
char **     flatpak_dir_search_for_dependency (FlatpakDir   *self,
                                               const char   *runtime_ref,
                                               GCancellable *cancellable,
                                               GError      **error);
char *      flatpak_dir_find_remote_ref (FlatpakDir   *self,
                                         const char   *remote,
                                         const char   *name,
                                         const char   *opt_branch,
                                         const char   *opt_default_branch,
                                         const char   *opt_arch,
                                         FlatpakKinds  kinds,
                                         FlatpakKinds *out_kind,
                                         GCancellable *cancellable,
                                         GError      **error);
char **     flatpak_dir_find_remote_refs (FlatpakDir   *self,
                                          const char   *remote,
                                          const char   *name,
                                          const char   *opt_branch,
                                          const char   *opt_arch,
                                          FlatpakKinds  kinds,
                                          GCancellable *cancellable,
                                          GError      **error);
char *      flatpak_dir_find_installed_ref (FlatpakDir  *self,
                                            const char  *opt_name,
                                            const char  *opt_branch,
                                            const char  *opt_arch,
                                            FlatpakKinds kinds,
                                            FlatpakKinds *out_kind,
                                            GError     **error);
char **     flatpak_dir_find_installed_refs (FlatpakDir  *self,
                                             const char  *opt_name,
                                             const char  *opt_branch,
                                             const char  *opt_arch,
                                             FlatpakKinds kinds,
                                             GError     **error);
FlatpakDeploy *flatpak_dir_load_deployed (FlatpakDir   *self,
                                          const char   *ref,
                                          const char   *checksum,
                                          GCancellable *cancellable,
                                          GError      **error);
char *    flatpak_dir_load_override (FlatpakDir *dir,
                                     const char *app_id,
                                     gsize      *length,
                                     GError    **error);
OstreeRepo *flatpak_dir_get_repo (FlatpakDir *self);
gboolean    flatpak_dir_ensure_path (FlatpakDir   *self,
                                     GCancellable *cancellable,
                                     GError      **error);
gboolean    flatpak_dir_use_child_repo (FlatpakDir *self);
gboolean    flatpak_dir_ensure_system_child_repo (FlatpakDir *self,
                                                  GError    **error);
gboolean    flatpak_dir_recreate_repo (FlatpakDir   *self,
                                       GCancellable *cancellable,
                                       GError      **error);
gboolean    flatpak_dir_ensure_repo (FlatpakDir   *self,
                                     GCancellable *cancellable,
                                     GError      **error);
gboolean    flatpak_dir_mark_changed (FlatpakDir *self,
                                      GError    **error);
gboolean    flatpak_dir_remove_appstream (FlatpakDir   *self,
                                          const char   *remote,
                                          GCancellable *cancellable,
                                          GError      **error);
gboolean    flatpak_dir_deploy_appstream (FlatpakDir          *self,
                                          const char          *remote,
                                          const char          *arch,
                                          gboolean            *out_changed,
                                          GCancellable        *cancellable,
                                          GError             **error);
gboolean    flatpak_dir_update_appstream (FlatpakDir          *self,
                                          const char          *remote,
                                          const char          *arch,
                                          gboolean            *out_changed,
                                          OstreeAsyncProgress *progress,
                                          GCancellable        *cancellable,
                                          GError             **error);
gboolean    flatpak_dir_pull (FlatpakDir          *self,
                              const char          *repository,
                              const char          *ref,
                              const char          *opt_rev,
                              const OstreeRepoFinderResult * const *results,
                              const char         **subpaths,
                              OstreeRepo          *repo,
                              FlatpakPullFlags     flatpak_flags,
                              OstreeRepoPullFlags  flags,
                              OstreeAsyncProgress *progress,
                              GCancellable        *cancellable,
                              GError             **error);
gboolean    flatpak_dir_pull_untrusted_local (FlatpakDir          *self,
                                              const char          *src_path,
                                              const char          *remote_name,
                                              const char          *ref,
                                              const char        **subpaths,
                                              OstreeAsyncProgress *progress,
                                              GCancellable        *cancellable,
                                              GError             **error);
gboolean    flatpak_dir_list_refs_for_name (FlatpakDir   *self,
                                            const char   *kind,
                                            const char   *name,
                                            char       ***refs,
                                            GCancellable *cancellable,
                                            GError      **error);
gboolean    flatpak_dir_list_refs (FlatpakDir   *self,
                                   const char   *kind,
                                   char       ***refs,
                                   GCancellable *cancellable,
                                   GError      **error);
char *      flatpak_dir_read_latest (FlatpakDir   *self,
                                     const char   *remote,
                                     const char   *ref,
                                     char        **out_alt_id,
                                     GCancellable *cancellable,
                                     GError      **error);
char *      flatpak_dir_read_active (FlatpakDir   *self,
                                     const char   *ref,
                                     GCancellable *cancellable);
gboolean    flatpak_dir_set_active (FlatpakDir   *self,
                                    const char   *ref,
                                    const char   *checksum,
                                    GCancellable *cancellable,
                                    GError      **error);
char *      flatpak_dir_current_ref (FlatpakDir   *self,
                                     const char   *name,
                                     GCancellable *cancellable);
gboolean    flatpak_dir_drop_current_ref (FlatpakDir   *self,
                                          const char   *name,
                                          GCancellable *cancellable,
                                          GError      **error);
gboolean    flatpak_dir_make_current_ref (FlatpakDir   *self,
                                          const char   *ref,
                                          GCancellable *cancellable,
                                          GError      **error);
gboolean    flatpak_dir_list_deployed (FlatpakDir   *self,
                                       const char   *ref,
                                       char       ***deployed_checksums,
                                       GCancellable *cancellable,
                                       GError      **error);
gboolean    flatpak_dir_lock (FlatpakDir   *self,
                              GLnxLockFile *lockfile,
                              GCancellable *cancellable,
                              GError      **error);
gboolean    flatpak_dir_deploy (FlatpakDir          *self,
                                const char          *origin,
                                const char          *ref,
                                const char          *checksum_or_latest,
                                const char * const * subpaths,
                                GVariant            *old_deploy_data,
                                GCancellable        *cancellable,
                                GError             **error);
gboolean    flatpak_dir_deploy_update (FlatpakDir   *self,
                                       const char   *ref,
                                       const char   *checksum,
                                       const char **opt_subpaths,
                                       GCancellable *cancellable,
                                       GError      **error);
gboolean   flatpak_dir_deploy_install (FlatpakDir   *self,
                                       const char   *ref,
                                       const char   *origin,
                                       const char  **subpaths,
                                       GCancellable *cancellable,
                                       GError      **error);
gboolean   flatpak_dir_install (FlatpakDir          *self,
                                gboolean             no_pull,
                                gboolean             no_deploy,
                                gboolean             no_static_deltas,
                                const char          *ref,
                                const char          *remote_name,
                                const char         **subpaths,
                                OstreeAsyncProgress *progress,
                                GCancellable        *cancellable,
                                GError             **error);
char *flatpak_dir_ensure_bundle_remote (FlatpakDir          *self,
                                        GFile               *file,
                                        GBytes              *extra_gpg_data,
                                        char               **out_ref,
                                        char               **out_metadata,
                                        gboolean            *out_created_remote,
                                        GCancellable        *cancellable,
                                        GError             **error);
gboolean flatpak_dir_install_bundle (FlatpakDir          *self,
                                     GFile               *file,
                                     const char          *remote,
                                     char               **out_ref,
                                     GCancellable        *cancellable,
                                     GError             **error);
char * flatpak_dir_check_for_update (FlatpakDir          *self,
                                     const char          *ref,
                                     const char          *remote_name,
                                     const char          *checksum_or_latest,
                                     const char         **opt_subpaths,
                                     gboolean             no_pull,
                                     OstreeRepoFinderResult ***out_results,
                                     GCancellable        *cancellable,
                                     GError             **error);
gboolean   flatpak_dir_update (FlatpakDir          *self,
                               gboolean             no_pull,
                               gboolean             no_deploy,
                               gboolean             no_static_deltas,
                               gboolean             allow_downgrade,
                               const char          *ref,
                               const char          *remote_name,
                               const char          *checksum_or_latest,
                               const OstreeRepoFinderResult * const *results,
                               const char         **opt_subpaths,
                               OstreeAsyncProgress *progress,
                               GCancellable        *cancellable,
                               GError             **error);
gboolean flatpak_dir_uninstall (FlatpakDir          *self,
                                const char          *ref,
                                FlatpakHelperUninstallFlags flags,
                                GCancellable        *cancellable,
                                GError             **error);
gboolean    flatpak_dir_undeploy (FlatpakDir   *self,
                                  const char   *ref,
                                  const char   *checksum,
                                  gboolean      is_update,
                                  gboolean      force_remove,
                                  GCancellable *cancellable,
                                  GError      **error);
gboolean    flatpak_dir_undeploy_all (FlatpakDir   *self,
                                      const char   *ref,
                                      gboolean      force_remove,
                                      gboolean     *was_deployed_out,
                                      GCancellable *cancellable,
                                      GError      **error);
gboolean    flatpak_dir_remove_ref (FlatpakDir   *self,
                                    const char   *remote_name,
                                    const char   *ref,
                                    GCancellable *cancellable,
                                    GError      **error);
gboolean    flatpak_dir_update_exports (FlatpakDir   *self,
                                        const char   *app,
                                        GCancellable *cancellable,
                                        GError      **error);
gboolean    flatpak_dir_prune (FlatpakDir   *self,
                               GCancellable *cancellable,
                               GError      **error);
gboolean    flatpak_dir_cleanup_removed (FlatpakDir   *self,
                                         GCancellable *cancellable,
                                         GError      **error);
gboolean    flatpak_dir_collect_deployed_refs (FlatpakDir   *self,
                                               const char   *type,
                                               const char   *name_prefix,
                                               const char   *branch,
                                               const char   *arch,
                                               GHashTable   *hash,
                                               GCancellable *cancellable,
                                               GError      **error);
gboolean    flatpak_dir_collect_unmaintained_refs (FlatpakDir   *self,
                                                   const char   *name_prefix,
                                                   const char   *arch,
                                                   const char   *branch,
                                                   GHashTable   *hash,
                                                   GCancellable *cancellable,
                                                   GError      **error);
gboolean   flatpak_dir_remote_has_deploys (FlatpakDir   *self,
                                           const char   *remote);
char      *flatpak_dir_create_origin_remote (FlatpakDir   *self,
                                             const char   *url,
                                             const char   *id,
                                             const char   *title,
                                             const char   *main_ref,
                                             GBytes       *gpg_data,
                                             const char   *collection_id,
                                             GCancellable *cancellable,
                                             GError      **error);
gboolean   flatpak_dir_create_remote_for_ref_file (FlatpakDir   *self,
                                                   GBytes  *data,
                                                   const char *default_arch,
                                                   char   **remote_name_out,
                                                   char   **ref_out,
                                                   GError **error);
gboolean   flatpak_dir_create_suggested_remote_for_ref_file (FlatpakDir   *self,
                                                             GBytes  *data,
                                                             GError **error);
GKeyFile * flatpak_dir_parse_repofile (FlatpakDir   *self,
                                       const char   *remote_name,
                                       gboolean      from_ref,
                                       GBytes       *data,
                                       GBytes      **gpg_data_out,
                                       GCancellable *cancellable,
                                       GError      **error);

char      *flatpak_dir_find_remote_by_uri (FlatpakDir   *self,
                                           const char   *uri,
                                           const char   *collection_id);
char     **flatpak_dir_list_remotes (FlatpakDir   *self,
                                     GCancellable *cancellable,
                                     GError      **error);
char     **flatpak_dir_list_enumerated_remotes (FlatpakDir   *self,
                                                GCancellable *cancellable,
                                                GError      **error);
gboolean   flatpak_dir_modify_remote (FlatpakDir   *self,
                                      const char   *remote_name,
                                      GKeyFile     *config,
                                      GBytes       *gpg_data,
                                      GCancellable *cancellable,
                                      GError      **error);
gboolean   flatpak_dir_remove_remote (FlatpakDir   *self,
                                      gboolean      force_remove,
                                      const char   *remote_name,
                                      GCancellable *cancellable,
                                      GError      **error);
char      *flatpak_dir_get_remote_title (FlatpakDir *self,
                                         const char *remote_name);
char      *flatpak_dir_get_remote_collection_id (FlatpakDir *self,
                                                 const char *remote_name);
char      *flatpak_dir_get_remote_main_ref (FlatpakDir *self,
                                            const char *remote_name);
gboolean   flatpak_dir_get_remote_oci (FlatpakDir *self,
                                       const char *remote_name);
char      *flatpak_dir_get_remote_default_branch (FlatpakDir *self,
                                                  const char *remote_name);
int        flatpak_dir_get_remote_prio (FlatpakDir *self,
                                        const char *remote_name);
gboolean   flatpak_dir_get_remote_noenumerate (FlatpakDir *self,
                                               const char *remote_name);
gboolean   flatpak_dir_get_remote_nodeps (FlatpakDir *self,
                                          const char *remote_name);
gboolean   flatpak_dir_get_remote_disabled (FlatpakDir *self,
                                            const char *remote_name);
gboolean   flatpak_dir_list_remote_refs (FlatpakDir   *self,
                                         const char   *remote,
                                         GHashTable  **refs,
                                         GCancellable *cancellable,
                                         GError      **error);
gboolean flatpak_dir_fetch_remote_repo_metadata (FlatpakDir    *self,
                                                 const char    *remote_name,
                                                 GCancellable  *cancellable,
                                                 GError       **error);
char *   flatpak_dir_fetch_remote_title (FlatpakDir   *self,
                                         const char   *remote,
                                         GCancellable *cancellable,
                                         GError      **error);
char *   flatpak_dir_fetch_remote_default_branch (FlatpakDir   *self,
                                                  const char   *remote,
                                                  GCancellable *cancellable,
                                                  GError      **error);
gboolean flatpak_dir_update_remote_configuration (FlatpakDir   *self,
                                                  const char   *remote,
                                                  GCancellable *cancellable,
                                                  GError      **error);
gboolean flatpak_dir_update_remote_configuration_for_summary (FlatpakDir   *self,
                                                              const char   *remote,
                                                              GVariant     *summary,
                                                              gboolean      dry_run,
                                                              gboolean     *has_changed_out,
                                                              GCancellable *cancellable,
                                                              GError      **error);
gboolean flatpak_dir_update_remote_configuration_for_repo_metadata (FlatpakDir    *self,
                                                                    const char    *remote,
                                                                    GVariant      *summary,
                                                                    gboolean       dry_run,
                                                                    gboolean      *has_changed_out,
                                                                    GCancellable  *cancellable,
                                                                    GError       **error);
gboolean flatpak_dir_fetch_ref_cache (FlatpakDir   *self,
                                      const char   *remote_name,
                                      const char   *ref,
                                      guint64      *download_size,
                                      guint64      *installed_size,
                                      char        **metadata,
                                      GCancellable *cancellable,
                                      GError      **error);
GPtrArray * flatpak_dir_find_remote_related (FlatpakDir *dir,
                                             const char *remote_name,
                                             const char *ref,
                                             GCancellable *cancellable,
                                             GError **error);
GPtrArray * flatpak_dir_find_local_related (FlatpakDir *self,
                                            const char *remote_name,
                                            const char *ref,
                                            GCancellable *cancellable,
                                            GError **error);

gboolean flatpak_dir_lookup_repo_metadata (FlatpakDir    *self,
                                           const char    *remote_name,
                                           GCancellable  *cancellable,
                                           GError       **error,
                                           const char    *key,
                                           const char    *format_string,
                                           ...);

char ** flatpak_dir_get_locale_subpaths (FlatpakDir *self);

#endif /* __FLATPAK_DIR_H__ */
