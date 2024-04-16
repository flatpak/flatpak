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

#include "flatpak-common-types-private.h"
#include "flatpak-context-private.h"
#include "flatpak-progress-private.h"
#include "flatpak-variant-private.h"
#include "flatpak-ref-utils-private.h"
#include "libglnx.h"

/* Version history:
 * The version field was added in flatpak 1.2, anything before is 0.
 *
 * Version 1 added appdata-name/summary/version/license
 * Version 2 added extension-of/appdata-content-rating
 * Version 3 added timestamp
 * Version 4 guarantees that alt-id/eol/eolr/runtime/extension-of/appdata-content-rating
 *           are present if in the commit metadata or metadata file or appdata
 */
#define FLATPAK_DEPLOY_VERSION_CURRENT 4
#define FLATPAK_DEPLOY_VERSION_ANY 0

#define FLATPAK_TYPE_DIR flatpak_dir_get_type ()
#define FLATPAK_DIR(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_DIR, FlatpakDir))
#define FLATPAK_IS_DIR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_DIR))

#define SYSTEM_DIR_DEFAULT_ID "default"
#define SYSTEM_DIR_DEFAULT_DISPLAY_NAME _("Default system installation")
#define SYSTEM_DIR_DEFAULT_STORAGE_TYPE FLATPAK_DIR_STORAGE_TYPE_DEFAULT
#define SYSTEM_DIR_DEFAULT_PRIORITY 0

#define FLATPAK_TYPE_DEPLOY flatpak_deploy_get_type ()
#define FLATPAK_DEPLOY(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_DEPLOY, FlatpakDeploy))
#define FLATPAK_IS_DEPLOY(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_DEPLOY))

GType flatpak_dir_get_type (void);
GType flatpak_deploy_get_type (void);

#define FLATPAK_CLI_UPDATE_INTERVAL_MS 300

typedef struct
{
  FlatpakDecomposed *ref;
  char              *remote;
  char              *commit;
  char             **subpaths;
  gboolean           download;
  gboolean           delete;
  gboolean           auto_prune;
} FlatpakRelated;

void         flatpak_related_free (FlatpakRelated *related);

typedef struct {
  OstreeRepo *repo;
  GVariant   *summary;
} FlatpakSideloadState;

/* The remote state represent the state of the remote at a particular
   time, including the summary file and the metadata (which may be from
   the summary or from a branch. We create this once per highlevel operation
   to avoid looking up the summary multiple times, but also to avoid races
   if it happened to change in the middle of the operation */
typedef struct
{
  char     *remote_name;
  gboolean  is_file_uri;
  char     *collection_id;

  /* New format summary */

  GVariant   *index;
  GBytes     *index_sig_bytes;
  GHashTable *index_ht; /* Arch -> subsummary digest (filtered by subsystem) */
  GHashTable *subsummaries; /* digest -> GVariant */

  /* Compat summary */
  GVariant *summary;
  GBytes   *summary_bytes;
  GBytes   *summary_sig_bytes;
  GError   *summary_fetch_error;

  GRegex   *allow_refs;
  GRegex   *deny_refs;
  int       refcount;
  gint32    default_token_type;
  GPtrArray *sideload_repos;
} FlatpakRemoteState;

FlatpakRemoteState *flatpak_remote_state_ref (FlatpakRemoteState *remote_state);
void flatpak_remote_state_unref (FlatpakRemoteState *remote_state);
gboolean flatpak_remote_state_ensure_summary (FlatpakRemoteState *self,
                                              GError            **error);
gboolean flatpak_remote_state_ensure_subsummary (FlatpakRemoteState *self,
                                                 FlatpakDir         *dir,
                                                 const char         *arch,
                                                 gboolean            only_cached,
                                                 GCancellable       *cancellable,
                                                 GError            **error);
gboolean flatpak_remote_state_ensure_subsummary_all_arches (FlatpakRemoteState *self,
                                                            FlatpakDir         *dir,
                                                            gboolean            only_cached,
                                                            GCancellable       *cancellable,
                                                            GError            **error);
gboolean flatpak_remote_state_allow_ref (FlatpakRemoteState *self,
                                         const char *ref);
gboolean flatpak_remote_state_lookup_ref (FlatpakRemoteState *self,
                                          const char         *ref,
                                          char              **out_checksum,
                                          guint64            *out_timestamp,
                                          VarRefInfoRef      *out_info,
                                          GFile             **out_sideload_path,
                                          GError            **error);
GPtrArray *flatpak_remote_state_match_subrefs (FlatpakRemoteState *self,
                                               FlatpakDecomposed *ref);
GFile *flatpak_remote_state_lookup_sideload_checksum (FlatpakRemoteState *self,
                                                      char               *checksum);
gboolean flatpak_remote_state_lookup_cache (FlatpakRemoteState *self,
                                            const char         *ref,
                                            guint64            *download_size,
                                            guint64            *installed_size,
                                            const char        **metadata,
                                            GError            **error);
gboolean flatpak_remote_state_load_data (FlatpakRemoteState *self,
                                         const char         *ref,
                                         guint64            *out_download_size,
                                         guint64            *out_installed_size,
                                         char              **out_metadata,
                                         GError            **error);
