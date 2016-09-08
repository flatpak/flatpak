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

#define FLATPAK_METADATA_GROUP_CONTEXT "Context"
#define FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY "Session Bus Policy"
#define FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY "System Bus Policy"
#define FLATPAK_METADATA_GROUP_ENVIRONMENT "Environment"
#define FLATPAK_METADATA_KEY_SHARED "shared"
#define FLATPAK_METADATA_KEY_SOCKETS "sockets"
#define FLATPAK_METADATA_KEY_FILESYSTEMS "filesystems"
#define FLATPAK_METADATA_KEY_PERSISTENT "persistent"
#define FLATPAK_METADATA_KEY_DEVICES "devices"
#define FLATPAK_METADATA_KEY_FEATURES "features"

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

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakContext, flatpak_context_free)

gboolean  flatpak_run_add_extension_args (GPtrArray    *argv_array,
                                          GKeyFile     *metakey,
                                          const char   *full_ref,
                                          GCancellable *cancellable,
                                          GError      **error);
void     flatpak_run_add_environment_args (GPtrArray      *argv_array,
                                           GArray         *fd_array,
                                           char         ***envp_p,
                                           GPtrArray      *session_bus_proxy_argv,
                                           GPtrArray      *system_bus_proxy_argv,
                                           const char     *app_id,
                                           FlatpakContext *context,
                                           GFile          *app_id_dir);
char **  flatpak_run_get_minimal_env (gboolean devel);
char **  flatpak_run_apply_env_default (char **envp);
char **  flatpak_run_apply_env_appid (char **envp,
                                      GFile *app_dir);
char **  flatpak_run_apply_env_vars (char          **envp,
                                     FlatpakContext *context);

GFile *flatpak_get_data_dir (const char *app_id);
GFile *flatpak_ensure_data_dir (const char   *app_id,
                                GCancellable *cancellable,
                                GError      **error);

typedef enum {
  FLATPAK_RUN_FLAG_DEVEL              = (1 << 0),
  FLATPAK_RUN_FLAG_BACKGROUND         = (1 << 1),
  FLATPAK_RUN_FLAG_LOG_SESSION_BUS    = (1 << 2),
  FLATPAK_RUN_FLAG_LOG_SYSTEM_BUS     = (1 << 3),
  FLATPAK_RUN_FLAG_NO_SESSION_HELPER  = (1 << 4),
} FlatpakRunFlags;

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
