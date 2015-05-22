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

void xdg_app_run_in_transient_unit (const char *app_id);

typedef struct XdgAppContext XdgAppContext;

#define XDG_APP_METADATA_GROUP_CONTEXT "Context"
#define XDG_APP_METADATA_GROUP_SESSION_BUS_POLICY "Session Bus Policy"
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

G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdgAppContext, xdg_app_context_free)

gboolean xdg_app_run_verify_environment_keys (const char **keys,
					      GError     **error);
void     xdg_app_run_add_environment_args    (GPtrArray   *argv_array,
					      GPtrArray   *dbus_proxy_argv,
                                              const char  *app_id,
					      GKeyFile    *runtime_metakey,
					      GKeyFile    *metakey,
					      const char **allow,
					      const char **forbid);
char **  xdg_app_run_get_minimal_env         (gboolean     devel);
char **  xdg_app_run_apply_env_default       (char       **envp);
char **  xdg_app_run_apply_env_appid         (char       **envp,
                                              GFile       *app_dir);
char **  xdg_app_run_apply_env_vars          (char       **envp,
                                              GKeyFile    *metakey);

GFile *xdg_app_get_data_dir (const char *app_id);
GFile *xdg_app_ensure_data_dir (const char *app_id,
				GCancellable  *cancellable,
				GError **error);

#endif /* __XDG_APP_RUN_H__ */