gboolean flatpak_remote_state_lookup_sparse_cache (FlatpakRemoteState *self,
                                                   const char         *ref,
                                                   VarMetadataRef     *out_metadata,
                                                   GError            **error);
GVariant *flatpak_remote_state_load_ref_commit (FlatpakRemoteState *self,
                                                FlatpakDir         *dir,
                                                const char         *ref,
                                                const char         *opt_commit,
                                                const char         *token,
                                                char              **out_commit,
                                                GCancellable       *cancellable,
                                                GError            **error);
void flatpak_remote_state_add_sideload_dir (FlatpakRemoteState *self,
                                            GFile              *path);


G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakDir, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakDeploy, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakRelated, flatpak_related_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakRemoteState, flatpak_remote_state_unref)

typedef enum {
  FLATPAK_HELPER_DEPLOY_FLAGS_NONE = 0,
  FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE = 1 << 0,
  FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY = 1 << 1,
  FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL = 1 << 2,
  FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL = 1 << 3,
  FLATPAK_HELPER_DEPLOY_FLAGS_NO_INTERACTION = 1 << 4,
  FLATPAK_HELPER_DEPLOY_FLAGS_APP_HINT = 1 << 5,
  FLATPAK_HELPER_DEPLOY_FLAGS_INSTALL_HINT = 1 << 6,
  FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE_PINNED = 1 << 7,
  FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE_PREINSTALLED = 1 << 8,
} FlatpakHelperDeployFlags;

#define FLATPAK_HELPER_DEPLOY_FLAGS_ALL (FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE | \
                                         FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY | \
                                         FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL | \
                                         FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL | \
                                         FLATPAK_HELPER_DEPLOY_FLAGS_NO_INTERACTION | \
                                         FLATPAK_HELPER_DEPLOY_FLAGS_APP_HINT | \
                                         FLATPAK_HELPER_DEPLOY_FLAGS_INSTALL_HINT | \
                                         FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE_PINNED | \
                                         FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE_PREINSTALLED)

typedef enum {
  FLATPAK_HELPER_UNINSTALL_FLAGS_NONE = 0,
  FLATPAK_HELPER_UNINSTALL_FLAGS_KEEP_REF = 1 << 0,
  FLATPAK_HELPER_UNINSTALL_FLAGS_FORCE_REMOVE = 1 << 1,
  FLATPAK_HELPER_UNINSTALL_FLAGS_NO_INTERACTION = 1 << 2,
  FLATPAK_HELPER_UNINSTALL_FLAGS_UPDATE_PREINSTALLED = 1 << 3,
} FlatpakHelperUninstallFlags;

#define FLATPAK_HELPER_UNINSTALL_FLAGS_ALL (FLATPAK_HELPER_UNINSTALL_FLAGS_KEEP_REF | \
                                            FLATPAK_HELPER_UNINSTALL_FLAGS_FORCE_REMOVE | \
                                            FLATPAK_HELPER_UNINSTALL_FLAGS_NO_INTERACTION | \
                                            FLATPAK_HELPER_UNINSTALL_FLAGS_UPDATE_PREINSTALLED)

typedef enum {
  FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_NONE = 0,
  FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_FORCE_REMOVE = 1 << 0,
  FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_NO_INTERACTION = 1 << 1,
} FlatpakHelperConfigureRemoteFlags;

#define FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_ALL (FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_FORCE_REMOVE | \
                                                   FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_NO_INTERACTION)

typedef enum {
  FLATPAK_HELPER_CONFIGURE_FLAGS_NONE = 0,
  FLATPAK_HELPER_CONFIGURE_FLAGS_UNSET = 1 << 0,
  FLATPAK_HELPER_CONFIGURE_FLAGS_NO_INTERACTION = 1 << 1,
} FlatpakHelperConfigureFlags;

#define FLATPAK_HELPER_CONFIGURE_FLAGS_ALL (FLATPAK_HELPER_CONFIGURE_FLAGS_UNSET | \
                                            FLATPAK_HELPER_CONFIGURE_FLAGS_NO_INTERACTION)

typedef enum {
  FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_NONE = 0,
  FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_NO_INTERACTION = 1 << 0,
  FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_SUMMARY_IS_INDEX = 1 << 1,
} FlatpakHelperUpdateRemoteFlags;

#define FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_ALL (FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_NO_INTERACTION | \
                                                FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_SUMMARY_IS_INDEX)

typedef enum {
  FLATPAK_HELPER_GET_REVOKEFS_FD_FLAGS_NONE = 0,
  FLATPAK_HELPER_GET_REVOKEFS_FD_FLAGS_NO_INTERACTION = 1 << 0,
} FlatpakHelperGetRevokefsFdFlags;

#define FLATPAK_HELPER_GET_REVOKEFS_FD_FLAGS_ALL (FLATPAK_HELPER_GET_REVOKEFS_FD_FLAGS_NO_INTERACTION)

