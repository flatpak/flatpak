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
#include "flatpak-run-sockets-private.h"

/* Setup for simple sockets that only need one function goes in this file.
 * Setup for more complicated sockets should go in its own file. */

#include "flatpak-run-cups-private.h"
#include "flatpak-run-pulseaudio-private.h"
#include "flatpak-run-wayland-private.h"
#include "flatpak-run-x11-private.h"
#include "flatpak-utils-private.h"

static void
flatpak_run_add_gssproxy_args (FlatpakBwrap *bwrap)
{
  /* We only expose the gssproxy user service. The gssproxy system service is
   * not intended to be exposed to sandboxed environments.
   */
  g_autofree char *gssproxy_host_dir = g_build_filename (g_get_user_runtime_dir (), "gssproxy", NULL);
  const char *gssproxy_sandboxed_dir = "/run/flatpak/gssproxy/";

  if (g_file_test (gssproxy_host_dir, G_FILE_TEST_EXISTS))
    flatpak_bwrap_add_args (bwrap, "--ro-bind", gssproxy_host_dir, gssproxy_sandboxed_dir, NULL);
}

static void
flatpak_run_add_resolved_args (FlatpakBwrap *bwrap)
{
  const char *resolved_socket = "/run/systemd/resolve/io.systemd.Resolve";

  if (g_file_test (resolved_socket, G_FILE_TEST_EXISTS))
    flatpak_bwrap_add_args (bwrap, "--bind", resolved_socket, resolved_socket, NULL);
}

static void
flatpak_run_add_journal_args (FlatpakBwrap *bwrap)
{
  g_autofree char *journal_socket_socket = g_strdup ("/run/systemd/journal/socket");
  g_autofree char *journal_stdout_socket = g_strdup ("/run/systemd/journal/stdout");

  if (g_file_test (journal_socket_socket, G_FILE_TEST_EXISTS))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", journal_socket_socket, journal_socket_socket,
                              NULL);
    }
  if (g_file_test (journal_stdout_socket, G_FILE_TEST_EXISTS))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", journal_stdout_socket, journal_stdout_socket,
                              NULL);
    }
}

static void
flatpak_run_add_pcsc_args (FlatpakBwrap *bwrap)
{
  const char * pcsc_socket;
  const char * sandbox_pcsc_socket = "/run/pcscd/pcscd.comm";

  pcsc_socket = g_getenv ("PCSCLITE_CSOCK_NAME");
  if (pcsc_socket)
    {
      if (!g_file_test (pcsc_socket, G_FILE_TEST_EXISTS))
        {
          flatpak_bwrap_unset_env (bwrap, "PCSCLITE_CSOCK_NAME");
          return;
        }
    }
  else
    {
      pcsc_socket = "/run/pcscd/pcscd.comm";
      if (!g_file_test (pcsc_socket, G_FILE_TEST_EXISTS))
        return;
    }

  flatpak_bwrap_add_args (bwrap,
                          "--ro-bind", pcsc_socket, sandbox_pcsc_socket,
                          NULL);
  flatpak_bwrap_set_env (bwrap, "PCSCLITE_CSOCK_NAME", sandbox_pcsc_socket, TRUE);
}

static void
flatpak_run_add_gpg_agent_args (FlatpakBwrap *bwrap)
{
  const char * agent_socket;
  g_autofree char * sandbox_agent_socket = NULL;
  g_autoptr(GError) gpgconf_error = NULL;
  g_autoptr(GSubprocess) process = NULL;
  GInputStream *base_stream = NULL;
  g_autoptr(GDataInputStream) data_stream = NULL;

  process = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE,
                    &gpgconf_error,
                    "gpgconf", "--list-dir", "agent-socket", NULL);

  if (gpgconf_error)
    {
      g_info ("GPG-Agent directories: %s", gpgconf_error->message);
      return;
    }

  base_stream = g_subprocess_get_stdout_pipe (process);
  data_stream = g_data_input_stream_new (base_stream);

  agent_socket = g_data_input_stream_read_line (data_stream,
                                                NULL, NULL,
                                                &gpgconf_error);

  if (!agent_socket || gpgconf_error)
    {
      g_info ("GPG-Agent directories: %s", gpgconf_error->message);
      return;
    }

  sandbox_agent_socket = g_strdup_printf ("/run/user/%d/gnupg/S.gpg-agent", getuid ());

  flatpak_bwrap_add_args (bwrap,
                          "--ro-bind-try", agent_socket, sandbox_agent_socket,
                          NULL);
}

