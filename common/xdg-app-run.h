/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#ifndef __XDG_APP_RUN_H__
#define __XDG_APP_RUN_H__

#include "libglnx/libglnx.h"
#include "dbus-proxy/xdg-app-proxy.h"
#include "xdg-app-common-types.h"

gboolean xdg_app_run_in_transient_unit (const char *app_id,
                                        GError **error);

#define XDG_APP_METADATA_GROUP_CONTEXT "Context"
#define XDG_APP_METADATA_GROUP_SESSION_BUS_POLICY "Session Bus Policy"
#define XDG_APP_METADATA_GROUP_SYSTEM_BUS_POLICY "System Bus Policy"
#define XDG_APP_METADATA_GROUP_ENVIRONMENT "Environment"
#define XDG_APP_METADATA_KEY_SHARED "shared"
#define XDG_APP_METADATA_KEY_SOCKETS "sockets"
#define XDG_APP_METADATA_KEY_FILESYSTEMS "filesystems"
#define XDG_APP_METADATA_KEY_PERSISTENT "persistent"
#define XDG_APP_METADATA_KEY_DEVICES "devices"

XdgAppContext *xdg_app_context_new                    (void);
void           xdg_app_context_free                   (XdgAppContext            *context);
void           xdg_app_context_merge                  (XdgAppContext            *context,
                                                       XdgAppContext            *other);
GOptionGroup  *xdg_app_context_get_options            (XdgAppContext            *context);
gboolean       xdg_app_context_load_metadata          (XdgAppContext            *context,
                                                       GKeyFile                 *metakey,
                                                       GError                  **error);
void           xdg_app_context_save_metadata          (XdgAppContext            *context,
                                                       GKeyFile                 *metakey);
void           xdg_app_context_allow_host_fs          (XdgAppContext            *context);
void           xdg_app_context_set_session_bus_policy (XdgAppContext            *context,
                                                       const char               *name,
                                                       XdgAppPolicy              policy);
void           xdg_app_context_set_system_bus_policy  (XdgAppContext            *context,
                                                       const char               *name,
                                                       XdgAppPolicy              policy);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdgAppContext, xdg_app_context_free)

gboolean  xdg_app_run_add_extension_args     (GPtrArray   *argv_array,
                                              GKeyFile    *metakey,
                                              const char  *full_ref,
                                              GCancellable *cancellable,
                                              GError     **error);
void     xdg_app_run_add_environment_args    (GPtrArray   *argv_array,
					      GPtrArray   *session_bus_proxy_argv,
					      GPtrArray   *system_bus_proxy_argv,
                                              const char  *app_id,
                                              XdgAppContext *context,
                                              GFile       *app_id_dir);
char **  xdg_app_run_get_minimal_env         (gboolean     devel);
char **  xdg_app_run_apply_env_default       (char       **envp);
char **  xdg_app_run_apply_env_appid         (char       **envp,
                                              GFile       *app_dir);
char **  xdg_app_run_apply_env_vars          (char       **envp,
                                              XdgAppContext *context);

GFile *xdg_app_get_data_dir (const char *app_id);
GFile *xdg_app_ensure_data_dir (const char *app_id,
				GCancellable  *cancellable,
				GError **error);

typedef enum {
  XDG_APP_RUN_FLAG_DEVEL           = (1<<0),
  XDG_APP_RUN_FLAG_BACKGROUND      = (1<<1),
  XDG_APP_RUN_FLAG_LOG_SESSION_BUS = (1<<2),
  XDG_APP_RUN_FLAG_LOG_SYSTEM_BUS  = (1<<3),
} XdgAppRunFlags;

gboolean xdg_app_run_app (const char *app_ref,
                          XdgAppDeploy *app_deploy,
                          XdgAppContext *extra_context,
                          const char *custom_runtime,
                          const char *custom_runtime_version,
                          XdgAppRunFlags flags,
                          const char *custom_command,
                          char *args[],
                          int n_args,
                          GCancellable *cancellable,
                          GError **error);


#endif /* __XDG_APP_RUN_H__ */
