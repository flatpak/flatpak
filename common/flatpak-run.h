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

#ifndef __FLATPAK_RUN_H__
#define __FLATPAK_RUN_H__

#include "libglnx/libglnx.h"
#include "dbus-proxy/flatpak-proxy.h"
#include "flatpak-common-types.h"
#include "flatpak-utils.h"

gboolean flatpak_run_in_transient_unit (const char *app_id,
                                        GError    **error);

/* See flatpak-metadata(5) */

#define FLATPAK_METADATA_GROUP_APPLICATION "Application"
#define FLATPAK_METADATA_GROUP_RUNTIME "Runtime"
#define FLATPAK_METADATA_KEY_COMMAND "command"
#define FLATPAK_METADATA_KEY_NAME "name"
#define FLATPAK_METADATA_KEY_REQUIRED_FLATPAK "required-flatpak"
#define FLATPAK_METADATA_KEY_RUNTIME "runtime"
#define FLATPAK_METADATA_KEY_SDK "sdk"
#define FLATPAK_METADATA_KEY_TAGS "tags"

#define FLATPAK_METADATA_GROUP_CONTEXT "Context"
#define FLATPAK_METADATA_KEY_SHARED "shared"
#define FLATPAK_METADATA_KEY_SOCKETS "sockets"
#define FLATPAK_METADATA_KEY_FILESYSTEMS "filesystems"
#define FLATPAK_METADATA_KEY_PERSISTENT "persistent"
#define FLATPAK_METADATA_KEY_DEVICES "devices"
#define FLATPAK_METADATA_KEY_FEATURES "features"

#define FLATPAK_METADATA_GROUP_INSTANCE "Instance"
#define FLATPAK_METADATA_KEY_APP_PATH "app-path"
#define FLATPAK_METADATA_KEY_BRANCH "branch"
#define FLATPAK_METADATA_KEY_FLATPAK_VERSION "flatpak-version"
#define FLATPAK_METADATA_KEY_RUNTIME_PATH "runtime-path"
#define FLATPAK_METADATA_KEY_SESSION_BUS_PROXY "session-bus-proxy"
#define FLATPAK_METADATA_KEY_SYSTEM_BUS_PROXY "system-bus-proxy"

#define FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY "Session Bus Policy"
#define FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY "System Bus Policy"
#define FLATPAK_METADATA_GROUP_PREFIX_POLICY "Policy "
#define FLATPAK_METADATA_GROUP_ENVIRONMENT "Environment"

#define FLATPAK_METADATA_GROUP_PREFIX_EXTENSION "Extension "
#define FLATPAK_METADATA_KEY_ADD_LD_PATH "add-ld-path"
#define FLATPAK_METADATA_KEY_AUTODELETE "autodelete"
#define FLATPAK_METADATA_KEY_DIRECTORY "directory"
#define FLATPAK_METADATA_KEY_DOWNLOAD_IF "download-if"
#define FLATPAK_METADATA_KEY_ENABLE_IF "enable-if"
#define FLATPAK_METADATA_KEY_MERGE_DIRS "merge-dirs"
#define FLATPAK_METADATA_KEY_NO_AUTODOWNLOAD "no-autodownload"
#define FLATPAK_METADATA_KEY_SUBDIRECTORIES "subdirectories"
#define FLATPAK_METADATA_KEY_SUBDIRECTORY_SUFFIX "subdirectory-suffix"
#define FLATPAK_METADATA_KEY_LOCALE_SUBSET "locale-subset"
#define FLATPAK_METADATA_KEY_VERSION "version"
#define FLATPAK_METADATA_KEY_VERSIONS "versions"

#ifdef FLATPAK_ENABLE_P2P
#define FLATPAK_METADATA_KEY_COLLECTION_ID "collection-id"
#endif  /* FLATPAK_ENABLE_P2P */

#define FLATPAK_METADATA_GROUP_EXTRA_DATA "Extra Data"
#define FLATPAK_METADATA_KEY_EXTRA_DATA_CHECKSUM "checksum"
#define FLATPAK_METADATA_KEY_EXTRA_DATA_INSTALLED_SIZE "installed-size"
#define FLATPAK_METADATA_KEY_EXTRA_DATA_NAME "name"
#define FLATPAK_METADATA_KEY_EXTRA_DATA_SIZE "size"
#define FLATPAK_METADATA_KEY_EXTRA_DATA_URI "uri"
#define FLATPAK_METADATA_KEY_NO_RUNTIME "NoRuntime"

#define FLATPAK_METADATA_GROUP_EXTENSION_OF "ExtensionOf"
#define FLATPAK_METADATA_KEY_PRIORITY "priority"
#define FLATPAK_METADATA_KEY_REF "ref"

extern const char *flatpak_context_sockets[];
extern const char *flatpak_context_devices[];
extern const char *flatpak_context_features[];
extern const char *flatpak_context_shares[];

FlatpakContext *flatpak_context_new (void);
void           flatpak_context_free (FlatpakContext *context);
void           flatpak_context_merge (FlatpakContext *context,
                                      FlatpakContext *other);
GOptionGroup  *flatpak_context_get_options (FlatpakContext *context);
void           flatpak_context_complete (FlatpakContext *context,
                                         FlatpakCompletion *completion);
