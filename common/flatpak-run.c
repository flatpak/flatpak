/*
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

#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <gio/gdesktopappinfo.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <sys/personality.h>
#include <grp.h>
#include <unistd.h>
#include <gio/gunixfdlist.h>
#ifdef HAVE_DCONF
#include <dconf/dconf.h>
#endif
#ifdef HAVE_LIBMALCONTENT
#include <libmalcontent/malcontent.h>
#endif

#include "flatpak-syscalls-private.h"

#ifdef ENABLE_SECCOMP
#include <seccomp.h>
#endif

#ifdef ENABLE_XAUTH
#include <X11/Xauth.h>
#endif

#include <glib/gi18n-lib.h>

#include <gio/gio.h>
#include "libglnx/libglnx.h"

#include "flatpak-run-private.h"
#include "flatpak-proxy.h"
#include "flatpak-utils-base-private.h"
#include "flatpak-dir-private.h"
#include "flatpak-instance-private.h"
#include "flatpak-systemd-dbus-generated.h"
#include "flatpak-document-dbus-generated.h"
#include "flatpak-error.h"

#define DEFAULT_SHELL "/bin/sh"

const char * const abs_usrmerged_dirs[] =
{
  "/bin",
  "/lib",
  "/lib32",
  "/lib64",
  "/sbin",
  NULL
};
const char * const *flatpak_abs_usrmerged_dirs = abs_usrmerged_dirs;

static char *
extract_unix_path_from_dbus_address (const char *address)
{
  const char *path, *path_end;

  if (address == NULL)
    return NULL;

  if (!g_str_has_prefix (address, "unix:"))
    return NULL;

  path = strstr (address, "path=");
  if (path == NULL)
    return NULL;
  path += strlen ("path=");
  path_end = path;
  while (*path_end != 0 && *path_end != ',')
    path_end++;

  return g_strndup (path, path_end - path);
}

#ifdef ENABLE_XAUTH
static gboolean
auth_streq (char *str,
            char *au_str,
            int   au_len)
{
  return au_len == strlen (str) && memcmp (str, au_str, au_len) == 0;
}

static gboolean
xauth_entry_should_propagate (Xauth *xa,
                              char  *hostname,
                              char  *number)
{
  /* ensure entry isn't for remote access */
  if (xa->family != FamilyLocal && xa->family != FamilyWild)
    return FALSE;

  /* ensure entry is for this machine */
  if (xa->family == FamilyLocal && !auth_streq (hostname, xa->address, xa->address_length))
    return FALSE;

  /* ensure entry is for this session */
  if (xa->number != NULL && !auth_streq (number, xa->number, xa->number_length))
    return FALSE;

  return TRUE;
}

static void
write_xauth (char *number, FILE *output)
{
  Xauth *xa, local_xa;
  char *filename;
  FILE *f;
  struct utsname unames;

  if (uname (&unames))
    {
      g_warning ("uname failed");
      return;
    }

  filename = XauFileName ();
  f = fopen (filename, "rb");
  if (f == NULL)
    return;

  while (TRUE)
    {
      xa = XauReadAuth (f);
      if (xa == NULL)
        break;
      if (xauth_entry_should_propagate (xa, unames.nodename, number))
        {
          local_xa = *xa;
          if (local_xa.number)
            {
              local_xa.number = "99";
              local_xa.number_length = 2;
            }

          if (!XauWriteAuth (output, &local_xa))
            g_warning ("xauth write error");
        }

      XauDisposeAuth (xa);
    }

  fclose (f);
}
#endif /* ENABLE_XAUTH */

static void
flatpak_run_add_x11_args (FlatpakBwrap *bwrap,
                          gboolean      allowed)
{
  g_autofree char *x11_socket = NULL;
  const char *display;

  /* Always cover /tmp/.X11-unix, that way we never see the host one in case
   * we have access to the host /tmp. If you request X access we'll put the right
   * thing in this anyway.
   */
  flatpak_bwrap_add_args (bwrap,
                          "--tmpfs", "/tmp/.X11-unix",
                          NULL);

  if (!allowed)
    {
      flatpak_bwrap_unset_env (bwrap, "DISPLAY");
      return;
    }

  g_debug ("Allowing x11 access");

  display = g_getenv ("DISPLAY");
  if (display && display[0] == ':' && g_ascii_isdigit (display[1]))
    {
      const char *display_nr = &display[1];
      const char *display_nr_end = display_nr;
      g_autofree char *d = NULL;

      while (g_ascii_isdigit (*display_nr_end))
        display_nr_end++;

      d = g_strndup (display_nr, display_nr_end - display_nr);
      x11_socket = g_strdup_printf ("/tmp/.X11-unix/X%s", d);

      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", x11_socket, "/tmp/.X11-unix/X99",
                              NULL);
      flatpak_bwrap_set_env (bwrap, "DISPLAY", ":99.0", TRUE);

#ifdef ENABLE_XAUTH
      g_auto(GLnxTmpfile) xauth_tmpf  = { 0, };

      if (glnx_open_anonymous_tmpfile_full (O_RDWR | O_CLOEXEC, "/tmp", &xauth_tmpf, NULL))
        {
          FILE *output = fdopen (xauth_tmpf.fd, "wb");
          if (output != NULL)
            {
              /* fd is now owned by output, steal it from the tmpfile */
              int tmp_fd = dup (glnx_steal_fd (&xauth_tmpf.fd));
              if (tmp_fd != -1)
                {
                  g_autofree char *dest = g_strdup_printf ("/run/user/%d/Xauthority", getuid ());

                  write_xauth (d, output);
                  flatpak_bwrap_add_args_data_fd (bwrap, "--ro-bind-data", tmp_fd, dest);

                  flatpak_bwrap_set_env (bwrap, "XAUTHORITY", dest, TRUE);
                }

              fclose (output);

              if (tmp_fd != -1)
                lseek (tmp_fd, 0, SEEK_SET);
            }
        }
#endif
    }
  else
    {
      flatpak_bwrap_unset_env (bwrap, "DISPLAY");
    }
}

static gboolean
flatpak_run_add_wayland_args (FlatpakBwrap *bwrap)
{
  const char *wayland_display;
  g_autofree char *user_runtime_dir = flatpak_get_real_xdg_runtime_dir ();
  g_autofree char *wayland_socket = NULL;
  g_autofree char *sandbox_wayland_socket = NULL;
  gboolean res = FALSE;
  struct stat statbuf;

  wayland_display = g_getenv ("WAYLAND_DISPLAY");
  if (!wayland_display)
    wayland_display = "wayland-0";

  wayland_socket = g_build_filename (user_runtime_dir, wayland_display, NULL);
  sandbox_wayland_socket = g_strdup_printf ("/run/user/%d/%s", getuid (), wayland_display);

  if (stat (wayland_socket, &statbuf) == 0 &&
      (statbuf.st_mode & S_IFMT) == S_IFSOCK)
    {
      res = TRUE;
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", wayland_socket, sandbox_wayland_socket,
                              NULL);
    }
  return res;
}

static void
flatpak_run_add_ssh_args (FlatpakBwrap *bwrap)
{
  const char * auth_socket;
  g_autofree char * sandbox_auth_socket = NULL;

  auth_socket = g_getenv ("SSH_AUTH_SOCK");

  if (!auth_socket)
    return; /* ssh agent not present */

  if (!g_file_test (auth_socket, G_FILE_TEST_EXISTS))
    {
      /* Let's clean it up, so that the application will not try to connect */
      flatpak_bwrap_unset_env (bwrap, "SSH_AUTH_SOCK");
      return;
    }

  sandbox_auth_socket = g_strdup_printf ("/run/user/%d/ssh-auth", getuid ());

  flatpak_bwrap_add_args (bwrap,
                          "--ro-bind", auth_socket, sandbox_auth_socket,
                          NULL);
  flatpak_bwrap_set_env (bwrap, "SSH_AUTH_SOCK", sandbox_auth_socket, TRUE);
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
      g_debug ("CUPS configuration file '%s': %s", path, my_error->message);
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

static void
flatpak_run_add_cups_args (FlatpakBwrap *bwrap)
{
  g_autofree char * sandbox_server_name = g_strdup ("/var/run/cups/cups.sock");
  g_autofree char * cups_server_name = flatpak_run_get_cups_server_name ();

  if (!g_file_test (cups_server_name, G_FILE_TEST_EXISTS))
    {
      g_debug ("Could not find CUPS server");
      return;
    }

  flatpak_bwrap_add_args (bwrap,
                          "--ro-bind", cups_server_name, sandbox_server_name,
                          NULL);
}

/* Try to find a default server from a pulseaudio confguration file */
static char *
flatpak_run_get_pulseaudio_server_user_config (const char *path)
{
  g_autoptr(GFile) file = g_file_new_for_path (path);
  g_autoptr(GError) my_error = NULL;
  g_autoptr(GFileInputStream) input_stream = NULL;
  g_autoptr(GDataInputStream) data_stream = NULL;
  size_t len;

  input_stream = g_file_read (file, NULL, &my_error);
  if (my_error)
    {
      g_debug ("Pulseaudio user configuration file '%s': %s", path, my_error->message);
      return NULL;
    }

  data_stream = g_data_input_stream_new (G_INPUT_STREAM (input_stream));

  while (TRUE)
    {
      g_autofree char *line = g_data_input_stream_read_line (data_stream, &len, NULL, NULL);
      if (line == NULL)
        break;

      g_strchug (line);

      if ((*line  == '\0') || (*line == ';') || (*line == '#'))
        continue;

      if (g_str_has_prefix (line, ".include "))
        {
          g_autofree char *rec_path = g_strdup (line + 9);
          g_strstrip (rec_path);
          char *found = flatpak_run_get_pulseaudio_server_user_config (rec_path);
          if (found)
            return found;
        }
      else if (g_str_has_prefix (line, "["))
        {
          return NULL;
        }
      else
        {
          g_auto(GStrv) tokens = g_strsplit (line, "=", 2);

          if ((tokens[0] != NULL) && (tokens[1] != NULL))
            {
              g_strchomp (tokens[0]);
              if (strcmp ("default-server", tokens[0]) == 0)
                {
                  g_strstrip (tokens[1]);
                  g_debug ("Found pulseaudio socket from configuration file '%s': %s", path, tokens[1]);
                  return g_strdup (tokens[1]);
                }
            }
        }
    }

  return NULL;
}

static char *
flatpak_run_get_pulseaudio_server (void)
{
  const char * pulse_clientconfig;
  char *pulse_server;
  g_autofree char *pulse_user_config = NULL;

  pulse_server = g_strdup (g_getenv ("PULSE_SERVER"));
  if (pulse_server)
    return pulse_server;

  pulse_clientconfig = g_getenv ("PULSE_CLIENTCONFIG");
  if (pulse_clientconfig)
    return flatpak_run_get_pulseaudio_server_user_config (pulse_clientconfig);

  pulse_user_config = g_build_filename (g_get_user_config_dir (), "pulse/client.conf", NULL);
  pulse_server = flatpak_run_get_pulseaudio_server_user_config (pulse_user_config);
  if (pulse_server)
    return pulse_server;

  pulse_server = flatpak_run_get_pulseaudio_server_user_config ("/etc/pulse/client.conf");
  if (pulse_server)
    return pulse_server;

  return NULL;
}

static char *
flatpak_run_parse_pulse_server (const char *value)
{
  g_auto(GStrv) servers = g_strsplit (value, " ", 0);
  gsize i;

  for (i = 0; servers[i] != NULL; i++)
    {
      const char *server = servers[i];
      if (g_str_has_prefix (server, "{"))
        {
          const char * closing = strstr (server, "}");
          if (closing == NULL)
            continue;
          server = closing + 1;
        }
      if (g_str_has_prefix (server, "unix:"))
        return g_strdup (server + 5);
    }

  return NULL;
}

static void
flatpak_run_add_pulseaudio_args (FlatpakBwrap *bwrap)
{
  g_autofree char *pulseaudio_server = flatpak_run_get_pulseaudio_server ();
  g_autofree char *pulseaudio_socket = NULL;
  g_autofree char *user_runtime_dir = flatpak_get_real_xdg_runtime_dir ();

  if (pulseaudio_server)
    pulseaudio_socket = flatpak_run_parse_pulse_server (pulseaudio_server);

  if (!pulseaudio_socket)
    pulseaudio_socket = g_build_filename (user_runtime_dir, "pulse/native", NULL);

  flatpak_bwrap_unset_env (bwrap, "PULSE_SERVER");

  if (g_file_test (pulseaudio_socket, G_FILE_TEST_EXISTS))
    {
      gboolean share_shm = FALSE; /* TODO: When do we add this? */
      g_autofree char *client_config = g_strdup_printf ("enable-shm=%s\n", share_shm ? "yes" : "no");
      g_autofree char *sandbox_socket_path = g_strdup_printf ("/run/user/%d/pulse/native", getuid ());
      g_autofree char *pulse_server = g_strdup_printf ("unix:/run/user/%d/pulse/native", getuid ());
      g_autofree char *config_path = g_strdup_printf ("/run/user/%d/pulse/config", getuid ());

      /* FIXME - error handling */
      if (!flatpak_bwrap_add_args_data (bwrap, "pulseaudio", client_config, -1, config_path, NULL))
        return;

      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", pulseaudio_socket, sandbox_socket_path,
                              NULL);

      flatpak_bwrap_set_env (bwrap, "PULSE_SERVER", pulse_server, TRUE);
      flatpak_bwrap_set_env (bwrap, "PULSE_CLIENTCONFIG", config_path, TRUE);
    }
  else
    g_debug ("Could not find pulseaudio socket");

  /* Also allow ALSA access. This was added in 1.8, and is not ideally named. However,
   * since the practical permission of ALSA and PulseAudio are essentially the same, and
   * since we don't want to add more permissions for something we plan to replace with
   * portals/pipewire going forward we reinterpret pulseaudio to also mean ALSA.
   */
  if (g_file_test ("/dev/snd", G_FILE_TEST_IS_DIR))
    flatpak_bwrap_add_args (bwrap, "--dev-bind", "/dev/snd", "/dev/snd", NULL);
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

static char *
create_proxy_socket (char *template)
{
  g_autofree char *user_runtime_dir = flatpak_get_real_xdg_runtime_dir ();
  g_autofree char *proxy_socket_dir = g_build_filename (user_runtime_dir, ".dbus-proxy", NULL);
  g_autofree char *proxy_socket = g_build_filename (proxy_socket_dir, template, NULL);
  int fd;

  if (!glnx_shutil_mkdir_p_at (AT_FDCWD, proxy_socket_dir, 0755, NULL, NULL))
    return NULL;

  fd = g_mkstemp (proxy_socket);
  if (fd == -1)
    return NULL;

  close (fd);

  return g_steal_pointer (&proxy_socket);
}

static gboolean
flatpak_run_add_system_dbus_args (FlatpakBwrap   *app_bwrap,
                                  FlatpakBwrap   *proxy_arg_bwrap,
                                  FlatpakContext *context,
                                  FlatpakRunFlags flags)
{
  gboolean unrestricted, no_proxy;
  const char *dbus_address = g_getenv ("DBUS_SYSTEM_BUS_ADDRESS");
  g_autofree char *real_dbus_address = NULL;
  g_autofree char *dbus_system_socket = NULL;

  unrestricted = (context->sockets & FLATPAK_CONTEXT_SOCKET_SYSTEM_BUS) != 0;
  if (unrestricted)
    g_debug ("Allowing system-dbus access");

  no_proxy = (flags & FLATPAK_RUN_FLAG_NO_SYSTEM_BUS_PROXY) != 0;

  if (dbus_address != NULL)
    dbus_system_socket = extract_unix_path_from_dbus_address (dbus_address);
  else if (g_file_test ("/var/run/dbus/system_bus_socket", G_FILE_TEST_EXISTS))
    dbus_system_socket = g_strdup ("/var/run/dbus/system_bus_socket");

  if (dbus_system_socket != NULL && unrestricted)
    {
      flatpak_bwrap_add_args (app_bwrap,
                              "--ro-bind", dbus_system_socket, "/run/dbus/system_bus_socket",
                              NULL);
      flatpak_bwrap_set_env (app_bwrap, "DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/dbus/system_bus_socket", TRUE);

      return TRUE;
    }
  else if (!no_proxy && flatpak_context_get_needs_system_bus_proxy (context))
    {
      g_autofree char *proxy_socket = create_proxy_socket ("system-bus-proxy-XXXXXX");

      if (proxy_socket == NULL)
        return FALSE;

      if (dbus_address)
        real_dbus_address = g_strdup (dbus_address);
      else
        real_dbus_address = g_strdup_printf ("unix:path=%s", dbus_system_socket);

      flatpak_bwrap_add_args (proxy_arg_bwrap, real_dbus_address, proxy_socket, NULL);

      if (!unrestricted)
        flatpak_context_add_bus_filters (context, NULL, FALSE, flags & FLATPAK_RUN_FLAG_SANDBOX, proxy_arg_bwrap);

      if ((flags & FLATPAK_RUN_FLAG_LOG_SYSTEM_BUS) != 0)
        flatpak_bwrap_add_args (proxy_arg_bwrap, "--log", NULL);

      flatpak_bwrap_add_args (app_bwrap,
                              "--ro-bind", proxy_socket, "/run/dbus/system_bus_socket",
                              NULL);
      flatpak_bwrap_set_env (app_bwrap, "DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/dbus/system_bus_socket", TRUE);

      return TRUE;
    }
  return FALSE;
}

static gboolean
flatpak_run_add_session_dbus_args (FlatpakBwrap   *app_bwrap,
                                   FlatpakBwrap   *proxy_arg_bwrap,
                                   FlatpakContext *context,
                                   FlatpakRunFlags flags,
                                   const char     *app_id)
{
  gboolean unrestricted, no_proxy;
  const char *dbus_address = g_getenv ("DBUS_SESSION_BUS_ADDRESS");
  g_autofree char *dbus_session_socket = NULL;
  g_autofree char *sandbox_socket_path = g_strdup_printf ("/run/user/%d/bus", getuid ());
  g_autofree char *sandbox_dbus_address = g_strdup_printf ("unix:path=/run/user/%d/bus", getuid ());

  unrestricted = (context->sockets & FLATPAK_CONTEXT_SOCKET_SESSION_BUS) != 0;

  if (dbus_address != NULL)
    {
      dbus_session_socket = extract_unix_path_from_dbus_address (dbus_address);
    }
  else
    {
      g_autofree char *user_runtime_dir = flatpak_get_real_xdg_runtime_dir ();
      struct stat statbuf;

      dbus_session_socket = g_build_filename (user_runtime_dir, "bus", NULL);

      if (stat (dbus_session_socket, &statbuf) < 0
          || (statbuf.st_mode & S_IFMT) != S_IFSOCK
          || statbuf.st_uid != getuid ())
        return FALSE;
    }

  if (unrestricted)
    g_debug ("Allowing session-dbus access");

  no_proxy = (flags & FLATPAK_RUN_FLAG_NO_SESSION_BUS_PROXY) != 0;

  if (dbus_session_socket != NULL && unrestricted)
    {
      flatpak_bwrap_add_args (app_bwrap,
                              "--ro-bind", dbus_session_socket, sandbox_socket_path,
                              NULL);
      flatpak_bwrap_set_env (app_bwrap, "DBUS_SESSION_BUS_ADDRESS", sandbox_dbus_address, TRUE);

      return TRUE;
    }
  else if (!no_proxy && dbus_address != NULL)
    {
      g_autofree char *proxy_socket = create_proxy_socket ("session-bus-proxy-XXXXXX");

      if (proxy_socket == NULL)
        return FALSE;

      flatpak_bwrap_add_args (proxy_arg_bwrap, dbus_address, proxy_socket, NULL);

      if (!unrestricted)
        {
          flatpak_context_add_bus_filters (context, app_id, TRUE, flags & FLATPAK_RUN_FLAG_SANDBOX, proxy_arg_bwrap);

          /* Allow calling any interface+method on all portals, but only receive broadcasts under /org/desktop/portal */
          flatpak_bwrap_add_arg (proxy_arg_bwrap,
                                 "--call=org.freedesktop.portal.*=*");
          flatpak_bwrap_add_arg (proxy_arg_bwrap,
                                 "--broadcast=org.freedesktop.portal.*=@/org/freedesktop/portal/*");
        }

      if ((flags & FLATPAK_RUN_FLAG_LOG_SESSION_BUS) != 0)
        flatpak_bwrap_add_args (proxy_arg_bwrap, "--log", NULL);

      flatpak_bwrap_add_args (app_bwrap,
                              "--ro-bind", proxy_socket, sandbox_socket_path,
                              NULL);
      flatpak_bwrap_set_env (app_bwrap, "DBUS_SESSION_BUS_ADDRESS", sandbox_dbus_address, TRUE);

      return TRUE;
    }

  return FALSE;
}

static gboolean
flatpak_run_add_a11y_dbus_args (FlatpakBwrap   *app_bwrap,
                                FlatpakBwrap   *proxy_arg_bwrap,
                                FlatpakContext *context,
                                FlatpakRunFlags flags)
{
  g_autoptr(GDBusConnection) session_bus = NULL;
  g_autofree char *a11y_address = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GDBusMessage) reply = NULL;
  g_autoptr(GDBusMessage) msg = NULL;
  g_autofree char *proxy_socket = NULL;

  if ((flags & FLATPAK_RUN_FLAG_NO_A11Y_BUS_PROXY) != 0)
    return FALSE;

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (session_bus == NULL)
    return FALSE;

  msg = g_dbus_message_new_method_call ("org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress");
  g_dbus_message_set_body (msg, g_variant_new ("()"));
  reply =
    g_dbus_connection_send_message_with_reply_sync (session_bus, msg,
                                                    G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                    30000,
                                                    NULL,
                                                    NULL,
                                                    NULL);
  if (reply)
    {
      if (g_dbus_message_to_gerror (reply, &local_error))
        {
          if (!g_error_matches (local_error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN))
            g_message ("Can't find a11y bus: %s", local_error->message);
        }
      else
        {
          g_variant_get (g_dbus_message_get_body (reply),
                         "(s)", &a11y_address);
        }
    }

  if (!a11y_address)
    return FALSE;

  proxy_socket = create_proxy_socket ("a11y-bus-proxy-XXXXXX");
  if (proxy_socket == NULL)
    return FALSE;

  g_autofree char *sandbox_socket_path = g_strdup_printf ("/run/user/%d/at-spi-bus", getuid ());
  g_autofree char *sandbox_dbus_address = g_strdup_printf ("unix:path=/run/user/%d/at-spi-bus", getuid ());

  flatpak_bwrap_add_args (proxy_arg_bwrap,
                          a11y_address,
                          proxy_socket, "--filter", "--sloppy-names",
                          "--call=org.a11y.atspi.Registry=org.a11y.atspi.Socket.Embed@/org/a11y/atspi/accessible/root",
                          "--call=org.a11y.atspi.Registry=org.a11y.atspi.Socket.Unembed@/org/a11y/atspi/accessible/root",
                          "--call=org.a11y.atspi.Registry=org.a11y.atspi.Registry.GetRegisteredEvents@/org/a11y/atspi/registry",
                          "--call=org.a11y.atspi.Registry=org.a11y.atspi.DeviceEventController.GetKeystrokeListeners@/org/a11y/atspi/registry/deviceeventcontroller",
                          "--call=org.a11y.atspi.Registry=org.a11y.atspi.DeviceEventController.GetDeviceEventListeners@/org/a11y/atspi/registry/deviceeventcontroller",
                          "--call=org.a11y.atspi.Registry=org.a11y.atspi.DeviceEventController.NotifyListenersSync@/org/a11y/atspi/registry/deviceeventcontroller",
                          "--call=org.a11y.atspi.Registry=org.a11y.atspi.DeviceEventController.NotifyListenersAsync@/org/a11y/atspi/registry/deviceeventcontroller",
                          NULL);

  if ((flags & FLATPAK_RUN_FLAG_LOG_A11Y_BUS) != 0)
    flatpak_bwrap_add_args (proxy_arg_bwrap, "--log", NULL);

  flatpak_bwrap_add_args (app_bwrap,
                          "--ro-bind", proxy_socket, sandbox_socket_path,
                          NULL);
  flatpak_bwrap_set_env (app_bwrap, "AT_SPI_BUS_ADDRESS", sandbox_dbus_address, TRUE);

  return TRUE;
}

