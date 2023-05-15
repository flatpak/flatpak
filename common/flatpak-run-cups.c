/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2014-2019 Red Hat, Inc
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
#include "flatpak-run-cups-private.h"

#include "flatpak-utils-private.h"

static gboolean
flatpak_run_cups_check_server_is_socket (const char *server)
{
  if (g_str_has_prefix (server, "/") && strstr (server, ":") == NULL)
    return TRUE;

  return FALSE;
}

/* Try to find a default server from a cups confguration file */
static char *
flatpak_run_get_cups_server_name_config (const char *path)
{
  g_autoptr(GFile) file = g_file_new_for_path (path);
  g_autoptr(GError) my_error = NULL;
  g_autoptr(GFileInputStream) input_stream = NULL;
  g_autoptr(GDataInputStream) data_stream = NULL;
  size_t len;

  input_stream = g_file_read (file, NULL, &my_error);
  if (my_error)
    {
      g_info ("CUPS configuration file '%s': %s", path, my_error->message);
      return NULL;
    }

  data_stream = g_data_input_stream_new (G_INPUT_STREAM (input_stream));

  while (TRUE)
    {
      g_autofree char *line = g_data_input_stream_read_line (data_stream, &len, NULL, NULL);
      if (line == NULL)
        break;

      g_strchug (line);

      if ((*line  == '\0') || (*line == '#'))
        continue;

      g_auto(GStrv) tokens = g_strsplit (line, " ", 2);

      if ((tokens[0] != NULL) && (tokens[1] != NULL))
        {
          if (strcmp ("ServerName", tokens[0]) == 0)
            {
              g_strchug (tokens[1]);

              if (flatpak_run_cups_check_server_is_socket (tokens[1]))
                return g_strdup (tokens[1]);
            }
        }
    }

    return NULL;
}

static char *
flatpak_run_get_cups_server_name (void)
{
  g_autofree char * cups_server = NULL;
  g_autofree char * cups_config_path = NULL;

  /* TODO
   * we don't currently support cups servers located on the network, if such
   * server is detected, we simply ignore it and in the worst case we fallback
   * to the default socket
   */
  cups_server = g_strdup (g_getenv ("CUPS_SERVER"));
  if (cups_server && flatpak_run_cups_check_server_is_socket (cups_server))
    return g_steal_pointer (&cups_server);
  g_clear_pointer (&cups_server, g_free);

  cups_config_path = g_build_filename (g_get_home_dir (), ".cups/client.conf", NULL);
  cups_server = flatpak_run_get_cups_server_name_config (cups_config_path);
  if (cups_server && flatpak_run_cups_check_server_is_socket (cups_server))
    return g_steal_pointer (&cups_server);
  g_clear_pointer (&cups_server, g_free);

  cups_server = flatpak_run_get_cups_server_name_config ("/etc/cups/client.conf");
  if (cups_server && flatpak_run_cups_check_server_is_socket (cups_server))
    return g_steal_pointer (&cups_server);

  // Fallback to default socket
  return g_strdup ("/var/run/cups/cups.sock");
}

void
flatpak_run_add_cups_args (FlatpakBwrap *bwrap)
{
  g_autofree char * sandbox_server_name = g_strdup ("/var/run/cups/cups.sock");
  g_autofree char * cups_server_name = flatpak_run_get_cups_server_name ();

  if (!g_file_test (cups_server_name, G_FILE_TEST_EXISTS))
    {
      g_info ("Could not find CUPS server");
      return;
    }

  flatpak_bwrap_add_args (bwrap,
                          "--ro-bind", cups_server_name, sandbox_server_name,
                          NULL);
}
