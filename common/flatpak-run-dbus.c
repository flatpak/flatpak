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
#include "flatpak-run-dbus-private.h"

#include <glib/gi18n-lib.h>

#include <sys/vfs.h>

#include "flatpak-utils-private.h"

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
  flatpak_bwrap_add_args (bwrap, "--perms", "0600", NULL);
  flatpak_bwrap_add_args_data_fd (bwrap, "--file", g_steal_fd (&app_info_fd), "/.flatpak-info");

  if (!flatpak_bwrap_bundle_args (bwrap, 1, -1, FALSE, error))
    return FALSE;

  /* End of options: the next argument will be the executable name */
  flatpak_bwrap_add_arg (bwrap, "--");

  return TRUE;
}

gboolean
flatpak_run_maybe_start_dbus_proxy (FlatpakBwrap *app_bwrap,
                                    FlatpakBwrap *proxy_arg_bwrap,
                                    const char   *app_info_path,
                                    GError      **error)
{
  char x = 'x';
  const char *proxy;
  g_autofree char *commandline = NULL;
  g_autoptr(FlatpakBwrap) proxy_bwrap = NULL;
  int proxy_start_index;
  int sync_fd;

  if (flatpak_bwrap_is_empty (proxy_arg_bwrap))
    {
      g_debug ("D-Bus proxy not needed");
      return TRUE;
    }

  proxy_bwrap = flatpak_bwrap_new (NULL);

  if (!add_bwrap_wrapper (proxy_bwrap, app_info_path, error))
    return FALSE;

  proxy = g_getenv ("FLATPAK_DBUSPROXY");
  if (proxy == NULL)
    proxy = DBUSPROXY;

  flatpak_bwrap_add_arg (proxy_bwrap, proxy);

  proxy_start_index = proxy_bwrap->argv->len;

  sync_fd = flatpak_bwrap_add_sync_fd (app_bwrap);
  if (sync_fd < 0)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           _("Unable to create sync pipe"));
      return FALSE;
    }

  flatpak_bwrap_add_fd (proxy_bwrap, sync_fd);
  flatpak_bwrap_add_arg_printf (proxy_bwrap, "--fd=%d", sync_fd);

  /* Note: This steals the fds from proxy_arg_bwrap */
  flatpak_bwrap_append_bwrap (proxy_bwrap, proxy_arg_bwrap);

  if (!flatpak_bwrap_bundle_args (proxy_bwrap, proxy_start_index, -1, TRUE, error))
    return FALSE;

  flatpak_bwrap_finish (proxy_bwrap);

  commandline = flatpak_quote_argv ((const char **) proxy_bwrap->argv->pdata, -1);
  g_info ("Running '%s'", commandline);

  /* We use LEAVE_DESCRIPTORS_OPEN and close them in the child_setup
   * to work around a deadlock in GLib < 2.60 */
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
  if (read (app_bwrap->sync_fds[0], &x, 1) != 1)
    {
      g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           _("Failed to sync with dbus proxy"));
      return FALSE;
    }

  return TRUE;
}

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

static char *
create_proxy_socket (const char *template)
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

gboolean
flatpak_run_add_session_dbus_args (FlatpakBwrap   *app_bwrap,
                                   FlatpakBwrap   *proxy_arg_bwrap,
                                   FlatpakContext *context,
                                   FlatpakRunFlags flags,
                                   const char     *app_id)
{
  static const char sandbox_socket_path[] = "/run/flatpak/bus";
  static const char sandbox_dbus_address[] = "unix:path=/run/flatpak/bus";
  gboolean unrestricted, no_proxy;
  const char *dbus_address = g_getenv ("DBUS_SESSION_BUS_ADDRESS");
  g_autofree char *dbus_session_socket = NULL;

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
    g_info ("Allowing session-dbus access");

  no_proxy = (flags & FLATPAK_RUN_FLAG_NO_SESSION_BUS_PROXY) != 0;

  if (dbus_session_socket != NULL && unrestricted)
    {
      flatpak_bwrap_add_args (app_bwrap,
                              "--ro-bind", dbus_session_socket, sandbox_socket_path,
                              NULL);
      flatpak_bwrap_set_env (app_bwrap, "DBUS_SESSION_BUS_ADDRESS", sandbox_dbus_address, TRUE);
      flatpak_bwrap_add_runtime_dir_member (app_bwrap, "bus");

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
      flatpak_bwrap_add_runtime_dir_member (app_bwrap, "bus");

      return TRUE;
    }

  return FALSE;
}

gboolean
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
    g_info ("Allowing system-dbus access");

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

gboolean
flatpak_run_add_a11y_dbus_args (FlatpakBwrap   *app_bwrap,
                                FlatpakBwrap   *proxy_arg_bwrap,
                                FlatpakContext *context,
                                FlatpakRunFlags flags)
{
  static const char sandbox_socket_path[] = "/run/flatpak/at-spi-bus";
  static const char sandbox_dbus_address[] = "unix:path=/run/flatpak/at-spi-bus";
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

  flatpak_bwrap_add_args (proxy_arg_bwrap,
                          a11y_address,
                          proxy_socket, "--filter", "--sloppy-names",
                          "--broadcast=org.a11y.atspi.Registry.EventListenerRegistered=@/org/a11y/atspi/registry",
                          "--broadcast=org.a11y.atspi.Registry.EventListenerDeregistered=@/org/a11y/atspi/registry",
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