typedef enum {
  FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_NONE = 0,
  FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_NO_INTERACTION = 1 << 0,
} FlatpakHelperInstallBundleFlags;

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
  FLATPAK_HELPER_CANCEL_PULL_FLAGS_NONE = 0,
  FLATPAK_HELPER_CANCEL_PULL_FLAGS_PRESERVE_PULL = 1 << 0,
  FLATPAK_HELPER_CANCEL_PULL_FLAGS_NO_INTERACTION = 1 << 1,
} FlatpakHelperCancelPullFlags;

#define FLATPAK_HELPER_CANCEL_PULL_FLAGS_ALL (FLATPAK_HELPER_CANCEL_PULL_FLAGS_PRESERVE_PULL |\
                                              FLATPAK_HELPER_CANCEL_PULL_FLAGS_NO_INTERACTION)

typedef enum {
  FLATPAK_HELPER_ENSURE_REPO_FLAGS_NONE = 0,
  FLATPAK_HELPER_ENSURE_REPO_FLAGS_NO_INTERACTION = 1 << 0,
} FlatpakHelperEnsureRepoFlags;

#define FLATPAK_HELPER_ENSURE_REPO_FLAGS_ALL (FLATPAK_HELPER_ENSURE_REPO_FLAGS_NO_INTERACTION)

typedef enum {
  FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_NONE = 0,
  FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_NO_INTERACTION = 1 << 0,
  FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_DELETE = 1 << 1,
} FlatpakHelperUpdateSummaryFlags;

#define FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_ALL (FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_NO_INTERACTION |\
                                                 FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_DELETE)

typedef enum {
  FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_NONE = 0,
  FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_NO_INTERACTION = 1 << 0,
  FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_ONLY_CACHED = 1 << 1,
} FlatpakHelperGenerateOciSummaryFlags;

#define FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_ALL (FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_NO_INTERACTION |\
                                                       FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_ONLY_CACHED)

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
  FLATPAK_DIR_FILTER_NONE = 0,
  FLATPAK_DIR_FILTER_EOL = 1 << 0,
  FLATPAK_DIR_FILTER_AUTOPRUNE = 1 << 1,
} FlatpakDirFilterFlags;