/* This wraps the argv in a bwrap call, primary to allow the
   command to be run with a proper /.flatpak-info with data
   taken from app_info_path */
static gboolean
add_bwrap_wrapper (FlatpakBwrap *bwrap,
                   const char   *app_info_path,
                   GError      **error)
{
  glnx_autofd int app_info_fd = -1;
  g_auto(GLnxDirFdIterator) dir_iter = { 0 };
  struct dirent *dent;
  g_autofree char *user_runtime_dir = flatpak_get_real_xdg_runtime_dir ();
  g_autofree char *proxy_socket_dir = g_build_filename (user_runtime_dir, ".dbus-proxy/", NULL);

  app_info_fd = open (app_info_path, O_RDONLY | O_CLOEXEC);
  if (app_info_fd == -1)
    return glnx_throw_errno_prefix (error, _("Failed to open app info file"));

  if (!glnx_dirfd_iterator_init_at (AT_FDCWD, "/", FALSE, &dir_iter, error))
    return FALSE;

  flatpak_bwrap_add_arg (bwrap, flatpak_get_bwrap ());

  while (TRUE)
    {
      glnx_autofd int o_path_fd = -1;
      struct statfs stfs;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dir_iter, &dent, NULL, error))
        return FALSE;

      if (dent == NULL)
        break;

      if (strcmp (dent->d_name, ".flatpak-info") == 0)
        continue;

      /* O_PATH + fstatfs is the magic that we need to statfs without automounting the target */
      o_path_fd = openat (dir_iter.fd, dent->d_name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
      if (o_path_fd == -1 || fstatfs (o_path_fd, &stfs) != 0 || stfs.f_type == AUTOFS_SUPER_MAGIC)
        continue; /* AUTOFS mounts are risky and can cause us to block (see issue #1633), so ignore it. Its unlikely the proxy needs such a directory. */

      if (dent->d_type == DT_DIR)
        {
          if (strcmp (dent->d_name, "tmp") == 0 ||
              strcmp (dent->d_name, "var") == 0 ||
              strcmp (dent->d_name, "run") == 0)
            flatpak_bwrap_add_arg (bwrap, "--bind");
          else
            flatpak_bwrap_add_arg (bwrap, "--ro-bind");

          flatpak_bwrap_add_arg_printf (bwrap, "/%s", dent->d_name);
          flatpak_bwrap_add_arg_printf (bwrap, "/%s", dent->d_name);
        }
      else if (dent->d_type == DT_LNK)
        {
          g_autofree gchar *target = NULL;

          target = glnx_readlinkat_malloc (dir_iter.fd, dent->d_name,
                                           NULL, error);
          if (target == NULL)
            return FALSE;
          flatpak_bwrap_add_args (bwrap, "--symlink", target, NULL);
          flatpak_bwrap_add_arg_printf (bwrap, "/%s", dent->d_name);
        }
    }

  flatpak_bwrap_add_args (bwrap, "--bind", proxy_socket_dir, proxy_socket_dir, NULL);

  /* This is a file rather than a bind mount, because it will then
     not be unmounted from the namespace when the namespace dies. */
  flatpak_bwrap_add_args_data_fd (bwrap, "--file", glnx_steal_fd (&app_info_fd), "/.flatpak-info");

  if (!flatpak_bwrap_bundle_args (bwrap, 1, -1, FALSE, error))
    return FALSE;

  return TRUE;
}

static gboolean
start_dbus_proxy (FlatpakBwrap *app_bwrap,
                  FlatpakBwrap *proxy_arg_bwrap,
                  const char   *app_info_path,
                  GError      **error)
{
  char x = 'x';
  const char *proxy;
  g_autofree char *commandline = NULL;
  g_autoptr(FlatpakBwrap) proxy_bwrap = NULL;
  int sync_fds[2] = {-1, -1};
  int proxy_start_index;
  g_auto(GStrv) minimal_envp = NULL;

  minimal_envp = flatpak_run_get_minimal_env (FALSE, FALSE);
  proxy_bwrap = flatpak_bwrap_new (NULL);

  if (!add_bwrap_wrapper (proxy_bwrap, app_info_path, error))
    return FALSE;

  proxy = g_getenv ("FLATPAK_DBUSPROXY");
  if (proxy == NULL)
    proxy = DBUSPROXY;

  flatpak_bwrap_add_arg (proxy_bwrap, proxy);

  proxy_start_index = proxy_bwrap->argv->len;

  if (pipe2 (sync_fds, O_CLOEXEC) < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           _("Unable to create sync pipe"));
      return FALSE;
    }

  /* read end goes to app */
  flatpak_bwrap_add_args_data_fd (app_bwrap, "--sync-fd", sync_fds[0], NULL);

  /* write end goes to proxy */
  flatpak_bwrap_add_fd (proxy_bwrap, sync_fds[1]);
  flatpak_bwrap_add_arg_printf (proxy_bwrap, "--fd=%d", sync_fds[1]);

  /* Note: This steals the fds from proxy_arg_bwrap */
  flatpak_bwrap_append_bwrap (proxy_bwrap, proxy_arg_bwrap);

  if (!flatpak_bwrap_bundle_args (proxy_bwrap, proxy_start_index, -1, TRUE, error))
    return FALSE;

  flatpak_bwrap_finish (proxy_bwrap);

  commandline = flatpak_quote_argv ((const char **) proxy_bwrap->argv->pdata, -1);
  g_debug ("Running '%s'", commandline);

  /* We use LEAVE_DESCRIPTORS_OPEN to work around dead-lock, see flatpak_close_fds_workaround */
  if (!g_spawn_async (NULL,
                      (char **) proxy_bwrap->argv->pdata,
                      NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                      flatpak_bwrap_child_setup_cb, proxy_bwrap->fds,
                      NULL, error))
    return FALSE;

  /* The write end can be closed now, otherwise the read below will hang of xdg-dbus-proxy
     fails to start. */
  g_clear_pointer (&proxy_bwrap, flatpak_bwrap_free);

  /* Sync with proxy, i.e. wait until its listening on the sockets */
  if (read (sync_fds[0], &x, 1) != 1)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           _("Failed to sync with dbus proxy"));
      return FALSE;
    }

  return TRUE;
}

static int
flatpak_extension_compare_by_path (gconstpointer _a,
                                   gconstpointer _b)
{
  const FlatpakExtension *a = _a;
  const FlatpakExtension *b = _b;

  return g_strcmp0 (a->directory, b->directory);
}

gboolean
flatpak_run_add_extension_args (FlatpakBwrap *bwrap,
                                GKeyFile     *metakey,
                                const char   *full_ref,
                                gboolean      use_ld_so_cache,
                                char        **extensions_out,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_auto(GStrv) parts = NULL;
  g_autoptr(GString) used_extensions = g_string_new ("");
  gboolean is_app;
  GList *extensions, *path_sorted_extensions, *l;
  g_autoptr(GString) ld_library_path = g_string_new ("");
  int count = 0;
  g_autoptr(GHashTable) mounted_tmpfs =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GHashTable) created_symlink =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  parts = g_strsplit (full_ref, "/", 0);
  if (g_strv_length (parts) != 4)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Failed to determine parts from ref: %s"), full_ref);

  is_app = strcmp (parts[0], "app") == 0;

  extensions = flatpak_list_extensions (metakey,
                                        parts[2], parts[3]);

  /* First we apply all the bindings, they are sorted alphabetically in order for parent directory
     to be mounted before child directories */
  path_sorted_extensions = g_list_copy (extensions);
  path_sorted_extensions = g_list_sort (path_sorted_extensions, flatpak_extension_compare_by_path);

  for (l = path_sorted_extensions; l != NULL; l = l->next)
    {
      FlatpakExtension *ext = l->data;
      g_autofree char *directory = g_build_filename (is_app ? "/app" : "/usr", ext->directory, NULL);
      g_autofree char *full_directory = g_build_filename (directory, ext->subdir_suffix, NULL);
      g_autofree char *ref = g_build_filename (full_directory, ".ref", NULL);
      g_autofree char *real_ref = g_build_filename (ext->files_path, ext->directory, ".ref", NULL);

      if (ext->needs_tmpfs)
        {
          g_autofree char *parent = g_path_get_dirname (directory);

          if (g_hash_table_lookup (mounted_tmpfs, parent) == NULL)
            {
              flatpak_bwrap_add_args (bwrap,
                                      "--tmpfs", parent,
                                      NULL);
              g_hash_table_insert (mounted_tmpfs, g_steal_pointer (&parent), "mounted");
            }
        }

      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", ext->files_path, full_directory,
                              NULL);

      if (g_file_test (real_ref, G_FILE_TEST_EXISTS))
        flatpak_bwrap_add_args (bwrap,
                                "--lock-file", ref,
                                NULL);
    }

  g_list_free (path_sorted_extensions);

  /* Then apply library directories and file merging, in extension prio order */

  for (l = extensions; l != NULL; l = l->next)
    {
      FlatpakExtension *ext = l->data;
      g_autofree char *directory = g_build_filename (is_app ? "/app" : "/usr", ext->directory, NULL);
      g_autofree char *full_directory = g_build_filename (directory, ext->subdir_suffix, NULL);
      int i;

      if (used_extensions->len > 0)
        g_string_append (used_extensions, ";");
      g_string_append (used_extensions, ext->installed_id);
      g_string_append (used_extensions, "=");
      if (ext->commit != NULL)
        g_string_append (used_extensions, ext->commit);
      else
        g_string_append (used_extensions, "local");

      if (ext->add_ld_path)
        {
          g_autofree char *ld_path = g_build_filename (full_directory, ext->add_ld_path, NULL);

          if (use_ld_so_cache)
            {
              g_autofree char *contents = g_strconcat (ld_path, "\n", NULL);
              /* We prepend app or runtime and a counter in order to get the include order correct for the conf files */
              g_autofree char *ld_so_conf_file = g_strdup_printf ("%s-%03d-%s.conf", parts[0], ++count, ext->installed_id);
              g_autofree char *ld_so_conf_file_path = g_build_filename ("/run/flatpak/ld.so.conf.d", ld_so_conf_file, NULL);

              if (!flatpak_bwrap_add_args_data (bwrap, "ld-so-conf",
                                                contents, -1, ld_so_conf_file_path, error))
                return FALSE;
            }
          else
            {
              if (ld_library_path->len != 0)
                g_string_append (ld_library_path, ":");
              g_string_append (ld_library_path, ld_path);
            }
        }

      for (i = 0; ext->merge_dirs != NULL && ext->merge_dirs[i] != NULL; i++)
        {
          g_autofree char *parent = g_path_get_dirname (directory);
          g_autofree char *merge_dir = g_build_filename (parent, ext->merge_dirs[i], NULL);
          g_autofree char *source_dir = g_build_filename (ext->files_path, ext->merge_dirs[i], NULL);
          g_auto(GLnxDirFdIterator) source_iter = { 0 };
          struct dirent *dent;

          if (glnx_dirfd_iterator_init_at (AT_FDCWD, source_dir, TRUE, &source_iter, NULL))
            {
              while (glnx_dirfd_iterator_next_dent (&source_iter, &dent, NULL, NULL) && dent != NULL)
                {
                  g_autofree char *symlink_path = g_build_filename (merge_dir, dent->d_name, NULL);
                  /* Only create the first, because extensions are listed in prio order */
                  if (g_hash_table_lookup (created_symlink, symlink_path) == NULL)
                    {
                      g_autofree char *symlink = g_build_filename (directory, ext->merge_dirs[i], dent->d_name, NULL);
                      flatpak_bwrap_add_args (bwrap,
                                              "--symlink", symlink, symlink_path,
                                              NULL);
                      g_hash_table_insert (created_symlink, g_steal_pointer (&symlink_path), "created");
                    }
                }
            }
        }
    }

  g_list_free_full (extensions, (GDestroyNotify) flatpak_extension_free);

  if (ld_library_path->len != 0)
    {
      const gchar *old_ld_path = g_environ_getenv (bwrap->envp, "LD_LIBRARY_PATH");

      if (old_ld_path != NULL && *old_ld_path != 0)
        {
          if (is_app)
            {
              g_string_append (ld_library_path, ":");
              g_string_append (ld_library_path, old_ld_path);
            }
          else
            {
              g_string_prepend (ld_library_path, ":");
              g_string_prepend (ld_library_path, old_ld_path);
            }
        }

      flatpak_bwrap_set_env (bwrap, "LD_LIBRARY_PATH", ld_library_path->str, TRUE);
    }

  if (extensions_out)
    *extensions_out = g_string_free (g_steal_pointer (&used_extensions), FALSE);

  return TRUE;
}

