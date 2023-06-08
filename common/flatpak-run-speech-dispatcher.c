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

#include "flatpak-run-speech-dispatcher-private.h"

static gchar *
flatpak_run_default_speechd_socket_path (void)
{
  return g_strdup_printf ("%s/speech-dispatcher/speechd.sock", g_get_user_runtime_dir ());
}

static char *
flatpak_run_get_socket_path_from_speechd_address (const char * speechd_address)
{
  char *method_separator = strstr (speechd_address, ":");
  if (method_separator == NULL || strlen (method_separator + 1) == 0)
    return flatpak_run_default_speechd_socket_path ();
  else
    return g_strdup (method_separator + 1);
}

static char *
flatpak_run_get_host_speechd_socket_path (void)
{
  const char * speechd_address = g_getenv ("SPEECHD_ADDRESS");
  if (speechd_address && g_str_has_prefix (speechd_address, "unix_socket"))
    return flatpak_run_get_socket_path_from_speechd_address (speechd_address);
  else
    return flatpak_run_default_speechd_socket_path ();
}

void
flatpak_run_add_speech_dispatcher_args (FlatpakBwrap *bwrap)
{
  /*
  * TODO: We only support unix sockets for communication with
  * speech dispatcher. Supporting inet sockets would require network
  * access for the sandbox though, so they're left out for now.
  */
  g_autofree char * host_speechd_socket = NULL;
  g_autofree char * sandbox_speechd_socket = NULL;

  host_speechd_socket = flatpak_run_get_host_speechd_socket_path ();
  sandbox_speechd_socket = g_strdup_printf ("%s/speech-dispatcher/speechd.sock", g_get_user_runtime_dir ());

  flatpak_bwrap_add_args (bwrap,
                          "--ro-bind-try", host_speechd_socket, sandbox_speechd_socket,
                          NULL);
}
