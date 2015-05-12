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

void xdg_app_run_in_transient_unit (const char *app_id);

gboolean xdg_app_run_verify_environment_keys (const char **keys,
					      GError     **error);
void     xdg_app_run_add_environment_args    (GPtrArray   *argv_array,
					      GPtrArray   *dbus_proxy_argv,
					      GKeyFile    *metakey,
					      const char **allow,
					      const char **forbid);
char **  xdg_app_run_get_minimal_env         (gboolean     devel);

void xdg_app_run_add_x11_args          (GPtrArray *argv_array);
void xdg_app_run_add_no_x11_args       (GPtrArray *argv_array);
void xdg_app_run_add_wayland_args      (GPtrArray *argv_array);
void xdg_app_run_add_pulseaudio_args   (GPtrArray *argv_array);
void xdg_app_run_add_system_dbus_args  (GPtrArray *argv_array,
					GPtrArray *dbus_proxy_argv);
void xdg_app_run_add_session_dbus_args (GPtrArray *argv_array,
					GPtrArray *dbus_proxy_argv);

GFile *xdg_app_get_data_dir (const char *app_id);
GFile *xdg_app_ensure_data_dir (const char *app_id,
				GCancellable  *cancellable,
				GError **error);

#endif /* __XDG_APP_RUN_H__ */