gboolean
flatpak_run_add_environment_args (FlatpakBwrap    *bwrap,
                                  const char      *app_info_path,
                                  FlatpakRunFlags  flags,
                                  const char      *app_id,
                                  FlatpakContext  *context,
                                  GFile           *app_id_dir,
                                  GPtrArray       *previous_app_id_dirs,
                                  FlatpakExports **exports_out,
                                  GCancellable    *cancellable,
                                  GError         **error)
{
  g_autoptr(GError) my_error = NULL;
  g_autoptr(FlatpakExports) exports = NULL;
  g_autoptr(FlatpakBwrap) proxy_arg_bwrap = flatpak_bwrap_new (flatpak_bwrap_empty_env);
  gboolean has_wayland = FALSE;
  gboolean allow_x11 = FALSE;

  if ((context->shares & FLATPAK_CONTEXT_SHARED_IPC) == 0)
    {
      g_debug ("Disallowing ipc access");
      flatpak_bwrap_add_args (bwrap, "--unshare-ipc", NULL);
    }

  if ((context->shares & FLATPAK_CONTEXT_SHARED_NETWORK) == 0)
    {
      g_debug ("Disallowing network access");
      flatpak_bwrap_add_args (bwrap, "--unshare-net", NULL);
    }

  if (context->devices & FLATPAK_CONTEXT_DEVICE_ALL)
    {
      flatpak_bwrap_add_args (bwrap,
                              "--dev-bind", "/dev", "/dev",
                              NULL);
      /* Don't expose the host /dev/shm, just the device nodes, unless explicitly allowed */
      if (g_file_test ("/dev/shm", G_FILE_TEST_IS_DIR))
        {
          if ((context->devices & FLATPAK_CONTEXT_DEVICE_SHM) == 0)
            flatpak_bwrap_add_args (bwrap,
                                    "--tmpfs", "/dev/shm",
                                    NULL);
        }
      else if (g_file_test ("/dev/shm", G_FILE_TEST_IS_SYMLINK))
        {
          g_autofree char *link = flatpak_readlink ("/dev/shm", NULL);

          /* On debian (with sysv init) the host /dev/shm is a symlink to /run/shm, so we can't
             mount on top of it. */
          if (g_strcmp0 (link, "/run/shm") == 0)
            {
              if (context->devices & FLATPAK_CONTEXT_DEVICE_SHM &&
                  g_file_test ("/run/shm", G_FILE_TEST_IS_DIR))
                flatpak_bwrap_add_args (bwrap,
                                        "--bind", "/run/shm", "/run/shm",
                                        NULL);
              else
                flatpak_bwrap_add_args (bwrap,
                                        "--dir", "/run/shm",
                                        NULL);
            }
          else
            g_warning ("Unexpected /dev/shm symlink %s", link);
        }
    }
  else
    {
      flatpak_bwrap_add_args (bwrap,
                              "--dev", "/dev",
                              NULL);
      if (context->devices & FLATPAK_CONTEXT_DEVICE_DRI)
        {
          g_debug ("Allowing dri access");
          int i;
          char *dri_devices[] = {
            "/dev/dri",
            /* mali */
            "/dev/mali",
            "/dev/mali0",
            "/dev/umplock",
            /* nvidia */
            "/dev/nvidiactl",
            "/dev/nvidia-modeset",
            /* nvidia OpenCL/CUDA */
            "/dev/nvidia-uvm",
            "/dev/nvidia-uvm-tools",
          };

          for (i = 0; i < G_N_ELEMENTS (dri_devices); i++)
            {
              if (g_file_test (dri_devices[i], G_FILE_TEST_EXISTS))
                flatpak_bwrap_add_args (bwrap, "--dev-bind", dri_devices[i], dri_devices[i], NULL);
            }

          /* Each Nvidia card gets its own device.
             This is a fairly arbitrary limit but ASUS sells mining boards supporting 20 in theory. */
          char nvidia_dev[14]; /* /dev/nvidia plus up to 2 digits */
          for (i = 0; i < 20; i++)
            {
              g_snprintf (nvidia_dev, sizeof (nvidia_dev), "/dev/nvidia%d", i);
              if (g_file_test (nvidia_dev, G_FILE_TEST_EXISTS))
                flatpak_bwrap_add_args (bwrap, "--dev-bind", nvidia_dev, nvidia_dev, NULL);
            }
        }

      if (context->devices & FLATPAK_CONTEXT_DEVICE_KVM)
        {
          g_debug ("Allowing kvm access");
          if (g_file_test ("/dev/kvm", G_FILE_TEST_EXISTS))
            flatpak_bwrap_add_args (bwrap, "--dev-bind", "/dev/kvm", "/dev/kvm", NULL);
        }

      if (context->devices & FLATPAK_CONTEXT_DEVICE_SHM)
        {
          /* This is a symlink to /run/shm on debian, so bind to real target */
          g_autofree char *real_dev_shm = realpath ("/dev/shm", NULL);

          g_debug ("Allowing /dev/shm access (as %s)", real_dev_shm);
          if (real_dev_shm != NULL)
              flatpak_bwrap_add_args (bwrap, "--bind", real_dev_shm, "/dev/shm", NULL);
        }
    }

  flatpak_context_append_bwrap_filesystem (context, bwrap, app_id, app_id_dir, previous_app_id_dirs, &exports);

  if (context->sockets & FLATPAK_CONTEXT_SOCKET_WAYLAND)
    {
      g_debug ("Allowing wayland access");
      has_wayland = flatpak_run_add_wayland_args (bwrap);
    }

  if ((context->sockets & FLATPAK_CONTEXT_SOCKET_FALLBACK_X11) != 0)
    allow_x11 = !has_wayland;
  else
    allow_x11 = (context->sockets & FLATPAK_CONTEXT_SOCKET_X11) != 0;

  flatpak_run_add_x11_args (bwrap, allow_x11);

  if (context->sockets & FLATPAK_CONTEXT_SOCKET_SSH_AUTH)
    {
      flatpak_run_add_ssh_args (bwrap);
    }

  if (context->sockets & FLATPAK_CONTEXT_SOCKET_PULSEAUDIO)
    {
      g_debug ("Allowing pulseaudio access");
      flatpak_run_add_pulseaudio_args (bwrap);
    }

  if (context->sockets & FLATPAK_CONTEXT_SOCKET_PCSC)
    {
      flatpak_run_add_pcsc_args (bwrap);
    }

  if (context->sockets & FLATPAK_CONTEXT_SOCKET_CUPS)
    {
      flatpak_run_add_cups_args (bwrap);
    }

  flatpak_run_add_session_dbus_args (bwrap, proxy_arg_bwrap, context, flags, app_id);
  flatpak_run_add_system_dbus_args (bwrap, proxy_arg_bwrap, context, flags);
  flatpak_run_add_a11y_dbus_args (bwrap, proxy_arg_bwrap, context, flags);

  /* Must run this before spawning the dbus proxy, to ensure it
     ends up in the app cgroup */
  if (!flatpak_run_in_transient_unit (app_id, &my_error))
    {
      /* We still run along even if we don't get a cgroup, as nothing
         really depends on it. Its just nice to have */
      g_debug ("Failed to run in transient scope: %s", my_error->message);
      g_clear_error (&my_error);
    }

  if (!flatpak_bwrap_is_empty (proxy_arg_bwrap) &&
      !start_dbus_proxy (bwrap, proxy_arg_bwrap, app_info_path, error))
    return FALSE;

  if (exports_out)
    *exports_out = g_steal_pointer (&exports);

  return TRUE;
}

typedef struct
{
  const char *env;
  const char *val;
} ExportData;

static const ExportData default_exports[] = {
  {"PATH", "/app/bin:/usr/bin"},
  /* We always want to unset LD_LIBRARY_PATH to avoid inheriting weird
   * dependencies from the host. But if not using ld.so.cache this is
   * later set. */
  {"LD_LIBRARY_PATH", NULL},
  {"XDG_CONFIG_DIRS", "/app/etc/xdg:/etc/xdg"},
  {"XDG_DATA_DIRS", "/app/share:/usr/share"},
  {"SHELL", "/bin/sh"},
  {"TMPDIR", NULL}, /* Unset TMPDIR as it may not exist in the sandbox */

  /* Some env vars are common enough and will affect the sandbox badly
     if set on the host. We clear these always. */
  {"PYTHONPATH", NULL},
  {"PERLLIB", NULL},
  {"PERL5LIB", NULL},
  {"XCURSOR_PATH", NULL},
};

static const ExportData no_ld_so_cache_exports[] = {
  {"LD_LIBRARY_PATH", "/app/lib"},
};

static const ExportData devel_exports[] = {
  {"ACLOCAL_PATH", "/app/share/aclocal"},
  {"C_INCLUDE_PATH", "/app/include"},
  {"CPLUS_INCLUDE_PATH", "/app/include"},
  {"LDFLAGS", "-L/app/lib "},
  {"PKG_CONFIG_PATH", "/app/lib/pkgconfig:/app/share/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig"},
  {"LC_ALL", "en_US.utf8"},
};

static void
add_exports (GPtrArray        *env_array,
             const ExportData *exports,
             gsize             n_exports)
{
  int i;

  for (i = 0; i < n_exports; i++)
    {
      if (exports[i].val)
        g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", exports[i].env, exports[i].val));
    }
}

char **
flatpak_run_get_minimal_env (gboolean devel, gboolean use_ld_so_cache)
{
  GPtrArray *env_array;
  static const char * const copy[] = {
    "PWD",
    "GDMSESSION",
    "XDG_CURRENT_DESKTOP",
    "XDG_SESSION_DESKTOP",
    "DESKTOP_SESSION",
    "EMAIL_ADDRESS",
    "HOME",
    "HOSTNAME",
    "LOGNAME",
    "REAL_NAME",
    "TERM",
    "USER",
    "USERNAME",
  };
  static const char * const copy_nodevel[] = {
    "LANG",
    "LANGUAGE",
    "LC_ALL",
    "LC_ADDRESS",
    "LC_COLLATE",
    "LC_CTYPE",
    "LC_IDENTIFICATION",
    "LC_MEASUREMENT",
    "LC_MESSAGES",
    "LC_MONETARY",
    "LC_NAME",
    "LC_NUMERIC",
    "LC_PAPER",
    "LC_TELEPHONE",
    "LC_TIME",
  };
  int i;

  env_array = g_ptr_array_new_with_free_func (g_free);

  add_exports (env_array, default_exports, G_N_ELEMENTS (default_exports));

  if (!use_ld_so_cache)
    add_exports (env_array, no_ld_so_cache_exports, G_N_ELEMENTS (no_ld_so_cache_exports));

  if (devel)
    add_exports (env_array, devel_exports, G_N_ELEMENTS (devel_exports));

  for (i = 0; i < G_N_ELEMENTS (copy); i++)
    {
      const char *current = g_getenv (copy[i]);
      if (current)
        g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", copy[i], current));
    }

  if (!devel)
    {
      for (i = 0; i < G_N_ELEMENTS (copy_nodevel); i++)
        {
          const char *current = g_getenv (copy_nodevel[i]);
          if (current)
            g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", copy_nodevel[i], current));
        }
    }

  g_ptr_array_add (env_array, NULL);
  return (char **) g_ptr_array_free (env_array, FALSE);
}

static char **
apply_exports (char            **envp,
               const ExportData *exports,
               gsize             n_exports)
{
  int i;

  for (i = 0; i < n_exports; i++)
    {
      const char *value = exports[i].val;

      if (value)
        envp = g_environ_setenv (envp, exports[i].env, value, TRUE);
      else
        envp = g_environ_unsetenv (envp, exports[i].env);
    }

  return envp;
}

void
flatpak_run_apply_env_default (FlatpakBwrap *bwrap, gboolean use_ld_so_cache)
{
  bwrap->envp = apply_exports (bwrap->envp, default_exports, G_N_ELEMENTS (default_exports));

  if (!use_ld_so_cache)
    bwrap->envp = apply_exports (bwrap->envp, no_ld_so_cache_exports, G_N_ELEMENTS (no_ld_so_cache_exports));
}

static void
flatpak_run_apply_env_prompt (FlatpakBwrap *bwrap, const char *app_id)
{
  /* A custom shell prompt. FLATPAK_ID is always set.
   * PS1 can be overwritten by runtime metadata or by --env overrides
   */
  flatpak_bwrap_set_env (bwrap, "FLATPAK_ID", app_id, TRUE);
  flatpak_bwrap_set_env (bwrap, "PS1", "[📦 $FLATPAK_ID \\W]\\$ ", FALSE);
}

void
flatpak_run_apply_env_appid (FlatpakBwrap *bwrap,
                             GFile        *app_dir)
{
  g_autoptr(GFile) app_dir_data = NULL;
  g_autoptr(GFile) app_dir_config = NULL;
  g_autoptr(GFile) app_dir_cache = NULL;

  app_dir_data = g_file_get_child (app_dir, "data");
  app_dir_config = g_file_get_child (app_dir, "config");
  app_dir_cache = g_file_get_child (app_dir, "cache");
  flatpak_bwrap_set_env (bwrap, "XDG_DATA_HOME", flatpak_file_get_path_cached (app_dir_data), TRUE);
  flatpak_bwrap_set_env (bwrap, "XDG_CONFIG_HOME", flatpak_file_get_path_cached (app_dir_config), TRUE);
  flatpak_bwrap_set_env (bwrap, "XDG_CACHE_HOME", flatpak_file_get_path_cached (app_dir_cache), TRUE);

  if (g_getenv ("XDG_DATA_HOME"))
    flatpak_bwrap_set_env (bwrap, "HOST_XDG_DATA_HOME", g_getenv ("XDG_DATA_HOME"), TRUE);
  if (g_getenv ("XDG_CONFIG_HOME"))
    flatpak_bwrap_set_env (bwrap, "HOST_XDG_CONFIG_HOME", g_getenv ("XDG_CONFIG_HOME"), TRUE);
  if (g_getenv ("XDG_CACHE_HOME"))
    flatpak_bwrap_set_env (bwrap, "HOST_XDG_CACHE_HOME", g_getenv ("XDG_CACHE_HOME"), TRUE);
}

void
flatpak_run_apply_env_vars (FlatpakBwrap *bwrap, FlatpakContext *context)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, context->env_vars);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *var = key;
      const char *val = value;

      if (val && val[0] != 0)
        flatpak_bwrap_set_env (bwrap, var, val, TRUE);
      else
        flatpak_bwrap_unset_env (bwrap, var);
    }
}

GFile *
flatpak_get_data_dir (const char *app_id)
{
  g_autoptr(GFile) home = g_file_new_for_path (g_get_home_dir ());
  g_autoptr(GFile) var_app = g_file_resolve_relative_path (home, ".var/app");

  return g_file_get_child (var_app, app_id);
}

gboolean
flatpak_ensure_data_dir (GFile        *app_id_dir,
                         GCancellable *cancellable,
                         GError      **error)
{
  g_autoptr(GFile) data_dir = g_file_get_child (app_id_dir, "data");
  g_autoptr(GFile) cache_dir = g_file_get_child (app_id_dir, "cache");
  g_autoptr(GFile) fontconfig_cache_dir = g_file_get_child (cache_dir, "fontconfig");
  g_autoptr(GFile) tmp_dir = g_file_get_child (cache_dir, "tmp");
  g_autoptr(GFile) config_dir = g_file_get_child (app_id_dir, "config");

  if (!flatpak_mkdir_p (data_dir, cancellable, error))
    return FALSE;

  if (!flatpak_mkdir_p (cache_dir, cancellable, error))
    return FALSE;

  if (!flatpak_mkdir_p (fontconfig_cache_dir, cancellable, error))
    return FALSE;

  if (!flatpak_mkdir_p (tmp_dir, cancellable, error))
    return FALSE;

  if (!flatpak_mkdir_p (config_dir, cancellable, error))
    return FALSE;

  return TRUE;
}

struct JobData
{
  char      *job;
  GMainLoop *main_loop;
};

static void
job_removed_cb (SystemdManager *manager,
                guint32         id,
                char           *job,
                char           *unit,
                char           *result,
                struct JobData *data)
{
  if (strcmp (job, data->job) == 0)
    g_main_loop_quit (data->main_loop);
}

static gchar *
systemd_unit_name_escape (const gchar *in)
{
  /* Adapted from systemd source */
  GString * const str = g_string_sized_new (strlen (in));

  for (; *in; in++)
    {
      if (g_ascii_isalnum (*in) || *in == ':' || *in == '_' || *in == '.')
        g_string_append_c (str, *in);
      else
        g_string_append_printf (str, "\\x%02x", *in);
    }
  return g_string_free (str, FALSE);
}

