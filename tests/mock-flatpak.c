/*
 * Copyright © 2018-2021 Collabora Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "libglnx.h"

#include <glib.h>
#include <gio/gio.h>

#include "flatpak-context-private.h"

int
main (int argc,
      char **argv)
{
  int i;

  g_debug ("This is a mock implementation of `flatpak run` for the portal");

  for (i = 0; i < argc; i++)
    g_print ("argv[%d] = %s\n", i, argv[i]);

  for (i = 0; i < argc; i++)
    {
      if (g_str_has_prefix (argv[i], "--env-fd="))
        {
          g_autoptr(FlatpakContext) context = flatpak_context_new ();
          const char *value = argv[i] + strlen ("--env-fd=");
          g_autoptr(GError) error = NULL;
          guint64 fd;
          gchar *endptr;
          GHashTableIter iter;
          gpointer k, v;

          fd = g_ascii_strtoull (value, &endptr, 10);

          if (endptr == NULL || *endptr != '\0' || fd > G_MAXINT)
            g_error ("Not a valid file descriptor: %s", value);

          flatpak_context_parse_env_fd (context, (int) fd, &error);
          g_assert_no_error (error);

          g_hash_table_iter_init (&iter, context->env_vars);

          while (g_hash_table_iter_next (&iter, &k, &v))
            g_print ("env[%s] = %s\n", (const char *) k, (const char *) v);
        }
    }

  for (i = 0; i < 256; i++)
    {
      struct stat stat_buf;

      if (fstat (i, &stat_buf) < 0)
        {
          int saved_errno = errno;

          g_assert_cmpint (saved_errno, ==, EBADF);
        }
      else
        {
          g_print ("fd[%d] = (dev=%" G_GUINT64_FORMAT " ino=%" G_GUINT64_FORMAT ")\n",
                   i,
                   (guint64) stat_buf.st_dev,
                   (guint64) stat_buf.st_ino);
        }
    }

  return 0;
}
