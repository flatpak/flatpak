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
#include <flatpak-common-types-private.h>
#include <flatpak-context-private.h>


/* Version history:
 * The version field was added in flatpak 1.2, anything before is 0.
 *
 * Version 1 added appdata-name/summary/version/license
 */
#define FLATPAK_DEPLOY_VERSION_CURRENT 1
#define FLATPAK_DEPLOY_VERSION_ANY 0

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
#define FLATPAK_REF_COLLECTION_ID_KEY "CollectionID"
#define FLATPAK_REF_DEPLOY_COLLECTION_ID_KEY "DeployCollectionID"

#define FLATPAK_REPO_GROUP "Flatpak Repo"
#define FLATPAK_REPO_VERSION_KEY "Version"
#define FLATPAK_REPO_URL_KEY "Url"
#define FLATPAK_REPO_TITLE_KEY "Title"
#define FLATPAK_REPO_DEFAULT_BRANCH_KEY "DefaultBranch"
#define FLATPAK_REPO_GPGKEY_KEY "GPGKey"
#define FLATPAK_REPO_NODEPS_KEY "NoDeps"

#define FLATPAK_REPO_COLLECTION_ID_KEY "CollectionID"
#define FLATPAK_REPO_DEPLOY_COLLECTION_ID_KEY "DeployCollectionID"

#define FLATPAK_DEFAULT_UPDATE_FREQUENCY 100
#define FLATPAK_CLI_UPDATE_FREQUENCY 300

typedef struct
{
  char    *collection_id;         /* (nullable) */
  char    *ref;
  char    *commit;
  char   **subpaths;
  gboolean download;
  gboolean        delete;
  gboolean auto_prune;
} FlatpakRelated;

void         flatpak_related_free (FlatpakRelated *related);


/* The remote state represent the state of the remote at a particular
   time, including the summary file and the metadata (which may be from
   the summary or from a branch. We create this once per highlevel operation
   to avoid looking up the summary multiple times, but also to avoid races
   if it happened to change in the middle of the operation */
typedef struct
{
  char     *remote_name;
  char     *collection_id;
  GVariant *summary;
  GBytes   *summary_sig_bytes;
  GError   *summary_fetch_error;
  GVariant *metadata;
  GError   *metadata_fetch_error;
  int      refcount;
} FlatpakRemoteState;

FlatpakRemoteState *flatpak_remote_state_ref (FlatpakRemoteState *remote_state);
void flatpak_remote_state_unref (FlatpakRemoteState *remote_state);
gboolean flatpak_remote_state_ensure_summary (FlatpakRemoteState *self,
                                              GError            **error);
gboolean flatpak_remote_state_ensure_metadata (FlatpakRemoteState *self,
                                               GError            **error);
gboolean flatpak_remote_state_lookup_ref (FlatpakRemoteState *self,
                                          const char         *ref,
                                          char              **out_checksum,
                                          GVariant          **out_variant,
                                          GError            **error);
char **flatpak_remote_state_match_subrefs (FlatpakRemoteState *self,
                                           const char         *ref);
gboolean flatpak_remote_state_lookup_repo_metadata (FlatpakRemoteState *self,
                                                    const char         *key,
                                                    const char         *format_string,
                                                    ...);
gboolean flatpak_remote_state_lookup_cache (FlatpakRemoteState *self,
                                            const char         *ref,
                                            guint64            *download_size,
                                            guint64            *installed_size,
                                            const char        **metadata,
                                            GError            **error);
GVariant *flatpak_remote_state_lookup_sparse_cache (FlatpakRemoteState *self,
                                                    const char         *ref,
                                                    GError            **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakDir, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakDeploy, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakRelated, flatpak_related_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakRemoteState, flatpak_remote_state_unref)

typedef struct
{
  char *collection_id;
  char *ref_name;
} FlatpakCollectionRef;

FlatpakCollectionRef *    flatpak_collection_ref_new (const char *collection_id,
                                                      const char *ref_name);
void                      flatpak_collection_ref_free (FlatpakCollectionRef *ref);
guint                     flatpak_collection_ref_hash (gconstpointer ref);
gboolean                  flatpak_collection_ref_equal (gconstpointer ref1,
                                                        gconstpointer ref2);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakCollectionRef, flatpak_collection_ref_free)