gboolean
flatpak_run_in_transient_unit (const char *appid, GError **error)
{
  g_autoptr(GDBusConnection) conn = NULL;
  g_autofree char *path = NULL;
  g_autofree char *address = NULL;
  g_autofree char *name = NULL;
  g_autofree char *appid_escaped = NULL;
  g_autofree char *job = NULL;
  SystemdManager *manager = NULL;
  GVariantBuilder builder;
  GVariant *properties = NULL;
  GVariant *aux = NULL;
  guint32 pid;
  GMainLoop *main_loop = NULL;
  struct JobData data;
  gboolean res = FALSE;
  g_autoptr(GMainContextPopDefault) main_context = NULL;

  path = g_strdup_printf ("/run/user/%d/systemd/private", getuid ());

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    return flatpak_fail_error (error, FLATPAK_ERROR_SETUP_FAILED,
                               _("No systemd user session available, cgroups not available"));

  main_context = flatpak_main_context_new_default ();
  main_loop = g_main_loop_new (main_context, FALSE);

  address = g_strconcat ("unix:path=", path, NULL);

  conn = g_dbus_connection_new_for_address_sync (address,
                                                 G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                                 NULL,
                                                 NULL, error);
  if (!conn)
    goto out;

  manager = systemd_manager_proxy_new_sync (conn,
                                            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                            NULL,
                                            "/org/freedesktop/systemd1",
                                            NULL, error);
  if (!manager)
    goto out;

  appid_escaped = systemd_unit_name_escape (appid);
  name = g_strdup_printf ("app-flatpak-%s-%d.scope", appid_escaped, getpid ());

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sv)"));

  pid = getpid ();
  g_variant_builder_add (&builder, "(sv)",
                         "PIDs",
                         g_variant_new_fixed_array (G_VARIANT_TYPE ("u"),
                                                    &pid, 1, sizeof (guint32))
                        );

  properties = g_variant_builder_end (&builder);

  aux = g_variant_new_array (G_VARIANT_TYPE ("(sa(sv))"), NULL, 0);

  if (!systemd_manager_call_start_transient_unit_sync (manager,
                                                       name,
                                                       "fail",
                                                       properties,
                                                       aux,
                                                       &job,
                                                       NULL,
                                                       error))
    goto out;

  data.job = job;
  data.main_loop = main_loop;
  g_signal_connect (manager, "job-removed", G_CALLBACK (job_removed_cb), &data);

  g_main_loop_run (main_loop);

  res = TRUE;

out:
  if (main_loop)
    g_main_loop_unref (main_loop);
  if (manager)
    g_object_unref (manager);

  return res;
}

static void
add_font_path_args (FlatpakBwrap *bwrap)
{
  g_autoptr(GString) xml_snippet = g_string_new ("");
  gchar *path_build_tmp = NULL;
  g_autoptr(GFile) user_font1 = NULL;
  g_autoptr(GFile) user_font2 = NULL;
  g_autoptr(GFile) user_font_cache = NULL;
  g_auto(GStrv) system_cache_dirs = NULL;
  gboolean found_cache = FALSE;
  int i;


  g_string_append (xml_snippet,
                   "<?xml version=\"1.0\"?>\n"
                   "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
                   "<fontconfig>\n");

  if (g_file_test (SYSTEM_FONTS_DIR, G_FILE_TEST_EXISTS))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", SYSTEM_FONTS_DIR, "/run/host/fonts",
                              NULL);
      g_string_append_printf (xml_snippet,
                              "\t<remap-dir as-path=\"%s\">/run/host/fonts</remap-dir>\n",
                              SYSTEM_FONTS_DIR);
    }

  if (g_file_test ("/usr/local/share/fonts", G_FILE_TEST_EXISTS))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", "/usr/local/share/fonts", "/run/host/local-fonts",
                              NULL);
      g_string_append_printf (xml_snippet,
                              "\t<remap-dir as-path=\"%s\">/run/host/local-fonts</remap-dir>\n",
                              "/usr/local/share/fonts");
    }

  system_cache_dirs = g_strsplit (SYSTEM_FONT_CACHE_DIRS, ":", 0);
  for (i = 0; system_cache_dirs[i] != NULL; i++)
    {
      if (g_file_test (system_cache_dirs[i], G_FILE_TEST_EXISTS))
        {
          flatpak_bwrap_add_args (bwrap,
                                  "--ro-bind", system_cache_dirs[i], "/run/host/fonts-cache",
                                  NULL);
          found_cache = TRUE;
          break;
        }
    }

  if (!found_cache)
    {
      /* We ensure these directories are never writable, or fontconfig
         will use them to write the default cache */
      flatpak_bwrap_add_args (bwrap,
                              "--tmpfs", "/run/host/fonts-cache",
                              "--remount-ro", "/run/host/fonts-cache",
                              NULL);
    }

  path_build_tmp = g_build_filename (g_get_user_data_dir (), "fonts", NULL);
  user_font1 = g_file_new_for_path (path_build_tmp);
  g_clear_pointer (&path_build_tmp, g_free);

  path_build_tmp = g_build_filename (g_get_home_dir (), ".fonts", NULL);
  user_font2 = g_file_new_for_path (path_build_tmp);
  g_clear_pointer (&path_build_tmp, g_free);

  if (g_file_query_exists (user_font1, NULL))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", flatpak_file_get_path_cached (user_font1), "/run/host/user-fonts",
                              NULL);
      g_string_append_printf (xml_snippet,
                              "\t<remap-dir as-path=\"%s\">/run/host/user-fonts</remap-dir>\n",
                              flatpak_file_get_path_cached (user_font1));
    }
  else if (g_file_query_exists (user_font2, NULL))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", flatpak_file_get_path_cached (user_font2), "/run/host/user-fonts",
                              NULL);
      g_string_append_printf (xml_snippet,
                              "\t<remap-dir as-path=\"%s\">/run/host/user-fonts</remap-dir>\n",
                              flatpak_file_get_path_cached (user_font2));
    }

  path_build_tmp = g_build_filename (g_get_user_cache_dir (), "fontconfig", NULL);
  user_font_cache = g_file_new_for_path (path_build_tmp);
  g_clear_pointer (&path_build_tmp, g_free);

  if (g_file_query_exists (user_font_cache, NULL))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", flatpak_file_get_path_cached (user_font_cache), "/run/host/user-fonts-cache",
                              NULL);
    }
  else
    {
      /* We ensure these directories are never writable, or fontconfig
         will use them to write the default cache */
      flatpak_bwrap_add_args (bwrap,
                              "--tmpfs", "/run/host/user-fonts-cache",
                              "--remount-ro", "/run/host/user-fonts-cache",
                              NULL);
    }

  g_string_append (xml_snippet,
                   "</fontconfig>\n");

  if (!flatpak_bwrap_add_args_data (bwrap, "font-dirs.xml", xml_snippet->str, xml_snippet->len, "/run/host/font-dirs.xml", NULL))
    g_warning ("Unable to add fontconfig data snippet");
}

static void
add_icon_path_args (FlatpakBwrap *bwrap)
{
  g_autofree gchar *user_icons_path = NULL;
  g_autoptr(GFile) user_icons = NULL;

  if (g_file_test ("/usr/share/icons", G_FILE_TEST_IS_DIR))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", "/usr/share/icons", "/run/host/share/icons",
                              NULL);
    }

  user_icons_path = g_build_filename (g_get_user_data_dir (), "icons", NULL);
  user_icons = g_file_new_for_path (user_icons_path);
  if (g_file_query_exists (user_icons, NULL))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", flatpak_file_get_path_cached (user_icons), "/run/host/user-share/icons",
                              NULL);
    }
}

FlatpakContext *
flatpak_app_compute_permissions (GKeyFile *app_metadata,
                                 GKeyFile *runtime_metadata,
                                 GError  **error)
{
  g_autoptr(FlatpakContext) app_context = NULL;

  app_context = flatpak_context_new ();

  if (runtime_metadata != NULL)
    {
      if (!flatpak_context_load_metadata (app_context, runtime_metadata, error))
        return NULL;

      /* Don't inherit any permissions from the runtime, only things like env vars. */
      flatpak_context_reset_permissions (app_context);
    }

  if (app_metadata != NULL &&
      !flatpak_context_load_metadata (app_context, app_metadata, error))
    return NULL;

  return g_steal_pointer (&app_context);
}

static void
flatpak_run_gc_ids (void)
{
  flatpak_instance_iterate_all_and_gc (NULL);
}

static char *
flatpak_run_allocate_id (int *lock_fd_out)
{
  g_autofree char *user_runtime_dir = flatpak_get_real_xdg_runtime_dir ();
  g_autofree char *base_dir = g_build_filename (user_runtime_dir, ".flatpak", NULL);
  int count;

  g_mkdir_with_parents (base_dir, 0755);

  flatpak_run_gc_ids ();

  for (count = 0; count < 1000; count++)
    {
      g_autofree char *instance_id = NULL;
      g_autofree char *instance_dir = NULL;

      instance_id = g_strdup_printf ("%u", g_random_int ());

      instance_dir = g_build_filename (base_dir, instance_id, NULL);

      /* We use an atomic mkdir to ensure the instance id is unique */
      if (mkdir (instance_dir, 0755) == 0)
        {
          g_autofree char *lock_file = g_build_filename (instance_dir, ".ref", NULL);
          glnx_autofd int lock_fd = -1;
          struct flock l = {
            .l_type = F_RDLCK,
            .l_whence = SEEK_SET,
            .l_start = 0,
            .l_len = 0
          };

          /* Then we take a file lock inside the dir, hold that during
           * setup and in bwrap. Anyone trying to clean up unused
           * directories need to first verify that there is a .ref
           * file and take a write lock on .ref to ensure its not in
           * use. */
          lock_fd = open (lock_file, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
          /* There is a tiny race here between the open creating the file and the lock succeeding.
             We work around that by only gc:ing "old" .ref files */
          if (lock_fd != -1 && fcntl (lock_fd, F_SETLK, &l) == 0)
            {
              *lock_fd_out = glnx_steal_fd (&lock_fd);
              g_debug ("Allocated instance id %s", instance_id);
              return g_steal_pointer (&instance_id);
            }
        }
    }

  return NULL;
}

#ifdef HAVE_DCONF

static void
add_dconf_key_to_keyfile (GKeyFile      *keyfile,
                          DConfClient   *client,
                          const char    *key,
                          DConfReadFlags flags)
{
  g_autofree char *group = g_path_get_dirname (key);
  g_autofree char *k = g_path_get_basename (key);
  GVariant *value = dconf_client_read_full (client, key, flags, NULL);

  if (value)
    {
      g_autofree char *val = g_variant_print (value, TRUE);
      g_key_file_set_value (keyfile, group + 1, k, val);
    }
}

static void
add_dconf_dir_to_keyfile (GKeyFile      *keyfile,
                          DConfClient   *client,
                          const char    *dir,
                          DConfReadFlags flags)
{
  g_auto(GStrv) keys = NULL;
  int i;

  keys = dconf_client_list (client, dir, NULL);
  for (i = 0; keys[i]; i++)
    {
      g_autofree char *k = g_strconcat (dir, keys[i], NULL);
      if (dconf_is_dir (k, NULL))
        add_dconf_dir_to_keyfile (keyfile, client, k, flags);
      else if (dconf_is_key (k, NULL))
        add_dconf_key_to_keyfile (keyfile, client, k, flags);
    }
}

static void
add_dconf_locks_to_list (GString     *s,
                         DConfClient *client,
                         const char  *dir)
{
  g_auto(GStrv) locks = NULL;
  int i;

  locks = dconf_client_list_locks (client, dir, NULL);
  for (i = 0; locks[i]; i++)
    {
      g_string_append (s, locks[i]);
      g_string_append_c (s, '\n');
    }
}

#endif /* HAVE_DCONF */

static void
get_dconf_data (const char  *app_id,
                const char **paths,
                const char  *migrate_path,
                char       **defaults,
                gsize       *defaults_size,
                char       **values,
                gsize       *values_size,
                char       **locks,
                gsize       *locks_size)
{
#ifdef HAVE_DCONF
  DConfClient *client = NULL;
  g_autofree char *prefix = NULL;
#endif
  g_autoptr(GKeyFile) defaults_data = NULL;
  g_autoptr(GKeyFile) values_data = NULL;
  g_autoptr(GString) locks_data = NULL;

  defaults_data = g_key_file_new ();
  values_data = g_key_file_new ();
  locks_data = g_string_new ("");

#ifdef HAVE_DCONF

  client = dconf_client_new ();

  prefix = flatpak_dconf_path_for_app_id (app_id);

  if (migrate_path)
    {
      g_debug ("Add values in dir '%s', prefix is '%s'", migrate_path, prefix);
      if (flatpak_dconf_path_is_similar (migrate_path, prefix))
        add_dconf_dir_to_keyfile (values_data, client, migrate_path, DCONF_READ_USER_VALUE);
      else
        g_warning ("Ignoring D-Conf migrate-path setting %s", migrate_path);
    }

  g_debug ("Add defaults in dir %s", prefix);
  add_dconf_dir_to_keyfile (defaults_data, client, prefix, DCONF_READ_DEFAULT_VALUE);

  g_debug ("Add locks in dir %s", prefix);
  add_dconf_locks_to_list (locks_data, client, prefix);

  /* We allow extra paths for defaults and locks, but not for user values */
  if (paths)
    {
      int i;
      for (i = 0; paths[i]; i++)
        {
          if (dconf_is_dir (paths[i], NULL))
            {
              g_debug ("Add defaults in dir %s", paths[i]);
              add_dconf_dir_to_keyfile (defaults_data, client, paths[i], DCONF_READ_DEFAULT_VALUE);

              g_debug ("Add locks in dir %s", paths[i]);
              add_dconf_locks_to_list (locks_data, client, paths[i]);
            }
          else if (dconf_is_key (paths[i], NULL))
            {
              g_debug ("Add individual key %s", paths[i]);
              add_dconf_key_to_keyfile (defaults_data, client, paths[i], DCONF_READ_DEFAULT_VALUE);
              add_dconf_key_to_keyfile (values_data, client, paths[i], DCONF_READ_USER_VALUE);
            }
          else
            {
              g_warning ("Ignoring settings path '%s': neither dir nor key", paths[i]);
            }
        }
    }
#endif

  *defaults = g_key_file_to_data (defaults_data, defaults_size, NULL);
  *values = g_key_file_to_data (values_data, values_size, NULL);
  *locks_size = locks_data->len;
  *locks = g_string_free (g_steal_pointer (&locks_data), FALSE);

#ifdef HAVE_DCONF
  g_object_unref (client);
#endif
}

static gboolean
flatpak_run_add_dconf_args (FlatpakBwrap *bwrap,
                            const char   *app_id,
                            GKeyFile     *metakey,
                            GError      **error)
{
  g_auto(GStrv) paths = NULL;
  g_autofree char *migrate_path = NULL;
  g_autofree char *defaults = NULL;
  g_autofree char *values = NULL;
  g_autofree char *locks = NULL;
  gsize defaults_size;
  gsize values_size;
  gsize locks_size;

  if (metakey)
    {
      paths = g_key_file_get_string_list (metakey,
                                          FLATPAK_METADATA_GROUP_DCONF,
                                          FLATPAK_METADATA_KEY_DCONF_PATHS,
                                          NULL, NULL);
      migrate_path = g_key_file_get_string (metakey,
                                            FLATPAK_METADATA_GROUP_DCONF,
                                            FLATPAK_METADATA_KEY_DCONF_MIGRATE_PATH,
                                            NULL);
    }

  get_dconf_data (app_id,
                  (const char **) paths,
                  migrate_path,
                  &defaults, &defaults_size,
                  &values, &values_size,
                  &locks, &locks_size);

  if (defaults_size != 0 &&
      !flatpak_bwrap_add_args_data (bwrap,
                                    "dconf-defaults",
                                    defaults, defaults_size,
                                    "/etc/glib-2.0/settings/defaults",
                                    error))
    return FALSE;

  if (locks_size != 0 &&
      !flatpak_bwrap_add_args_data (bwrap,
                                    "dconf-locks",
                                    locks, locks_size,
                                    "/etc/glib-2.0/settings/locks",
                                    error))
    return FALSE;

  /* We do a one-time conversion of existing dconf settings to a keyfile.
   * Only do that once the app stops requesting dconf access.
   */
  if (migrate_path)
    {
      g_autofree char *filename = NULL;

      filename = g_build_filename (g_get_home_dir (),
                                   ".var/app", app_id,
                                   "config/glib-2.0/settings/keyfile",
                                   NULL);

      g_debug ("writing D-Conf values to %s", filename);

      if (values_size != 0 && !g_file_test (filename, G_FILE_TEST_EXISTS))
        {
          g_autofree char *dir = g_path_get_dirname (filename);

          if (g_mkdir_with_parents (dir, 0700) == -1)
            {
              g_warning ("failed creating dirs for %s", filename);
              return FALSE;
            }

          if (!g_file_set_contents (filename, values, values_size, error))
            {
              g_warning ("failed writing %s", filename);
              return FALSE;
            }
        }
    }

  return TRUE;
}

gboolean
flatpak_run_add_app_info_args (FlatpakBwrap   *bwrap,
                               GFile          *app_files,
                               GBytes         *app_deploy_data,
                               const char     *app_extensions,
                               GFile          *runtime_files,
                               GBytes         *runtime_deploy_data,
                               const char     *runtime_extensions,
                               const char     *app_id,
                               const char     *app_branch,
                               const char     *runtime_ref,
                               GFile          *app_id_dir,
                               FlatpakContext *final_app_context,
                               FlatpakContext *cmdline_context,
                               gboolean        sandbox,
                               gboolean        build,
                               gboolean        devel,
                               char          **app_info_path_out,
                               int             instance_id_fd,
                               char          **instance_id_host_dir_out,
                               GError        **error)
{
  g_autofree char *info_path = NULL;
  g_autofree char *bwrapinfo_path = NULL;
  int fd, fd2, fd3;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree char *runtime_path = NULL;
  g_autofree char *old_dest = g_strdup_printf ("/run/user/%d/flatpak-info", getuid ());
  g_auto(GStrv) runtime_ref_parts = g_strsplit (runtime_ref, "/", 0);
  const char *group;
  g_autofree char *instance_id = NULL;
  glnx_autofd int lock_fd = -1;
  g_autofree char *instance_id_host_dir = NULL;
  g_autofree char *instance_id_sandbox_dir = NULL;
  g_autofree char *instance_id_lock_file = NULL;
  g_autofree char *user_runtime_dir = flatpak_get_real_xdg_runtime_dir ();

  instance_id = flatpak_run_allocate_id (&lock_fd);
  if (instance_id == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_SETUP_FAILED, _("Unable to allocate instance id"));

  instance_id_host_dir = g_build_filename (user_runtime_dir, ".flatpak", instance_id, NULL);
  instance_id_sandbox_dir = g_strdup_printf ("/run/user/%d/.flatpak/%s", getuid (), instance_id);
  instance_id_lock_file = g_build_filename (instance_id_sandbox_dir, ".ref", NULL);

  flatpak_bwrap_add_args (bwrap,
                          "--ro-bind",
                          instance_id_host_dir,
                          instance_id_sandbox_dir,
                          "--lock-file",
                          instance_id_lock_file,
                          NULL);
  /* Keep the .ref lock held until we've started bwrap to avoid races */
  flatpak_bwrap_add_noinherit_fd (bwrap, glnx_steal_fd (&lock_fd));

  info_path = g_build_filename (instance_id_host_dir, "info", NULL);

  keyfile = g_key_file_new ();

  if (app_files)
    group = FLATPAK_METADATA_GROUP_APPLICATION;
  else
    group = FLATPAK_METADATA_GROUP_RUNTIME;

  g_key_file_set_string (keyfile, group, FLATPAK_METADATA_KEY_NAME, app_id);
  g_key_file_set_string (keyfile, group, FLATPAK_METADATA_KEY_RUNTIME,
                         runtime_ref);

  g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                         FLATPAK_METADATA_KEY_INSTANCE_ID, instance_id);
  if (app_id_dir)
    {
      g_autofree char *instance_path = g_file_get_path (app_id_dir);
      g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                             FLATPAK_METADATA_KEY_INSTANCE_PATH, instance_path);
    }

  if (app_files)
    {
      g_autofree char *app_path = g_file_get_path (app_files);
      g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                             FLATPAK_METADATA_KEY_APP_PATH, app_path);
    }
  if (app_deploy_data)
    g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                           FLATPAK_METADATA_KEY_APP_COMMIT, flatpak_deploy_data_get_commit (app_deploy_data));
  if (app_extensions && *app_extensions != 0)
    g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                           FLATPAK_METADATA_KEY_APP_EXTENSIONS, app_extensions);
  runtime_path = g_file_get_path (runtime_files);
  g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                         FLATPAK_METADATA_KEY_RUNTIME_PATH, runtime_path);
  if (runtime_deploy_data)
    g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                           FLATPAK_METADATA_KEY_RUNTIME_COMMIT, flatpak_deploy_data_get_commit (runtime_deploy_data));
  if (runtime_extensions && *runtime_extensions != 0)
    g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                           FLATPAK_METADATA_KEY_RUNTIME_EXTENSIONS, runtime_extensions);
  if (app_branch != NULL)
    g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                           FLATPAK_METADATA_KEY_BRANCH, app_branch);
  g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                         FLATPAK_METADATA_KEY_ARCH, runtime_ref_parts[2]);

  g_key_file_set_string (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                         FLATPAK_METADATA_KEY_FLATPAK_VERSION, PACKAGE_VERSION);

  if ((final_app_context->sockets & FLATPAK_CONTEXT_SOCKET_SESSION_BUS) == 0)
    g_key_file_set_boolean (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                            FLATPAK_METADATA_KEY_SESSION_BUS_PROXY, TRUE);

  if ((final_app_context->sockets & FLATPAK_CONTEXT_SOCKET_SYSTEM_BUS) == 0)
    g_key_file_set_boolean (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                            FLATPAK_METADATA_KEY_SYSTEM_BUS_PROXY, TRUE);

  if (sandbox)
    g_key_file_set_boolean (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                            FLATPAK_METADATA_KEY_SANDBOX, TRUE);
  if (build)
    g_key_file_set_boolean (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                            FLATPAK_METADATA_KEY_BUILD, TRUE);
  if (devel)
    g_key_file_set_boolean (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                            FLATPAK_METADATA_KEY_DEVEL, TRUE);

  if (cmdline_context)
    {
      g_autoptr(GPtrArray) cmdline_args = g_ptr_array_new_with_free_func (g_free);
      flatpak_context_to_args (cmdline_context, cmdline_args);
      if (cmdline_args->len > 0)
        {
          g_key_file_set_string_list (keyfile, FLATPAK_METADATA_GROUP_INSTANCE,
                                      FLATPAK_METADATA_KEY_EXTRA_ARGS,
                                      (const char * const *) cmdline_args->pdata,
                                      cmdline_args->len);
        }
    }

  flatpak_context_save_metadata (final_app_context, TRUE, keyfile);

  if (!g_key_file_save_to_file (keyfile, info_path, error))
    return FALSE;

  /* We want to create a file on /.flatpak-info that the app cannot modify, which
     we do by creating a read-only bind mount. This way one can openat()
     /proc/$pid/root, and if that succeeds use openat via that to find the
     unfakable .flatpak-info file. However, there is a tiny race in that if
     you manage to open /proc/$pid/root, but then the pid dies, then
     every mount but the root is unmounted in the namespace, so the
     .flatpak-info will be empty. We fix this by first creating a real file
     with the real info in, then bind-mounting on top of that, the same info.
     This way even if the bind-mount is unmounted we can find the real data.
   */

  fd = open (info_path, O_RDONLY);
  if (fd == -1)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                   _("Failed to open flatpak-info file: %s"), g_strerror (errsv));
      return FALSE;
    }

  fd2 = open (info_path, O_RDONLY);
  if (fd2 == -1)
    {
      close (fd);
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                   _("Failed to open flatpak-info file: %s"), g_strerror (errsv));
      return FALSE;
    }

  flatpak_bwrap_add_args_data_fd (bwrap,
                                  "--file", fd, "/.flatpak-info");
  flatpak_bwrap_add_args_data_fd (bwrap,
                                  "--ro-bind-data", fd2, "/.flatpak-info");
  flatpak_bwrap_add_args (bwrap,
                          "--symlink", "../../../.flatpak-info", old_dest,
                          NULL);

  bwrapinfo_path = g_build_filename (instance_id_host_dir, "bwrapinfo.json", NULL);
  fd3 = open (bwrapinfo_path, O_RDWR | O_CREAT, 0644);
  if (fd3 == -1)
    {
      close (fd);
      close (fd2);
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                   _("Failed to open bwrapinfo.json file: %s"), g_strerror (errsv));
      return FALSE;
    }

  /* NOTE: It is important that this takes place after bwrapinfo.json is created,
     otherwise start notifications in the portal may not work. */
  if (instance_id_fd != -1)
    {
      gsize instance_id_position = 0;
      gsize instance_id_size = strlen (instance_id);

      while (instance_id_size > 0)
        {
          gssize bytes_written = write (instance_id_fd, instance_id + instance_id_position, instance_id_size);
          if (G_UNLIKELY (bytes_written <= 0))
            {
              int errsv = bytes_written == -1 ? errno : ENOSPC;
              if (errsv == EINTR)
                continue;

              close (fd);
              close (fd2);
              close (fd3);

              g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errsv),
                           _("Failed to write to instance id fd: %s"), g_strerror (errsv));
              return FALSE;
            }

          instance_id_position += bytes_written;
          instance_id_size -= bytes_written;
        }

      close (instance_id_fd);
    }

  flatpak_bwrap_add_args_data_fd (bwrap, "--info-fd", fd3, NULL);

  if (app_info_path_out != NULL)
    *app_info_path_out = g_strdup_printf ("/proc/self/fd/%d", fd);

  if (instance_id_host_dir_out != NULL)
    *instance_id_host_dir_out = g_steal_pointer (&instance_id_host_dir);

  return TRUE;
}

