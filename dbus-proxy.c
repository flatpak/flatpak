/*
 * Copyright © 2015 Red Hat, Inc
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

#include "xdg-app-proxy.h"

int
main (int argc, char *argv[])
{
  GMainLoop *service_loop;
  XdgAppProxy *proxy;
  GError *error = NULL;

  proxy = xdg_app_proxy_new (g_getenv ("DBUS_SESSION_BUS_ADDRESS"));

  xdg_app_proxy_set_log_messages (proxy, TRUE);
  xdg_app_proxy_set_filter (proxy, TRUE);
  xdg_app_proxy_add_policy (proxy, "ca.desrt.dconf", XDG_APP_POLICY_TALK);
  xdg_app_proxy_add_policy (proxy, "org.gnome.gedit", XDG_APP_POLICY_TALK);
  xdg_app_proxy_add_policy (proxy, "org.gnome.d-feet", XDG_APP_POLICY_OWN);
  xdg_app_proxy_add_policy (proxy, "org.gtk.vfs.Daemon", XDG_APP_POLICY_TALK);

  xdg_app_proxy_start (proxy, &error);
  g_assert_no_error (error);

  service_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (service_loop);

  g_main_loop_unref (service_loop);

  return 0;
}
