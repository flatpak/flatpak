/*
 * Copyright © 2018 Red Hat, Inc
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include <gdk-pixbuf/gdk-pixbuf.h>

static int
validate_icon (int max_width,
               int max_height,
               const char *filename)
{
  GdkPixbufFormat *format;
  int width, height;
  const char *name;
  const char *allowed_formats[] = { "png", "jpeg", "svg", NULL };
  g_autoptr(GdkPixbuf) pixbuf = NULL;
  g_autoptr(GError) error = NULL;

  format = gdk_pixbuf_get_file_info (filename, &width, &height);
  if (format == NULL) 
    {
      g_printerr ("Format not recognized\n");
      return 1;
    }

  name = gdk_pixbuf_format_get_name (format);
  if (!g_strv_contains (allowed_formats, name))
    {
      g_printerr ("Format %s not accepted\n", name);
      return 1;
    }

  if (width > max_width || height > max_height)
    {
      g_printerr ("Image too large (%dx%d)\n", width, height);
      return 1;
    }

  pixbuf = gdk_pixbuf_new_from_file (filename, &error);
  if (pixbuf == NULL)
    {
      g_printerr ("Failed to load image: %s\n", error->message);
      return 1;
    }

  return 0;
}

int
main (int argc, char *argv[])
{
  int width;
  int height;
  const char *path;

  if (argc != 4)
    {
      g_printerr ("Usage: %s WIDTH HEIGHT PATH\n", argv[0]);
      return 1;
    }

  width = g_ascii_strtoll (argv[1], NULL, 10);
  if (width < 16 || width > 4096)
    {
      g_printerr ("Bad width limit: %s\n", argv[1]);
      return 1;
    }

  height = g_ascii_strtoll (argv[2], NULL, 10);
  if (height < 16 || height > 4096)
    {
      g_printerr ("Bad height limit: %s\n", argv[2]);
      return 1;
    }

  path = argv[3];

  return validate_icon (width, height, path);
}