static void
add_tzdata_args (FlatpakBwrap *bwrap,
                 GFile *runtime_files)
{
  g_autofree char *timezone = flatpak_get_timezone ();
  g_autofree char *timezone_content = g_strdup_printf ("%s\n", timezone);
  g_autofree char *localtime_content = g_strconcat ("../usr/share/zoneinfo/", timezone, NULL);
  g_autoptr(GFile) runtime_zoneinfo = NULL;

  if (runtime_files)
    runtime_zoneinfo = g_file_resolve_relative_path (runtime_files, "share/zoneinfo");

  /* Check for runtime /usr/share/zoneinfo */
  if (runtime_zoneinfo != NULL && g_file_query_exists (runtime_zoneinfo, NULL))
    {
      /* Check for host /usr/share/zoneinfo */
      if (g_file_test ("/usr/share/zoneinfo", G_FILE_TEST_IS_DIR))
        {
          /* Here we assume the host timezone file exist in the host data */
          flatpak_bwrap_add_args (bwrap,
                                  "--ro-bind", "/usr/share/zoneinfo", "/usr/share/zoneinfo",
                                  "--symlink", localtime_content, "/etc/localtime",
                                  NULL);
        }
      else
        {
          g_autoptr(GFile) runtime_tzfile = g_file_resolve_relative_path (runtime_zoneinfo, timezone);

          /* Check if host timezone file exist in the runtime tzdata */
          if (g_file_query_exists (runtime_tzfile, NULL))
            flatpak_bwrap_add_args (bwrap,
                                    "--symlink", localtime_content, "/etc/localtime",
                                    NULL);
        }
    }

  flatpak_bwrap_add_args_data (bwrap, "timezone",
                               timezone_content, -1, "/etc/timezone",
                               NULL);
}

static void
add_monitor_path_args (gboolean      use_session_helper,
                       FlatpakBwrap *bwrap)
{
  g_autoptr(AutoFlatpakSessionHelper) session_helper = NULL;
  g_autofree char *monitor_path = NULL;
  g_autofree char *pkcs11_socket_path = NULL;
  g_autoptr(GVariant) session_data = NULL;

  if (use_session_helper)
    {
      session_helper =
        flatpak_session_helper_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                       G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                                       "org.freedesktop.Flatpak",
                                                       "/org/freedesktop/Flatpak/SessionHelper",
                                                       NULL, NULL);
    }

  if (session_helper &&
      flatpak_session_helper_call_request_session_sync (session_helper,
                                                        &session_data,
                                                        NULL, NULL))
    {
      if (g_variant_lookup (session_data, "path", "s", &monitor_path))
        flatpak_bwrap_add_args (bwrap,
                                "--ro-bind", monitor_path, "/run/host/monitor",
                                "--symlink", "/run/host/monitor/resolv.conf", "/etc/resolv.conf",
                                "--symlink", "/run/host/monitor/host.conf", "/etc/host.conf",
                                "--symlink", "/run/host/monitor/hosts", "/etc/hosts",
                                NULL);

      if (g_variant_lookup (session_data, "pkcs11-socket", "s", &pkcs11_socket_path))
        {
          g_autofree char *sandbox_pkcs11_socket_path = g_strdup_printf ("/run/user/%d/p11-kit/pkcs11", getuid ());
          const char *trusted_module_contents =
            "# This overrides the runtime p11-kit-trusted module with a client one talking to the trust module on the host\n"
            "module: p11-kit-client.so\n";

          if (flatpak_bwrap_add_args_data (bwrap, "p11-kit-trust.module",
                                           trusted_module_contents, -1,
                                           "/etc/pkcs11/modules/p11-kit-trust.module", NULL))
            {
              flatpak_bwrap_add_args (bwrap,
                                      "--ro-bind", pkcs11_socket_path, sandbox_pkcs11_socket_path,
                                      NULL);
              flatpak_bwrap_unset_env (bwrap, "P11_KIT_SERVER_ADDRESS");
            }
        }
    }
  else
    {
      if (g_file_test ("/etc/resolv.conf", G_FILE_TEST_EXISTS))
        flatpak_bwrap_add_args (bwrap,
                                "--ro-bind", "/etc/resolv.conf", "/etc/resolv.conf",
                                NULL);
      if (g_file_test ("/etc/host.conf", G_FILE_TEST_EXISTS))
        flatpak_bwrap_add_args (bwrap,
                                "--ro-bind", "/etc/host.conf", "/etc/host.conf",
                                NULL);
      if (g_file_test ("/etc/hosts", G_FILE_TEST_EXISTS))
        flatpak_bwrap_add_args (bwrap,
                                "--ro-bind", "/etc/hosts", "/etc/hosts",
                                NULL);
    }
}

static void
add_document_portal_args (FlatpakBwrap *bwrap,
                          const char   *app_id,
                          char        **out_mount_path)
{
  g_autoptr(GDBusConnection) session_bus = NULL;
  g_autofree char *doc_mount_path = NULL;

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (session_bus)
    {
      g_autoptr(GError) local_error = NULL;
      g_autoptr(GDBusMessage) reply = NULL;
      g_autoptr(GDBusMessage) msg =
        g_dbus_message_new_method_call ("org.freedesktop.portal.Documents",
                                        "/org/freedesktop/portal/documents",
                                        "org.freedesktop.portal.Documents",
                                        "GetMountPoint");
      g_dbus_message_set_body (msg, g_variant_new ("()"));
      reply =
        g_dbus_connection_send_message_with_reply_sync (session_bus, msg,
                                                        G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                        30000,
                                                        NULL,
                                                        NULL,
                                                        NULL);
      if (reply)
        {
          if (g_dbus_message_to_gerror (reply, &local_error))
            {
              if (g_error_matches (local_error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN))
                g_debug ("Document portal not available, not mounting /run/user/%d/doc", getuid ());
              else
                g_message ("Can't get document portal: %s", local_error->message);
            }
          else
            {
              g_autofree char *src_path = NULL;
              g_autofree char *dst_path = NULL;
              g_variant_get (g_dbus_message_get_body (reply),
                             "(^ay)", &doc_mount_path);

              src_path = g_strdup_printf ("%s/by-app/%s",
                                          doc_mount_path, app_id);
              dst_path = g_strdup_printf ("/run/user/%d/doc", getuid ());
              flatpak_bwrap_add_args (bwrap, "--bind", src_path, dst_path, NULL);
            }
        }
    }

  *out_mount_path = g_steal_pointer (&doc_mount_path);
}

#ifdef ENABLE_SECCOMP
static const uint32_t seccomp_x86_64_extra_arches[] = { SCMP_ARCH_X86, 0, };

#ifdef SCMP_ARCH_AARCH64
static const uint32_t seccomp_aarch64_extra_arches[] = { SCMP_ARCH_ARM, 0 };
#endif

/*
 * @negative_errno: Result code as returned by libseccomp functions
 *
 * Translate a libseccomp error code into an error message. libseccomp
 * mostly returns negative `errno` values such as `-ENOMEM`, but some
 * standard `errno` values are used for non-standard purposes where their
 * `strerror()` would be misleading.
 *
 * Returns: a string version of @negative_errno if possible
 */
static const char *
flatpak_seccomp_strerror (int negative_errno)
{
  g_return_val_if_fail (negative_errno < 0, "Non-negative error value from libseccomp?");
  g_return_val_if_fail (negative_errno > INT_MIN, "Out of range error value from libseccomp?");

  switch (negative_errno)
    {
      case -EDOM:
        return "Architecture specific failure";

      case -EFAULT:
        return "Internal libseccomp failure (unknown syscall?)";

      case -ECANCELED:
        return "System failure beyond the control of libseccomp";
    }

  /* e.g. -ENOMEM: the result of strerror() is good enough */
  return g_strerror (-negative_errno);
}

static inline void
cleanup_seccomp (void *p)
{
  scmp_filter_ctx *pp = (scmp_filter_ctx *) p;

  if (*pp)
    seccomp_release (*pp);
}

