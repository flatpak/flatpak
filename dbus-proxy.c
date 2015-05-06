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

#include "config.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "libglnx/libglnx.h"

#include "xdg-app-proxy.h"

GList *proxies;

int
start_proxy (int n_args, const char *args[])
{
  g_autoptr(XdgAppProxy) proxy = NULL;
  g_autoptr (GError) error = NULL;
  const char *bus_address, *socket_path;
  int n;

  n = 0;
  if (n_args < n+1 || args[n][0] == '-')
    {
      g_printerr ("No bus address given\n");
      return -1;
    }
  bus_address = args[n++];

  if (n_args < n+1 || args[n][0] == '-')
    {
      g_printerr ("No socket path given\n");
      return -1;
    }
  socket_path = args[n++];

  proxy = xdg_app_proxy_new (bus_address, socket_path);

  while (n < n_args)
    {
      if (args[n][0] != '-')
        break;

      if (g_str_has_prefix (args[n], "--see=") ||
          g_str_has_prefix (args[n], "--talk=") ||
          g_str_has_prefix (args[n], "--own="))
        {
          XdgAppPolicy policy = XDG_APP_POLICY_SEE;
          g_autofree char *name = NULL;
          gboolean wildcard = FALSE;

          if (args[n][2] == 't')
            policy = XDG_APP_POLICY_TALK;
          else if (args[n][2] == 'o')
            policy = XDG_APP_POLICY_OWN;

          name = g_strdup (strchr (args[n], '=') + 1);
          if (g_str_has_suffix (name, ".*"))
            {
              name[strlen (name) - 2] = 0;
              wildcard = TRUE;
            }

          if (name[0] == ':' || !g_dbus_is_name (name))
            {
              g_printerr ("'%s' is not a valid dbus name\n", name);
              return -1;
            }

          if (wildcard)
            xdg_app_proxy_add_wildcarded_policy (proxy, name, policy);
          else
            xdg_app_proxy_add_policy (proxy, name, policy);
        }
      else if (g_str_equal (args[n], "--log"))
        {
          xdg_app_proxy_set_log_messages (proxy, TRUE);
        }
      else if (g_str_equal (args[n], "--filter"))
        {
          xdg_app_proxy_set_filter (proxy, TRUE);
        }
      else
        {
          g_printerr ("Unknown argument %s\n", args[n]);
          return -1;
        }

      n++;
    }

  if (!xdg_app_proxy_start (proxy, &error))
    {
      g_printerr ("Failed to start proxy for %s: %s\n", bus_address, error->message);
      return -1;
    }

  proxies = g_list_prepend (proxies, g_object_ref (proxy));

  return n;
}


int
main (int argc, const char *argv[])
{
  GMainLoop *service_loop;
  int n_args, res;
  const char **args;

  n_args = argc - 1;
  args = &argv[1];

  while (n_args > 0)
    {
      res = start_proxy (n_args, args);
      if (res == -1)
        return 1;

      n_args -= res;
      args += n_args;
    }

  if (proxies == NULL)
    {
      g_printerr ("No proxies specied\n");
      return 1;
    }

  service_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (service_loop);

  g_main_loop_unref (service_loop);

  return 0;
}