typedef enum {
  FLATPAK_HELPER_DEPLOY_FLAGS_NONE = 0,
  FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE = 1 << 0,
  FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY = 1 << 1,
  FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL = 1 << 2,
  FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL = 1 << 3,
  FLATPAK_HELPER_DEPLOY_FLAGS_NO_INTERACTION = 1 << 4,
  FLATPAK_HELPER_DEPLOY_FLAGS_APP_HINT = 1 << 5,
  FLATPAK_HELPER_DEPLOY_FLAGS_INSTALL_HINT = 1 << 6,
} FlatpakHelperDeployFlags;

#define FLATPAK_HELPER_DEPLOY_FLAGS_ALL (FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE |\
                                         FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY |\
                                         FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL |\
                                         FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL |\
                                         FLATPAK_HELPER_DEPLOY_FLAGS_NO_INTERACTION |\
                                         FLATPAK_HELPER_DEPLOY_FLAGS_APP_HINT |\
                                         FLATPAK_HELPER_DEPLOY_FLAGS_INSTALL_HINT)

typedef enum {
  FLATPAK_HELPER_UNINSTALL_FLAGS_NONE = 0,
  FLATPAK_HELPER_UNINSTALL_FLAGS_KEEP_REF = 1 << 0,
  FLATPAK_HELPER_UNINSTALL_FLAGS_FORCE_REMOVE = 1 << 1,
  FLATPAK_HELPER_UNINSTALL_FLAGS_NO_INTERACTION = 1 << 2,
} FlatpakHelperUninstallFlags;

#define FLATPAK_HELPER_UNINSTALL_FLAGS_ALL (FLATPAK_HELPER_UNINSTALL_FLAGS_KEEP_REF |\
                                            FLATPAK_HELPER_UNINSTALL_FLAGS_FORCE_REMOVE |\
                                            FLATPAK_HELPER_UNINSTALL_FLAGS_NO_INTERACTION)

typedef enum {
  FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_NONE = 0,
  FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_FORCE_REMOVE = 1 << 0,
  FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_NO_INTERACTION = 1 << 1,
} FlatpakHelperConfigureRemoteFlags;

#define FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_ALL (FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_FORCE_REMOVE |\
                                                   FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_NO_INTERACTION)

typedef enum {
  FLATPAK_HELPER_CONFIGURE_FLAGS_NONE = 0,
  FLATPAK_HELPER_CONFIGURE_FLAGS_UNSET = 1 << 0,
  FLATPAK_HELPER_CONFIGURE_FLAGS_NO_INTERACTION = 1 << 1,
} FlatpakHelperConfigureFlags;

#define FLATPAK_HELPER_CONFIGURE_FLAGS_ALL (FLATPAK_HELPER_CONFIGURE_FLAGS_UNSET |\
                                            FLATPAK_HELPER_CONFIGURE_FLAGS_NO_INTERACTION)

typedef enum {
  FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_NONE = 0,
  FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_NO_INTERACTION = 1 << 0,
} FlatpakHelperUpdateRemoteFlags;

#define FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_ALL (FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_NO_INTERACTION)

typedef enum {
  FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_NONE = 0,
  FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_NO_INTERACTION = 1 << 0,
} FlatpakHelperInstalBundleFlags;

#define FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_ALL (FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_NO_INTERACTION)

typedef enum {
  FLATPAK_HELPER_DEPLOY_APPSTREAM_FLAGS_NONE = 0,
  FLATPAK_HELPER_DEPLOY_APPSTREAM_FLAGS_NO_INTERACTION = 1 << 0,
} FlatpakHelperDeployAppstreamFlags;

#define FLATPAK_HELPER_DEPLOY_APPSTREAM_FLAGS_ALL (FLATPAK_HELPER_DEPLOY_APPSTREAM_FLAGS_NO_INTERACTION)

typedef enum {
  FLATPAK_HELPER_REMOVE_LOCAL_REF_FLAGS_NONE = 0,
  FLATPAK_HELPER_REMOVE_LOCAL_REF_FLAGS_NO_INTERACTION = 1 << 0,
} FlatpakHelperRemoveLocalRefFlags;

