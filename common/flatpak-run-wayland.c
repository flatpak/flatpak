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

#include "flatpak-utils-private.h"

/**
 * flatpak_run_add_wayland_args:
 *
 * Returns: %TRUE if a Wayland socket was found.
 */
gboolean
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
