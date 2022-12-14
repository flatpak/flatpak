/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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

/* The canonical copy of this file is in:
 * - https://github.com/flatpak/flatpak at icon-validator/validate-icon.c
 * Known copies of this file are in:
 * - https://github.com/flatpak/xdg-desktop-portal at src/validate-icon.c
 */

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <unistd.h>

#define ICON_VALIDATOR_GROUP "Icon Validator"

static int
validate_icon (const char *arg_width,
               const char *arg_height,
               const char *filename)
{
  GdkPixbufFormat *format;
  int max_width, max_height;
  int width, height;
  const char *name;
  const char *allowed_formats[] = { "png", "jpeg", "svg", NULL };
  g_autoptr(GdkPixbuf) pixbuf = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GKeyFile) key_file = NULL;
  g_autofree char *key_file_data = NULL;

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

  if (!g_str_equal (name, "svg"))
    {
      max_width = g_ascii_strtoll (arg_width, NULL, 10);
      if (max_width < 16 || max_width > 4096)
        {
          g_printerr ("Bad width limit: %s\n", arg_width);
          return 1;
        }

      max_height = g_ascii_strtoll (arg_height, NULL, 10);
      if (max_height < 16 || max_height > 4096)
        {
          g_printerr ("Bad height limit: %s\n", arg_height);
          return 1;
        }
    }
  else
    {
      /* Sanity check for vector files */
      max_height = max_width = 4096;
    }

  if (width > max_width || height > max_height)
    {
      g_printerr ("Image too large (%dx%d). Max. size %dx%d\n", width, height, max_width, max_height);
      return 1;
    }

  pixbuf = gdk_pixbuf_new_from_file (filename, &error);
  if (pixbuf == NULL)
    {
      g_printerr ("Failed to load image: %s\n", error->message);
      return 1;
    }

  if (width != height)
    {
      g_printerr ("Expected a square icon but got: %dx%d\n", width, height);
      return 1;
    }

  /* Print the format and size for consumption by (at least) the dynamic
   * launcher portal. xdg-desktop-portal has a copy of this file. Use a
   * GKeyFile so the output can be easily extended in the future in a backwards
   * compatible way.
   */
  key_file = g_key_file_new ();
  g_key_file_set_string (key_file, ICON_VALIDATOR_GROUP, "format", name);
  g_key_file_set_integer (key_file, ICON_VALIDATOR_GROUP, "width", width);
  key_file_data = g_key_file_to_data (key_file, NULL, NULL);
  g_print ("%s", key_file_data);

  return 0;
}

G_GNUC_NULL_TERMINATED
static void
add_args (GPtrArray *argv_array, ...)
{
  va_list args;
  const char *arg;

  va_start (args, argv_array);
  while ((arg = va_arg (args, const gchar *)))
    g_ptr_array_add (argv_array, g_strdup (arg));
  va_end (args);
}

static const char *
flatpak_get_bwrap (void)
{
  const char *e = g_getenv ("FLATPAK_BWRAP");

  if (e != NULL)
    return e;
  return HELPER;
}


static gboolean
path_is_usrmerged (const char *dir)
{
  /* does /dir point to /usr/dir? */
  g_autofree char *target = NULL;
  GStatBuf stat_buf_src, stat_buf_target;

  if (g_stat (dir, &stat_buf_src) < 0)
    return FALSE;

  target = g_strdup_printf ("/usr/%s", dir);

  if (g_stat (target, &stat_buf_target) < 0)
    return FALSE;

  return (stat_buf_src.st_dev == stat_buf_target.st_dev) &&
         (stat_buf_src.st_ino == stat_buf_target.st_ino);
}

static int
rerun_in_sandbox (const char *arg_width,
                  const char *arg_height,
                  const char *filename)
{
  const char * const usrmerged_dirs[] = { "bin", "lib32", "lib64", "lib", "sbin" };
  int i;
  g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func (g_free);
  char validate_icon[PATH_MAX + 1];
  ssize_t symlink_size;

  symlink_size = readlink ("/proc/self/exe", validate_icon, sizeof (validate_icon) - 1);
  if (symlink_size < 0 || (size_t) symlink_size >= sizeof (validate_icon))
    {
      g_printerr ("Error: failed to read /proc/self/exe\n");
      return 1;
    }

  validate_icon[symlink_size] = 0;

  add_args (args,
            flatpak_get_bwrap (),
            "--unshare-ipc",
            "--unshare-net",
            "--unshare-pid",
            "--ro-bind", "/usr", "/usr",
            "--ro-bind-try", "/etc/ld.so.cache", "/etc/ld.so.cache",
            "--ro-bind", validate_icon, validate_icon,
            NULL);

  /* These directories might be symlinks into /usr/... */
  for (i = 0; i < G_N_ELEMENTS (usrmerged_dirs); i++)
    {
      g_autofree char *absolute_dir = g_strdup_printf ("/%s", usrmerged_dirs[i]);

      if (!g_file_test (absolute_dir, G_FILE_TEST_EXISTS))
        continue;

      if (path_is_usrmerged (absolute_dir))
        {
          g_autofree char *symlink_target = g_strdup_printf ("/usr/%s", absolute_dir);

          add_args (args,
                    "--symlink", symlink_target, absolute_dir,
                    NULL);
        }
      else
        {
          add_args (args,
                    "--ro-bind", absolute_dir, absolute_dir,
                    NULL);
        }
    }

  add_args (args,
            "--tmpfs", "/tmp",
            "--proc", "/proc",
            "--dev", "/dev",
            "--chdir", "/",
            "--setenv", "GIO_USE_VFS", "local",
            "--unsetenv", "TMPDIR",
            "--die-with-parent",
            "--ro-bind", filename, filename,
            NULL);

  if (g_getenv ("G_MESSAGES_DEBUG"))
    add_args (args, "--setenv", "G_MESSAGES_DEBUG", g_getenv ("G_MESSAGES_DEBUG"), NULL);
  if (g_getenv ("G_MESSAGES_PREFIXED"))
    add_args (args, "--setenv", "G_MESSAGES_PREFIXED", g_getenv ("G_MESSAGES_PREFIXED"), NULL);

  add_args (args, validate_icon, arg_width, arg_height, filename, NULL);
  g_ptr_array_add (args, NULL);

  {
    g_autofree char *cmdline = g_strjoinv (" ", (char **) args->pdata);
    g_info ("Icon validation: Spawning %s", cmdline);
  }

  execvpe (flatpak_get_bwrap (), (char **) args->pdata, NULL);
  /* If we get here, then execvpe() failed. */
  g_printerr ("Icon validation: execvpe %s: %s\n", flatpak_get_bwrap (), g_strerror (errno));
  return 1;
}

static gboolean opt_sandbox;

static GOptionEntry entries[] = {
  { "sandbox", 0, 0, G_OPTION_ARG_NONE, &opt_sandbox, "Run in a sandbox", NULL },
  { NULL }
};

int
main (int argc, char *argv[])
{
  GOptionContext *context;
  GError *error = NULL;

  context = g_option_context_new ("WIDTH HEIGHT PATH");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("Error: %s\n", error->message);
      return 1;
    }

  if (argc != 4)
    {
      g_printerr ("Usage: %s [OPTION…] WIDTH HEIGHT PATH\n", argv[0]);
      return 1;
    }

  if (opt_sandbox)
    return rerun_in_sandbox (argv[1], argv[2], argv[3]);
  else
    return validate_icon (argv[1], argv[2], argv[3]);
}