#define FLATPAK_HELPER_REMOVE_LOCAL_REF_FLAGS_ALL (FLATPAK_HELPER_REMOVE_LOCAL_REF_FLAGS_NO_INTERACTION)

typedef enum {
  FLATPAK_HELPER_PRUNE_LOCAL_REPO_FLAGS_NONE = 0,
  FLATPAK_HELPER_PRUNE_LOCAL_REPO_FLAGS_NO_INTERACTION = 1 << 0,
} FlatpakHelperPruneLocalRepoFlags;

#define FLATPAK_HELPER_PRUNE_LOCAL_REPO_FLAGS_ALL (FLATPAK_HELPER_PRUNE_LOCAL_REPO_FLAGS_NO_INTERACTION)

typedef enum {
  FLATPAK_HELPER_RUN_TRIGGERS_FLAGS_NONE = 0,
  FLATPAK_HELPER_RUN_TRIGGERS_FLAGS_NO_INTERACTION = 1 << 0,
} FlatpakHelperRunTriggersFlags;

#define FLATPAK_HELPER_RUN_TRIGGERS_FLAGS_ALL (FLATPAK_HELPER_RUN_TRIGGERS_FLAGS_NO_INTERACTION)

typedef enum {
  FLATPAK_HELPER_ENSURE_REPO_FLAGS_NONE = 0,
  FLATPAK_HELPER_ENSURE_REPO_FLAGS_NO_INTERACTION = 1 << 0,
} FlatpakHelperEnsureRepoFlags;

#define FLATPAK_HELPER_ENSURE_REPO_FLAGS_ALL (FLATPAK_HELPER_ENSURE_REPO_FLAGS_NO_INTERACTION)

typedef enum {
  FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_NONE = 0,
  FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_NO_INTERACTION = 1 << 0,
} FlatpakHelperUpdateSummaryFlags;

#define FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_ALL (FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_NO_INTERACTION)

typedef enum {
  FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_NONE = 0,
  FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_NO_INTERACTION = 1 << 0,
} FlatpakHelperGenerateOciSummaryFlags;

#define FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_ALL (FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_NO_INTERACTION)

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
  FLATPAK_DIR_STORAGE_TYPE_NETWORK,
} FlatpakDirStorageType;

typedef enum {
  FIND_MATCHING_REFS_FLAGS_NONE = 0,
  FIND_MATCHING_REFS_FLAGS_KEEP_REMOTE = (1 << 0),
  FIND_MATCHING_REFS_FLAGS_FUZZY = (1 << 1),
} FindMatchingRefsFlags;

GQuark       flatpak_dir_error_quark (void);

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
gboolean       flatpak_remove_override_keyfile (const char  *app_id,
                                                gboolean     user,
                                                GError     **error);

int                 flatpak_deploy_data_get_version (GVariant *deploy_data);
const char *        flatpak_deploy_data_get_origin (GVariant *deploy_data);
const char *        flatpak_deploy_data_get_commit (GVariant *deploy_data);
const char **       flatpak_deploy_data_get_subpaths (GVariant *deploy_data);
guint64             flatpak_deploy_data_get_installed_size (GVariant *deploy_data);
const char *        flatpak_deploy_data_get_alt_id (GVariant *deploy_data);
const char *        flatpak_deploy_data_get_eol (GVariant *deploy_data);
const char *        flatpak_deploy_data_get_eol_rebase (GVariant *deploy_data);
const char *        flatpak_deploy_data_get_runtime (GVariant *deploy_data);
const char *        flatpak_deploy_data_get_appdata_name (GVariant *deploy_data);
const char *        flatpak_deploy_data_get_appdata_summary (GVariant *deploy_data);
const char *        flatpak_deploy_data_get_appdata_version (GVariant *deploy_data);
const char *        flatpak_deploy_data_get_appdata_license (GVariant *deploy_data);

GFile *        flatpak_deploy_get_dir (FlatpakDeploy *deploy);
GVariant *     flatpak_load_deploy_data (GFile        *deploy_dir,
                                         const char   *ref,
                                         int           required_version,
                                         GCancellable *cancellable,
                                         GError      **error);
