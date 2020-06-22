/*
 * Copyright © 2019 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include "flatpak-utils-base-private.h"

#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>
#include "libglnx/libglnx.h"

char *
flatpak_get_timezone (void)
{
  g_autofree gchar *symlink = NULL;
  gchar *etc_timezone = NULL;
  const gchar *tzdir;
  const gchar *default_tzdir = "/usr/share/zoneinfo";

  symlink = flatpak_resolve_link ("/etc/localtime", NULL);
  if (symlink != NULL)
    {
      /* Resolve relative path */
      g_autofree gchar *canonical = flatpak_canonicalize_filename (symlink);
      char *canonical_suffix;

      /* Strip the prefix and slashes if possible. */

      tzdir = getenv ("TZDIR");
      if (tzdir != NULL && g_str_has_prefix (canonical, tzdir))
        {
          canonical_suffix = canonical + strlen (tzdir);
          while (*canonical_suffix == '/')
            canonical_suffix++;

          return g_strdup (canonical_suffix);
        }

      if (g_str_has_prefix (canonical, default_tzdir))
        {
          canonical_suffix = canonical + strlen (default_tzdir);
          while (*canonical_suffix == '/')
            canonical_suffix++;

          return g_strdup (canonical_suffix);
        }
    }

  if (g_file_get_contents ("/etc/timezone", &etc_timezone,
                           NULL, NULL))
    {
      g_strchomp (etc_timezone);
      return etc_timezone;
    }

  /* Final fall-back is UTC */
  return g_strdup ("UTC");
}

char *
flatpak_readlink (const char *path,
                  GError    **error)
{
  return glnx_readlinkat_malloc (-1, path, NULL, error);
}

char *
flatpak_resolve_link (const char *path,
                      GError    **error)
{
  g_autofree char *link = flatpak_readlink (path, error);
  g_autofree char *dirname = NULL;

  if (link == NULL)
    return NULL;

  if (g_path_is_absolute (link))
    return g_steal_pointer (&link);

  dirname = g_path_get_dirname (path);
  return g_build_filename (dirname, link, NULL);
}

char *
flatpak_canonicalize_filename (const char *path)
{
  g_autoptr(GFile) file = g_file_new_for_path (path);
  return g_file_get_path (file);
}

/* There is a dead-lock in glib versions before 2.60 when it closes
 * the fds. See:  https://gitlab.gnome.org/GNOME/glib/merge_requests/490
 * This was hitting the test-suite a lot, so we work around it by using
 * the G_SPAWN_LEAVE_DESCRIPTORS_OPEN/G_SUBPROCESS_FLAGS_INHERIT_FDS flag
 * and setting CLOEXEC ourselves.
 */
void
flatpak_close_fds_workaround (int start_fd)
{
  int max_open_fds = sysconf (_SC_OPEN_MAX);
  int fd;

  for (fd = start_fd; fd < max_open_fds; fd++)
    fcntl (fd, F_SETFD, FD_CLOEXEC);
}
