/*
 * Copyright © 2015 Red Hat, Inc
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

#include "config.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include "libglnx/libglnx.h"

#include "flatpak-proxy.h"
#include "flatpak-utils.h"

static GList *proxies;
static int sync_fd = -1;
static gchar *app_id = NULL;

static int
parse_generic_args (int n_args, const char *args[])
{
  if (g_str_has_prefix (args[0], "--fd="))
    {
      const char *fd_s = args[0] + strlen ("--fd=");
      char *endptr;
      int fd;

      fd = strtol (fd_s, &endptr, 10);
      if (fd < 0 || endptr == fd_s || *endptr != 0)
        {
          g_printerr ("Invalid fd %s\n", fd_s);
          return -1;
        }
      sync_fd = fd;

      return 1;
    }
  else if (g_str_has_prefix (args[0], "--app-id="))
    {
      g_autoptr(GError) error = NULL;

      g_free (app_id);
      app_id = g_strdup (args[0] + strlen ("--app-id="));

      if (!flatpak_is_valid_name (app_id, &error))
        {
          g_printerr ("Invalid app ID %s: %s\n", app_id, error->message);
          return -1;
        }

      return 1;
    }
  else
    {
      g_printerr ("Unknown argument %s\n", args[0]);
      return -1;
    }
}

static int
start_proxy (int n_args, const char *args[])
{
  g_autoptr(FlatpakProxy) proxy = NULL;
  g_autoptr(GError) error = NULL;
  const char *bus_address, *socket_path;
  int n;

  n = 0;
  if (n_args < n + 1 || args[n][0] == '-')
    {
      g_printerr ("No bus address given\n");
      return -1;
    }
  bus_address = args[n++];

  if (n_args < n + 1 || args[n][0] == '-')
    {
      g_printerr ("No socket path given\n");
      return -1;
    }
  socket_path = args[n++];

  proxy = flatpak_proxy_new (bus_address, socket_path, app_id);

  while (n < n_args)
    {
      if (args[n][0] != '-')
        break;

      if (g_str_has_prefix (args[n], "--see=") ||
          g_str_has_prefix (args[n], "--talk=") ||
          g_str_has_prefix (args[n], "--filter=") ||
          g_str_has_prefix (args[n], "--own="))
        {
          FlatpakPolicy policy = FLATPAK_POLICY_SEE;
          g_autofree char *name = NULL;
          gboolean wildcard = FALSE;

          if (args[n][2] == 't')
            policy = FLATPAK_POLICY_TALK;
          else if (args[n][2] == 'f')
            policy = FLATPAK_POLICY_FILTERED;
          else if (args[n][2] == 'o')
            policy = FLATPAK_POLICY_OWN;

          name = g_strdup (strchr (args[n], '=') + 1);

          if (policy == FLATPAK_POLICY_FILTERED)
            {
              char *rule = strchr (name, '=');
              if (rule != NULL)
                {
                  *rule++ = 0;
                  flatpak_proxy_add_filter (proxy, name, rule);
                }
            }
          else
            {
              if (g_str_has_suffix (name, ".*"))
                {
                  name[strlen (name) - 2] = 0;
                  wildcard = TRUE;
                }
            }

          if (name[0] == ':' || !g_dbus_is_name (name))
            {
              g_printerr ("'%s' is not a valid dbus name\n", name);
              return -1;
            }

          if (wildcard)
            flatpak_proxy_add_wildcarded_policy (proxy, name, policy);
          else
            flatpak_proxy_add_policy (proxy, name, policy);
        }
      else if (g_str_equal (args[n], "--log"))
        {
          flatpak_proxy_set_log_messages (proxy, TRUE);
        }
      else if (g_str_equal (args[n], "--filter"))
        {
          flatpak_proxy_set_filter (proxy, TRUE);
        }
      else if (g_str_equal (args[n], "--sloppy-names"))
        {
          /* This means we're reporing the name changes for all unique names,
             which is needed for the a11y bus */
          flatpak_proxy_set_sloppy_names (proxy, TRUE);
        }
      else
        {
          int res = parse_generic_args (n_args - n, &args[n]);
          if (res == -1)
            return -1;

          n += res - 1; /* res - 1, because we ++ below */
        }

      n++;
    }

  if (!flatpak_proxy_start (proxy, &error))
    {
      g_printerr ("Failed to start proxy for %s: %s\n", bus_address, error->message);
      return -1;
    }

  proxies = g_list_prepend (proxies, g_object_ref (proxy));

  return n;
}

static gboolean
sync_closed_cb (GIOChannel  *source,
                GIOCondition condition,
                gpointer     data)
{
  GList *l;

  for (l = proxies; l != NULL; l = l->next)
    flatpak_proxy_stop (FLATPAK_PROXY (l->data));

  exit (0);
  return TRUE;
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
      if (args[0][0] == '-')
        {
          res = parse_generic_args (n_args, &args[0]);
          if (res == -1)
            return 1;
        }
      else
        {
          res = start_proxy (n_args, args);
          if (res == -1)
            return 1;
        }

      g_assert (res > 0);
      n_args -= res;
      args += res;
    }

  if (proxies == NULL)
    {
      g_printerr ("No proxies specified\n");
      return 1;
    }

  if (sync_fd >= 0)
    {
      ssize_t written;
      GIOChannel *sync_channel;
      written = write (sync_fd, "x", 1);
      if (written != 1)
        g_warning ("Can't write to sync socket");

      sync_channel = g_io_channel_unix_new (sync_fd);
      g_io_add_watch (sync_channel, G_IO_ERR | G_IO_HUP,
                      sync_closed_cb, NULL);
    }

  service_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (service_loop);

  g_main_loop_unref (service_loop);

  g_free (app_id);
  return 0;
}