GVariant *     flatpak_deploy_get_deploy_data (FlatpakDeploy *deploy,
                                               int            required_version,
                                               GCancellable  *cancellable,
                                               GError       **error);
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
void        flatpak_dir_set_no_interaction (FlatpakDir *self,
                                            gboolean    no_interaction);
gboolean    flatpak_dir_get_no_interaction (FlatpakDir *self);
GFile *     flatpak_dir_get_path (FlatpakDir *self);
GFile *     flatpak_dir_get_changed_path (FlatpakDir *self);
const char *flatpak_dir_get_id (FlatpakDir *self);
char       *flatpak_dir_get_display_name (FlatpakDir *self);
char *      flatpak_dir_get_name (FlatpakDir *self);
const char *flatpak_dir_get_name_cached (FlatpakDir *self);
gint        flatpak_dir_get_priority (FlatpakDir *self);
FlatpakDirStorageType flatpak_dir_get_storage_type (FlatpakDir *self);
GFile *     flatpak_dir_get_deploy_dir (FlatpakDir *self,
                                        const char *ref);
char *      flatpak_dir_get_deploy_subdir (FlatpakDir          *self,
                                           const char          *checksum,
                                           const char * const * subpaths);
GFile *     flatpak_dir_get_unmaintained_extension_dir (FlatpakDir *self,
                                                        const char *name,
                                                        const char *arch,
                                                        const char *branch);
GVariant *  flatpak_dir_get_deploy_data (FlatpakDir   *dir,
                                         const char   *ref,
                                         int           required_version,
                                         GCancellable *cancellable,
                                         GError      **error);
char *      flatpak_dir_get_origin (FlatpakDir   *self,
                                    const char   *ref,
                                    GCancellable *cancellable,
                                    GError      **error);
GFile *     flatpak_dir_get_exports_dir (FlatpakDir *self);
GFile *     flatpak_dir_get_removed_dir (FlatpakDir *self);
GFile *     flatpak_dir_get_if_deployed (FlatpakDir   *self,
                                         const char   *ref,
                                         const char   *checksum,
                                         GCancellable *cancellable);
GFile *     flatpak_dir_get_unmaintained_extension_dir_if_exists (FlatpakDir   *self,
                                                                  const char   *name,
                                                                  const char   *arch,
                                                                  const char   *branch,
                                                                  GCancellable *cancellable);

char **     flatpak_dir_search_for_local_dependency (FlatpakDir   *self,
                                                     const char   *runtime_ref,
                                                     GCancellable *cancellable,
                                                     GError      **error);
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
char **     flatpak_dir_find_remote_refs (FlatpakDir            *self,
                                          const char            *remote,
                                          const char            *name,
                                          const char            *opt_branch,
                                          const char            *opt_default_branch,
                                          const char            *opt_arch,
                                          const char            *opt_default_arch,
                                          FlatpakKinds           kinds,
                                          FindMatchingRefsFlags  flags,
                                          GCancellable          *cancellable,
                                          GError               **error);
char **     flatpak_dir_find_local_refs (FlatpakDir           *self,
                                         const char           *remote,
                                         const char           *name,
                                         const char           *opt_branch,
                                         const char           *opt_default_branch,
                                         const char           *opt_arch,
                                         const char           *opt_default_arch,
                                         FlatpakKinds          kinds,
                                         FindMatchingRefsFlags flags,
                                         GCancellable          *cancellable,
                                         GError               **error);
char *      flatpak_dir_find_installed_ref (FlatpakDir   *self,
                                            const char   *opt_name,
                                            const char   *opt_branch,
                                            const char   *opt_arch,
                                            FlatpakKinds  kinds,
                                            FlatpakKinds *out_kind,
                                            GError      **error);
char **     flatpak_dir_find_installed_refs (FlatpakDir            *self,
                                             const char            *opt_name,
                                             const char            *opt_branch,
                                             const char            *opt_arch,
                                             FlatpakKinds           kinds,
                                             FindMatchingRefsFlags  flags,
                                             GError               **error);
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
gboolean    flatpak_dir_maybe_ensure_repo (FlatpakDir   *self,
                                           GCancellable *cancellable,
                                           GError      **error);
