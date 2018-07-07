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

static GList *proxies;
static int sync_fd = -1;

static void
add_args (GBytes    *bytes,
          GPtrArray *args,
          int        pos)
{
  gsize data_len, remainder_len;
  const guchar *data = g_bytes_get_data (bytes, &data_len);
  guchar *s;
  const guchar *remainder;

  remainder = data;
  remainder_len = data_len;
  s = memchr (remainder, 0, remainder_len);
  while (s)
    {
      gsize len = s - remainder;
      char *arg = g_strndup ((char *) remainder, len);
      g_ptr_array_insert (args, pos++, arg);
      remainder = s + 1;
      remainder_len -= len + 1;
      s = memchr (remainder, 0, remainder_len);
    }

  if (remainder_len)
    {
      char *arg = g_strndup ((char *) remainder, remainder_len);
      g_ptr_array_insert (args, pos++, arg);
    }
}


static gboolean
parse_generic_args (GPtrArray *args, int *args_i)
{
  const char *arg = g_ptr_array_index (args, *args_i);

  if (g_str_has_prefix (arg, "--fd="))
    {
      const char *fd_s = arg + strlen ("--fd=");
      char *endptr;
      int fd;

      fd = strtol (fd_s, &endptr, 10);
      if (fd < 0 || endptr == fd_s || *endptr != 0)
        {
          g_printerr ("Invalid fd %s\n", fd_s);
          return FALSE;
        }
      sync_fd = fd;

      *args_i += 1;

      return TRUE;
    }
  else if (g_str_has_prefix (arg, "--args="))
    {
      const char *fd_s = arg + strlen ("--args=");
      char *endptr;
      int fd;
      g_autoptr(GBytes) data = NULL;
      g_autoptr(GError) error = NULL;

      fd = strtol (fd_s, &endptr, 10);
      if (fd < 0 || endptr == fd_s || *endptr != 0)
        {
          g_printerr ("Invalid --args fd %s\n", fd_s);
          return FALSE;
        }

      data = glnx_fd_readall_bytes (fd, NULL, &error);

      if (data == NULL)
        {
          g_printerr ("Failed to load --args: %s\n", error->message);
          return FALSE;
        }

      *args_i += 1;

      add_args (data, args, *args_i);

      return TRUE;
    }
  else
    {
      g_printerr ("Unknown argument %s\n", arg);
      return FALSE;
    }
}

static gboolean
start_proxy (GPtrArray *args, int *args_i)
{
  g_autoptr(FlatpakProxy) proxy = NULL;
  g_autoptr(GError) error = NULL;
  const char *bus_address, *socket_path;
  const char *arg;

  if (*args_i >= args->len || ((char *) g_ptr_array_index (args, *args_i))[0] == '-')
    {
      g_printerr ("No bus address given\n");
      return FALSE;
    }

  bus_address = g_ptr_array_index (args, *args_i);
  *args_i += 1;

  if (*args_i >= args->len || ((char *) g_ptr_array_index (args, *args_i))[0] == '-')
    {
      g_printerr ("No socket path given\n");
      return FALSE;
    }

  socket_path = g_ptr_array_index (args, *args_i);
  *args_i += 1;

  proxy = flatpak_proxy_new (bus_address, socket_path);

  while (*args_i < args->len)
    {
      arg = g_ptr_array_index (args, *args_i);

      if (arg[0] != '-')
        break;

      if (g_str_has_prefix (arg, "--see=") ||
          g_str_has_prefix (arg, "--talk=") ||
          g_str_has_prefix (arg, "--own="))
        {
          FlatpakPolicy policy = FLATPAK_POLICY_SEE;
          g_autofree char *name = g_strdup (strchr (arg, '=') + 1);
          gboolean wildcard = FALSE;

          if (arg[2] == 't')
            policy = FLATPAK_POLICY_TALK;
          else if (arg[2] == 'o')
            policy = FLATPAK_POLICY_OWN;

          if (g_str_has_suffix (name, ".*"))
            {
              name[strlen (name) - 2] = 0;
              wildcard = TRUE;
            }

          if (name[0] == ':' || !g_dbus_is_name (name))
            {
              g_printerr ("'%s' is not a valid dbus name\n", name);
              return FALSE;
            }

          flatpak_proxy_add_policy (proxy, name, wildcard, policy);

          *args_i += 1;
        }
      else if (g_str_has_prefix (arg, "--call=") ||
               g_str_has_prefix (arg, "--broadcast="))
        {
          g_autofree char *rest = g_strdup (strchr (arg, '=') + 1);
          char *name = rest;
          char *rule;
          char *name_end = strchr (rest, '=');
          gboolean wildcard = FALSE;

          if (name_end == NULL)
            {
              g_printerr ("'%s' is not a valid name + rule\n", rest);
              return FALSE;
            }

          *name_end = 0;
          rule = name_end + 1;

          if (g_str_has_suffix (name, ".*"))
            {
              name[strlen (name) - 2] = 0;
              wildcard = TRUE;
            }

          if (g_str_has_prefix (arg, "--call="))
            flatpak_proxy_add_call_rule (proxy, name, wildcard, rule);
          else
            flatpak_proxy_add_broadcast_rule (proxy, name, wildcard, rule);

          *args_i += 1;
        }
      else if (g_str_equal (arg, "--log"))
        {
          flatpak_proxy_set_log_messages (proxy, TRUE);
          *args_i += 1;
        }
      else if (g_str_equal (arg, "--filter"))
        {
          flatpak_proxy_set_filter (proxy, TRUE);
          *args_i += 1;
        }
      else if (g_str_equal (arg, "--sloppy-names"))
        {
          /* This means we're reporing the name changes for all unique names,
             which is needed for the a11y bus */
          flatpak_proxy_set_sloppy_names (proxy, TRUE);
          *args_i += 1;
        }
      else
        {
          if (!parse_generic_args (args, args_i))
            return FALSE;
        }
    }

  if (!flatpak_proxy_start (proxy, &error))
    {
      g_printerr ("Failed to start proxy for %s: %s\n", bus_address, error->message);
      return FALSE;
    }

  proxies = g_list_prepend (proxies, g_object_ref (proxy));

  return TRUE;
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
  int i, args_i;

  g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func (g_free);

  for (i = 1; i < argc; i++)
    g_ptr_array_add (args, g_strdup ((char *) argv[i]));

  args_i = 0;
  while (args_i < args->len)
    {
      const char *arg = g_ptr_array_index (args, args_i);
      if (arg[0] == '-')
        {
          if (!parse_generic_args (args, &args_i))
            return 1;
        }
      else
        {
          if (!start_proxy (args, &args_i))
            return 1;
        }
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

  return 0;
}