static void
flatpak_run_add_ssh_args (FlatpakBwrap *bwrap)
{
  static const char sandbox_auth_socket[] = "/run/flatpak/ssh-auth";
  const char * auth_socket;

  auth_socket = g_getenv ("SSH_AUTH_SOCK");

  if (!auth_socket)
    return; /* ssh agent not present */

  if (!g_file_test (auth_socket, G_FILE_TEST_EXISTS))
    {
      /* Let's clean it up, so that the application will not try to connect */
      flatpak_bwrap_unset_env (bwrap, "SSH_AUTH_SOCK");
      return;
    }

  flatpak_bwrap_add_args (bwrap,
                          "--ro-bind", auth_socket, sandbox_auth_socket,
                          NULL);
  flatpak_bwrap_set_env (bwrap, "SSH_AUTH_SOCK", sandbox_auth_socket, TRUE);
}

/*
 * Expose sockets that are available for `flatpak build`, apply_extra, and
 * `flatpak run`, except for D-Bus which is handled separately due to its
 * use of a proxy.
 */
void
flatpak_run_add_socket_args_environment (FlatpakBwrap         *bwrap,
                                         FlatpakContextShares  shares,
                                         FlatpakContextSockets sockets,
                                         const char           *app_id,
                                         const char           *instance_id)
{
  if (sockets & FLATPAK_CONTEXT_SOCKET_WAYLAND)
    {
      gboolean inherit_wayland_socket;

      g_info ("Allowing wayland access");
      g_assert (app_id && instance_id);

      inherit_wayland_socket =
        (sockets & FLATPAK_CONTEXT_SOCKET_INHERIT_WAYLAND_SOCKET) != 0;

      flatpak_run_add_wayland_args (bwrap, app_id, instance_id,
                                    inherit_wayland_socket);
    }

  flatpak_run_add_x11_args (bwrap,
                            !!(sockets & FLATPAK_CONTEXT_SOCKET_X11),
                            shares);

  if (sockets & FLATPAK_CONTEXT_SOCKET_SSH_AUTH)
    {
      flatpak_run_add_ssh_args (bwrap);
    }

  if (sockets & FLATPAK_CONTEXT_SOCKET_PULSEAUDIO)
    {
      g_info ("Allowing pulseaudio access");
      flatpak_run_add_pulseaudio_args (bwrap, shares);
    }

  if (sockets & FLATPAK_CONTEXT_SOCKET_PCSC)
    {
      flatpak_run_add_pcsc_args (bwrap);
    }

  if (sockets & FLATPAK_CONTEXT_SOCKET_CUPS)
    {
      flatpak_run_add_cups_args (bwrap);
    }

  if (sockets & FLATPAK_CONTEXT_SOCKET_GPG_AGENT)
    {
      flatpak_run_add_gpg_agent_args (bwrap);
    }
}

/*
 * Expose sockets that are available for `flatpak run` only.
 */
void
flatpak_run_add_socket_args_late (FlatpakBwrap *bwrap,
                                  FlatpakContextShares shares)
{
  if ((shares & FLATPAK_CONTEXT_SHARED_NETWORK) != 0)
    {
      flatpak_run_add_gssproxy_args (bwrap);
      flatpak_run_add_resolved_args (bwrap);
    }

  flatpak_run_add_journal_args (bwrap);
}