char *      flatpak_dir_get_config (FlatpakDir *self,
                                    const char *key,
                                    GError    **error);
gboolean    flatpak_dir_set_config (FlatpakDir *self,
                                    const char *key,
                                    const char *value,
                                    GError    **error);
gboolean    flatpak_dir_mark_changed (FlatpakDir *self,
                                      GError    **error);
gboolean    flatpak_dir_remove_appstream (FlatpakDir   *self,
                                          const char   *remote,
                                          GCancellable *cancellable,
                                          GError      **error);
gboolean    flatpak_dir_deploy_appstream (FlatpakDir   *self,
                                          const char   *remote,
                                          const char   *arch,
                                          gboolean     *out_changed,
                                          GCancellable *cancellable,
                                          GError      **error);
gboolean    flatpak_dir_update_appstream (FlatpakDir          *self,
                                          const char          *remote,
                                          const char          *arch,
                                          gboolean            *out_changed,
                                          OstreeAsyncProgress *progress,
                                          GCancellable        *cancellable,
                                          GError             **error);
gboolean    flatpak_dir_pull (FlatpakDir                           *self,
                              FlatpakRemoteState                   *state,
                              const char                           *ref,
                              const char                           *opt_rev,
                              const OstreeRepoFinderResult * const *results,
                              const char                          **subpaths,
                              GBytes                               *require_metadata,
                              const char                           *token,
                              OstreeRepo                           *repo,
                              FlatpakPullFlags                      flatpak_flags,
                              OstreeRepoPullFlags                   flags,
                              OstreeAsyncProgress                  *progress,
                              GCancellable                         *cancellable,
                              GError                              **error);
