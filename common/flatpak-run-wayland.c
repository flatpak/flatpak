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
#include "flatpak-run-wayland-private.h"

#ifdef ENABLE_WAYLAND_SECURITY_CONTEXT
#include <sys/socket.h>
#include <sys/un.h>
#include <wayland-client.h>
#include "security-context-v1-protocol.h"
#endif

#include "flatpak-utils-private.h"

#ifdef ENABLE_WAYLAND_SECURITY_CONTEXT

static void registry_handle_global (void *data, struct wl_registry *registry,
                                    uint32_t name, const char *interface,
                                    uint32_t version)
{
  struct wp_security_context_manager_v1 **out = data;

  if (strcmp (interface, wp_security_context_manager_v1_interface.name) == 0)
    {
      *out = wl_registry_bind (registry, name,
                               &wp_security_context_manager_v1_interface, 1);
    }
}

static void registry_handle_global_remove (void *data,
                                           struct wl_registry *registry,
                                           uint32_t name)
{
  /* no-op */
}

static const struct wl_registry_listener registry_listener = {
  .global = registry_handle_global,
  .global_remove = registry_handle_global_remove,
};

static char *
create_wl_socket (char *template)
{
  g_autofree char *user_runtime_dir = flatpak_get_real_xdg_runtime_dir ();
  g_autofree char *proxy_socket_dir = g_build_filename (user_runtime_dir, ".flatpak/wl", NULL);
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
flatpak_run_add_wayland_security_context_args (FlatpakBwrap *bwrap,
                                               const char   *app_id,
                                               const char   *instance_id,
                                               gboolean     *available_out)
{
  gboolean res = FALSE;
  struct wl_display *display;
  struct wl_registry *registry;
  struct wp_security_context_manager_v1 *security_context_manager = NULL;
  struct wp_security_context_v1 *security_context;
  struct sockaddr_un sockaddr = {0};
  g_autofree char *socket_path = NULL;
  int listen_fd = -1, sync_fd, ret;

  *available_out = TRUE;

  display = wl_display_connect (NULL);
  if (!display)
    return FALSE;

  registry = wl_display_get_registry (display);
  wl_registry_add_listener (registry, &registry_listener,
                            &security_context_manager);
  ret = wl_display_roundtrip (display);
  wl_registry_destroy (registry);
  if (ret < 0)
    goto out;

  if (!security_context_manager)
    {
      *available_out = FALSE;
      goto out;
    }

  socket_path = create_wl_socket ("wayland-XXXXXX");
  if (!socket_path)
    goto out;

  unlink (socket_path);

  listen_fd = socket (AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd < 0)
    goto out;

  sockaddr.sun_family = AF_UNIX;
  snprintf (sockaddr.sun_path, sizeof(sockaddr.sun_path), "%s", socket_path);
  if (bind (listen_fd, (struct sockaddr *) &sockaddr, sizeof (sockaddr)) != 0)
    goto out;

  if (listen (listen_fd, 0) != 0)
    goto out;

  sync_fd = flatpak_bwrap_add_sync_fd (bwrap);
  if (sync_fd < 0)
    goto out;

  security_context = wp_security_context_manager_v1_create_listener (security_context_manager,
                                                                     listen_fd,
                                                                     sync_fd);
  wp_security_context_v1_set_sandbox_engine (security_context, "org.flatpak");
  wp_security_context_v1_set_app_id (security_context, app_id);
  wp_security_context_v1_set_instance_id (security_context, instance_id);
  wp_security_context_v1_commit (security_context);
  wp_security_context_v1_destroy (security_context);
  if (wl_display_roundtrip (display) < 0)
    goto out;

  flatpak_bwrap_add_args (bwrap,
                          "--ro-bind", socket_path, "/run/flatpak/wayland-0",
                          NULL);
  flatpak_bwrap_set_env (bwrap, "WAYLAND_DISPLAY", "/run/flatpak/wayland-0", TRUE);

  res = TRUE;

out:
  if (listen_fd >= 0)
    close (listen_fd);
  if (security_context_manager)
    wp_security_context_manager_v1_destroy (security_context_manager);
  wl_display_disconnect (display);
  return res;
}

#endif /* ENABLE_WAYLAND_SECURITY_CONTEXT */

/**
 * flatpak_run_add_wayland_args:
 *
 * Returns: %TRUE if a Wayland socket was found.
 */
gboolean
flatpak_run_add_wayland_args (FlatpakBwrap *bwrap,
                              const char   *app_id,
                              const char   *instance_id)
{
  const char *wayland_display;
  g_autofree char *user_runtime_dir = flatpak_get_real_xdg_runtime_dir ();
  g_autofree char *wayland_socket = NULL;
  g_autofree char *sandbox_wayland_socket = NULL;
  gboolean res = FALSE;
  struct stat statbuf;

#ifdef ENABLE_WAYLAND_SECURITY_CONTEXT
  gboolean security_context_available = FALSE;
  if (flatpak_run_add_wayland_security_context_args (bwrap, app_id, instance_id,
                                                     &security_context_available))
    return TRUE;
  /* If security-context is available but we failed to set it up, bail out */
  if (security_context_available)
    return FALSE;
#endif /* ENABLE_WAYLAND_SECURITY_CONTEXT */

  wayland_display = g_getenv ("WAYLAND_DISPLAY");
  if (!wayland_display)
    wayland_display = "wayland-0";

  if (wayland_display[0] == '/')
    wayland_socket = g_strdup (wayland_display);
  else
    wayland_socket = g_build_filename (user_runtime_dir, wayland_display, NULL);

  if (!g_str_has_prefix (wayland_display, "wayland-") ||
      strchr (wayland_display, '/') != NULL)
    {
      wayland_display = "wayland-0";
      flatpak_bwrap_set_env (bwrap, "WAYLAND_DISPLAY", wayland_display, TRUE);
    }

  sandbox_wayland_socket = g_strdup_printf ("/run/flatpak/%s", wayland_display);

  if (stat (wayland_socket, &statbuf) == 0 &&
      (statbuf.st_mode & S_IFMT) == S_IFSOCK)
    {
      res = TRUE;
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", wayland_socket, sandbox_wayland_socket,
                              NULL);
      flatpak_bwrap_add_runtime_dir_member (bwrap, wayland_display);
    }
  return res;
}