static gboolean
setup_seccomp (FlatpakBwrap   *bwrap,
               const char     *arch,
               gulong          allowed_personality,
               FlatpakRunFlags run_flags,
               GError        **error)
{
  gboolean multiarch = (run_flags & FLATPAK_RUN_FLAG_MULTIARCH) != 0;
  gboolean devel = (run_flags & FLATPAK_RUN_FLAG_DEVEL) != 0;

  __attribute__((cleanup (cleanup_seccomp))) scmp_filter_ctx seccomp = NULL;

  /**** BEGIN NOTE ON CODE SHARING
   *
   * There are today a number of different Linux container
   * implementations.  That will likely continue for long into the
   * future.  But we can still try to share code, and it's important
   * to do so because it affects what library and application writers
   * can do, and we should support code portability between different
   * container tools.
   *
   * This syscall blocklist is copied from linux-user-chroot, which was in turn
   * clearly influenced by the Sandstorm.io blocklist.
   *
   * If you make any changes here, I suggest sending the changes along
   * to other sandbox maintainers.  Using the libseccomp list is also
   * an appropriate venue:
   * https://groups.google.com/forum/#!topic/libseccomp
   *
   * A non-exhaustive list of links to container tooling that might
   * want to share this blocklist:
   *
   *  https://github.com/sandstorm-io/sandstorm
   *    in src/sandstorm/supervisor.c++
   *  https://github.com/flatpak/flatpak.git
   *    in common/flatpak-run.c
   *  https://git.gnome.org/browse/linux-user-chroot
   *    in src/setup-seccomp.c
   *
   * Other useful resources:
   * https://github.com/systemd/systemd/blob/HEAD/src/shared/seccomp-util.c
   * https://github.com/moby/moby/blob/HEAD/profiles/seccomp/default.json
   *
   **** END NOTE ON CODE SHARING
   */
  struct
  {
    int                  scall;
    int                  errnum;
    struct scmp_arg_cmp *arg;
  } syscall_blocklist[] = {
    /* Block dmesg */
    {SCMP_SYS (syslog), EPERM},
    /* Useless old syscall */
    {SCMP_SYS (uselib), EPERM},
    /* Don't allow disabling accounting */
    {SCMP_SYS (acct), EPERM},
    /* 16-bit code is unnecessary in the sandbox, and modify_ldt is a
       historic source of interesting information leaks. */
    {SCMP_SYS (modify_ldt), EPERM},
    /* Don't allow reading current quota use */
    {SCMP_SYS (quotactl), EPERM},

    /* Don't allow access to the kernel keyring */
    {SCMP_SYS (add_key), EPERM},
    {SCMP_SYS (keyctl), EPERM},
    {SCMP_SYS (request_key), EPERM},

    /* Scary VM/NUMA ops */
    {SCMP_SYS (move_pages), EPERM},
    {SCMP_SYS (mbind), EPERM},
    {SCMP_SYS (get_mempolicy), EPERM},
    {SCMP_SYS (set_mempolicy), EPERM},
    {SCMP_SYS (migrate_pages), EPERM},

    /* Don't allow subnamespace setups: */
    {SCMP_SYS (unshare), EPERM},
    {SCMP_SYS (setns), EPERM},
    {SCMP_SYS (mount), EPERM},
    {SCMP_SYS (umount), EPERM},
    {SCMP_SYS (umount2), EPERM},
    {SCMP_SYS (pivot_root), EPERM},
    {SCMP_SYS (chroot), EPERM},
#if defined(__s390__) || defined(__s390x__) || defined(__CRIS__)
    /* Architectures with CONFIG_CLONE_BACKWARDS2: the child stack
     * and flags arguments are reversed so the flags come second */
    {SCMP_SYS (clone), EPERM, &SCMP_A1 (SCMP_CMP_MASKED_EQ, CLONE_NEWUSER, CLONE_NEWUSER)},
#else
    /* Normally the flags come first */
    {SCMP_SYS (clone), EPERM, &SCMP_A0 (SCMP_CMP_MASKED_EQ, CLONE_NEWUSER, CLONE_NEWUSER)},
#endif

    /* Don't allow faking input to the controlling tty (CVE-2017-5226) */
    {SCMP_SYS (ioctl), EPERM, &SCMP_A1 (SCMP_CMP_MASKED_EQ, 0xFFFFFFFFu, (int) TIOCSTI)},

    /* seccomp can't look into clone3()'s struct clone_args to check whether
     * the flags are OK, so we have no choice but to block clone3().
     * Return ENOSYS so user-space will fall back to clone().
     * (GHSA-67h7-w3jq-vh4q; see also https://github.com/moby/moby/commit/9f6b562d) */
    {SCMP_SYS (clone3), ENOSYS},

    /* New mount manipulation APIs can also change our VFS. There's no
     * legitimate reason to do these in the sandbox, so block all of them
     * rather than thinking about which ones might be dangerous.
     * (GHSA-67h7-w3jq-vh4q) */
    {SCMP_SYS (open_tree), ENOSYS},
    {SCMP_SYS (move_mount), ENOSYS},
    {SCMP_SYS (fsopen), ENOSYS},
    {SCMP_SYS (fsconfig), ENOSYS},
    {SCMP_SYS (fsmount), ENOSYS},
    {SCMP_SYS (fspick), ENOSYS},
    {SCMP_SYS (mount_setattr), ENOSYS},
  };

  struct
  {
    int                  scall;
    int                  errnum;
    struct scmp_arg_cmp *arg;
  } syscall_nondevel_blocklist[] = {
    /* Profiling operations; we expect these to be done by tools from outside
     * the sandbox.  In particular perf has been the source of many CVEs.
     */
    {SCMP_SYS (perf_event_open), EPERM},
    /* Don't allow you to switch to bsd emulation or whatnot */
    {SCMP_SYS (personality), EPERM, &SCMP_A0 (SCMP_CMP_NE, allowed_personality)},
    {SCMP_SYS (ptrace), EPERM}
  };
  /* Blocklist all but unix, inet, inet6 and netlink */
  struct
  {
    int             family;
    FlatpakRunFlags flags_mask;
  } socket_family_allowlist[] = {
    /* NOTE: Keep in numerical order */
    { AF_UNSPEC, 0 },
    { AF_LOCAL, 0 },
    { AF_INET, 0 },
    { AF_INET6, 0 },
    { AF_NETLINK, 0 },
    { AF_CAN, FLATPAK_RUN_FLAG_CANBUS },
    { AF_BLUETOOTH, FLATPAK_RUN_FLAG_BLUETOOTH },
  };
  int last_allowed_family;
  int i, r;
  g_auto(GLnxTmpfile) seccomp_tmpf  = { 0, };

  seccomp = seccomp_init (SCMP_ACT_ALLOW);
  if (!seccomp)
    return flatpak_fail_error (error, FLATPAK_ERROR_SETUP_FAILED, _("Initialize seccomp failed"));

  if (arch != NULL)
    {
      uint32_t arch_id = 0;
      const uint32_t *extra_arches = NULL;

      if (strcmp (arch, "i386") == 0)
        {
          arch_id = SCMP_ARCH_X86;
        }
      else if (strcmp (arch, "x86_64") == 0)
        {
          arch_id = SCMP_ARCH_X86_64;
          extra_arches = seccomp_x86_64_extra_arches;
        }
      else if (strcmp (arch, "arm") == 0)
        {
          arch_id = SCMP_ARCH_ARM;
        }
#ifdef SCMP_ARCH_AARCH64
      else if (strcmp (arch, "aarch64") == 0)
        {
          arch_id = SCMP_ARCH_AARCH64;
          extra_arches = seccomp_aarch64_extra_arches;
        }
#endif

      /* We only really need to handle arches on multiarch systems.
       * If only one arch is supported the default is fine */
      if (arch_id != 0)
        {
          /* This *adds* the target arch, instead of replacing the
             native one. This is not ideal, because we'd like to only
             allow the target arch, but we can't really disallow the
             native arch at this point, because then bubblewrap
             couldn't continue running. */
          r = seccomp_arch_add (seccomp, arch_id);
          if (r < 0 && r != -EEXIST)
            return flatpak_fail_error (error, FLATPAK_ERROR_SETUP_FAILED, _("Failed to add architecture to seccomp filter: %s"), flatpak_seccomp_strerror (r));

          if (multiarch && extra_arches != NULL)
            {
              unsigned i;
              for (i = 0; extra_arches[i] != 0; i++)
                {
                  r = seccomp_arch_add (seccomp, extra_arches[i]);
                  if (r < 0 && r != -EEXIST)
                    return flatpak_fail_error (error, FLATPAK_ERROR_SETUP_FAILED, _("Failed to add multiarch architecture to seccomp filter: %s"), flatpak_seccomp_strerror (r));
                }
            }
        }
    }

  /* TODO: Should we filter the kernel keyring syscalls in some way?
   * We do want them to be used by desktop apps, but they could also perhaps
   * leak system stuff or secrets from other apps.
   */

  for (i = 0; i < G_N_ELEMENTS (syscall_blocklist); i++)
    {
      int scall = syscall_blocklist[i].scall;
      int errnum = syscall_blocklist[i].errnum;

      g_return_val_if_fail (errnum == EPERM || errnum == ENOSYS, FALSE);

      if (syscall_blocklist[i].arg)
        r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO (errnum), scall, 1, *syscall_blocklist[i].arg);
      else
        r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO (errnum), scall, 0);

      /* EFAULT means "internal libseccomp error", but in practice we get
       * this for syscall numbers added via flatpak-syscalls-private.h
       * when trying to filter them on a non-native architecture, because
       * libseccomp cannot map the syscall number to a name and back to a
       * number for the non-native architecture. */
      if (r == -EFAULT)
        flatpak_debug2 ("Unable to block syscall %d: syscall not known to libseccomp?",
                        scall);
      else if (r < 0)
        return flatpak_fail_error (error, FLATPAK_ERROR_SETUP_FAILED, _("Failed to block syscall %d: %s"), scall, flatpak_seccomp_strerror (r));
    }

  if (!devel)
    {
      for (i = 0; i < G_N_ELEMENTS (syscall_nondevel_blocklist); i++)
        {
          int scall = syscall_nondevel_blocklist[i].scall;
          int errnum = syscall_nondevel_blocklist[i].errnum;

          g_return_val_if_fail (errnum == EPERM || errnum == ENOSYS, FALSE);

          if (syscall_nondevel_blocklist[i].arg)
            r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO (errnum), scall, 1, *syscall_nondevel_blocklist[i].arg);
          else
            r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO (errnum), scall, 0);

          /* See above for the meaning of EFAULT. */
          if (r == -EFAULT)
            flatpak_debug2 ("Unable to block syscall %d: syscall not known to libseccomp?",
                            scall);
          else if (r < 0)
            return flatpak_fail_error (error, FLATPAK_ERROR_SETUP_FAILED, _("Failed to block syscall %d: %s"), scall, flatpak_seccomp_strerror (r));
        }
    }

  /* Socket filtering doesn't work on e.g. i386, so ignore failures here
   * However, we need to user seccomp_rule_add_exact to avoid libseccomp doing
   * something else: https://github.com/seccomp/libseccomp/issues/8 */
  last_allowed_family = -1;
  for (i = 0; i < G_N_ELEMENTS (socket_family_allowlist); i++)
    {
      int family = socket_family_allowlist[i].family;
      int disallowed;

      if (socket_family_allowlist[i].flags_mask != 0 &&
          (socket_family_allowlist[i].flags_mask & run_flags) != socket_family_allowlist[i].flags_mask)
        continue;

      for (disallowed = last_allowed_family + 1; disallowed < family; disallowed++)
        {
          /* Blocklist the in-between valid families */
          seccomp_rule_add_exact (seccomp, SCMP_ACT_ERRNO (EAFNOSUPPORT), SCMP_SYS (socket), 1, SCMP_A0 (SCMP_CMP_EQ, disallowed));
        }
      last_allowed_family = family;
    }
  /* Blocklist the rest */
  seccomp_rule_add_exact (seccomp, SCMP_ACT_ERRNO (EAFNOSUPPORT), SCMP_SYS (socket), 1, SCMP_A0 (SCMP_CMP_GE, last_allowed_family + 1));

  if (!glnx_open_anonymous_tmpfile_full (O_RDWR | O_CLOEXEC, "/tmp", &seccomp_tmpf, error))
    return FALSE;

  r = seccomp_export_bpf (seccomp, seccomp_tmpf.fd);

  if (r != 0)
    return flatpak_fail_error (error, FLATPAK_ERROR_SETUP_FAILED, _("Failed to export bpf: %s"), flatpak_seccomp_strerror (r));

  lseek (seccomp_tmpf.fd, 0, SEEK_SET);

  flatpak_bwrap_add_args_data_fd (bwrap,
                                  "--seccomp", glnx_steal_fd (&seccomp_tmpf.fd), NULL);

  return TRUE;
}
#endif

static void
flatpak_run_setup_usr_links (FlatpakBwrap *bwrap,
                             GFile        *runtime_files)
{
  int i;

  if (runtime_files == NULL)
    return;

  for (i = 0; flatpak_abs_usrmerged_dirs[i] != NULL; i++)
    {
      const char *subdir = flatpak_abs_usrmerged_dirs[i];
      g_autoptr(GFile) runtime_subdir = NULL;

      g_assert (subdir[0] == '/');
      /* Skip the '/' when using as a subdirectory of the runtime */
      runtime_subdir = g_file_get_child (runtime_files, subdir + 1);

      if (g_file_query_exists (runtime_subdir, NULL))
        {
          g_autofree char *link = g_strconcat ("usr", subdir, NULL);
          flatpak_bwrap_add_args (bwrap,
                                  "--symlink", link, subdir,
                                  NULL);
        }
    }
}

gboolean
flatpak_run_setup_base_argv (FlatpakBwrap   *bwrap,
                             GFile          *runtime_files,
                             GFile          *app_id_dir,
                             const char     *arch,
                             FlatpakRunFlags flags,
                             GError        **error)
{
  g_autofree char *run_dir = NULL;
  g_autofree char *passwd_contents = NULL;
  g_autoptr(GString) group_contents = NULL;
  const char *pkcs11_conf_contents = NULL;
  struct group *g;
  gulong pers;
  gid_t gid = getgid ();
  g_autoptr(GFile) etc = NULL;

  run_dir = g_strdup_printf ("/run/user/%d", getuid ());

  passwd_contents = g_strdup_printf ("%s:x:%d:%d:%s:%s:%s\n"
                                     "nfsnobody:x:65534:65534:Unmapped user:/:/sbin/nologin\n",
                                     g_get_user_name (),
                                     getuid (), gid,
                                     g_get_real_name (),
                                     g_get_home_dir (),
                                     DEFAULT_SHELL);

  group_contents = g_string_new ("");
  g = getgrgid (gid);
  /* if NULL, the primary group is not known outside the container, so
   * it might as well stay unknown inside the container... */
  if (g != NULL)
    g_string_append_printf (group_contents, "%s:x:%d:%s\n",
                            g->gr_name, gid, g_get_user_name ());
  g_string_append (group_contents, "nfsnobody:x:65534:\n");

  pkcs11_conf_contents =
    "# Disable user pkcs11 config, because the host modules don't work in the runtime\n"
    "user-config: none\n";

  if ((flags & FLATPAK_RUN_FLAG_NO_PROC) == 0)
    flatpak_bwrap_add_args (bwrap,
                            "--proc", "/proc",
                            NULL);

  flatpak_bwrap_add_args (bwrap,
                          "--unshare-pid",
                          "--dir", "/tmp",
                          "--dir", "/var/tmp",
                          "--dir", "/run/host",
                          "--dir", run_dir,
                          "--setenv", "XDG_RUNTIME_DIR", run_dir,
                          "--symlink", "../run", "/var/run",
                          "--ro-bind", "/sys/block", "/sys/block",
                          "--ro-bind", "/sys/bus", "/sys/bus",
                          "--ro-bind", "/sys/class", "/sys/class",
                          "--ro-bind", "/sys/dev", "/sys/dev",
                          "--ro-bind", "/sys/devices", "/sys/devices",
                          "--ro-bind-try", "/proc/self/ns/user", "/run/.userns",
                          /* glib uses this like /etc/timezone */
                          "--symlink", "/etc/timezone", "/var/db/zoneinfo",
                          NULL);

  if (flags & FLATPAK_RUN_FLAG_DIE_WITH_PARENT)
    flatpak_bwrap_add_args (bwrap,
                            "--die-with-parent",
                            NULL);

  if (flags & FLATPAK_RUN_FLAG_WRITABLE_ETC)
    flatpak_bwrap_add_args (bwrap,
                            "--dir", "/usr/etc",
                            "--symlink", "usr/etc", "/etc",
                            NULL);

  if (!flatpak_bwrap_add_args_data (bwrap, "passwd", passwd_contents, -1, "/etc/passwd", error))
    return FALSE;

  if (!flatpak_bwrap_add_args_data (bwrap, "group", group_contents->str, -1, "/etc/group", error))
    return FALSE;

  if (!flatpak_bwrap_add_args_data (bwrap, "pkcs11.conf", pkcs11_conf_contents, -1, "/etc/pkcs11/pkcs11.conf", error))
    return FALSE;

  if (g_file_test ("/etc/machine-id", G_FILE_TEST_EXISTS))
    flatpak_bwrap_add_args (bwrap, "--ro-bind", "/etc/machine-id", "/etc/machine-id", NULL);
  else if (g_file_test ("/var/lib/dbus/machine-id", G_FILE_TEST_EXISTS))
    flatpak_bwrap_add_args (bwrap, "--ro-bind", "/var/lib/dbus/machine-id", "/etc/machine-id", NULL);

  if (runtime_files)
    etc = g_file_get_child (runtime_files, "etc");
  if (etc != NULL &&
      (flags & FLATPAK_RUN_FLAG_WRITABLE_ETC) == 0 &&
      g_file_query_exists (etc, NULL))
    {
      g_auto(GLnxDirFdIterator) dfd_iter = { 0, };
      struct dirent *dent;
      gboolean inited;

      inited = glnx_dirfd_iterator_init_at (AT_FDCWD, flatpak_file_get_path_cached (etc), FALSE, &dfd_iter, NULL);

      while (inited)
        {
          g_autofree char *src = NULL;
          g_autofree char *dest = NULL;

          if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&dfd_iter, &dent, NULL, NULL) || dent == NULL)
            break;

          if (strcmp (dent->d_name, "passwd") == 0 ||
              strcmp (dent->d_name, "group") == 0 ||
              strcmp (dent->d_name, "machine-id") == 0 ||
              strcmp (dent->d_name, "resolv.conf") == 0 ||
              strcmp (dent->d_name, "host.conf") == 0 ||
              strcmp (dent->d_name, "hosts") == 0 ||
              strcmp (dent->d_name, "localtime") == 0 ||
              strcmp (dent->d_name, "timezone") == 0 ||
              strcmp (dent->d_name, "pkcs11") == 0)
            continue;

          src = g_build_filename (flatpak_file_get_path_cached (etc), dent->d_name, NULL);
          dest = g_build_filename ("/etc", dent->d_name, NULL);
          if (dent->d_type == DT_LNK)
            {
              g_autofree char *target = NULL;

              target = glnx_readlinkat_malloc (dfd_iter.fd, dent->d_name,
                                               NULL, error);
              if (target == NULL)
                return FALSE;

              flatpak_bwrap_add_args (bwrap, "--symlink", target, dest, NULL);
            }
          else
            {
              flatpak_bwrap_add_args (bwrap, "--ro-bind", src, dest, NULL);
            }
        }
    }

  if (app_id_dir != NULL)
    {
      g_autoptr(GFile) app_cache_dir = g_file_get_child (app_id_dir, "cache");
      g_autoptr(GFile) app_tmp_dir = g_file_get_child (app_cache_dir, "tmp");
      g_autoptr(GFile) app_data_dir = g_file_get_child (app_id_dir, "data");
      g_autoptr(GFile) app_config_dir = g_file_get_child (app_id_dir, "config");

      flatpak_bwrap_add_args (bwrap,
                              /* These are nice to have as a fixed path */
                              "--bind", flatpak_file_get_path_cached (app_cache_dir), "/var/cache",
                              "--bind", flatpak_file_get_path_cached (app_data_dir), "/var/data",
                              "--bind", flatpak_file_get_path_cached (app_config_dir), "/var/config",
                              "--bind", flatpak_file_get_path_cached (app_tmp_dir), "/var/tmp",
                              NULL);
    }

  flatpak_run_setup_usr_links (bwrap, runtime_files);

  add_tzdata_args (bwrap, runtime_files);

  pers = PER_LINUX;

  if ((flags & FLATPAK_RUN_FLAG_SET_PERSONALITY) &&
      flatpak_is_linux32_arch (arch))
    {
      g_debug ("Setting personality linux32");
      pers = PER_LINUX32;
    }

  /* Always set the personallity, and clear all weird flags */
  personality (pers);