typedef enum {
  FIND_MATCHING_REFS_FLAGS_NONE = 0,
  FIND_MATCHING_REFS_FLAGS_FUZZY = (1 << 0),
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

GPtrArray *flatpak_get_system_base_dir_locations        (GCancellable  *cancellable,
                                                         GError       **error);
GFile *    flatpak_get_system_default_base_dir_location (void);

GKeyFile *      flatpak_load_override_keyfile   (const char  *app_id,
                                                 gboolean     user,
                                                 GError     **error);
FlatpakContext *flatpak_load_override_file      (const char  *app_id,
                                                 gboolean     user,
                                                 GError     **error);
gboolean        flatpak_save_override_keyfile   (GKeyFile    *metakey,
                                                 const char  *app_id,
                                                 gboolean     user,
                                                 GError     **error);
gboolean        flatpak_remove_override_keyfile (const char  *app_id,
                                                 gboolean     user,
                                                 GError     **error);

char **  flatpak_get_preinstall_config_file_paths (GCancellable       *cancellable,
                                                   GError            **error);
gboolean flatpak_parse_preinstall_config_file     (const char         *file_path,
                                                   const char         *default_arch,
                                                   FlatpakDecomposed **ref_out,
                                                   char              **collection_id_out,
                                                   GError            **error);

int          flatpak_deploy_data_get_version                     (GBytes *deploy_data);
const char * flatpak_deploy_data_get_origin                      (GBytes *deploy_data);
const char * flatpak_deploy_data_get_commit                      (GBytes *deploy_data);
const char * flatpak_deploy_data_get_appdata_content_rating_type (GBytes *deploy_data);
GHashTable * flatpak_deploy_data_get_appdata_content_rating      (GBytes *deploy_data);
const char **flatpak_deploy_data_get_subpaths                    (GBytes *deploy_data);
gboolean     flatpak_deploy_data_has_subpaths                    (GBytes *deploy_data);
guint64      flatpak_deploy_data_get_installed_size              (GBytes *deploy_data);
guint64      flatpak_deploy_data_get_timestamp                   (GBytes *deploy_data);
const char * flatpak_deploy_data_get_alt_id                      (GBytes *deploy_data);
const char * flatpak_deploy_data_get_eol                         (GBytes *deploy_data);
const char * flatpak_deploy_data_get_eol_rebase                  (GBytes *deploy_data);
const char * flatpak_deploy_data_get_runtime                     (GBytes *deploy_data);
const char * flatpak_deploy_data_get_extension_of                (GBytes *deploy_data);
const char * flatpak_deploy_data_get_appdata_name                (GBytes *deploy_data);
const char * flatpak_deploy_data_get_appdata_summary             (GBytes *deploy_data);
const char * flatpak_deploy_data_get_appdata_version             (GBytes *deploy_data);
const char * flatpak_deploy_data_get_appdata_license             (GBytes *deploy_data);
const char **flatpak_deploy_data_get_previous_ids                (GBytes *deploy_data,
                                                                  gsize  *length);

GFile *         flatpak_deploy_get_dir         (FlatpakDeploy      *deploy);
GBytes *        flatpak_load_deploy_data       (GFile              *deploy_dir,
                                                FlatpakDecomposed  *ref,
                                                OstreeRepo         *repo,
                                                int                 required_version,
                                                GCancellable       *cancellable,
                                                GError            **error);
GBytes *        flatpak_deploy_get_deploy_data (FlatpakDeploy      *deploy,
                                                int                 required_version,
                                                GCancellable       *cancellable,
                                                GError            **error);
GFile *         flatpak_deploy_get_files       (FlatpakDeploy      *deploy);
FlatpakContext *flatpak_deploy_get_overrides   (FlatpakDeploy      *deploy);
GKeyFile *      flatpak_deploy_get_metadata    (FlatpakDeploy      *deploy);

FlatpakDir *          flatpak_dir_new                                       (GFile                         *basedir,
                                                                             gboolean                       user);
FlatpakDir *          flatpak_dir_clone                                     (FlatpakDir                    *self);
FlatpakDir  *         flatpak_dir_get_user                                  (void);
FlatpakDir  *         flatpak_dir_get_system_default                        (void);
GPtrArray   *         flatpak_dir_get_system_list                           (GCancellable                  *cancellable,
                                                                             GError                       **error);
FlatpakDir  *         flatpak_dir_get_system_by_id                          (const char                    *id,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
FlatpakDir *          flatpak_dir_get_by_path                               (GFile                         *path);
gboolean              flatpak_dir_is_user                                   (FlatpakDir                    *self);
void                  flatpak_dir_set_no_system_helper                      (FlatpakDir                    *self,
                                                                             gboolean                       no_system_helper);
void                  flatpak_dir_set_no_interaction                        (FlatpakDir                    *self,
                                                                             gboolean                       no_interaction);
gboolean              flatpak_dir_get_no_interaction                        (FlatpakDir                    *self);
GFile *               flatpak_dir_get_path                                  (FlatpakDir                    *self);
GFile *               flatpak_dir_get_changed_path                          (FlatpakDir                    *self);
const char *          flatpak_dir_get_id                                    (FlatpakDir                    *self);
char       *          flatpak_dir_get_display_name                          (FlatpakDir                    *self);
char *                flatpak_dir_get_name                                  (FlatpakDir                    *self);
const char *          flatpak_dir_get_name_cached                           (FlatpakDir                    *self);
gint                  flatpak_dir_get_priority                              (FlatpakDir                    *self);
FlatpakDirStorageType flatpak_dir_get_storage_type                          (FlatpakDir                    *self);
GFile *               flatpak_dir_get_deploy_dir                            (FlatpakDir                    *self,
                                                                             FlatpakDecomposed             *ref);
char *                flatpak_dir_get_deploy_subdir                         (FlatpakDir                    *self,
                                                                             const char                    *checksum,
                                                                             const char * const            *subpaths);
GFile *               flatpak_dir_get_unmaintained_extension_dir            (FlatpakDir                    *self,
                                                                             const char                    *name,
                                                                             const char                    *arch,
                                                                             const char                    *branch);
GBytes *              flatpak_dir_get_deploy_data                           (FlatpakDir                    *dir,
                                                                             FlatpakDecomposed             *ref,
                                                                             int                            required_version,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
char *                flatpak_dir_get_origin                                (FlatpakDir                    *self,
                                                                             FlatpakDecomposed             *ref,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
GFile *               flatpak_dir_get_exports_dir                           (FlatpakDir                    *self);
GFile *               flatpak_dir_get_removed_dir                           (FlatpakDir                    *self);
GFile *               flatpak_dir_get_sideload_repos_dir                    (FlatpakDir                    *self);
GFile *               flatpak_dir_get_runtime_sideload_repos_dir            (FlatpakDir                    *self);
GFile *               flatpak_dir_get_if_deployed                           (FlatpakDir                    *self,
                                                                             FlatpakDecomposed             *ref,
                                                                             const char                    *checksum,
                                                                             GCancellable                  *cancellable);
GFile *               flatpak_dir_get_unmaintained_extension_dir_if_exists  (FlatpakDir                    *self,
                                                                             const char                    *name,
                                                                             const char                    *arch,
                                                                             const char                    *branch,
                                                                             GCancellable                  *cancellable);
gboolean              flatpak_dir_ref_is_masked                             (FlatpakDir                    *self,
                                                                             const char                    *ref);
gboolean              flatpak_dir_ref_is_pinned                             (FlatpakDir                    *self,
                                                                             const char                    *ref);
FlatpakDecomposed *   flatpak_dir_find_remote_ref                           (FlatpakDir                    *self,
                                                                             FlatpakRemoteState            *state,
                                                                             const char                    *name,
                                                                             const char                    *opt_branch,
                                                                             const char                    *opt_default_branch,
                                                                             const char                    *opt_arch,
                                                                             FlatpakKinds                   kinds,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
GPtrArray *           flatpak_dir_find_remote_refs                          (FlatpakDir                    *self,
                                                                             FlatpakRemoteState            *state,
                                                                             const char                    *name,
                                                                             const char                    *opt_branch,
                                                                             const char                    *opt_default_branch,
                                                                             const char                    *opt_arch,
                                                                             const char                    *opt_default_arch,
                                                                             FlatpakKinds                   kinds,
                                                                             FindMatchingRefsFlags          flags,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
GPtrArray *           flatpak_dir_find_local_refs                           (FlatpakDir                    *self,
                                                                             const char                    *remote,
                                                                             const char                    *name,
                                                                             const char                    *opt_branch,
                                                                             const char                    *opt_default_branch,
                                                                             const char                    *opt_arch,
                                                                             const char                    *opt_default_arch,
                                                                             FlatpakKinds                   kinds,
                                                                             FindMatchingRefsFlags          flags,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
FlatpakDecomposed *   flatpak_dir_find_installed_ref                        (FlatpakDir                    *self,
                                                                             const char                    *opt_name,
                                                                             const char                    *opt_branch,
                                                                             const char                    *opt_arch,
                                                                             FlatpakKinds                   kinds,
                                                                             GError                       **error);
GPtrArray *           flatpak_dir_find_installed_refs                       (FlatpakDir                    *self,
                                                                             const char                    *opt_name,
                                                                             const char                    *opt_branch,
                                                                             const char                    *opt_arch,
                                                                             FlatpakKinds                   kinds,
                                                                             FindMatchingRefsFlags          flags,
                                                                             GError                       **error);
FlatpakDeploy *       flatpak_dir_load_deployed                             (FlatpakDir                    *self,
                                                                             FlatpakDecomposed             *ref,
                                                                             const char                    *checksum,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
char *                flatpak_dir_load_override                             (FlatpakDir                    *dir,
                                                                             const char                    *app_id,
                                                                             gsize                         *length,
                                                                             GFile                        **file_out,
                                                                             GError                       **error);
OstreeRepo *          flatpak_dir_get_repo                                  (FlatpakDir                    *self);
gboolean              flatpak_dir_ensure_path                               (FlatpakDir                    *self,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_use_child_repo                            (FlatpakDir                    *self);
gboolean              flatpak_dir_ensure_system_child_repo                  (FlatpakDir                    *self,
                                                                             GError                       **error);
gboolean              flatpak_dir_recreate_repo                             (FlatpakDir                    *self,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_ensure_repo                               (FlatpakDir                    *self,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_maybe_ensure_repo                         (FlatpakDir                    *self,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
char *                flatpak_dir_get_config                                (FlatpakDir                    *self,
                                                                             const char                    *key,
                                                                             GError                       **error);
GPtrArray *           flatpak_dir_get_config_patterns                       (FlatpakDir                    *self,
                                                                             const char                    *key);
gboolean              flatpak_dir_set_config                                (FlatpakDir                    *self,
                                                                             const char                    *key,
                                                                             const char                    *value,
                                                                             GError                       **error);
gboolean              flatpak_dir_config_append_pattern                     (FlatpakDir                    *self,
                                                                             const char                    *key,
                                                                             const char                    *pattern,
                                                                             gboolean                       runtime_only,
                                                                             gboolean                      *out_already_present,
                                                                             GError                       **error);
gboolean              flatpak_dir_config_remove_pattern                     (FlatpakDir                    *self,
                                                                             const char                    *key,
                                                                             const char                    *pattern,
                                                                             GError                       **error);
gboolean              flatpak_dir_mark_changed                              (FlatpakDir                    *self,
                                                                             GError                       **error);
gboolean              flatpak_dir_remove_appstream                          (FlatpakDir                    *self,
                                                                             const char                    *remote,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_deploy_appstream                          (FlatpakDir                    *self,
                                                                             const char                    *remote,
                                                                             const char                    *arch,
                                                                             gboolean                      *out_changed,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_update_appstream                          (FlatpakDir                    *self,
                                                                             const char                    *remote,
                                                                             const char                    *arch,
                                                                             gboolean                      *out_changed,
                                                                             FlatpakProgress               *progress,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_pull                                      (FlatpakDir                    *self,
                                                                             FlatpakRemoteState            *state,
                                                                             const char                    *ref,
                                                                             const char                    *opt_rev,
                                                                             const char                   **subpaths,
                                                                             GFile                         *sideload_repo,
                                                                             GBytes                        *require_metadata,
                                                                             const char                    *token,
                                                                             OstreeRepo                    *repo,
                                                                             FlatpakPullFlags               flatpak_flags,
                                                                             OstreeRepoPullFlags            flags,
                                                                             FlatpakProgress               *progress,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_pull_untrusted_local                      (FlatpakDir                    *self,
                                                                             const char                    *src_path,
                                                                             const char                    *remote_name,
                                                                             const char                    *ref,
                                                                             const char                   **subpaths,
                                                                             FlatpakProgress               *progress,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
GPtrArray *           flatpak_dir_list_refs_for_name                        (FlatpakDir                    *self,
                                                                             FlatpakKinds                   kind,
                                                                             const char                    *name,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
GPtrArray *           flatpak_dir_list_refs                                 (FlatpakDir                    *self,
                                                                             FlatpakKinds                   kinds,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_is_runtime_extension                      (FlatpakDir                    *self,
                                                                             FlatpakDecomposed             *ref);
GPtrArray *           flatpak_dir_list_app_refs_with_runtime                (FlatpakDir                    *self,
                                                                             GHashTable                   **runtime_app_map,
                                                                             FlatpakDecomposed             *runtime_ref,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
GPtrArray *           flatpak_dir_list_app_refs_with_runtime_extension      (FlatpakDir                    *self,
                                                                             GHashTable                   **runtime_app_map,
                                                                             GHashTable                   **extension_app_map,
                                                                             FlatpakDecomposed             *runtime_ext_ref,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
GVariant *            flatpak_dir_read_latest_commit                        (FlatpakDir                    *self,
                                                                             const char                    *remote,
                                                                             FlatpakDecomposed             *ref,
                                                                             char                         **out_checksum,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
char *                flatpak_dir_read_latest                               (FlatpakDir                    *self,
                                                                             const char                    *remote,
                                                                             const char                    *ref,
                                                                             char                         **out_alt_id,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
char *                flatpak_dir_read_active                               (FlatpakDir                    *self,
                                                                             FlatpakDecomposed             *ref,
                                                                             GCancellable                  *cancellable);
gboolean              flatpak_dir_set_active                                (FlatpakDir                    *self,
                                                                             FlatpakDecomposed             *ref,
                                                                             const char                    *checksum,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
FlatpakDecomposed *   flatpak_dir_current_ref                               (FlatpakDir                    *self,
                                                                             const char                    *name,
                                                                             GCancellable                  *cancellable);
gboolean              flatpak_dir_drop_current_ref                          (FlatpakDir                    *self,
                                                                             const char                    *name,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_make_current_ref                          (FlatpakDir                    *self,
                                                                             FlatpakDecomposed             *ref,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_list_deployed                             (FlatpakDir                    *self,
                                                                             FlatpakDecomposed             *ref,
                                                                             char                        ***deployed_checksums,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_lock                                      (FlatpakDir                    *self,
                                                                             GLnxLockFile                  *lockfile,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_repo_lock                                 (FlatpakDir                    *self,
                                                                             GLnxLockFile                  *lockfile,
                                                                             gboolean                       exclusive,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_deploy                                    (FlatpakDir                    *self,
                                                                             const char                    *origin,
                                                                             FlatpakDecomposed             *ref,
                                                                             const char                    *checksum_or_latest,
                                                                             const char * const            *subpaths,
                                                                             const char * const            *previous_ids,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_deploy_update                             (FlatpakDir                    *self,
                                                                             FlatpakDecomposed             *ref,
                                                                             const char                    *checksum,
                                                                             const char                   **opt_subpaths,
                                                                             const char                   **opt_previous_ids,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_deploy_install                            (FlatpakDir                    *self,
                                                                             FlatpakDecomposed             *ref,
                                                                             const char                    *origin,
                                                                             const char                   **subpaths,
                                                                             const char                   **previous_ids,
                                                                             gboolean                       reinstall,
                                                                             gboolean                       pin_on_deploy,
                                                                             gboolean                       update_preinstalled_on_deploy,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_install                                   (FlatpakDir                    *self,
                                                                             gboolean                       no_pull,
                                                                             gboolean                       no_deploy,
                                                                             gboolean                       no_static_deltas,
                                                                             gboolean                       reinstall,
                                                                             gboolean                       app_hint,
                                                                             gboolean                       pin_on_deploy,
                                                                             gboolean                       update_preinstalled_on_deploy,
                                                                             FlatpakRemoteState            *state,
                                                                             FlatpakDecomposed             *ref,
                                                                             const char                    *opt_commit,
                                                                             const char                   **subpaths,
                                                                             const char                   **previous_ids,
                                                                             GFile                         *sideload_repo,
                                                                             GBytes                        *require_metadata,
                                                                             const char                    *token,
                                                                             FlatpakProgress               *progress,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
char *                flatpak_dir_ensure_bundle_remote                      (FlatpakDir                    *self,
                                                                             GFile                         *file,
                                                                             GBytes                        *extra_gpg_data,
                                                                             FlatpakDecomposed            **out_ref,
                                                                             char                         **out_commit,
                                                                             char                         **out_metadata,
                                                                             gboolean                      *out_created_remote,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_install_bundle                            (FlatpakDir                    *self,
                                                                             GFile                         *file,
                                                                             const char                    *remote,
                                                                             FlatpakDecomposed            **out_ref,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_needs_update_for_commit_and_subpaths      (FlatpakDir                    *self,
                                                                             const char                    *remote,
                                                                             FlatpakDecomposed             *ref,
                                                                             const char                    *target_commit,
                                                                             const char                   **opt_subpaths);
char *                flatpak_dir_check_for_update                          (FlatpakDir                    *self,
                                                                             FlatpakRemoteState            *state,
                                                                             FlatpakDecomposed             *ref,
                                                                             const char                    *checksum_or_latest,
                                                                             const char                   **opt_subpaths,
                                                                             gboolean                       no_pull,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_update                                    (FlatpakDir                    *self,
                                                                             gboolean                       no_pull,
                                                                             gboolean                       no_deploy,
                                                                             gboolean                       no_static_deltas,
                                                                             gboolean                       allow_downgrade,
                                                                             gboolean                       app_hint,
                                                                             gboolean                       install_hint,
                                                                             FlatpakRemoteState            *state,
                                                                             FlatpakDecomposed             *ref,
                                                                             const char                    *checksum_or_latest,
                                                                             const char                   **opt_subpaths,
                                                                             const char                   **opt_previous_ids,
                                                                             GFile                         *sideload_repo,
                                                                             GBytes                        *require_metadata,
                                                                             const char                    *token,
                                                                             FlatpakProgress               *progress,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_uninstall                                 (FlatpakDir                    *self,
                                                                             FlatpakDecomposed             *ref,
                                                                             FlatpakHelperUninstallFlags    flags,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_undeploy                                  (FlatpakDir                    *self,
                                                                             FlatpakDecomposed             *ref,
                                                                             const char                    *checksum,
                                                                             gboolean                       is_update,
                                                                             gboolean                       force_remove,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_undeploy_all                              (FlatpakDir                    *self,
                                                                             FlatpakDecomposed             *ref,
                                                                             gboolean                       force_remove,
                                                                             gboolean                      *was_deployed_out,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_remove_ref                                (FlatpakDir                    *self,
                                                                             const char                    *remote_name,
                                                                             const char                    *ref,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_update_exports                            (FlatpakDir                    *self,
                                                                             const char                    *app,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_prune                                     (FlatpakDir                    *self,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_run_triggers                              (FlatpakDir                    *self,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_update_summary                            (FlatpakDir                    *self,
                                                                             gboolean                       delete,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_cleanup_removed                           (FlatpakDir                    *self,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_cleanup_undeployed_refs                   (FlatpakDir                    *self,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_collect_deployed_refs                     (FlatpakDir                    *self,
                                                                             const char                    *type,
                                                                             const char                    *name_prefix,
                                                                             const char                    *arch,
                                                                             const char                    *branch,
                                                                             GHashTable                    *hash,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_collect_unmaintained_refs                 (FlatpakDir                    *self,
                                                                             const char                    *name_prefix,
                                                                             const char                    *arch,
                                                                             const char                    *branch,
                                                                             GHashTable                    *hash,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_remote_has_deploys                        (FlatpakDir                    *self,
                                                                             const char                    *remote);
char      *           flatpak_dir_create_origin_remote                      (FlatpakDir                    *self,
                                                                             const char                    *url,
                                                                             const char                    *id,
                                                                             const char                    *title,
                                                                             const char                    *main_ref,
                                                                             GBytes                        *gpg_data,
                                                                             const char                    *collection_id,
                                                                             gboolean                      *changed_config,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
void                  flatpak_dir_prune_origin_remote                       (FlatpakDir                    *self,
                                                                             const char                    *remote);
gboolean              flatpak_dir_create_remote_for_ref_file                (FlatpakDir                    *self,
                                                                             GKeyFile                      *keyfile,
                                                                             const char                    *default_arch,
                                                                             char                         **remote_name_out,
                                                                             char                         **collection_id_out,
                                                                             FlatpakDecomposed            **ref_out,
                                                                             GError                       **error);
gboolean              flatpak_dir_create_suggested_remote_for_ref_file      (FlatpakDir                    *self,
                                                                             GBytes                        *data,
                                                                             GError                       **error);
char      *           flatpak_dir_find_remote_by_uri                        (FlatpakDir                    *self,
                                                                             const char                    *uri);
gboolean              flatpak_dir_has_remote                                (FlatpakDir                    *self,
                                                                             const char                    *remote_name,
                                                                             GError                       **error);
char     **           flatpak_dir_list_remotes                              (FlatpakDir                    *self,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
char     **           flatpak_dir_list_enumerated_remotes                   (FlatpakDir                    *self,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
char     **           flatpak_dir_list_dependency_remotes                   (FlatpakDir                    *self,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_modify_remote                             (FlatpakDir                    *self,
                                                                             const char                    *remote_name,
                                                                             GKeyFile                      *config,
                                                                             GBytes                        *gpg_data,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_remove_remote                             (FlatpakDir                    *self,
                                                                             gboolean                       force_remove,
                                                                             const char                    *remote_name,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
GPtrArray *           flatpak_dir_get_sideload_repo_paths                   (FlatpakDir                    *self);
char **               flatpak_dir_list_remote_config_keys                   (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
char      *           flatpak_dir_get_remote_title                          (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
char      *           flatpak_dir_get_remote_comment                        (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
char      *           flatpak_dir_get_remote_description                    (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
gint32                flatpak_dir_get_remote_default_token_type             (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
char      *           flatpak_dir_get_remote_homepage                       (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
char      *           flatpak_dir_get_remote_icon                           (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
char      *           flatpak_dir_get_remote_collection_id                  (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
char      *           flatpak_dir_get_remote_main_ref                       (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
gboolean              flatpak_dir_get_remote_oci                            (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
char      *           flatpak_dir_get_remote_default_branch                 (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
int                   flatpak_dir_get_remote_prio                           (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
gboolean              flatpak_dir_get_remote_noenumerate                    (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
gboolean              flatpak_dir_get_remote_nodeps                         (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
char      *           flatpak_dir_get_remote_filter                         (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
char      *           flatpak_dir_get_remote_subset                         (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
gboolean              flatpak_dir_compare_remote_filter                     (FlatpakDir                    *self,
                                                                             const char                    *remote_name,
                                                                             const char                    *filter);
gboolean              flatpak_dir_get_remote_disabled                       (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
gboolean              flatpak_dir_list_remote_refs                          (FlatpakDir                    *self,
                                                                             FlatpakRemoteState            *state,
                                                                             GHashTable                   **refs,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_list_all_remote_refs                      (FlatpakDir                    *self,
                                                                             FlatpakRemoteState            *state,
                                                                             GHashTable                   **out_all_refs,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_update_remote_configuration               (FlatpakDir                    *self,
                                                                             const char                    *remote,
                                                                             FlatpakRemoteState            *optional_remote_state,
                                                                             gboolean                      *changed_out,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_update_remote_configuration_for_state     (FlatpakDir                    *self,
                                                                             FlatpakRemoteState            *remote_state,
                                                                             gboolean                       dry_run,
                                                                             gboolean                      *has_changed_out,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
FlatpakRemoteState *  flatpak_dir_get_remote_state                          (FlatpakDir                    *self,
                                                                             const char                    *remote,
                                                                             gboolean                       only_cached,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
FlatpakRemoteState *  flatpak_dir_get_remote_state_for_summary              (FlatpakDir                    *self,
                                                                             const char                    *remote,
                                                                             GBytes                        *opt_summary,
                                                                             GBytes                        *opt_summary_sig,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
FlatpakRemoteState *  flatpak_dir_get_remote_state_for_index                (FlatpakDir                    *self,
                                                                             const char                    *remote,
                                                                             GBytes                        *opt_index,
                                                                             GBytes                        *opt_index_sig,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_migrate_config                            (FlatpakDir                    *self,
                                                                             gboolean                      *changed,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_remote_make_oci_summary                   (FlatpakDir                    *self,
                                                                             const char                    *remote,
                                                                             gboolean                       only_cached,
                                                                             GBytes                       **out_summary,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
FlatpakRemoteState *  flatpak_dir_get_remote_state_optional                 (FlatpakDir                    *self,
                                                                             const char                    *remote,
                                                                             gboolean                       only_cached,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
FlatpakRemoteState *  flatpak_dir_get_remote_state_local_only               (FlatpakDir                    *self,
                                                                             const char                    *remote,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
GPtrArray *           flatpak_dir_find_remote_related_for_metadata          (FlatpakDir                    *self,
                                                                             FlatpakRemoteState            *state,
                                                                             FlatpakDecomposed             *ref,
                                                                             GKeyFile                      *metakey,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
GPtrArray *           flatpak_dir_find_remote_related                       (FlatpakDir                    *dir,
                                                                             FlatpakRemoteState            *state,
                                                                             FlatpakDecomposed             *ref,
                                                                             gboolean                       use_installed_metadata,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
GPtrArray *           flatpak_dir_find_local_related_for_metadata           (FlatpakDir                    *self,
                                                                             FlatpakDecomposed             *ref,
                                                                             const char                    *remote_name,
                                                                             GKeyFile                      *metakey,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
GPtrArray *           flatpak_dir_find_local_related                        (FlatpakDir                    *self,
                                                                             FlatpakDecomposed             *ref,
                                                                             const char                    *remote_name,
                                                                             gboolean                       deployed,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
gboolean              flatpak_dir_find_latest_rev                           (FlatpakDir                    *self,
                                                                             FlatpakRemoteState            *state,
                                                                             const char                    *ref,
                                                                             const char                    *checksum_or_latest,
                                                                             char                         **out_rev,
                                                                             guint64                       *out_timestamp,
                                                                             GFile                        **out_sideload_path,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
FlatpakDecomposed *   flatpak_dir_get_remote_auto_install_authenticator_ref (FlatpakDir                    *self,
                                                                             const char                    *remote_name);
char **               flatpak_dir_get_default_locales                       (FlatpakDir                    *self);
char **               flatpak_dir_get_default_locale_languages              (FlatpakDir                    *self);
char **               flatpak_dir_get_locales                               (FlatpakDir                    *self);
char **               flatpak_dir_get_locale_languages                      (FlatpakDir                    *self);
char **               flatpak_dir_get_locale_subpaths                       (FlatpakDir                    *self);
void                  flatpak_dir_set_source_pid                            (FlatpakDir                    *self,
                                                                             pid_t                          pid);
pid_t                 flatpak_dir_get_source_pid                            (FlatpakDir                    *self);
gboolean              flatpak_dir_delete_mirror_refs                        (FlatpakDir                    *self,
                                                                             gboolean                       dry_run,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);
char **               flatpak_dir_list_unused_refs                          (FlatpakDir                    *self,
                                                                             const char                    *arch,
                                                                             GHashTable                    *metadata_injection,
                                                                             GHashTable                    *eol_injection,
                                                                             const char * const            *refs_to_exclude,
                                                                             FlatpakDirFilterFlags          filter_flags,
                                                                             GCancellable                  *cancellable,
                                                                             GError                       **error);

#endif /* __FLATPAK_DIR_H__ */