gboolean       flatpak_context_load_metadata (FlatpakContext *context,
                                              GKeyFile       *metakey,
                                              GError        **error);
void           flatpak_context_save_metadata (FlatpakContext *context,
                                              gboolean        flatten,
                                              GKeyFile       *metakey);
void           flatpak_context_allow_host_fs (FlatpakContext *context);
void           flatpak_context_set_session_bus_policy (FlatpakContext *context,
                                                       const char     *name,
                                                       FlatpakPolicy   policy);
void           flatpak_context_set_system_bus_policy (FlatpakContext *context,
                                                      const char     *name,
                                                      FlatpakPolicy   policy);
void           flatpak_context_to_args (FlatpakContext *context,
                                        GPtrArray *args);
gboolean       flatpak_context_get_needs_session_bus_proxy (FlatpakContext *context);
gboolean       flatpak_context_get_needs_system_bus_proxy (FlatpakContext *context);

FlatpakContext *flatpak_context_load_for_app (const char     *app_id,
                                              GError        **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakContext, flatpak_context_free)

typedef enum {
  FLATPAK_RUN_FLAG_DEVEL              = (1 << 0),
  FLATPAK_RUN_FLAG_BACKGROUND         = (1 << 1),
  FLATPAK_RUN_FLAG_LOG_SESSION_BUS    = (1 << 2),
  FLATPAK_RUN_FLAG_LOG_SYSTEM_BUS     = (1 << 3),
  FLATPAK_RUN_FLAG_NO_SESSION_HELPER  = (1 << 4),
  FLATPAK_RUN_FLAG_MULTIARCH          = (1 << 5),
  FLATPAK_RUN_FLAG_WRITABLE_ETC       = (1 << 6),
  FLATPAK_RUN_FLAG_NO_SESSION_BUS_PROXY = (1 << 7),
  FLATPAK_RUN_FLAG_NO_SYSTEM_BUS_PROXY = (1 << 8),
  FLATPAK_RUN_FLAG_SET_PERSONALITY    = (1 << 9),
  FLATPAK_RUN_FLAG_FILE_FORWARDING    = (1 << 10),
  FLATPAK_RUN_FLAG_DIE_WITH_PARENT    = (1 << 11),
  FLATPAK_RUN_FLAG_LOG_A11Y_BUS       = (1 << 12),
  FLATPAK_RUN_FLAG_NO_A11Y_BUS_PROXY  = (1 << 13),
} FlatpakRunFlags;

typedef struct _FlatpakExports FlatpakExports;

void flatpak_exports_free (FlatpakExports *exports);

gboolean flatpak_exports_path_is_visible (FlatpakExports *exports,
                                          const char *path);
FlatpakExports *flatpak_exports_from_context (FlatpakContext *context,
                                              const char *app_id);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakExports, flatpak_exports_free);

gboolean  flatpak_run_add_extension_args (GPtrArray    *argv_array,
                                          char       ***envp_p,
                                          GKeyFile     *metakey,
                                          const char   *full_ref,
                                          GCancellable *cancellable,
                                          GError      **error);
gboolean flatpak_run_add_environment_args (GPtrArray      *argv_array,
                                           GArray         *fd_array,
                                           char         ***envp_p,
                                           const char     *app_info_path,
                                           FlatpakRunFlags flags,
                                           const char     *app_id,
                                           FlatpakContext *context,
                                           GFile          *app_id_dir,
                                           FlatpakExports **exports_out,
                                           GCancellable *cancellable,
                                           GError      **error);
char **  flatpak_run_get_minimal_env (gboolean devel);
char **  flatpak_run_apply_env_default (char **envp);
char **  flatpak_run_apply_env_appid (char **envp,
                                      GFile *app_dir);
char **  flatpak_run_apply_env_vars (char          **envp,
                                     FlatpakContext *context);
FlatpakContext *flatpak_app_compute_permissions (GKeyFile *app_metadata,
                                                 GKeyFile *runtime_metadata,
                                                 GError  **error);

GFile *flatpak_get_data_dir (const char *app_id);
GFile *flatpak_ensure_data_dir (const char   *app_id,
                                GCancellable *cancellable,
                                GError      **error);

gboolean flatpak_run_setup_base_argv (GPtrArray      *argv_array,
                                      GArray         *fd_array,
                                      GFile          *runtime_files,
                                      GFile          *app_id_dir,
                                      const char     *arch,
                                      FlatpakRunFlags flags,
                                      GError        **error);
gboolean flatpak_run_add_app_info_args (GPtrArray      *argv_array,
                                        GArray         *fd_array,
                                        GFile          *app_files,
                                        GFile          *runtime_files,
                                        const char     *app_id,
                                        const char     *app_branch,
                                        const char     *runtime_ref,
                                        FlatpakContext *final_app_context,
                                        char          **app_info_path_out,
                                        GError        **error);

gboolean flatpak_run_app (const char     *app_ref,
                          FlatpakDeploy  *app_deploy,
                          FlatpakContext *extra_context,
                          const char     *custom_runtime,
                          const char     *custom_runtime_version,
                          FlatpakRunFlags flags,
                          const char     *custom_command,
                          char           *args[],
                          int             n_args,
                          GCancellable   *cancellable,
                          GError        **error);


#endif /* __FLATPAK_RUN_H__ */