gboolean    flatpak_dir_pull_untrusted_local (FlatpakDir          *self,
                                              const char          *src_path,
                                              const char          *remote_name,
                                              const char          *ref,
                                              const char         **subpaths,
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
GVariant *  flatpak_dir_read_latest_commit (FlatpakDir   *self,
                                            const char   *remote,
                                            const char   *ref,
                                            char        **out_checksum,
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
gboolean    flatpak_dir_repo_lock (FlatpakDir   *self,
                                   GLnxLockFile *lockfile,
                                   gboolean      exclusive,
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
                                       const char  **opt_subpaths,
                                       GCancellable *cancellable,
                                       GError      **error);
gboolean   flatpak_dir_deploy_install (FlatpakDir   *self,
                                       const char   *ref,
                                       const char   *origin,
                                       const char  **subpaths,
                                       gboolean      reinstall,
                                       GCancellable *cancellable,
                                       GError      **error);
gboolean   flatpak_dir_install (FlatpakDir          *self,
                                gboolean             no_pull,
                                gboolean             no_deploy,
                                gboolean             no_static_deltas,
                                gboolean             reinstall,
                                gboolean             app_hint,
                                FlatpakRemoteState  *state,
                                const char          *ref,
                                const char          *opt_commit,
                                const char         **subpaths,
                                const char         **previous_ids,
                                GBytes              *require_metadata,
                                const char          *token,
                                OstreeAsyncProgress *progress,
                                GCancellable        *cancellable,
                                GError             **error);
char *flatpak_dir_ensure_bundle_remote (FlatpakDir   *self,
                                        GFile        *file,
                                        GBytes       *extra_gpg_data,
                                        char        **out_ref,
                                        char        **out_commit,
                                        char        **out_metadata,
                                        gboolean     *out_created_remote,
                                        GCancellable *cancellable,
                                        GError      **error);
gboolean flatpak_dir_install_bundle (FlatpakDir   *self,
                                     GFile        *file,
                                     const char   *remote,
                                     char        **out_ref,
                                     GCancellable *cancellable,
                                     GError      **error);
gboolean flatpak_dir_needs_update_for_commit_and_subpaths (FlatpakDir  *self,
                                                           const char  *remote,
                                                           const char  *ref,
                                                           const char  *target_commit,
                                                           const char **opt_subpaths);
char * flatpak_dir_check_for_update (FlatpakDir               *self,
                                     FlatpakRemoteState       *state,
                                     const char               *ref,
                                     const char               *checksum_or_latest,
                                     const char              **opt_subpaths,
                                     gboolean                  no_pull,
                                     OstreeRepoFinderResult ***out_results,
                                     GCancellable             *cancellable,
                                     GError                  **error);
gboolean   flatpak_dir_update (FlatpakDir                           *self,
                               gboolean                              no_pull,
                               gboolean                              no_deploy,
                               gboolean                              no_static_deltas,
                               gboolean                              allow_downgrade,
                               gboolean                              app_hint,
                               gboolean                              install_hint,
                               FlatpakRemoteState                   *state,
                               const char                           *ref,
                               const char                           *checksum_or_latest,
                               const OstreeRepoFinderResult * const *results,
                               const char                          **opt_subpaths,
                               const char                          **opt_previous_ids,
                               GBytes                               *require_metadata,
                               const char                           *token,
                               OstreeAsyncProgress                  *progress,
                               GCancellable                         *cancellable,
                               GError                              **error);
gboolean flatpak_dir_uninstall (FlatpakDir                 *self,
                                const char                 *ref,
                                FlatpakHelperUninstallFlags flags,
                                GCancellable               *cancellable,
                                GError                    **error);
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
gboolean    flatpak_dir_run_triggers (FlatpakDir   *self,
                                      GCancellable *cancellable,
                                      GError      **error);
gboolean    flatpak_dir_update_summary (FlatpakDir   *self,
                                        GCancellable *cancellable,
                                        GError      **error);
gboolean    flatpak_dir_cleanup_removed (FlatpakDir   *self,
                                         GCancellable *cancellable,
                                         GError      **error);
gboolean    flatpak_dir_cleanup_undeployed_refs (FlatpakDir   *self,
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
gboolean   flatpak_dir_remote_has_deploys (FlatpakDir *self,
                                           const char *remote);
char      *flatpak_dir_create_origin_remote (FlatpakDir   *self,
                                             const char   *url,
                                             const char   *id,
                                             const char   *title,
                                             const char   *main_ref,
                                             GBytes       *gpg_data,
                                             const char   *collection_id,
                                             GCancellable *cancellable,
                                             GError      **error);
void       flatpak_dir_prune_origin_remote (FlatpakDir *self,
                                            const char *remote);
gboolean   flatpak_dir_create_remote_for_ref_file (FlatpakDir *self,
                                                   GKeyFile   *keyfile,
                                                   const char *default_arch,
                                                   char      **remote_name_out,
                                                   char      **collection_id_out,
                                                   char      **ref_out,
                                                   GError    **error);
gboolean   flatpak_dir_create_suggested_remote_for_ref_file (FlatpakDir *self,
                                                             GBytes     *data,
                                                             GError    **error);
GKeyFile * flatpak_dir_parse_repofile (FlatpakDir   *self,
                                       const char   *remote_name,
                                       gboolean      from_ref,
                                       GKeyFile     *keyfile,
                                       GBytes      **gpg_data_out,
                                       GCancellable *cancellable,
                                       GError      **error);

char      *flatpak_dir_find_remote_by_uri (FlatpakDir *self,
                                           const char *uri,
                                           const char *collection_id);
gboolean   flatpak_dir_has_remote (FlatpakDir *self,
                                   const char *remote_name,
                                   GError **error);
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
gboolean   flatpak_dir_list_remote_refs (FlatpakDir         *self,
                                         FlatpakRemoteState *state,
                                         GHashTable        **refs,
                                         GCancellable       *cancellable,
                                         GError            **error);
gboolean   flatpak_dir_list_all_remote_refs (FlatpakDir         *self,
                                             FlatpakRemoteState *state,
                                             GHashTable        **out_all_refs,
                                             GCancellable       *cancellable,
                                             GError            **error);
GVariant * flatpak_dir_fetch_remote_commit (FlatpakDir   *self,
                                            const char   *remote_name,
                                            const char   *ref,
                                            const char   *opt_commit,
                                            char        **out_commit,
                                            GCancellable *cancellable,
                                            GError      **error);
gboolean flatpak_dir_update_remote_configuration (FlatpakDir   *self,
                                                  const char   *remote,
                                                  GCancellable *cancellable,
                                                  GError      **error);
gboolean flatpak_dir_update_remote_configuration_for_state (FlatpakDir         *self,
                                                            FlatpakRemoteState *remote_state,
                                                            gboolean            dry_run,
                                                            gboolean           *has_changed_out,
                                                            GCancellable       *cancellable,
                                                            GError            **error);
FlatpakRemoteState * flatpak_dir_get_remote_state (FlatpakDir   *self,
                                                   const char   *remote,
                                                   GCancellable *cancellable,
                                                   GError      **error);
FlatpakRemoteState * flatpak_dir_get_remote_state_for_summary (FlatpakDir   *self,
                                                               const char   *remote,
                                                               GBytes       *opt_summary,
                                                               GBytes       *opt_summary_sig,
                                                               GCancellable *cancellable,
                                                               GError      **error);
gboolean flatpak_dir_remote_make_oci_summary (FlatpakDir   *self,
                                              const char   *remote,
                                              GBytes      **out_summary,
                                              GCancellable *cancellable,
                                              GError      **error);
FlatpakRemoteState * flatpak_dir_get_remote_state_optional (FlatpakDir   *self,
                                                            const char   *remote,
                                                            GCancellable *cancellable,
                                                            GError      **error);
FlatpakRemoteState * flatpak_dir_get_remote_state_local_only (FlatpakDir   *self,
                                                              const char   *remote,
                                                              GCancellable *cancellable,
                                                              GError      **error);
GPtrArray * flatpak_dir_find_remote_related_for_metadata (FlatpakDir         *self,
                                                          FlatpakRemoteState *state,
                                                          const char         *ref,
                                                          GKeyFile           *metakey,
                                                          GCancellable       *cancellable,
                                                          GError            **error);
GPtrArray * flatpak_dir_find_remote_related (FlatpakDir         *dir,
                                             FlatpakRemoteState *state,
                                             const char         *ref,
                                             GCancellable       *cancellable,
                                             GError            **error);
GPtrArray * flatpak_dir_find_local_related_for_metadata (FlatpakDir   *self,
                                                         const char   *ref,
                                                         const char   *remote_name,
                                                         GKeyFile     *metakey,
                                                         GCancellable *cancellable,
                                                         GError      **error);
GPtrArray * flatpak_dir_find_local_related (FlatpakDir   *self,
                                            const char   *remote_name,
                                            const char   *ref,
                                            gboolean      deployed,
                                            GCancellable *cancellable,
                                            GError      **error);
gboolean flatpak_dir_find_latest_rev (FlatpakDir               *self,
                                      FlatpakRemoteState       *state,
                                      const char               *ref,
                                      const char               *checksum_or_latest,
                                      char                    **out_rev,
                                      OstreeRepoFinderResult ***out_results,
                                      GCancellable             *cancellable,
                                      GError                  **error);

typedef struct
{
  /* in */
  char *remote;
  char *ref;
  char *opt_commit;

  /* out */
  char   *resolved_commit;
  GBytes *resolved_metadata;
  guint64 download_size;
  guint64 installed_size;
} FlatpakDirResolve;

FlatpakDirResolve *flatpak_dir_resolve_new (const char *remote,
                                            const char *ref,
                                            const char *opt_commit);
void               flatpak_dir_resolve_free (FlatpakDirResolve *resolve);
gboolean           flatpak_dir_resolve_p2p_refs (FlatpakDir         *self,
                                                 FlatpakDirResolve **resolves,
                                                 GCancellable       *cancellable,
                                                 GError            **error);


char ** flatpak_dir_get_default_locale_languages (FlatpakDir *self);
char ** flatpak_dir_get_locale_languages (FlatpakDir *self);
char ** flatpak_dir_get_locale_subpaths (FlatpakDir *self);

void flatpak_dir_set_source_pid (FlatpakDir *self,
                                  pid_t      pid);
pid_t flatpak_dir_get_source_pid (FlatpakDir *self);

#endif /* __FLATPAK_DIR_H__ */