#ifdef ENABLE_SECCOMP
  if (!setup_seccomp (bwrap, arch, pers, flags, error))
    return FALSE;
#endif

  if ((flags & FLATPAK_RUN_FLAG_WRITABLE_ETC) == 0)
    add_monitor_path_args ((flags & FLATPAK_RUN_FLAG_NO_SESSION_HELPER) == 0, bwrap);

  return TRUE;
}

static gboolean
forward_file (XdpDbusDocuments *documents,
              const char       *app_id,
              const char       *file,
              char            **out_doc_id,
              GError          **error)
{
  int fd, fd_id;
  g_autofree char *doc_id = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  const char *perms[] = { "read", "write", NULL };

  fd = open (file, O_PATH | O_CLOEXEC);
  if (fd == -1)
    return flatpak_fail (error, _("Failed to open ‘%s’"), file);

  fd_list = g_unix_fd_list_new ();
  fd_id = g_unix_fd_list_append (fd_list, fd, error);
  close (fd);

  if (!xdp_dbus_documents_call_add_sync (documents,
                                         g_variant_new ("h", fd_id),
                                         TRUE, /* reuse */
                                         FALSE, /* not persistent */
                                         fd_list,
                                         &doc_id,
                                         NULL,
                                         NULL,
                                         error))
    {
      if (error)
        g_dbus_error_strip_remote_error (*error);
      return FALSE;
    }

  if (!xdp_dbus_documents_call_grant_permissions_sync (documents,
                                                       doc_id,
                                                       app_id,
                                                       perms,
                                                       NULL,
                                                       error))
    {
      if (error)
        g_dbus_error_strip_remote_error (*error);
      return FALSE;
    }

  *out_doc_id = g_steal_pointer (&doc_id);

  return TRUE;
}

static gboolean
add_rest_args (FlatpakBwrap   *bwrap,
               const char     *app_id,
               FlatpakExports *exports,
               gboolean        file_forwarding,
               const char     *doc_mount_path,
               char           *args[],
               int             n_args,
               GError        **error)
{
  g_autoptr(XdpDbusDocuments) documents = NULL;
  gboolean forwarding = FALSE;
  gboolean forwarding_uri = FALSE;
  gboolean can_forward = TRUE;
  int i;

  if (file_forwarding && doc_mount_path == NULL)
    {
      g_message ("Can't get document portal mount path");
      can_forward = FALSE;
    }
  else if (file_forwarding)
    {
      g_autoptr(GError) local_error = NULL;

      documents = xdp_dbus_documents_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION, 0,
                                                             "org.freedesktop.portal.Documents",
                                                             "/org/freedesktop/portal/documents",
                                                             NULL,
                                                             &local_error);
      if (documents == NULL)
        {
          g_message ("Can't get document portal: %s", local_error->message);
          can_forward = FALSE;
        }
    }

  for (i = 0; i < n_args; i++)
    {
      g_autoptr(GFile) file = NULL;

      if (file_forwarding &&
          (strcmp (args[i], "@@") == 0 ||
           strcmp (args[i], "@@u") == 0))
        {
          forwarding_uri = strcmp (args[i], "@@u") == 0;
          forwarding = !forwarding;
          continue;
        }

      if (can_forward && forwarding)
        {
          if (forwarding_uri)
            {
              if (g_str_has_prefix (args[i], "file:"))
                file = g_file_new_for_uri (args[i]);
              else if (G_IS_DIR_SEPARATOR (args[i][0]))
                file = g_file_new_for_path (args[i]);
            }
          else
            file = g_file_new_for_path (args[i]);
        }

      if (file && !flatpak_exports_path_is_visible (exports,
                                                    flatpak_file_get_path_cached (file)))
        {
          g_autofree char *doc_id = NULL;
          g_autofree char *basename = NULL;
          g_autofree char *doc_path = NULL;
          if (!forward_file (documents, app_id, flatpak_file_get_path_cached (file),
                             &doc_id, error))
            return FALSE;

          basename = g_file_get_basename (file);
          doc_path = g_build_filename (doc_mount_path, doc_id, basename, NULL);

          if (forwarding_uri)
            {
              g_autofree char *path = doc_path;
              doc_path = g_filename_to_uri (path, NULL, NULL);
              /* This should never fail */
              g_assert (doc_path != NULL);
            }

          g_debug ("Forwarding file '%s' as '%s' to %s", args[i], doc_path, app_id);
          flatpak_bwrap_add_arg (bwrap, doc_path);
        }
      else
        flatpak_bwrap_add_arg (bwrap, args[i]);
    }

  return TRUE;
}

FlatpakContext *
flatpak_context_load_for_deploy (FlatpakDeploy *deploy,
                                 GError       **error)
{
  g_autoptr(FlatpakContext) context = NULL;
  g_autoptr(FlatpakContext) overrides = NULL;
  g_autoptr(GKeyFile) metakey = NULL;

  metakey = flatpak_deploy_get_metadata (deploy);
  context = flatpak_app_compute_permissions (metakey, NULL, error);
  if (context == NULL)
    return NULL;

  overrides = flatpak_deploy_get_overrides (deploy);
  flatpak_context_merge (context, overrides);

  return g_steal_pointer (&context);
}

static char *
calculate_ld_cache_checksum (GBytes   *app_deploy_data,
                             GBytes   *runtime_deploy_data,
                             const char *app_extensions,
                             const char *runtime_extensions)
{
  g_autoptr(GChecksum) ld_so_checksum = g_checksum_new (G_CHECKSUM_SHA256);
  if (app_deploy_data)
    g_checksum_update (ld_so_checksum, (guchar *) flatpak_deploy_data_get_commit (app_deploy_data), -1);
  g_checksum_update (ld_so_checksum, (guchar *) flatpak_deploy_data_get_commit (runtime_deploy_data), -1);
  if (app_extensions)
    g_checksum_update (ld_so_checksum, (guchar *) app_extensions, -1);
  if (runtime_extensions)
    g_checksum_update (ld_so_checksum, (guchar *) runtime_extensions, -1);

  return g_strdup (g_checksum_get_string (ld_so_checksum));
}

static gboolean
add_ld_so_conf (FlatpakBwrap *bwrap,
                GError      **error)
{
  const char *contents =
    "include /run/flatpak/ld.so.conf.d/app-*.conf\n"
    "include /app/etc/ld.so.conf\n"
    "/app/lib\n"
    "include /run/flatpak/ld.so.conf.d/runtime-*.conf\n";

  return flatpak_bwrap_add_args_data (bwrap, "ld-so-conf",
                                      contents, -1, "/etc/ld.so.conf", error);
}

static int
regenerate_ld_cache (GPtrArray    *base_argv_array,
                     GArray       *base_fd_array,
                     GFile        *app_id_dir,
                     const char   *checksum,
                     GFile        *runtime_files,
                     gboolean      generate_ld_so_conf,
                     GCancellable *cancellable,
                     GError      **error)
{
  g_autoptr(FlatpakBwrap) bwrap = NULL;
  g_autoptr(GArray) combined_fd_array = NULL;
  g_autoptr(GFile) ld_so_cache = NULL;
  g_autoptr(GFile) ld_so_cache_tmp = NULL;
  g_autofree char *sandbox_cache_path = NULL;
  g_autofree char *tmp_basename = NULL;
  g_auto(GStrv) minimal_envp = NULL;
  g_autofree char *commandline = NULL;
  int exit_status;
  glnx_autofd int ld_so_fd = -1;
  g_autoptr(GFile) ld_so_dir = NULL;

  if (app_id_dir)
    ld_so_dir = g_file_get_child (app_id_dir, ".ld.so");
  else
    {
      g_autoptr(GFile) base_dir = g_file_new_for_path (g_get_user_cache_dir ());
      ld_so_dir = g_file_resolve_relative_path (base_dir, "flatpak/ld.so");
    }

  ld_so_cache = g_file_get_child (ld_so_dir, checksum);
  ld_so_fd = open (flatpak_file_get_path_cached (ld_so_cache), O_RDONLY);
  if (ld_so_fd >= 0)
    return glnx_steal_fd (&ld_so_fd);

  g_debug ("Regenerating ld.so.cache %s", flatpak_file_get_path_cached (ld_so_cache));

  if (!flatpak_mkdir_p (ld_so_dir, cancellable, error))
    return FALSE;

  minimal_envp = flatpak_run_get_minimal_env (FALSE, FALSE);
  bwrap = flatpak_bwrap_new (minimal_envp);

  flatpak_bwrap_append_args (bwrap, base_argv_array);

  flatpak_run_setup_usr_links (bwrap, runtime_files);

  if (generate_ld_so_conf)
    {
      if (!add_ld_so_conf (bwrap, error))
        return -1;
    }
  else
    flatpak_bwrap_add_args (bwrap,
                            "--symlink", "../usr/etc/ld.so.conf", "/etc/ld.so.conf",
                            NULL);

  tmp_basename = g_strconcat (checksum, ".XXXXXX", NULL);
  glnx_gen_temp_name (tmp_basename);

  sandbox_cache_path = g_build_filename ("/run/ld-so-cache-dir", tmp_basename, NULL);
  ld_so_cache_tmp = g_file_get_child (ld_so_dir, tmp_basename);

  flatpak_bwrap_add_args (bwrap,
                          "--unshare-pid",
                          "--unshare-ipc",
                          "--unshare-net",
                          "--proc", "/proc",
                          "--dev", "/dev",
                          "--bind", flatpak_file_get_path_cached (ld_so_dir), "/run/ld-so-cache-dir",
                          NULL);

  if (!flatpak_bwrap_bundle_args (bwrap, 1, -1, FALSE, error))
    return -1;

  flatpak_bwrap_add_args (bwrap,
                          "ldconfig", "-X", "-C", sandbox_cache_path, NULL);

  flatpak_bwrap_finish (bwrap);

  commandline = flatpak_quote_argv ((const char **) bwrap->argv->pdata, -1);
  g_debug ("Running: '%s'", commandline);

  combined_fd_array = g_array_new (FALSE, TRUE, sizeof (int));
  g_array_append_vals (combined_fd_array, base_fd_array->data, base_fd_array->len);
  g_array_append_vals (combined_fd_array, bwrap->fds->data, bwrap->fds->len);

  /* We use LEAVE_DESCRIPTORS_OPEN to work around dead-lock, see flatpak_close_fds_workaround */
  if (!g_spawn_sync (NULL,
                     (char **) bwrap->argv->pdata,
                     bwrap->envp,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                     flatpak_bwrap_child_setup_cb, combined_fd_array,
                     NULL, NULL,
                     &exit_status,
                     error))
    return -1;

  if (!WIFEXITED (exit_status) || WEXITSTATUS (exit_status) != 0)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_SETUP_FAILED,
                          _("ldconfig failed, exit status %d"), exit_status);
      return -1;
    }

  ld_so_fd = open (flatpak_file_get_path_cached (ld_so_cache_tmp), O_RDONLY);
  if (ld_so_fd < 0)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_SETUP_FAILED, _("Can't open generated ld.so.cache"));
      return -1;
    }

  if (app_id_dir == NULL)
    {
      /* For runs without an app id dir we always regenerate the ld.so.cache */
      unlink (flatpak_file_get_path_cached (ld_so_cache_tmp));
    }
  else
    {
      g_autoptr(GFile) active = g_file_get_child (ld_so_dir, "active");

      /* For app-dirs we keep one checksum alive, by pointing the active symlink to it */

      /* Rename to known name, possibly overwriting existing ref if race */
      if (rename (flatpak_file_get_path_cached (ld_so_cache_tmp), flatpak_file_get_path_cached (ld_so_cache)) == -1)
        {
          glnx_set_error_from_errno (error);
          return -1;
        }

      if (!flatpak_switch_symlink_and_remove (flatpak_file_get_path_cached (active),
                                              checksum, error))
        return -1;
    }

  return glnx_steal_fd (&ld_so_fd);
}

/* Check that this user is actually allowed to run this app. When running
 * from the gnome-initial-setup session, an app filter might not be available. */
static gboolean
check_parental_controls (const char     *app_ref,
                         FlatpakDeploy  *deploy,
                         GCancellable   *cancellable,
                         GError        **error)
{
#ifdef HAVE_LIBMALCONTENT
  g_auto(GStrv) app_ref_parts = NULL;
  g_autoptr(MctManager) manager = NULL;
  g_autoptr(MctAppFilter) app_filter = NULL;
  g_autoptr(GAsyncResult) app_filter_result = NULL;
  g_autoptr(GDBusConnection) system_bus = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GDesktopAppInfo) app_info = NULL;
  gboolean allowed = FALSE;

  app_ref_parts = flatpak_decompose_ref (app_ref, error);
  if (app_ref_parts == NULL)
    return FALSE;

  system_bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, error);
  if (system_bus == NULL)
    return FALSE;

  manager = mct_manager_new (system_bus);
  app_filter = mct_manager_get_app_filter (manager, getuid (),
                                           MCT_GET_APP_FILTER_FLAGS_INTERACTIVE,
                                           cancellable, &local_error);
  if (g_error_matches (local_error, MCT_APP_FILTER_ERROR, MCT_APP_FILTER_ERROR_DISABLED))
    {
      g_debug ("Skipping parental controls check for %s since parental "
               "controls are disabled globally", app_ref);
      return TRUE;
    }
  else if (local_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  /* Always filter by app ID. Additionally, filter by app info (which runs
   * multiple checks, including whether the app ID, executable path and
   * content types are allowed) if available. If the flatpak contains
   * multiple .desktop files, we use the main one. The app ID check is
   * always done, as the binary executed by `flatpak run` isn’t necessarily
   * extracted from a .desktop file. */
  allowed = mct_app_filter_is_flatpak_ref_allowed (app_filter, app_ref);

  /* Look up the app’s main .desktop file. */
  if (deploy != NULL && allowed)
    {
      g_autoptr(GFile) deploy_dir = NULL;
      const char *deploy_path;
      g_autofree char *desktop_file_name = NULL;
      g_autofree char *desktop_file_path = NULL;

      deploy_dir = flatpak_deploy_get_dir (deploy);
      deploy_path = flatpak_file_get_path_cached (deploy_dir);

      desktop_file_name = g_strconcat (app_ref_parts[1], ".desktop", NULL);
      desktop_file_path = g_build_path (G_DIR_SEPARATOR_S,
                                        deploy_path,
                                        "export",
                                        "share",
                                        "applications",
                                        desktop_file_name,
                                        NULL);
      app_info = g_desktop_app_info_new_from_filename (desktop_file_path);
    }

  if (app_info != NULL)
    allowed = allowed && mct_app_filter_is_appinfo_allowed (app_filter,
                                                            G_APP_INFO (app_info));

  if (!allowed)
    return flatpak_fail_error (error, FLATPAK_ERROR_PERMISSION_DENIED,
                               /* Translators: The placeholder is for an app ref. */
                               _("Running %s is not allowed by the policy set by your administrator"),
                               app_ref);
#endif  /* HAVE_LIBMALCONTENT */

  return TRUE;
}

static int
open_namespace_fd_if_needed (const char *path,
                             const char *other_path) {
  struct stat s, other_s;

  if (stat (path, &s) != 0)
    return -1; /* No such namespace, ignore */

  if (stat (other_path, &other_s) != 0)
    return -1; /* No such namespace, ignore */

  /* setns calls fail if the process is already in the desired namespace, hence the
     check here to ensure the namespaces are different. */
  if (s.st_ino != other_s.st_ino)
    return open (path, O_RDONLY|O_CLOEXEC);

  return -1;
}

static gboolean
check_sudo (GError **error)
{
  const char *sudo_command_env = g_getenv ("SUDO_COMMAND");
  g_auto(GStrv) split_command = NULL;

  /* This check exists to stop accidental usage of `sudo flatpak run`
     and is not to prevent running as root.
   */

  if (!sudo_command_env)
    return TRUE;

  /* SUDO_COMMAND could be a value like `/usr/bin/flatpak run foo` */
  split_command = g_strsplit (sudo_command_env, " ", 2);
  if (g_str_has_suffix (split_command[0], "flatpak"))
    return flatpak_fail_error (error, FLATPAK_ERROR, _("\"flatpak run\" is not intended to be ran with sudo"));

  return TRUE;
}

gboolean
flatpak_run_app (const char     *app_ref,
                 FlatpakDeploy  *app_deploy,
                 FlatpakContext *extra_context,
                 const char     *custom_runtime,
                 const char     *custom_runtime_version,
                 const char     *custom_runtime_commit,
                 int             parent_pid,
                 FlatpakRunFlags flags,
                 const char     *cwd,
                 const char     *custom_command,
                 char           *args[],
                 int             n_args,
                 int             instance_id_fd,
                 char          **instance_dir_out,
                 GCancellable   *cancellable,
                 GError        **error)
{
  g_autoptr(FlatpakDeploy) runtime_deploy = NULL;
  g_autoptr(GBytes) runtime_deploy_data = NULL;
  g_autoptr(GBytes) app_deploy_data = NULL;
  g_autoptr(GFile) app_files = NULL;
  g_autoptr(GFile) runtime_files = NULL;
  g_autoptr(GFile) bin_ldconfig = NULL;
  g_autoptr(GFile) app_id_dir = NULL;
  g_autoptr(GFile) real_app_id_dir = NULL;
  g_autofree char *default_runtime = NULL;
  g_autofree char *default_command = NULL;
  g_autofree char *runtime_ref = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GKeyFile) runtime_metakey = NULL;
  g_autoptr(FlatpakBwrap) bwrap = NULL;
  const char *command = "/bin/sh";
  g_autoptr(GError) my_error = NULL;
  g_auto(GStrv) runtime_parts = NULL;
  int i;
  g_autoptr(GPtrArray) previous_app_id_dirs = NULL;
  g_autofree char *app_info_path = NULL;
  g_autofree char *instance_id_host_dir = NULL;
  g_autoptr(FlatpakContext) app_context = NULL;
  g_autoptr(FlatpakContext) overrides = NULL;
  g_autoptr(FlatpakExports) exports = NULL;
  g_auto(GStrv) app_ref_parts = NULL;
  g_autofree char *commandline = NULL;
  g_autofree char *doc_mount_path = NULL;
  g_autofree char *app_extensions = NULL;
  g_autofree char *runtime_extensions = NULL;
  g_autofree char *checksum = NULL;
  int ld_so_fd = -1;
  g_autoptr(GFile) runtime_ld_so_conf = NULL;
  gboolean generate_ld_so_conf = TRUE;
  gboolean use_ld_so_cache = TRUE;
  gboolean sandboxed = (flags & FLATPAK_RUN_FLAG_SANDBOX) != 0;
  gboolean parent_expose_pids = (flags & FLATPAK_RUN_FLAG_PARENT_EXPOSE_PIDS) != 0;

  struct stat s;

  if (!check_sudo (error))
    return FALSE;

  app_ref_parts = flatpak_decompose_ref (app_ref, error);
  if (app_ref_parts == NULL)
    return FALSE;

  /* Check the user is allowed to run this flatpak. */
  if (!check_parental_controls (app_ref, app_deploy, cancellable, error))
    return FALSE;

  /* Construct the bwrap context. */
  bwrap = flatpak_bwrap_new (NULL);
  flatpak_bwrap_add_arg (bwrap, flatpak_get_bwrap ());

  if (app_deploy == NULL)
    {
      g_assert (g_str_has_prefix (app_ref, "runtime/"));
      default_runtime = g_strdup (app_ref + strlen ("runtime/"));
    }
  else
    {
      const gchar *key;

      app_deploy_data = flatpak_deploy_get_deploy_data (app_deploy, FLATPAK_DEPLOY_VERSION_ANY, cancellable, error);
      if (app_deploy_data == NULL)
        return FALSE;

      if ((flags & FLATPAK_RUN_FLAG_DEVEL) != 0)
        key = FLATPAK_METADATA_KEY_SDK;
      else
        key = FLATPAK_METADATA_KEY_RUNTIME;

      metakey = flatpak_deploy_get_metadata (app_deploy);
      default_runtime = g_key_file_get_string (metakey,
                                               FLATPAK_METADATA_GROUP_APPLICATION,
                                               key, &my_error);
      if (my_error)
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return FALSE;
        }
    }

  runtime_parts = g_strsplit (default_runtime, "/", 0);
  if (g_strv_length (runtime_parts) != 3)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Wrong number of components in runtime %s"), default_runtime);

  if (custom_runtime)
    {
      g_auto(GStrv) custom_runtime_parts = g_strsplit (custom_runtime, "/", 0);

      for (i = 0; i < 3 && custom_runtime_parts[i] != NULL; i++)
        {
          if (strlen (custom_runtime_parts[i]) > 0)
            {
              g_free (runtime_parts[i]);
              runtime_parts[i] = g_steal_pointer (&custom_runtime_parts[i]);
            }
        }
    }

  if (custom_runtime_version)
    {
      g_free (runtime_parts[2]);
      runtime_parts[2] = g_strdup (custom_runtime_version);
    }

  runtime_ref = flatpak_compose_ref (FALSE,
                                     runtime_parts[0],
                                     runtime_parts[2],
                                     runtime_parts[1],
                                     error);
  if (runtime_ref == NULL)
    return FALSE;

  runtime_deploy = flatpak_find_deploy_for_ref (runtime_ref, custom_runtime_commit, cancellable, error);
  if (runtime_deploy == NULL)
    return FALSE;

  runtime_deploy_data = flatpak_deploy_get_deploy_data (runtime_deploy, FLATPAK_DEPLOY_VERSION_ANY, cancellable, error);
  if (runtime_deploy_data == NULL)
    return FALSE;

  runtime_metakey = flatpak_deploy_get_metadata (runtime_deploy);

  app_context = flatpak_app_compute_permissions (metakey, runtime_metakey, error);
  if (app_context == NULL)
    return FALSE;

  if (app_deploy != NULL)
    {
      overrides = flatpak_deploy_get_overrides (app_deploy);
      flatpak_context_merge (app_context, overrides);
    }

  if (sandboxed)
    flatpak_context_make_sandboxed (app_context);

  if (extra_context)
    flatpak_context_merge (app_context, extra_context);

  runtime_files = flatpak_deploy_get_files (runtime_deploy);
  bin_ldconfig = g_file_resolve_relative_path (runtime_files, "bin/ldconfig");
  if (!g_file_query_exists (bin_ldconfig, NULL))
    use_ld_so_cache = FALSE;

  if (app_deploy != NULL)
    {
      g_autofree const char **previous_ids = NULL;
      gsize len = 0;
      gboolean do_migrate;
      int i;

      real_app_id_dir = flatpak_get_data_dir (app_ref_parts[1]);
      app_files = flatpak_deploy_get_files (app_deploy);

      previous_app_id_dirs = g_ptr_array_new_with_free_func (g_object_unref);
      previous_ids = flatpak_deploy_data_get_previous_ids (app_deploy_data, &len);

      do_migrate = !g_file_query_exists (real_app_id_dir, cancellable);

      /* When migrating, find most recent old existing source and rename that to
       * the new name.
       *
       * We ignore other names than that. For more recent names that don't exist
       * we never ran them so nothing will even reference them. For older names
       * either they were not used, or they were used but then the more recent
       * name was used and a symlink to it was created.
       *
       * This means we may end up with a chain of symlinks: oldest -> old -> current.
       * This is unfortunate but not really a problem, but for robustness reasons we
       * don't want to mess with user files unnecessary. For example, the app dir could
       * actually be a symlink for other reasons. Imagine for instance that you want to put the
       * steam games somewhere else so you leave the app dir as a symlink to /mnt/steam.
       */
      for (i = len - 1; i >= 0; i--)
        {
          g_autoptr(GFile) previous_app_id_dir = NULL;
          g_autoptr(GFileInfo) previous_app_id_dir_info = NULL;
          g_autoptr(GError) local_error = NULL;

          previous_app_id_dir = flatpak_get_data_dir (previous_ids[i]);
          previous_app_id_dir_info = g_file_query_info (previous_app_id_dir,
                                                        G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK ","
                                                        G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
                                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                        cancellable,
                                                        &local_error);
          /* Warn about the migration failures, but don't make them fatal, then you can never run the app */
          if (previous_app_id_dir_info == NULL)
            {
              if  (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) && do_migrate)
                {
                  g_warning (_("Failed to migrate from %s: %s"), flatpak_file_get_path_cached (previous_app_id_dir),
                             local_error->message);
                  do_migrate = FALSE; /* Don't migrate older things, they are likely symlinks to the thing that we failed on */
                }

              g_clear_error (&local_error);
              continue;
            }

          if (do_migrate)
            {
              do_migrate = FALSE; /* Don't migrate older things, they are likely symlinks to this dir */

              if (!flatpak_file_rename (previous_app_id_dir, real_app_id_dir, cancellable, &local_error))
                {
                  g_warning (_("Failed to migrate old app data directory %s to new name %s: %s"),
                             flatpak_file_get_path_cached (previous_app_id_dir), app_ref_parts[1],
                             local_error->message);
                }
              else
                {
                  /* Leave a symlink in place of the old data dir */
                  if (!g_file_make_symbolic_link (previous_app_id_dir, app_ref_parts[1], cancellable, &local_error))
                    {
                      g_warning (_("Failed to create symlink while migrating %s: %s"),
                                 flatpak_file_get_path_cached (previous_app_id_dir),
                                 local_error->message);
                    }
                }
            }

          /* Give app access to this old dir */
          g_ptr_array_add (previous_app_id_dirs, g_steal_pointer (&previous_app_id_dir));
        }

      if (!flatpak_ensure_data_dir (real_app_id_dir, cancellable, error))
        return FALSE;

      if (!sandboxed)
        app_id_dir = g_object_ref (real_app_id_dir);
    }

  flatpak_run_apply_env_default (bwrap, use_ld_so_cache);
  flatpak_run_apply_env_vars (bwrap, app_context);
  flatpak_run_apply_env_prompt (bwrap, app_ref_parts[1]);

  if (real_app_id_dir)
    {
      g_autoptr(GFile) sandbox_dir = g_file_get_child (real_app_id_dir, "sandbox");
      flatpak_bwrap_set_env (bwrap, "FLATPAK_SANDBOX_DIR", flatpak_file_get_path_cached (sandbox_dir), TRUE);
    }

  flatpak_bwrap_add_args (bwrap,
                          "--ro-bind", flatpak_file_get_path_cached (runtime_files), "/usr",
                          "--lock-file", "/usr/.ref",
                          NULL);

  if (app_files != NULL)
    flatpak_bwrap_add_args (bwrap,
                            "--ro-bind", flatpak_file_get_path_cached (app_files), "/app",
                            "--lock-file", "/app/.ref",
                            NULL);
  else
    flatpak_bwrap_add_args (bwrap,
                            "--dir", "/app",
                            NULL);

  if (metakey != NULL &&
      !flatpak_run_add_extension_args (bwrap, metakey, app_ref, use_ld_so_cache, &app_extensions, cancellable, error))
    return FALSE;

  if (!flatpak_run_add_extension_args (bwrap, runtime_metakey, runtime_ref, use_ld_so_cache, &runtime_extensions, cancellable, error))
    return FALSE;

  runtime_ld_so_conf = g_file_resolve_relative_path (runtime_files, "etc/ld.so.conf");
  if (lstat (flatpak_file_get_path_cached (runtime_ld_so_conf), &s) == 0)
    generate_ld_so_conf = S_ISREG (s.st_mode) && s.st_size == 0;

  /* At this point we have the minimal argv set up, with just the app, runtime and extensions.
     We can reuse this to generate the ld.so.cache (if needed) */
  if (use_ld_so_cache)
    {
      checksum = calculate_ld_cache_checksum (app_deploy_data, runtime_deploy_data,
                                              app_extensions, runtime_extensions);
      ld_so_fd = regenerate_ld_cache (bwrap->argv,
                                      bwrap->fds,
                                      app_id_dir,
                                      checksum,
                                      runtime_files,
                                      generate_ld_so_conf,
                                      cancellable, error);
      if (ld_so_fd == -1)
        return FALSE;
      flatpak_bwrap_add_fd (bwrap, ld_so_fd);
    }

  flags |= flatpak_context_get_run_flags (app_context);

  if (!flatpak_run_setup_base_argv (bwrap, runtime_files, app_id_dir, app_ref_parts[2], flags, error))
    return FALSE;

  if (generate_ld_so_conf)
    {
      if (!add_ld_so_conf (bwrap, error))
        return FALSE;
    }

  if (ld_so_fd != -1)
    {
      /* Don't add to fd_array, its already there */
      flatpak_bwrap_add_arg (bwrap, "--ro-bind-data");
      flatpak_bwrap_add_arg_printf (bwrap, "%d", ld_so_fd);
      flatpak_bwrap_add_arg (bwrap, "/etc/ld.so.cache");
    }

  if (!flatpak_run_add_app_info_args (bwrap,
                                      app_files, app_deploy_data, app_extensions,
                                      runtime_files, runtime_deploy_data, runtime_extensions,
                                      app_ref_parts[1], app_ref_parts[3],
                                      runtime_ref, app_id_dir, app_context, extra_context,
                                      sandboxed, FALSE, flags & FLATPAK_RUN_FLAG_DEVEL,
                                      &app_info_path, instance_id_fd, &instance_id_host_dir,
                                      error))
    return FALSE;

  if (!flatpak_run_add_dconf_args (bwrap, app_ref_parts[1], metakey, error))
    return FALSE;

  if (!sandboxed && !(flags & FLATPAK_RUN_FLAG_NO_DOCUMENTS_PORTAL))
    add_document_portal_args (bwrap, app_ref_parts[1], &doc_mount_path);

  if (!flatpak_run_add_environment_args (bwrap, app_info_path, flags,
                                         app_ref_parts[1], app_context, app_id_dir, previous_app_id_dirs,
                                         &exports, cancellable, error))
    return FALSE;

  flatpak_run_add_journal_args (bwrap);
  add_font_path_args (bwrap);
  add_icon_path_args (bwrap);

  flatpak_bwrap_add_args (bwrap,
                          /* Not in base, because we don't want this for flatpak build */
                          "--symlink", "/app/lib/debug/source", "/run/build",
                          "--symlink", "/usr/lib/debug/source", "/run/build-runtime",
                          NULL);

  if (cwd)
    flatpak_bwrap_add_args (bwrap, "--chdir", cwd, NULL);

  if (parent_expose_pids)
    {
      g_autofree char *userns_path = NULL;
      g_autofree char *pidns_path = NULL;
      g_autofree char *userns2_path = NULL;
      int userns_fd, userns2_fd, pidns_fd;

      if (parent_pid == 0)
        return flatpak_fail (error, "No parent pid specified");

      userns_path = g_strdup_printf ("/proc/%d/root/run/.userns", parent_pid);

      userns_fd = open_namespace_fd_if_needed (userns_path, "/proc/self/ns/user");
      if (userns_fd != -1)
        {
          flatpak_bwrap_add_args_data_fd (bwrap, "--userns", userns_fd, NULL);

          userns2_path = g_strdup_printf ("/proc/%d/ns/user", parent_pid);
          userns2_fd = open_namespace_fd_if_needed (userns2_path, userns_path);
          if (userns2_fd != -1)
            flatpak_bwrap_add_args_data_fd (bwrap, "--userns2", userns2_fd, NULL);
        }

      pidns_path = g_strdup_printf ("/proc/%d/ns/pid", parent_pid);
      pidns_fd = open (pidns_path, O_RDONLY|O_CLOEXEC);
      if (pidns_fd != -1)
        flatpak_bwrap_add_args_data_fd (bwrap, "--pidns", pidns_fd, NULL);
    }

  if (custom_command)
    {
      command = custom_command;
    }
  else if (metakey)
    {
      default_command = g_key_file_get_string (metakey,
                                               FLATPAK_METADATA_GROUP_APPLICATION,
                                               FLATPAK_METADATA_KEY_COMMAND,
                                               &my_error);
      if (my_error)
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return FALSE;
        }
      command = default_command;
    }

  flatpak_bwrap_envp_to_args (bwrap);

  if (!flatpak_bwrap_bundle_args (bwrap, 1, -1, FALSE, error))
    return FALSE;

  flatpak_bwrap_add_arg (bwrap, command);

  if (!add_rest_args (bwrap, app_ref_parts[1],
                      exports, (flags & FLATPAK_RUN_FLAG_FILE_FORWARDING) != 0,
                      doc_mount_path,
                      args, n_args, error))
    return FALSE;

  flatpak_bwrap_finish (bwrap);

  commandline = flatpak_quote_argv ((const char **) bwrap->argv->pdata, -1);
  g_debug ("Running '%s'", commandline);

  if ((flags & FLATPAK_RUN_FLAG_BACKGROUND) != 0)
    {
      GPid child_pid;
      char pid_str[64];
      g_autofree char *pid_path = NULL;
      GSpawnFlags spawn_flags;

      spawn_flags = G_SPAWN_SEARCH_PATH;
      if (flags & FLATPAK_RUN_FLAG_DO_NOT_REAP)
        spawn_flags |= G_SPAWN_DO_NOT_REAP_CHILD;

      /* We use LEAVE_DESCRIPTORS_OPEN to work around dead-lock, see flatpak_close_fds_workaround */
      spawn_flags |= G_SPAWN_LEAVE_DESCRIPTORS_OPEN;

      /* flatpak_bwrap_envp_to_args() moved the environment variables to
       * be set into --setenv instructions in argv, so the environment
       * in which the bwrap command runs must be empty. */
      g_assert (bwrap->envp != NULL);
      g_assert (bwrap->envp[0] == NULL);

      if (!g_spawn_async (NULL,
                          (char **) bwrap->argv->pdata,
                          bwrap->envp,
                          spawn_flags,
                          flatpak_bwrap_child_setup_cb, bwrap->fds,
                          &child_pid,
                          error))
        return FALSE;

      g_snprintf (pid_str, sizeof (pid_str), "%d", child_pid);
      pid_path = g_build_filename (instance_id_host_dir, "pid", NULL);
      g_file_set_contents (pid_path, pid_str, -1, NULL);
    }
  else
    {
      char pid_str[64];
      g_autofree char *pid_path = NULL;

      g_snprintf (pid_str, sizeof (pid_str), "%d", getpid ());
      pid_path = g_build_filename (instance_id_host_dir, "pid", NULL);
      g_file_set_contents (pid_path, pid_str, -1, NULL);

      /* Ensure we unset O_CLOEXEC for marked fds and rewind fds as needed.
       * Note that this does not close fds that are not already marked O_CLOEXEC, because
       * we do want to allow inheriting fds into flatpak run. */
      flatpak_bwrap_child_setup (bwrap->fds, FALSE);

      /* flatpak_bwrap_envp_to_args() moved the environment variables to
       * be set into --setenv instructions in argv, so the environment
       * in which the bwrap command runs must be empty. */
      g_assert (bwrap->envp != NULL);
      g_assert (bwrap->envp[0] == NULL);

      if (execvpe (flatpak_get_bwrap (), (char **) bwrap->argv->pdata, bwrap->envp) == -1)
        {
          g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                               _("Unable to start app"));
          return FALSE;
        }
      /* Not actually reached... */
    }

  if (instance_dir_out)
    *instance_dir_out = g_steal_pointer (&instance_id_host_dir);

  return TRUE;
}
