/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 1995-1998 Free Software Foundation, Inc.
 * Copyright © 2014-2019 Red Hat, Inc
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

#include <glib/gi18n-lib.h>

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <termios.h>

#include <glib.h>
#include <gio/gunixoutputstream.h>
#include <gio/gunixinputstream.h>

#include "flatpak-dir-private.h"
#include "flatpak-error.h"
#include "flatpak-oci-registry-private.h"
#include "flatpak-run-private.h"
#include "flatpak-utils-base-private.h"
#include "flatpak-utils-private.h"
#include "flatpak-variant-impl-private.h"
#include "flatpak-xml-utils-private.h"
#include "libglnx.h"
#include "valgrind-private.h"

/* This is also here so the common code can report these errors to the lib */
static const GDBusErrorEntry flatpak_error_entries[] = {
  {FLATPAK_ERROR_ALREADY_INSTALLED,     "org.freedesktop.Flatpak.Error.AlreadyInstalled"},
  {FLATPAK_ERROR_NOT_INSTALLED,         "org.freedesktop.Flatpak.Error.NotInstalled"},
  {FLATPAK_ERROR_ONLY_PULLED,           "org.freedesktop.Flatpak.Error.OnlyPulled"}, /* Since: 1.0 */
  {FLATPAK_ERROR_DIFFERENT_REMOTE,      "org.freedesktop.Flatpak.Error.DifferentRemote"}, /* Since: 1.0 */
  {FLATPAK_ERROR_ABORTED,               "org.freedesktop.Flatpak.Error.Aborted"}, /* Since: 1.0 */
  {FLATPAK_ERROR_SKIPPED,               "org.freedesktop.Flatpak.Error.Skipped"}, /* Since: 1.0 */
  {FLATPAK_ERROR_NEED_NEW_FLATPAK,      "org.freedesktop.Flatpak.Error.NeedNewFlatpak"}, /* Since: 1.0 */
  {FLATPAK_ERROR_REMOTE_NOT_FOUND,      "org.freedesktop.Flatpak.Error.RemoteNotFound"}, /* Since: 1.0 */
  {FLATPAK_ERROR_RUNTIME_NOT_FOUND,     "org.freedesktop.Flatpak.Error.RuntimeNotFound"}, /* Since: 1.0 */
  {FLATPAK_ERROR_DOWNGRADE,             "org.freedesktop.Flatpak.Error.Downgrade"}, /* Since: 1.0 */
  {FLATPAK_ERROR_INVALID_REF,           "org.freedesktop.Flatpak.Error.InvalidRef"}, /* Since: 1.0.3 */
  {FLATPAK_ERROR_INVALID_DATA,          "org.freedesktop.Flatpak.Error.InvalidData"}, /* Since: 1.0.3 */
  {FLATPAK_ERROR_UNTRUSTED,             "org.freedesktop.Flatpak.Error.Untrusted"}, /* Since: 1.0.3 */
  {FLATPAK_ERROR_SETUP_FAILED,          "org.freedesktop.Flatpak.Error.SetupFailed"}, /* Since: 1.0.3 */
  {FLATPAK_ERROR_EXPORT_FAILED,         "org.freedesktop.Flatpak.Error.ExportFailed"}, /* Since: 1.0.3 */
  {FLATPAK_ERROR_REMOTE_USED,           "org.freedesktop.Flatpak.Error.RemoteUsed"}, /* Since: 1.0.3 */
  {FLATPAK_ERROR_RUNTIME_USED,          "org.freedesktop.Flatpak.Error.RuntimeUsed"}, /* Since: 1.0.3 */
  {FLATPAK_ERROR_INVALID_NAME,          "org.freedesktop.Flatpak.Error.InvalidName"}, /* Since: 1.0.3 */
  {FLATPAK_ERROR_OUT_OF_SPACE,          "org.freedesktop.Flatpak.Error.OutOfSpace"}, /* Since: 1.2.0 */
  {FLATPAK_ERROR_WRONG_USER,            "org.freedesktop.Flatpak.Error.WrongUser"}, /* Since: 1.2.0 */
  {FLATPAK_ERROR_NOT_CACHED,            "org.freedesktop.Flatpak.Error.NotCached"}, /* Since: 1.3.3 */
  {FLATPAK_ERROR_REF_NOT_FOUND,         "org.freedesktop.Flatpak.Error.RefNotFound"}, /* Since: 1.4.0 */
  {FLATPAK_ERROR_PERMISSION_DENIED,     "org.freedesktop.Flatpak.Error.PermissionDenied"}, /* Since: 1.5.1 */
  {FLATPAK_ERROR_AUTHENTICATION_FAILED, "org.freedesktop.Flatpak.Error.AuthenticationFailed"}, /* Since: 1.7.3 */
  {FLATPAK_ERROR_NOT_AUTHORIZED,        "org.freedesktop.Flatpak.Error.NotAuthorized"}, /* Since: 1.7.3 */
};

typedef struct archive FlatpakAutoArchiveRead;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakAutoArchiveRead, archive_read_free)

static void
propagate_libarchive_error (GError        **error,
                            struct archive *a)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "%s", archive_error_string (a));
}

GQuark
flatpak_error_quark (void)
{
  static volatile gsize quark_volatile = 0;

  g_dbus_error_register_error_domain ("flatpak-error-quark",
                                      &quark_volatile,
                                      flatpak_error_entries,
                                      G_N_ELEMENTS (flatpak_error_entries));
  return (GQuark) quark_volatile;
}

gboolean
flatpak_fail_error (GError **error, FlatpakError code, const char *fmt, ...)
{
  if (error == NULL)
    return FALSE;

  va_list args;
  va_start (args, fmt);
  GError *new = g_error_new_valist (FLATPAK_ERROR, code, fmt, args);
  va_end (args);
  g_propagate_error (error, g_steal_pointer (&new));
  return FALSE;
}

GBytes *
flatpak_zlib_compress_bytes (GBytes *bytes,
                             int level,
                             GError **error)
{
  g_autoptr(GZlibCompressor) compressor = NULL;
  g_autoptr(GOutputStream) out = NULL;
  g_autoptr(GOutputStream) mem = NULL;

  mem = g_memory_output_stream_new_resizable ();

  compressor = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, level);
  out = g_converter_output_stream_new (mem, G_CONVERTER (compressor));

  if (!g_output_stream_write_all (out, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes),
                                  NULL, NULL, error))
    return NULL;

  if (!g_output_stream_close (out, NULL, error))
    return NULL;

  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (mem));
}

GBytes *
flatpak_zlib_decompress_bytes (GBytes *bytes,
                               GError **error)
{
  g_autoptr(GZlibDecompressor) decompressor = NULL;
  g_autoptr(GOutputStream) out = NULL;
  g_autoptr(GOutputStream) mem = NULL;

  mem = g_memory_output_stream_new_resizable ();

  decompressor = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
  out = g_converter_output_stream_new (mem, G_CONVERTER (decompressor));

  if (!g_output_stream_write_all (out, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes),
                                  NULL, NULL, error))
    return NULL;

  if (!g_output_stream_close (out, NULL, error))
    return NULL;

  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (mem));
}

GBytes *
flatpak_read_stream (GInputStream *in,
                     gboolean      null_terminate,
                     GError      **error)
{
  g_autoptr(GOutputStream) mem_stream = NULL;

  mem_stream = g_memory_output_stream_new_resizable ();
  if (g_output_stream_splice (mem_stream, in,
                              0, NULL, error) < 0)
    return NULL;

  if (null_terminate)
    {
      if (!g_output_stream_write (G_OUTPUT_STREAM (mem_stream), "\0", 1, NULL, error))
        return NULL;
    }

  if (!g_output_stream_close (G_OUTPUT_STREAM (mem_stream), NULL, error))
    return NULL;

  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (mem_stream));
}

gint
flatpak_strcmp0_ptr (gconstpointer a,
                     gconstpointer b)
{
  return g_strcmp0 (*(char * const *) a, *(char * const *) b);
}

/* Sometimes this is /var/run which is a symlink, causing weird issues when we pass
 * it as a path into the sandbox */
char *
flatpak_get_real_xdg_runtime_dir (void)
{
  return realpath (g_get_user_runtime_dir (), NULL);
}

/* Compares if str has a specific path prefix. This differs
   from a regular prefix in two ways. First of all there may
   be multiple slashes separating the path elements, and
   secondly, if a prefix is matched that has to be en entire
   path element. For instance /a/prefix matches /a/prefix/foo/bar,
   but not /a/prefixfoo/bar. */
gboolean
flatpak_has_path_prefix (const char *str,
                         const char *prefix)
{
  while (TRUE)
    {
      /* Skip consecutive slashes to reach next path
         element */
      while (*str == '/')
        str++;
      while (*prefix == '/')
        prefix++;

      /* No more prefix path elements? Done! */
      if (*prefix == 0)
        return TRUE;

      /* Compare path element */
      while (*prefix != 0 && *prefix != '/')
        {
          if (*str != *prefix)
            return FALSE;
          str++;
          prefix++;
        }

      /* Matched prefix path element,
         must be entire str path element */
      if (*str != '/' && *str != 0)
        return FALSE;
    }
}

/* Returns end of matching path prefix, or NULL if no match */
const char *
flatpak_path_match_prefix (const char *pattern,
                           const char *string)
{
  char c, test;
  const char *tmp;

  while (*pattern == '/')
    pattern++;

  while (*string == '/')
    string++;

  while (TRUE)
    {
      switch (c = *pattern++)
        {
        case 0:
          if (*string == '/' || *string == 0)
            return string;
          return NULL;

        case '?':
          if (*string == '/' || *string == 0)
            return NULL;
          string++;
          break;

        case '*':
          c = *pattern;

          while (c == '*')
            c = *++pattern;

          /* special case * at end */
          if (c == 0)
            {
              tmp = strchr (string, '/');
              if (tmp != NULL)
                return tmp;
              return string + strlen (string);
            }
          else if (c == '/')
            {
              string = strchr (string, '/');
              if (string == NULL)
                return NULL;
              break;
            }

          while ((test = *string) != 0)
            {
              tmp = flatpak_path_match_prefix (pattern, string);
              if (tmp != NULL)
                return tmp;
              if (test == '/')
                break;
              string++;
            }
          return NULL;

        default:
          if (c != *string)
            return NULL;
          string++;
          break;
        }
    }
  return NULL; /* Should not be reached */
}

static const char *
flatpak_get_kernel_arch (void)
{
  static struct utsname buf;
  static char *arch = NULL;
  char *m;

  if (arch != NULL)
    return arch;

  if (uname (&buf))
    {
      arch = "unknown";
      return arch;
    }

  /* By default, just pass on machine, good enough for most arches */
  arch = buf.machine;

  /* Override for some arches */

  m = buf.machine;
  /* i?86 */
  if (strlen (m) == 4 && m[0] == 'i' && m[2] == '8'  && m[3] == '6')
    {
      arch = "i386";
    }
  else if (g_str_has_prefix (m, "arm"))
    {
      if (g_str_has_suffix (m, "b"))
        arch = "armeb";
      else
        arch = "arm";
    }
  else if (strcmp (m, "mips") == 0)
    {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
      arch = "mipsel";
#endif
    }
  else if (strcmp (m, "mips64") == 0)
    {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
      arch = "mips64el";
#endif
    }

  return arch;
}

/* This maps the kernel-reported uname to a single string representing
 * the cpu family, in the sense that all members of this family would
 * be able to understand and link to a binary file with such cpu
 * opcodes. That doesn't necessarily mean that all members of the
 * family can run all opcodes, for instance for modern 32bit intel we
 * report "i386", even though they support instructions that the
 * original i386 cpu cannot run. Still, such an executable would
 * at least try to execute a 386, whereas an arm binary would not.
 */
const char *
flatpak_get_arch (void)
{
  /* Avoid using uname on multiarch machines, because uname reports the kernels
   * arch, and that may be different from userspace. If e.g. the kernel is 64bit and
   * the userspace is 32bit we want to use 32bit by default. So, we take the current build
   * arch as the default. */
#if defined(__i386__)
  return "i386";
#elif defined(__x86_64__)
  return "x86_64";
#elif defined(__aarch64__)
  return "aarch64";
#elif defined(__arm__)
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  return "arm";
#else
  return "armeb";
#endif
#else
  return flatpak_get_kernel_arch ();
#endif
}

gboolean
flatpak_is_linux32_arch (const char *arch)
{
  const char *kernel_arch = flatpak_get_kernel_arch ();

  if (strcmp (kernel_arch, "x86_64") == 0 &&
      strcmp (arch, "i386") == 0)
    return TRUE;

  if (strcmp (kernel_arch, "aarch64") == 0 &&
      strcmp (arch, "arm") == 0)
    return TRUE;

  return FALSE;
}

static struct
{
  const char *kernel_arch;
  const char *compat_arch;
} compat_arches[] = {
  { "x86_64", "i386" },
  { "aarch64", "arm" },
};

static const char *
flatpak_get_compat_arch (const char *kernel_arch)
{
  int i;

  /* Also add all other arches that are compatible with the kernel arch */
  for (i = 0; i < G_N_ELEMENTS (compat_arches); i++)
    {
      if (strcmp (compat_arches[i].kernel_arch, kernel_arch) == 0)
        return compat_arches[i].compat_arch;
    }

  return NULL;
}

const char *
flatpak_get_compat_arch_reverse (const char *compat_arch)
{
  int i;

  /* Also add all other arches that are compatible with the kernel arch */
  for (i = 0; i < G_N_ELEMENTS (compat_arches); i++)
    {
      if (strcmp (compat_arches[i].compat_arch, compat_arch) == 0)
        return compat_arches[i].kernel_arch;
    }

  return NULL;
}

/* Get all compatible arches for this host in order of priority */
const char **
flatpak_get_arches (void)
{
  static gsize arches = 0;

  if (g_once_init_enter (&arches))
    {
      gsize new_arches = 0;
      const char *main_arch = flatpak_get_arch ();
      const char *kernel_arch = flatpak_get_kernel_arch ();
      const char *compat_arch;
      GPtrArray *array = g_ptr_array_new ();

      /* This is the userspace arch, i.e. the one flatpak itself was
         build for. It's always first. */
      g_ptr_array_add (array, (char *) main_arch);

      compat_arch = flatpak_get_compat_arch (kernel_arch);
      if (g_strcmp0 (compat_arch, main_arch) != 0)
        g_ptr_array_add (array, (char *) compat_arch);

      g_ptr_array_add (array, NULL);
      new_arches = (gsize) g_ptr_array_free (array, FALSE);

      g_once_init_leave (&arches, new_arches);
    }

  return (const char **) arches;
}

const char **
flatpak_get_gl_drivers (void)
{
  static gsize drivers = 0;

  if (g_once_init_enter (&drivers))
    {
      gsize new_drivers;
      char **new_drivers_c = 0;
      const char *env = g_getenv ("FLATPAK_GL_DRIVERS");
      if (env != NULL && *env != 0)
        new_drivers_c = g_strsplit (env, ":", -1);
      else
        {
          g_autofree char *nvidia_version = NULL;
          char *dot;
          GPtrArray *array = g_ptr_array_new ();

          if (g_file_get_contents ("/sys/module/nvidia/version",
                                   &nvidia_version, NULL, NULL))
            {
              g_strstrip (nvidia_version);
              /* Convert dots to dashes */
              while ((dot = strchr (nvidia_version, '.')) != NULL)
                *dot = '-';
              g_ptr_array_add (array, g_strconcat ("nvidia-", nvidia_version, NULL));
            }

          g_ptr_array_add (array, (char *) "default");
          g_ptr_array_add (array, (char *) "host");

          g_ptr_array_add (array, NULL);
          new_drivers_c = (char **) g_ptr_array_free (array, FALSE);
        }

      new_drivers = (gsize) new_drivers_c;
      g_once_init_leave (&drivers, new_drivers);
    }

  return (const char **) drivers;
}

static gboolean
flatpak_get_have_intel_gpu (void)
{
  static int have_intel = -1;

  if (have_intel == -1)
    have_intel = g_file_test ("/sys/module/i915", G_FILE_TEST_EXISTS);

  return have_intel;
}

static GHashTable *
load_kernel_module_list (void)
{
  GHashTable *modules = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autofree char *modules_data = NULL;
  g_autoptr(GError) error = NULL;
  char *start, *end;

  if (!g_file_get_contents ("/proc/modules", &modules_data, NULL, &error))
    {
      g_info ("Failed to read /proc/modules: %s", error->message);
      return modules;
    }

  /* /proc/modules is a table of modules.
   * Columns are split by spaces and rows by newlines.
   * The first column is the name. */
  start = modules_data;
  while (TRUE)
    {
      end = strchr (start, ' ');
      if (end == NULL)
        break;

      g_hash_table_add (modules, g_strndup (start, (end - start)));

      start = strchr (end, '\n');
      if (start == NULL)
        break;

      start++;
    }

  return modules;
}

static gboolean
flatpak_get_have_kernel_module (const char *module_name)
{
  static GHashTable *kernel_modules = NULL;

  if (g_once_init_enter (&kernel_modules))
    g_once_init_leave (&kernel_modules, load_kernel_module_list ());

  return g_hash_table_contains (kernel_modules, module_name);
}

static const char *
flatpak_get_gtk_theme (void)
{
  static char *gtk_theme;

  if (g_once_init_enter (&gtk_theme))
    {
      /* The schema may not be installed so check first */
      GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
      g_autoptr(GSettingsSchema) schema = NULL;

      if (source == NULL)
        g_once_init_leave (&gtk_theme, g_strdup (""));
      else
        {
          schema = g_settings_schema_source_lookup (source,
                                                    "org.gnome.desktop.interface", TRUE);

          if (schema == NULL)
            g_once_init_leave (&gtk_theme, g_strdup (""));
          else
            {
              /* GSettings is used to store the theme if you use Wayland or GNOME.
               * TODO: Check XSettings Net/ThemeName for other desktops.
               * We don't care about any other method (like settings.ini) because they
               *   aren't passed through the sandbox anyway. */
              g_autoptr(GSettings) settings = g_settings_new ("org.gnome.desktop.interface");
              g_once_init_leave (&gtk_theme, g_settings_get_string (settings, "gtk-theme"));
            }
        }
    }

  return (const char *) gtk_theme;
}

const char *
flatpak_get_bwrap (void)
{
  const char *e = g_getenv ("FLATPAK_BWRAP");

  if (e != NULL)
    return e;
  return HELPER;
}

gboolean
flatpak_bwrap_is_unprivileged (void)
{
  g_autofree char *path = g_find_program_in_path (flatpak_get_bwrap ());
  struct stat st;

  /* Various features are supported only if bwrap exists and is not setuid */
  return
    path != NULL &&
    stat (path, &st) == 0 &&
    (st.st_mode & S_ISUID) == 0;
}

static char *
line_get_word (char **line)
{
  char *word = NULL;

  while (g_ascii_isspace (**line))
    (*line)++;

  if (**line == 0)
    return NULL;

  word = *line;

  while (**line && !g_ascii_isspace (**line))
    (*line)++;

  if (**line)
    {
      **line = 0;
      (*line)++;
    }

  return word;
}

char *
flatpak_filter_glob_to_regexp (const char  *glob,
                               gboolean     runtime_only,
                               GError     **error)
{
  g_autoptr(GString) regexp = g_string_new ("");
  int parts = 1;
  gboolean empty_part;

  if (g_str_has_prefix (glob, "app/"))
    {
      if (runtime_only)
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Glob can't match apps"));
          return NULL;
        }
      else
        {
          glob += strlen ("app/");
          g_string_append (regexp, "app/");
        }
    }
  else if (g_str_has_prefix (glob, "runtime/"))
    {
      glob += strlen ("runtime/");
      g_string_append (regexp, "runtime/");
    }
  else
    {
      if (runtime_only)
        g_string_append (regexp, "runtime/");
      else
        g_string_append (regexp, "(app|runtime)/");
    }

  /* We really need an id part, the rest is optional */
  if (*glob == 0)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Empty glob"));
      return NULL;
    }

  empty_part = TRUE;
  while (*glob != 0)
    {
      char c = *glob;
      glob++;

      if (c == '/')
        {
          if (empty_part)
            g_string_append (regexp, "[.\\-_a-zA-Z0-9]*");
          empty_part = TRUE;
          parts++;
          g_string_append (regexp, "/");
          if (parts > 3)
            {
              flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Too many segments in glob"));
              return NULL;
            }
        }
      else if (c == '*')
        {
          empty_part = FALSE;
         g_string_append (regexp, "[.\\-_a-zA-Z0-9]*");
        }
      else if (c == '.')
        {
          empty_part = FALSE;
          g_string_append (regexp, "\\.");
        }
      else if (g_ascii_isalnum (c) || c == '-' || c == '_')
        {
          empty_part = FALSE;
          g_string_append_c (regexp, c);
        }
      else
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid glob character '%c'"), c);
          return NULL;
        }
    }

  while (parts < 3)
    {
      parts++;
      g_string_append (regexp, "/[.\\-_a-zA-Z0-9]*");
    }

  return g_string_free (g_steal_pointer (&regexp), FALSE);
}

gboolean
flatpak_parse_filters (const char *data,
                       GRegex **allow_refs_out,
                       GRegex **deny_refs_out,
                       GError **error)
{
  g_auto(GStrv) lines = NULL;
  int i;
  g_autoptr(GString) allow_regexp = g_string_new ("^(");
  g_autoptr(GString) deny_regexp = g_string_new ("^(");
  gboolean has_allow = FALSE;
  gboolean has_deny = FALSE;
  g_autoptr(GRegex) allow_refs = NULL;
  g_autoptr(GRegex) deny_refs = NULL;

  lines = g_strsplit (data, "\n", -1);
  for (i = 0; lines[i] != NULL; i++)
    {
      char *line = lines[i];
      char *comment, *command;

      /* Ignore shell-style comments */
      comment = strchr (line, '#');
      if (comment != NULL)
        *comment = 0;

      command = line_get_word (&line);
      /* Ignore empty lines */
      if (command == NULL)
        continue;

      if (strcmp (command, "allow") == 0 || strcmp (command, "deny") == 0)
        {
          char *glob, *next;
          g_autofree char *ref_regexp = NULL;
          GString *command_regexp;
          gboolean *has_type = NULL;

          glob = line_get_word (&line);
          if (glob == NULL)
            return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Missing glob on line %d"), i + 1);

          next = line_get_word (&line);
          if (next != NULL)
            return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Trailing text on line %d"), i + 1);

          ref_regexp = flatpak_filter_glob_to_regexp (glob, FALSE, error);
          if (ref_regexp == NULL)
            return glnx_prefix_error (error, _("on line %d"), i + 1);

          if (strcmp (command, "allow") == 0)
            {
              command_regexp = allow_regexp;
              has_type = &has_allow;
            }
          else
            {
              command_regexp = deny_regexp;
              has_type = &has_deny;
            }

          if (*has_type)
            g_string_append (command_regexp, "|");
          else
            *has_type = TRUE;

          g_string_append (command_regexp, ref_regexp);
        }
      else
        {
          return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Unexpected word '%s' on line %d"), command, i + 1);
        }
    }

  g_string_append (allow_regexp, ")$");
  g_string_append (deny_regexp, ")$");

  if (allow_regexp)
    {
      allow_refs = g_regex_new (allow_regexp->str, G_REGEX_DOLLAR_ENDONLY|G_REGEX_RAW|G_REGEX_OPTIMIZE, G_REGEX_MATCH_ANCHORED, error);
      if (allow_refs == NULL)
        return FALSE;
    }

  if (deny_regexp)
    {
      deny_refs = g_regex_new (deny_regexp->str, G_REGEX_DOLLAR_ENDONLY|G_REGEX_RAW|G_REGEX_OPTIMIZE, G_REGEX_MATCH_ANCHORED, error);
      if (deny_refs == NULL)
        return FALSE;
    }

  *allow_refs_out = g_steal_pointer (&allow_refs);
  *deny_refs_out = g_steal_pointer (&deny_refs);

  return TRUE;
}

gboolean
flatpak_filters_allow_ref (GRegex *allow_refs,
                           GRegex *deny_refs,
                           const char *ref)
{
  if (deny_refs == NULL)
    return TRUE; /* All refs are allowed by default */

  if (!g_regex_match (deny_refs, ref, G_REGEX_MATCH_ANCHORED, NULL))
    return TRUE; /* Not denied */

  if (allow_refs &&  g_regex_match (allow_refs, ref, G_REGEX_MATCH_ANCHORED, NULL))
    return TRUE; /* Explicitly allowed */

  return FALSE;
}

char **
flatpak_list_deployed_refs (const char   *type,
                            const char   *name_prefix,
                            const char   *arch,
                            const char   *branch,
                            GCancellable *cancellable,
                            GError      **error)
{
  gchar **ret = NULL;
  g_autoptr(GPtrArray) names = NULL;
  g_autoptr(GHashTable) hash = NULL;
  g_autoptr(FlatpakDir) user_dir = NULL;
  g_autoptr(GPtrArray) system_dirs = NULL;
  int i;

  hash = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash, (GEqualFunc)flatpak_decomposed_equal, (GDestroyNotify)flatpak_decomposed_unref, NULL);

  user_dir = flatpak_dir_get_user ();
  system_dirs = flatpak_dir_get_system_list (cancellable, error);
  if (system_dirs == NULL)
    goto out;

  if (!flatpak_dir_collect_deployed_refs (user_dir, type, name_prefix,
                                          arch, branch, hash, cancellable,
                                          error))
    goto out;

  for (i = 0; i < system_dirs->len; i++)
    {
      FlatpakDir *system_dir = g_ptr_array_index (system_dirs, i);
      if (!flatpak_dir_collect_deployed_refs (system_dir, type, name_prefix,
                                              arch, branch, hash, cancellable,
                                              error))
        goto out;
    }

  names = g_ptr_array_new ();

  GLNX_HASH_TABLE_FOREACH (hash, FlatpakDecomposed *, ref)
    {
      g_ptr_array_add (names, flatpak_decomposed_dup_id (ref));
    }

  g_ptr_array_sort (names, flatpak_strcmp0_ptr);
  g_ptr_array_add (names, NULL);

  ret = (char **) g_ptr_array_free (names, FALSE);
  names = NULL;

out:
  return ret;
}

char **
flatpak_list_unmaintained_refs (const char   *name_prefix,
                                const char   *arch,
                                const char   *branch,
                                GCancellable *cancellable,
                                GError      **error)
{
  gchar **ret = NULL;
  g_autoptr(GPtrArray) names = NULL;
  g_autoptr(GHashTable) hash = NULL;
  g_autoptr(FlatpakDir) user_dir = NULL;
  const char *key;
  GHashTableIter iter;
  g_autoptr(GPtrArray) system_dirs = NULL;
  int i;

  hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  user_dir = flatpak_dir_get_user ();

  if (!flatpak_dir_collect_unmaintained_refs (user_dir, name_prefix,
                                              arch, branch, hash, cancellable,
                                              error))
    return NULL;

  system_dirs = flatpak_dir_get_system_list (cancellable, error);
  if (system_dirs == NULL)
    return NULL;

  for (i = 0; i < system_dirs->len; i++)
    {
      FlatpakDir *system_dir = g_ptr_array_index (system_dirs, i);

      if (!flatpak_dir_collect_unmaintained_refs (system_dir, name_prefix,
                                                  arch, branch, hash, cancellable,
                                                  error))
        return NULL;
    }

  names = g_ptr_array_new ();
  g_hash_table_iter_init (&iter, hash);
  while (g_hash_table_iter_next (&iter, (gpointer *) &key, NULL))
    g_ptr_array_add (names, g_strdup (key));

  g_ptr_array_sort (names, flatpak_strcmp0_ptr);
  g_ptr_array_add (names, NULL);

  ret = (char **) g_ptr_array_free (names, FALSE);
  names = NULL;

  return ret;
}

GFile *
flatpak_find_deploy_dir_for_ref (FlatpakDecomposed *ref,
                                 FlatpakDir       **dir_out,
                                 GCancellable      *cancellable,
                                 GError           **error)
{
  g_autoptr(FlatpakDir) user_dir = NULL;
  g_autoptr(GPtrArray) system_dirs = NULL;
  FlatpakDir *dir = NULL;
  g_autoptr(GFile) deploy = NULL;

  user_dir = flatpak_dir_get_user ();
  system_dirs = flatpak_dir_get_system_list (cancellable, error);
  if (system_dirs == NULL)
    return NULL;

  dir = user_dir;
  deploy = flatpak_dir_get_if_deployed (dir, ref, NULL, cancellable);
  if (deploy == NULL)
    {
      int i;
      for (i = 0; deploy == NULL && i < system_dirs->len; i++)
        {
          dir = g_ptr_array_index (system_dirs, i);
          deploy = flatpak_dir_get_if_deployed (dir, ref, NULL, cancellable);
          if (deploy != NULL)
            break;
        }
    }

  if (deploy == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_NOT_INSTALLED, _("%s not installed"), flatpak_decomposed_get_ref (ref));
      return NULL;
    }

  if (dir_out)
    *dir_out = g_object_ref (dir);
  return g_steal_pointer (&deploy);
}

GFile *
flatpak_find_files_dir_for_ref (FlatpakDecomposed *ref,
                                GCancellable      *cancellable,
                                GError           **error)
{
  g_autoptr(GFile) deploy = NULL;

  deploy = flatpak_find_deploy_dir_for_ref (ref, NULL, cancellable, error);
  if (deploy == NULL)
    return NULL;

  return g_file_get_child (deploy, "files");
}

GFile *
flatpak_find_unmaintained_extension_dir_if_exists (const char   *name,
                                                   const char   *arch,
                                                   const char   *branch,
                                                   GCancellable *cancellable)
{
  g_autoptr(FlatpakDir) user_dir = NULL;
  g_autoptr(GFile) extension_dir = NULL;
  g_autoptr(GError) local_error = NULL;

  user_dir = flatpak_dir_get_user ();

  extension_dir = flatpak_dir_get_unmaintained_extension_dir_if_exists (user_dir, name, arch, branch, cancellable);
  if (extension_dir == NULL)
    {
      g_autoptr(GPtrArray) system_dirs = NULL;
      int i;

      system_dirs = flatpak_dir_get_system_list (cancellable, &local_error);
      if (system_dirs == NULL)
        {
          g_warning ("Could not get the system installations: %s", local_error->message);
          return NULL;
        }

      for (i = 0; i < system_dirs->len; i++)
        {
          FlatpakDir *system_dir = g_ptr_array_index (system_dirs, i);
          extension_dir = flatpak_dir_get_unmaintained_extension_dir_if_exists (system_dir, name, arch, branch, cancellable);
          if (extension_dir != NULL)
            break;
        }
    }

  if (extension_dir == NULL)
    return NULL;

  return g_steal_pointer (&extension_dir);
}

FlatpakDecomposed *
flatpak_find_current_ref (const char   *app_id,
                          GCancellable *cancellable,
                          GError      **error)
{
  g_autoptr(FlatpakDecomposed) current_ref = NULL;
  g_autoptr(FlatpakDir) user_dir = flatpak_dir_get_user ();
  int i;

  current_ref = flatpak_dir_current_ref (user_dir, app_id, NULL);
  if (current_ref == NULL)
    {
      g_autoptr(GPtrArray) system_dirs = NULL;

      system_dirs = flatpak_dir_get_system_list (cancellable, error);
      if (system_dirs == NULL)
        return FALSE;

      for (i = 0; i < system_dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (system_dirs, i);
          current_ref = flatpak_dir_current_ref (dir, app_id, cancellable);
          if (current_ref != NULL)
            break;
        }
    }

  if (current_ref)
    return g_steal_pointer (&current_ref);

  flatpak_fail_error (error, FLATPAK_ERROR_NOT_INSTALLED, _("%s not installed"), app_id);
  return NULL;
}

FlatpakDeploy *
flatpak_find_deploy_for_ref_in (GPtrArray    *dirs,
                                const char   *ref_str,
                                const char   *commit,
                                GCancellable *cancellable,
                                GError      **error)
{
  FlatpakDeploy *deploy = NULL;
  int i;
  g_autoptr(GError) my_error = NULL;

  g_autoptr(FlatpakDecomposed) ref = flatpak_decomposed_new_from_ref (ref_str, error);
  if (ref == NULL)
    return NULL;

  for (i = 0; deploy == NULL && i < dirs->len; i++)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, i);

      flatpak_log_dir_access (dir);
      g_clear_error (&my_error);
      deploy = flatpak_dir_load_deployed (dir, ref, commit, cancellable, &my_error);
    }

  if (deploy == NULL)
    g_propagate_error (error, g_steal_pointer (&my_error));

  return deploy;
}

FlatpakDeploy *
flatpak_find_deploy_for_ref (const char   *ref,
                             const char   *commit,
                             FlatpakDir   *opt_user_dir,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GPtrArray) dirs = NULL;

  dirs = flatpak_dir_get_system_list (cancellable, error);
  if (dirs == NULL)
    return NULL;

  /* If an custom dir was passed, use that instead of the user dir.
   * This is used when running apply-extra-data where if the target
   * is a custom installation location the regular user one may not
   * have the (possibly just installed in this transaction) runtime.
   */
  if (opt_user_dir)
    g_ptr_array_insert (dirs, 0, g_object_ref (opt_user_dir));
  else
    g_ptr_array_insert (dirs, 0, flatpak_dir_get_user ());

  return flatpak_find_deploy_for_ref_in (dirs, ref, commit, cancellable, error);
}

static gboolean
remove_dangling_symlinks (int           parent_fd,
                          const char   *name,
                          GCancellable *cancellable,
                          GError      **error)
{
  gboolean ret = FALSE;
  struct dirent *dent;
  g_auto(GLnxDirFdIterator) iter = { 0 };

  if (!glnx_dirfd_iterator_init_at (parent_fd, name, FALSE, &iter, error))
    goto out;

  while (TRUE)
    {
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&iter, &dent, cancellable, error))
        goto out;

      if (dent == NULL)
        break;

      if (dent->d_type == DT_DIR)
        {
          if (!remove_dangling_symlinks (iter.fd, dent->d_name, cancellable, error))
            goto out;
        }
      else if (dent->d_type == DT_LNK)
        {
          struct stat stbuf;
          if (fstatat (iter.fd, dent->d_name, &stbuf, 0) != 0 && errno == ENOENT)
            {
              if (unlinkat (iter.fd, dent->d_name, 0) != 0)
                {
                  glnx_set_error_from_errno (error);
                  goto out;
                }
            }
        }
    }

  ret = TRUE;
out:

  return ret;
}

gboolean
flatpak_remove_dangling_symlinks (GFile        *dir,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  gboolean ret = FALSE;

  /* The fd is closed by this call */
  if (!remove_dangling_symlinks (AT_FDCWD, flatpak_file_get_path_cached (dir),
                                 cancellable, error))
    goto out;

  ret = TRUE;

out:
  return ret;
}

/* This atomically replaces a symlink with a new value, removing the
 * existing symlink target, if it exstis and is different from
 * @target. This is atomic in the sense that we're guaranteed to
 * remove any existing symlink target (once), independent of how many
 * processes do the same operation in parallele. However, it is still
 * possible that we remove the old and then fail to create the new
 * symlink for some reason, ending up with neither the old or the new
 * target. That is fine if the reason for the symlink is keeping a
 * cache though.
 */
gboolean
flatpak_switch_symlink_and_remove (const char *symlink_path,
                                   const char *target,
                                   GError    **error)
{
  g_autofree char *symlink_dir = g_path_get_dirname (symlink_path);
  int try;

  for (try = 0; try < 100; try++)
    {
      g_autofree char *tmp_path = NULL;
      int fd;

      /* Try to atomically create the symlink */
      if (TEMP_FAILURE_RETRY (symlink (target, symlink_path)) == 0)
        return TRUE;

      if (errno != EEXIST)
        {
          /* Unexpected failure, bail */
          glnx_set_error_from_errno (error);
          return FALSE;
        }

      /* The symlink existed, move it to a temporary name atomically, and remove target
         if that succeeded. */
      tmp_path = g_build_filename (symlink_dir, ".switched-symlink-XXXXXX", NULL);

      fd = g_mkstemp_full (tmp_path, O_RDWR, 0644);
      if (fd == -1)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
      close (fd);

      if (TEMP_FAILURE_RETRY (rename (symlink_path, tmp_path)) == 0)
        {
          /* The move succeeded, now we can remove the old target */
          g_autofree char *old_target = flatpak_readlink (tmp_path, error);
          if (old_target == NULL)
            return FALSE;
          if (strcmp (old_target, target) != 0) /* Don't remove old file if its the same as the new one */
            {
              g_autofree char *old_target_path = g_build_filename (symlink_dir, old_target, NULL);
              unlink (old_target_path);
            }
        }
      else if (errno != ENOENT)
        {
          glnx_set_error_from_errno (error);
          unlink (tmp_path);
          return -1;
        }
      unlink (tmp_path);

      /* An old target was removed, try again */
    }

  return flatpak_fail (error, "flatpak_switch_symlink_and_remove looped too many times");
}

gboolean
flatpak_argument_needs_quoting (const char *arg)
{
  if (*arg == '\0')
    return TRUE;

  while (*arg != 0)
    {
      char c = *arg;
      if (!g_ascii_isalnum (c) &&
          !(c == '-' || c == '/' || c == '~' ||
            c == ':' || c == '.' || c == '_' ||
            c == '=' || c == '@'))
        return TRUE;
      arg++;
    }
  return FALSE;
}

char *
flatpak_quote_argv (const char *argv[],
                    gssize      len)
{
  GString *res = g_string_new ("");
  int i;

  if (len == -1)
    len = g_strv_length ((char **) argv);

  for (i = 0; i < len; i++)
    {
      if (i != 0)
        g_string_append_c (res, ' ');

      if (flatpak_argument_needs_quoting (argv[i]))
        {
          g_autofree char *quoted = g_shell_quote (argv[i]);
          g_string_append (res, quoted);
        }
      else
        g_string_append (res, argv[i]);
    }

  return g_string_free (res, FALSE);
}

/* This is useful, because it handles escaped characters in uris, and ? arguments at the end of the uri */
gboolean
flatpak_file_arg_has_suffix (const char *arg, const char *suffix)
{
  g_autoptr(GFile) file = g_file_new_for_commandline_arg (arg);
  g_autofree char *basename = g_file_get_basename (file);

  return g_str_has_suffix (basename, suffix);
}

GFile *
flatpak_build_file_va (GFile  *base,
                       va_list args)
{
  g_autoptr(GFile) res = g_object_ref (base);
  const gchar *arg;

  while ((arg = va_arg (args, const gchar *)))
    {
      g_autoptr(GFile) child = g_file_resolve_relative_path (res, arg);
      g_set_object (&res, child);
    }

  return g_steal_pointer (&res);
}

GFile *
flatpak_build_file (GFile *base, ...)
{
  GFile *res;
  va_list args;

  va_start (args, base);
  res = flatpak_build_file_va (base, args);
  va_end (args);

  return res;
}

const char *
flatpak_file_get_path_cached (GFile *file)
{
  const char *path;
  static GQuark _file_path_quark = 0;

  if (G_UNLIKELY (_file_path_quark == 0))
    _file_path_quark = g_quark_from_static_string ("flatpak-file-path");

  do
    {
      path = g_object_get_qdata ((GObject *) file, _file_path_quark);
      if (path == NULL)
        {
          g_autofree char *new_path = NULL;
          new_path = g_file_get_path (file);
          if (new_path == NULL)
            return NULL;

          if (g_object_replace_qdata ((GObject *) file, _file_path_quark,
                                      NULL, new_path, g_free, NULL))
            path = g_steal_pointer (&new_path);
        }
    }
  while (path == NULL);

  return path;
}

gboolean
flatpak_openat_noatime (int           dfd,
                        const char   *name,
                        int          *ret_fd,
                        GCancellable *cancellable,
                        GError      **error)
{
  int fd;
  int flags = O_RDONLY | O_CLOEXEC;

#ifdef O_NOATIME
  do
    fd = openat (dfd, name, flags | O_NOATIME, 0);
  while (G_UNLIKELY (fd == -1 && errno == EINTR));
  /* Only the owner or superuser may use O_NOATIME; so we may get
   * EPERM.  EINVAL may happen if the kernel is really old...
   */
  if (fd == -1 && (errno == EPERM || errno == EINVAL))
#endif
  do
    fd = openat (dfd, name, flags, 0);
  while (G_UNLIKELY (fd == -1 && errno == EINTR));

  if (fd == -1)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }
  else
    {
      *ret_fd = fd;
      return TRUE;
    }
}

gboolean
flatpak_cp_a (GFile         *src,
              GFile         *dest,
              FlatpakCpFlags flags,
              GCancellable  *cancellable,
              GError       **error)
{
  gboolean ret = FALSE;
  GFileEnumerator *enumerator = NULL;
  GFileInfo *src_info = NULL;
  GFile *dest_child = NULL;
  int dest_dfd = -1;
  gboolean merge = (flags & FLATPAK_CP_FLAGS_MERGE) != 0;
  gboolean no_chown = (flags & FLATPAK_CP_FLAGS_NO_CHOWN) != 0;
  gboolean move = (flags & FLATPAK_CP_FLAGS_MOVE) != 0;
  g_autoptr(GFileInfo) child_info = NULL;
  GError *temp_error = NULL;
  int r;

  enumerator = g_file_enumerate_children (src, "standard::type,standard::name,unix::uid,unix::gid,unix::mode",
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, error);
  if (!enumerator)
    goto out;

  src_info = g_file_query_info (src, "standard::name,unix::mode,unix::uid,unix::gid," \
                                     "time::modified,time::modified-usec,time::access,time::access-usec",
                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                cancellable, error);
  if (!src_info)
    goto out;

  do
    r = mkdir (flatpak_file_get_path_cached (dest), 0755);
  while (G_UNLIKELY (r == -1 && errno == EINTR));
  if (r == -1 &&
      (!merge || errno != EEXIST))
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (!glnx_opendirat (AT_FDCWD, flatpak_file_get_path_cached (dest), TRUE,
                       &dest_dfd, error))
    goto out;

  if (!no_chown)
    {
      do
        r = fchown (dest_dfd,
                    g_file_info_get_attribute_uint32 (src_info, "unix::uid"),
                    g_file_info_get_attribute_uint32 (src_info, "unix::gid"));
      while (G_UNLIKELY (r == -1 && errno == EINTR));
      if (r == -1)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  do
    r = fchmod (dest_dfd, g_file_info_get_attribute_uint32 (src_info, "unix::mode"));
  while (G_UNLIKELY (r == -1 && errno == EINTR));

  if (dest_dfd != -1)
    {
      (void) close (dest_dfd);
      dest_dfd = -1;
    }

  while ((child_info = g_file_enumerator_next_file (enumerator, cancellable, &temp_error)))
    {
      const char *name = g_file_info_get_name (child_info);
      g_autoptr(GFile) src_child = g_file_get_child (src, name);

      if (dest_child)
        g_object_unref (dest_child);
      dest_child = g_file_get_child (dest, name);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!flatpak_cp_a (src_child, dest_child, flags,
                             cancellable, error))
            goto out;
        }
      else
        {
          (void) unlink (flatpak_file_get_path_cached (dest_child));
          GFileCopyFlags copyflags = G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS;
          if (!no_chown)
            copyflags |= G_FILE_COPY_ALL_METADATA;
          if (move)
            {
              if (!g_file_move (src_child, dest_child, copyflags,
                                cancellable, NULL, NULL, error))
                goto out;
            }
          else
            {
              if (!g_file_copy (src_child, dest_child, copyflags,
                                cancellable, NULL, NULL, error))
                goto out;
            }
        }

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  if (move &&
      !g_file_delete (src, NULL, error))
    goto out;

  ret = TRUE;
out:
  if (dest_dfd != -1)
    (void) close (dest_dfd);
  g_clear_object (&src_info);
  g_clear_object (&enumerator);
  g_clear_object (&dest_child);
  return ret;
}

static gboolean
_flatpak_canonicalize_permissions (int         parent_dfd,
                                   const char *rel_path,
                                   gboolean    toplevel,
                                   int         uid,
                                   int         gid,
                                   GError    **error)
{
  struct stat stbuf;
  gboolean res = TRUE;

  /* Note, in order to not leave non-canonical things around in case
   * of error, this continues after errors, but returns the first
   * error. */

  if (TEMP_FAILURE_RETRY (fstatat (parent_dfd, rel_path, &stbuf, AT_SYMLINK_NOFOLLOW)) != 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  if ((uid != -1 && uid != stbuf.st_uid) || (gid != -1 && gid != stbuf.st_gid))
    {
      if (TEMP_FAILURE_RETRY (fchownat (parent_dfd, rel_path, uid, gid, AT_SYMLINK_NOFOLLOW)) != 0)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }

      /* Re-read st_mode for new owner */
      if (TEMP_FAILURE_RETRY (fstatat (parent_dfd, rel_path, &stbuf, AT_SYMLINK_NOFOLLOW)) != 0)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }

  if (S_ISDIR (stbuf.st_mode))
    {
      g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

      /* For the toplevel we set to 0700 so we can modify it, but not
         expose any non-canonical files to any other user, then we set
         it to 0755 afterwards. */
      if (fchmodat (parent_dfd, rel_path, toplevel ? 0700 : 0755, 0) != 0)
        {
          glnx_set_error_from_errno (error);
          error = NULL;
          res = FALSE;
        }

      if (glnx_dirfd_iterator_init_at (parent_dfd, rel_path, FALSE, &dfd_iter, NULL))
        {
          while (TRUE)
            {
              struct dirent *dent;

              if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, NULL, NULL) || dent == NULL)
                break;

              if (!_flatpak_canonicalize_permissions (dfd_iter.fd, dent->d_name, FALSE, uid, gid, error))
                {
                  error = NULL;
                  res = FALSE;
                }
            }
        }

      if (toplevel &&
          fchmodat (parent_dfd, rel_path, 0755, 0) != 0)
        {
          glnx_set_error_from_errno (error);
          error = NULL;
          res = FALSE;
        }

      return res;
    }
  else if (S_ISREG (stbuf.st_mode))
    {
      mode_t mode;

      /* If use can execute, make executable by all */
      if (stbuf.st_mode & S_IXUSR)
        mode = 0755;
      else /* otherwise executable by none */
        mode = 0644;

      if (fchmodat (parent_dfd, rel_path, mode, 0) != 0)
        {
          glnx_set_error_from_errno (error);
          res = FALSE;
        }
    }
  else if (S_ISLNK (stbuf.st_mode))
    {
      /* symlinks have no permissions */
    }
  else
    {
      /* some weird non-canonical type, lets delete it */
      if (unlinkat (parent_dfd, rel_path, 0) != 0)
        {
          glnx_set_error_from_errno (error);
          res = FALSE;
        }
    }

  return res;
}

/* Canonicalizes files to the same permissions as bare-user-only checkouts */
gboolean
flatpak_canonicalize_permissions (int         parent_dfd,
                                  const char *rel_path,
                                  int         uid,
                                  int         gid,
                                  GError    **error)
{
  return _flatpak_canonicalize_permissions (parent_dfd, rel_path, TRUE, uid, gid, error);
}

/* Make a directory, and its parent. Don't error if it already exists.
 * If you want a failure mode with EEXIST, use g_file_make_directory_with_parents. */
gboolean
flatpak_mkdir_p (GFile        *dir,
                 GCancellable *cancellable,
                 GError      **error)
{
  return glnx_shutil_mkdir_p_at (AT_FDCWD,
                                 flatpak_file_get_path_cached (dir),
                                 0777,
                                 cancellable,
                                 error);
}

gboolean
flatpak_rm_rf (GFile        *dir,
               GCancellable *cancellable,
               GError      **error)
{
  return glnx_shutil_rm_rf_at (AT_FDCWD,
                               flatpak_file_get_path_cached (dir),
                               cancellable, error);
}

gboolean
flatpak_file_rename (GFile        *from,
                     GFile        *to,
                     GCancellable *cancellable,
                     GError      **error)
{
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  if (rename (flatpak_file_get_path_cached (from),
              flatpak_file_get_path_cached (to)) < 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  return TRUE;
}

/* If memfd_create() is available, generate a sealed memfd with contents of
 * @str. Otherwise use an O_TMPFILE @tmpf in anonymous mode, write @str to
 * @tmpf, and lseek() back to the start. See also similar uses in e.g.
 * rpm-ostree for running dracut.
 */
gboolean
flatpak_buffer_to_sealed_memfd_or_tmpfile (GLnxTmpfile *tmpf,
                                           const char  *name,
                                           const char  *str,
                                           size_t       len,
                                           GError     **error)
{
  if (len == -1)
    len = strlen (str);
  glnx_autofd int memfd = memfd_create (name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
  int fd; /* Unowned */
  if (memfd != -1)
    {
      fd = memfd;
    }
  else
    {
      /* We use an anonymous fd (i.e. O_EXCL) since we don't want
       * the target container to potentially be able to re-link it.
       */
      if (!G_IN_SET (errno, ENOSYS, EOPNOTSUPP))
        return glnx_throw_errno_prefix (error, "memfd_create");
      if (!glnx_open_anonymous_tmpfile (O_RDWR | O_CLOEXEC, tmpf, error))
        return FALSE;
      fd = tmpf->fd;
    }
  if (ftruncate (fd, len) < 0)
    return glnx_throw_errno_prefix (error, "ftruncate");
  if (glnx_loop_write (fd, str, len) < 0)
    return glnx_throw_errno_prefix (error, "write");
  if (lseek (fd, 0, SEEK_SET) < 0)
    return glnx_throw_errno_prefix (error, "lseek");
  if (memfd != -1)
    {
      /* Valgrind doesn't currently handle G_ADD_SEALS, so lets not seal when debugging... */
      if ((!RUNNING_ON_VALGRIND) &&
          fcntl (memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0)
        return glnx_throw_errno_prefix (error, "fcntl(F_ADD_SEALS)");
      /* The other values can stay default */
      tmpf->fd = g_steal_fd (&memfd);
      tmpf->initialized = TRUE;
    }
  return TRUE;
}

gboolean
flatpak_open_in_tmpdir_at (int             tmpdir_fd,
                           int             mode,
                           char           *tmpl,
                           GOutputStream **out_stream,
                           GCancellable   *cancellable,
                           GError        **error)
{
  const int max_attempts = 128;
  int i;
  int fd;

  /* 128 attempts seems reasonable... */
  for (i = 0; i < max_attempts; i++)
    {
      glnx_gen_temp_name (tmpl);

      do
        fd = openat (tmpdir_fd, tmpl, O_WRONLY | O_CREAT | O_EXCL, mode);
      while (fd == -1 && errno == EINTR);
      if (fd < 0 && errno != EEXIST)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
      else if (fd != -1)
        break;
    }
  if (i == max_attempts)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Exhausted attempts to open temporary file");
      return FALSE;
    }

  if (out_stream)
    *out_stream = g_unix_output_stream_new (fd, TRUE);
  else
    (void) close (fd);

  return TRUE;
}

gboolean
flatpak_bytes_save (GFile        *dest,
                    GBytes       *bytes,
                    GCancellable *cancellable,
                    GError      **error)
{
  g_autoptr(GOutputStream) out = NULL;

  out = (GOutputStream *) g_file_replace (dest, NULL, FALSE,
                                          G_FILE_CREATE_REPLACE_DESTINATION,
                                          cancellable, error);
  if (out == NULL)
    return FALSE;

  if (!g_output_stream_write_all (out,
                                  g_bytes_get_data (bytes, NULL),
                                  g_bytes_get_size (bytes),
                                  NULL,
                                  cancellable,
                                  error))
    return FALSE;

  if (!g_output_stream_close (out, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_variant_save (GFile        *dest,
                      GVariant     *variant,
                      GCancellable *cancellable,
                      GError      **error)
{
  g_autoptr(GOutputStream) out = NULL;
  gsize bytes_written;

  out = (GOutputStream *) g_file_replace (dest, NULL, FALSE,
                                          G_FILE_CREATE_REPLACE_DESTINATION,
                                          cancellable, error);
  if (out == NULL)
    return FALSE;

  if (!g_output_stream_write_all (out,
                                  g_variant_get_data (variant),
                                  g_variant_get_size (variant),
                                  &bytes_written,
                                  cancellable,
                                  error))
    return FALSE;

  if (!g_output_stream_close (out, cancellable, error))
    return FALSE;

  return TRUE;
}

/* This special cases the ref lookup which by doing a
   bsearch since the array is sorted */
gboolean
flatpak_var_ref_map_lookup_ref (VarRefMapRef   ref_map,
                                const char    *ref,
                                VarRefInfoRef *out_info)
{
  gsize imax, imin;
  gsize imid;
  gsize n;

  g_return_val_if_fail (out_info != NULL, FALSE);

  n = var_ref_map_get_length (ref_map);
  if (n == 0)
    return FALSE;

  imax = n - 1;
  imin = 0;
  while (imax >= imin)
    {
      VarRefMapEntryRef entry;
      const char *cur;
      int cmp;

      imid = (imin + imax) / 2;

      entry = var_ref_map_get_at (ref_map, imid);
      cur = var_ref_map_entry_get_ref (entry);

      cmp = strcmp (cur, ref);
      if (cmp < 0)
        {
          imin = imid + 1;
        }
      else if (cmp > 0)
        {
          if (imid == 0)
            break;
          imax = imid - 1;
        }
      else
        {
          *out_info = var_ref_map_entry_get_info (entry);
          return TRUE;
        }
    }

  return FALSE;
}

/* Find the list of refs which belong to the given @collection_id in @summary.
 * If @collection_id is %NULL, the main refs list from the summary will be
 * returned. If @collection_id doesn’t match any collection IDs in the summary
 * file, %FALSE will be returned. */
gboolean
flatpak_summary_find_ref_map (VarSummaryRef summary,
                              const char *collection_id,
                              VarRefMapRef *refs_out)
{
  VarMetadataRef metadata = var_summary_get_metadata (summary);
  const char *summary_collection_id;

  summary_collection_id = var_metadata_lookup_string (metadata, "ostree.summary.collection-id", NULL);

  if (collection_id == NULL || g_strcmp0 (collection_id, summary_collection_id) == 0)
    {
      if (refs_out)
        *refs_out = var_summary_get_ref_map (summary);
      return TRUE;
    }
  else if (collection_id != NULL)
    {
      VarVariantRef collection_map_v;
      if (var_metadata_lookup (metadata, "ostree.summary.collection-map", NULL, &collection_map_v))
        {
          VarCollectionMapRef collection_map = var_collection_map_from_variant (collection_map_v);
          return var_collection_map_lookup (collection_map, collection_id, NULL, refs_out);
        }
    }

  return FALSE;
}

/* This matches all refs from @collection_id that have ref, followed by '.'  as prefix */
GPtrArray *
flatpak_summary_match_subrefs (GVariant          *summary_v,
                               const char        *collection_id,
                               FlatpakDecomposed *ref)
{
  GPtrArray *res = g_ptr_array_new_with_free_func ((GDestroyNotify)flatpak_decomposed_unref);
  gsize n, i;
  g_autofree char *parts_prefix = NULL;
  g_autofree char *ref_prefix = NULL;
  g_autofree char *ref_suffix = NULL;
  VarSummaryRef summary;
  VarRefMapRef ref_map;

  summary = var_summary_from_gvariant (summary_v);

  /* Work out which refs list to use, based on the @collection_id. */
  if (flatpak_summary_find_ref_map (summary, collection_id, &ref_map))
    {
      /* Match against the refs. */
      g_autofree char *id = flatpak_decomposed_dup_id (ref);
      g_autofree char *arch = flatpak_decomposed_dup_arch (ref);
      g_autofree char *branch = flatpak_decomposed_dup_branch (ref);
      parts_prefix = g_strconcat (id, ".", NULL);

      ref_prefix = g_strconcat (flatpak_decomposed_get_kind_str (ref), "/", NULL);
      ref_suffix = g_strconcat ("/", arch, "/", branch, NULL);

      n = var_ref_map_get_length (ref_map);
      for (i = 0; i < n; i++)
        {
          VarRefMapEntryRef entry = var_ref_map_get_at (ref_map, i);
          const char *cur;
          const char *id_start;
          const char *id_suffix;
          const char *id_end;

          cur = var_ref_map_entry_get_ref (entry);

          /* Must match type */
          if (!g_str_has_prefix (cur, ref_prefix))
            continue;

          /* Must match arch & branch */
          if (!g_str_has_suffix (cur, ref_suffix))
            continue;

          id_start = strchr (cur, '/');
          if (id_start == NULL)
            continue;
          id_start += 1;

          id_end = strchr (id_start, '/');
          if (id_end == NULL)
            continue;

          /* But only prefix of id */
          if (!g_str_has_prefix (id_start, parts_prefix))
            continue;

          /* And no dots (we want to install prefix.$ID, but not prefix.$ID.Sources) */
          id_suffix = id_start + strlen (parts_prefix);
          if (memchr (id_suffix, '.', id_end - id_suffix) != NULL)
            continue;

          FlatpakDecomposed *d = flatpak_decomposed_new_from_ref (cur, NULL);
          if (d)
            g_ptr_array_add (res, d);
        }
    }

  return g_steal_pointer (&res);
}

gboolean
flatpak_summary_lookup_ref (GVariant      *summary_v,
                            const char    *collection_id,
                            const char    *ref,
                            char         **out_checksum,
                            VarRefInfoRef *out_info)
{
  VarSummaryRef summary;
  VarRefMapRef ref_map;
  VarRefInfoRef info;
  const guchar *checksum_bytes;
  gsize checksum_bytes_len;

  summary = var_summary_from_gvariant (summary_v);

  /* Work out which refs list to use, based on the @collection_id. */
  if (!flatpak_summary_find_ref_map (summary, collection_id, &ref_map))
    return FALSE;

  if (!flatpak_var_ref_map_lookup_ref (ref_map, ref, &info))
    return FALSE;

  checksum_bytes = var_ref_info_peek_checksum (info, &checksum_bytes_len);
  if (G_UNLIKELY (checksum_bytes_len != OSTREE_SHA256_DIGEST_LEN))
    return FALSE;

  if (out_checksum)
    *out_checksum = ostree_checksum_from_bytes (checksum_bytes);

  if (out_info)
    *out_info = info;

  return TRUE;
}

char *
flatpak_keyfile_get_string_non_empty (GKeyFile   *keyfile,
                                      const char *group,
                                      const char *key)
{
  g_autofree char *value = NULL;

  value = g_key_file_get_string (keyfile, group, key, NULL);
  if (value != NULL && *value == '\0')
    g_clear_pointer (&value, g_free);

  return g_steal_pointer (&value);
}

GKeyFile *
flatpak_parse_repofile (const char   *remote_name,
                        gboolean      from_ref,
                        GKeyFile     *keyfile,
                        GBytes      **gpg_data_out,
                        GCancellable *cancellable,
                        GError      **error)
{
  g_autoptr(GBytes) gpg_data = NULL;
  g_autofree char *uri = NULL;
  g_autofree char *title = NULL;
  g_autofree char *gpg_key = NULL;
  g_autofree char *collection_id = NULL;
  g_autofree char *default_branch = NULL;
  g_autofree char *comment = NULL;
  g_autofree char *description = NULL;
  g_autofree char *icon = NULL;
  g_autofree char *homepage = NULL;
  g_autofree char *filter = NULL;
  g_autofree char *subset = NULL;
  g_autofree char *authenticator_name = NULL;
  gboolean nodeps;
  const char *source_group;
  g_autofree char *version = NULL;

  if (from_ref)
    source_group = FLATPAK_REF_GROUP;
  else
    source_group = FLATPAK_REPO_GROUP;

  GKeyFile *config = g_key_file_new ();
  g_autofree char *group = g_strdup_printf ("remote \"%s\"", remote_name);

  if (!g_key_file_has_group (keyfile, source_group))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid %s: Missing group ‘%s’"),
                          from_ref ? ".flatpakref" : ".flatpakrepo", source_group);
      return NULL;
    }

  uri = g_key_file_get_string (keyfile, source_group,
                               FLATPAK_REPO_URL_KEY, NULL);
  if (uri == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid %s: Missing key ‘%s’"),
                          from_ref ? ".flatpakref" : ".flatpakrepo", FLATPAK_REPO_URL_KEY);
      return NULL;
    }

  version = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                                   FLATPAK_REPO_VERSION_KEY, NULL);
  if (version != NULL && strcmp (version, "1") != 0)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA,
                          _("Invalid version %s, only 1 supported"), version);
      return NULL;
    }

  g_key_file_set_string (config, group, "url", uri);

  subset = g_key_file_get_locale_string (keyfile, source_group,
                                         FLATPAK_REPO_SUBSET_KEY, NULL, NULL);
  if (subset != NULL)
    g_key_file_set_string (config, group, "xa.subset", subset);

  /* Don't use the title from flatpakref files; that's the title of the app */
  if (!from_ref)
    title = g_key_file_get_locale_string (keyfile, FLATPAK_REPO_GROUP,
                                          FLATPAK_REPO_TITLE_KEY, NULL, NULL);
  if (title != NULL)
    g_key_file_set_string (config, group, "xa.title", title);

  default_branch = g_key_file_get_locale_string (keyfile, source_group,
                                                 FLATPAK_REPO_DEFAULT_BRANCH_KEY, NULL, NULL);
  if (default_branch != NULL)
    g_key_file_set_string (config, group, "xa.default-branch", default_branch);

  nodeps = g_key_file_get_boolean (keyfile, source_group,
                                   FLATPAK_REPO_NODEPS_KEY, NULL);
  if (nodeps)
    g_key_file_set_boolean (config, group, "xa.nodeps", TRUE);

  gpg_key = g_key_file_get_string (keyfile, source_group,
                                   FLATPAK_REPO_GPGKEY_KEY, NULL);
  if (gpg_key != NULL)
    {
      guchar *decoded;
      gsize decoded_len;

      gpg_key = g_strstrip (gpg_key);
      decoded = g_base64_decode (gpg_key, &decoded_len);
      if (decoded_len < 10) /* Check some minimal size so we don't get crap */
        {
          g_free (decoded);
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid gpg key"));
          return NULL;
        }

      gpg_data = g_bytes_new_take (decoded, decoded_len);
      g_key_file_set_boolean (config, group, "gpg-verify", TRUE);
    }
  else
    {
      g_key_file_set_boolean (config, group, "gpg-verify", FALSE);
    }

  /* We have a hierarchy of keys for setting the collection ID, which all have
   * the same effect. The only difference is which versions of Flatpak support
   * them, and therefore what P2P implementation is enabled by them:
   * DeploySideloadCollectionID: supported by Flatpak >= 1.12.8 (1.7.1
   *   introduced sideload support but this key was added late)
   * DeployCollectionID: supported by Flatpak >= 1.0.6 (but fully supported in
   *   >= 1.2.0)
   * CollectionID: supported by Flatpak >= 0.9.8
   */
  collection_id = flatpak_keyfile_get_string_non_empty (keyfile, source_group,
                                                        FLATPAK_REPO_DEPLOY_SIDELOAD_COLLECTION_ID_KEY);
  if (collection_id == NULL)
    collection_id = flatpak_keyfile_get_string_non_empty (keyfile, source_group,
                                                          FLATPAK_REPO_DEPLOY_COLLECTION_ID_KEY);
  if (collection_id == NULL)
    collection_id = flatpak_keyfile_get_string_non_empty (keyfile, source_group,
                                                          FLATPAK_REPO_COLLECTION_ID_KEY);
  if (collection_id != NULL)
    {
      if (gpg_key == NULL)
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Collection ID requires GPG key to be provided"));
          return NULL;
        }

      g_key_file_set_string (config, group, "collection-id", collection_id);
    }

  g_key_file_set_boolean (config, group, "gpg-verify-summary",
                          (gpg_key != NULL));

  authenticator_name = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                                              FLATPAK_REPO_AUTHENTICATOR_NAME_KEY, NULL);
  if (authenticator_name)
    g_key_file_set_string (config, group, "xa.authenticator-name", authenticator_name);

  if (g_key_file_has_key (keyfile, FLATPAK_REPO_GROUP, FLATPAK_REPO_AUTHENTICATOR_INSTALL_KEY, NULL))
    {
      gboolean authenticator_install = g_key_file_get_boolean (keyfile, FLATPAK_REPO_GROUP,
                                                               FLATPAK_REPO_AUTHENTICATOR_INSTALL_KEY, NULL);
      g_key_file_set_boolean (config, group, "xa.authenticator-install", authenticator_install);
    }

  comment = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                                   FLATPAK_REPO_COMMENT_KEY, NULL);
  if (comment)
    g_key_file_set_string (config, group, "xa.comment", comment);

  description = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                                       FLATPAK_REPO_DESCRIPTION_KEY, NULL);
  if (description)
    g_key_file_set_string (config, group, "xa.description", description);

  icon = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                                FLATPAK_REPO_ICON_KEY, NULL);
  if (icon)
    g_key_file_set_string (config, group, "xa.icon", icon);

  homepage  = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                                     FLATPAK_REPO_HOMEPAGE_KEY, NULL);
  if (homepage)
    g_key_file_set_string (config, group, "xa.homepage", homepage);

  filter = g_key_file_get_string (keyfile, FLATPAK_REPO_GROUP,
                                   FLATPAK_REPO_FILTER_KEY, NULL);
  if (filter)
    g_key_file_set_string (config, group, "xa.filter", filter);
  else
    g_key_file_set_string (config, group, "xa.filter", ""); /* Default to override any pre-existing filters */

  *gpg_data_out = g_steal_pointer (&gpg_data);

  return g_steal_pointer (&config);
}

gboolean
flatpak_repo_set_title (OstreeRepo *repo,
                        const char *title,
                        GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (title)
    g_key_file_set_string (config, "flatpak", "title", title);
  else
    g_key_file_remove_key (config, "flatpak", "title", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_comment (OstreeRepo *repo,
                          const char *comment,
                          GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (comment)
    g_key_file_set_string (config, "flatpak", "comment", comment);
  else
    g_key_file_remove_key (config, "flatpak", "comment", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_description (OstreeRepo *repo,
                              const char *description,
                              GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (description)
    g_key_file_set_string (config, "flatpak", "description", description);
  else
    g_key_file_remove_key (config, "flatpak", "description", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}


gboolean
flatpak_repo_set_icon (OstreeRepo *repo,
                       const char *icon,
                       GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (icon)
    g_key_file_set_string (config, "flatpak", "icon", icon);
  else
    g_key_file_remove_key (config, "flatpak", "icon", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_homepage (OstreeRepo *repo,
                           const char *homepage,
                           GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (homepage)
    g_key_file_set_string (config, "flatpak", "homepage", homepage);
  else
    g_key_file_remove_key (config, "flatpak", "homepage", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_redirect_url (OstreeRepo *repo,
                               const char *redirect_url,
                               GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (redirect_url)
    g_key_file_set_string (config, "flatpak", "redirect-url", redirect_url);
  else
    g_key_file_remove_key (config, "flatpak", "redirect-url", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_authenticator_name (OstreeRepo *repo,
                                     const char *authenticator_name,
                                     GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (authenticator_name)
    g_key_file_set_string (config, "flatpak", "authenticator-name", authenticator_name);
  else
    g_key_file_remove_key (config, "flatpak", "authenticator-name", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_authenticator_install (OstreeRepo *repo,
                                        gboolean authenticator_install,
                                        GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  g_key_file_set_boolean (config, "flatpak", "authenticator-install", authenticator_install);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_authenticator_option (OstreeRepo *repo,
                                       const char *key,
                                       const char *value,
                                       GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;
  g_autofree char *full_key = g_strdup_printf ("authenticator-options.%s", key);

  config = ostree_repo_copy_config (repo);

  if (value)
    g_key_file_set_string (config, "flatpak", full_key, value);
  else
    g_key_file_remove_key (config, "flatpak", full_key, NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_deploy_collection_id (OstreeRepo *repo,
                                       gboolean    deploy_collection_id,
                                       GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);
  g_key_file_set_boolean (config, "flatpak", "deploy-collection-id", deploy_collection_id);
  return ostree_repo_write_config (repo, config, error);
}

gboolean
flatpak_repo_set_deploy_sideload_collection_id (OstreeRepo *repo,
                                           gboolean    deploy_collection_id,
                                           GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);
  g_key_file_set_boolean (config, "flatpak", "deploy-sideload-collection-id", deploy_collection_id);
  return ostree_repo_write_config (repo, config, error);
}

gboolean
flatpak_repo_set_gpg_keys (OstreeRepo *repo,
                           GBytes     *bytes,
                           GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;
  g_autofree char *value_base64 = NULL;

  config = ostree_repo_copy_config (repo);

  value_base64 = g_base64_encode (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));

  g_key_file_set_string (config, "flatpak", "gpg-keys", value_base64);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_default_branch (OstreeRepo *repo,
                                 const char *branch,
                                 GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (branch)
    g_key_file_set_string (config, "flatpak", "default-branch", branch);
  else
    g_key_file_remove_key (config, "flatpak", "default-branch", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_collection_id (OstreeRepo *repo,
                                const char *collection_id,
                                GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  if (!ostree_repo_set_collection_id (repo, collection_id, error))
    return FALSE;

  config = ostree_repo_copy_config (repo);
  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_repo_set_summary_history_length (OstreeRepo *repo,
                                         guint       length,
                                         GError    **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (length)
    g_key_file_set_integer (config, "flatpak", "summary-history-length", length);
  else
    g_key_file_remove_key (config, "flatpak", "summary-history-length", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

guint
flatpak_repo_get_summary_history_length (OstreeRepo *repo)
{
  GKeyFile *config = ostree_repo_get_config (repo);
  int length;

  length = g_key_file_get_integer (config, "flatpak", "sumary-history-length", NULL);

  if (length <= 0)
    return FLATPAK_SUMMARY_HISTORY_LENGTH_DEFAULT;

  return length;
}

GVariant *
flatpak_commit_get_extra_data_sources (GVariant *commitv,
                                       GError  **error)
{
  g_autoptr(GVariant) commit_metadata = NULL;
  g_autoptr(GVariant) extra_data_sources = NULL;

  commit_metadata = g_variant_get_child_value (commitv, 0);
  extra_data_sources = g_variant_lookup_value (commit_metadata,
                                               "xa.extra-data-sources",
                                               G_VARIANT_TYPE ("a(ayttays)"));

  if (extra_data_sources == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   _("No extra data sources"));
      return NULL;
    }

  return g_steal_pointer (&extra_data_sources);
}


GVariant *
flatpak_repo_get_extra_data_sources (OstreeRepo   *repo,
                                     const char   *rev,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  g_autoptr(GVariant) commitv = NULL;

  if (!ostree_repo_load_variant (repo,
                                 OSTREE_OBJECT_TYPE_COMMIT,
                                 rev, &commitv, error))
    return NULL;

  return flatpak_commit_get_extra_data_sources (commitv, error);
}

void
flatpak_repo_parse_extra_data_sources (GVariant      *extra_data_sources,
                                       int            index,
                                       const char   **name,
                                       guint64       *download_size,
                                       guint64       *installed_size,
                                       const guchar **sha256,
                                       const char   **uri)
{
  g_autoptr(GVariant) sha256_v = NULL;
  g_variant_get_child (extra_data_sources, index, "(^aytt@ay&s)",
                       name,
                       download_size,
                       installed_size,
                       &sha256_v,
                       uri);

  if (download_size)
    *download_size = GUINT64_FROM_BE (*download_size);

  if (installed_size)
    *installed_size = GUINT64_FROM_BE (*installed_size);

  if (sha256)
    *sha256 = ostree_checksum_bytes_peek (sha256_v);
}

#define OSTREE_GIO_FAST_QUERYINFO ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target," \
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev")

static gboolean
_flatpak_repo_collect_sizes (OstreeRepo   *repo,
                             GFile        *file,
                             GFileInfo    *file_info,
                             guint64      *installed_size,
                             guint64      *download_size,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  GFileInfo *child_info_tmp;
  g_autoptr(GError) temp_error = NULL;

  if (file_info != NULL && g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    {
      const char *checksum = ostree_repo_file_get_checksum (OSTREE_REPO_FILE (file));
      guint64 obj_size;
      guint64 file_size = g_file_info_get_size (file_info);

      if (installed_size)
        *installed_size += ((file_size + 511) / 512) * 512;

      if (download_size)
        {
          g_autoptr(GInputStream) input = NULL;
          GInputStream *base_input;
          g_autoptr(GError) local_error = NULL;

          if (!ostree_repo_query_object_storage_size (repo,
                                                      OSTREE_OBJECT_TYPE_FILE, checksum,
                                                      &obj_size, cancellable, &local_error))
            {
              int fd;
              struct stat stbuf;

              /* Ostree does not look at the staging directory when querying storage
                 size, so may return a NOT_FOUND error here. We work around this
                 by loading the object and walking back until we find the original
                 fd which we can fstat(). */
              if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
                return FALSE;

              if (!ostree_repo_load_file (repo, checksum,  &input, NULL, NULL, NULL, error))
                return FALSE;

              base_input = input;
              while (G_IS_FILTER_INPUT_STREAM (base_input))
                base_input = g_filter_input_stream_get_base_stream (G_FILTER_INPUT_STREAM (base_input));

              if (!G_IS_UNIX_INPUT_STREAM (base_input))
                return flatpak_fail (error, "Unable to find size of commit %s, not an unix stream", checksum);

              fd = g_unix_input_stream_get_fd (G_UNIX_INPUT_STREAM (base_input));

              if (fstat (fd, &stbuf) != 0)
                return glnx_throw_errno_prefix (error, "Can't find commit size: ");

              obj_size = stbuf.st_size;
            }

          *download_size += obj_size;
        }
    }

  if (file_info == NULL || g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
    {
      dir_enum = g_file_enumerate_children (file, OSTREE_GIO_FAST_QUERYINFO,
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            cancellable, error);
      if (!dir_enum)
        return FALSE;


      while ((child_info_tmp = g_file_enumerator_next_file (dir_enum, cancellable, &temp_error)))
        {
          g_autoptr(GFileInfo) child_info = child_info_tmp;
          const char *name = g_file_info_get_name (child_info);
          g_autoptr(GFile) child = g_file_get_child (file, name);

          if (!_flatpak_repo_collect_sizes (repo, child, child_info, installed_size, download_size, cancellable, error))
            return FALSE;
        }
    }

  return TRUE;
}

gboolean
flatpak_repo_collect_sizes (OstreeRepo   *repo,
                            GFile        *root,
                            guint64      *installed_size,
                            guint64      *download_size,
                            GCancellable *cancellable,
                            GError      **error)
{
  /* Initialize the sums */
  if (installed_size)
    *installed_size = 0;
  if (download_size)
    *download_size = 0;
  return _flatpak_repo_collect_sizes (repo, root, NULL, installed_size, download_size, cancellable, error);
}


static void
flatpak_repo_collect_extra_data_sizes (OstreeRepo *repo,
                                       const char *rev,
                                       guint64    *installed_size,
                                       guint64    *download_size)
{
  g_autoptr(GVariant) extra_data_sources = NULL;
  gsize n_extra_data;
  int i;

  extra_data_sources = flatpak_repo_get_extra_data_sources (repo, rev, NULL, NULL);
  if (extra_data_sources == NULL)
    return;

  n_extra_data = g_variant_n_children (extra_data_sources);
  if (n_extra_data == 0)
    return;

  for (i = 0; i < n_extra_data; i++)
    {
      guint64 extra_download_size;
      guint64 extra_installed_size;

      flatpak_repo_parse_extra_data_sources (extra_data_sources, i,
                                             NULL,
                                             &extra_download_size,
                                             &extra_installed_size,
                                             NULL, NULL);
      if (installed_size)
        *installed_size += extra_installed_size;
      if (download_size)
        *download_size += extra_download_size;
    }
}

/* Loads the old compat summary file from a local repo */
GVariant *
flatpak_repo_load_summary (OstreeRepo *repo,
                           GError    **error)
{
  glnx_autofd int fd = -1;
  g_autoptr(GMappedFile) mfile = NULL;
  g_autoptr(GBytes) bytes = NULL;

  fd = openat (ostree_repo_get_dfd (repo), "summary", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      glnx_set_error_from_errno (error);
      return NULL;
    }

  mfile = g_mapped_file_new_from_fd (fd, FALSE, error);
  if (!mfile)
    return NULL;

  bytes = g_mapped_file_get_bytes (mfile);

  return g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, bytes, TRUE));
}

GVariant *
flatpak_repo_load_summary_index (OstreeRepo *repo,
                                 GError    **error)
{
  glnx_autofd int fd = -1;
  g_autoptr(GMappedFile) mfile = NULL;
  g_autoptr(GBytes) bytes = NULL;

  fd = openat (ostree_repo_get_dfd (repo), "summary.idx", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      glnx_set_error_from_errno (error);
      return NULL;
    }

  mfile = g_mapped_file_new_from_fd (fd, FALSE, error);
  if (!mfile)
    return NULL;

  bytes = g_mapped_file_get_bytes (mfile);

  return g_variant_ref_sink (g_variant_new_from_bytes (FLATPAK_SUMMARY_INDEX_GVARIANT_FORMAT, bytes, TRUE));
}

static gboolean
flatpak_repo_save_compat_summary (OstreeRepo   *repo,
                                  GVariant     *summary,
                                  time_t       *out_old_sig_mtime,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  int repo_dfd = ostree_repo_get_dfd (repo);
  struct stat stbuf;
  time_t old_sig_mtime = 0;
  GLnxFileReplaceFlags flags;

  flags = GLNX_FILE_REPLACE_INCREASING_MTIME;
  if (ostree_repo_get_disable_fsync (repo))
    flags |= GLNX_FILE_REPLACE_NODATASYNC;
  else
    flags |= GLNX_FILE_REPLACE_DATASYNC_NEW;

  if (!glnx_file_replace_contents_at (repo_dfd, "summary",
                                      g_variant_get_data (summary),
                                      g_variant_get_size (summary),
                                      flags,
                                      cancellable, error))
    return FALSE;

  if (fstatat (repo_dfd, "summary.sig", &stbuf, AT_SYMLINK_NOFOLLOW) == 0)
    old_sig_mtime = stbuf.st_mtime;

  if (unlinkat (repo_dfd, "summary.sig", 0) != 0 &&
      G_UNLIKELY (errno != ENOENT))
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  *out_old_sig_mtime = old_sig_mtime;
  return TRUE;
}

static gboolean
flatpak_repo_save_summary_index (OstreeRepo   *repo,
                                 GVariant     *index,
                                 const char   *index_digest,
                                 GBytes       *index_sig,
                                 GCancellable *cancellable,
                                 GError      **error)
{
  int repo_dfd = ostree_repo_get_dfd (repo);
  GLnxFileReplaceFlags  flags;

  if (index == NULL)
    {
      if (unlinkat (repo_dfd, "summary.idx", 0) != 0 &&
          G_UNLIKELY (errno != ENOENT))
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
      if (unlinkat (repo_dfd, "summary.idx.sig", 0) != 0 &&
          G_UNLIKELY (errno != ENOENT))
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }

      return TRUE;
    }

  flags = GLNX_FILE_REPLACE_INCREASING_MTIME;
  if (ostree_repo_get_disable_fsync (repo))
    flags |= GLNX_FILE_REPLACE_NODATASYNC;
  else
    flags |= GLNX_FILE_REPLACE_DATASYNC_NEW;

  if (index_sig)
    {
      g_autofree char *path = g_strconcat ("summaries/", index_digest, ".idx.sig", NULL);

      if (!glnx_shutil_mkdir_p_at (repo_dfd, "summaries",
                                   0775, cancellable, error))
        return FALSE;

      if (!glnx_file_replace_contents_at (repo_dfd, path,
                                          g_bytes_get_data (index_sig, NULL),
                                          g_bytes_get_size (index_sig),
                                          flags,
                                          cancellable, error))
        return FALSE;
    }

  if (!glnx_file_replace_contents_at (repo_dfd, "summary.idx",
                                      g_variant_get_data (index),
                                      g_variant_get_size (index),
                                      flags,
                                      cancellable, error))
    return FALSE;

  /* Update the non-indexed summary.idx.sig file that was introduced in 1.9.1 but
   * was made unnecessary in 1.9.3. Lets keep it for a while until everyone updates
   */
  if (index_sig)
    {
      if (!glnx_file_replace_contents_at (repo_dfd, "summary.idx.sig",
                                          g_bytes_get_data (index_sig, NULL),
                                          g_bytes_get_size (index_sig),
                                          flags,
                                          cancellable, error))
        return FALSE;
    }
  else
    {
      if (unlinkat (repo_dfd, "summary.idx.sig", 0) != 0 &&
          G_UNLIKELY (errno != ENOENT))
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }

  return TRUE;
}

GVariant *
flatpak_repo_load_digested_summary (OstreeRepo *repo,
                                   const char *digest,
                                   GError    **error)
{
  glnx_autofd int fd = -1;
  g_autoptr(GMappedFile) mfile = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GBytes) compressed_bytes = NULL;
  g_autofree char *path = NULL;
  g_autofree char *filename = NULL;

  filename = g_strconcat (digest, ".gz", NULL);
  path = g_build_filename ("summaries", filename, NULL);

  fd = openat (ostree_repo_get_dfd (repo), path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      glnx_set_error_from_errno (error);
      return NULL;
    }

  mfile = g_mapped_file_new_from_fd (fd, FALSE, error);
  if (!mfile)
    return NULL;

  compressed_bytes = g_mapped_file_get_bytes (mfile);
  bytes = flatpak_zlib_decompress_bytes (compressed_bytes, error);
  if (bytes == NULL)
    return NULL;

  return g_variant_ref_sink (g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, bytes, TRUE));
}

static char *
flatpak_repo_save_digested_summary (OstreeRepo   *repo,
                                    const char   *name,
                                    GVariant     *summary,
                                    GCancellable *cancellable,
                                    GError      **error)
{
  int repo_dfd = ostree_repo_get_dfd (repo);
  g_autofree char *digest = NULL;
  g_autofree char *filename = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GBytes) data = NULL;
  g_autoptr(GBytes) compressed_data = NULL;
  struct stat stbuf;

  if (!glnx_shutil_mkdir_p_at (repo_dfd, "summaries",
                               0775,
                               cancellable,
                               error))
    return NULL;

  digest = g_compute_checksum_for_data (G_CHECKSUM_SHA256,
                                        g_variant_get_data (summary),
                                        g_variant_get_size (summary));
  filename = g_strconcat (digest, ".gz", NULL);

  path = g_build_filename ("summaries", filename, NULL);

  /* Check for pre-existing (non-truncated) copy and avoid re-writing it */
  if (fstatat (repo_dfd, path, &stbuf, 0) == 0 &&
      stbuf.st_size != 0)
    {
      g_info ("Reusing digested summary at %s for %s", path, name);
      return g_steal_pointer (&digest);
    }

  data = g_variant_get_data_as_bytes (summary);
  compressed_data = flatpak_zlib_compress_bytes (data, -1, error);
  if (compressed_data == NULL)
    return NULL;

  if (!glnx_file_replace_contents_at (repo_dfd, path,
                                      g_bytes_get_data (compressed_data, NULL),
                                      g_bytes_get_size (compressed_data),
                                      ostree_repo_get_disable_fsync (repo) ? GLNX_FILE_REPLACE_NODATASYNC : GLNX_FILE_REPLACE_DATASYNC_NEW,
                                      cancellable, error))
    return NULL;

  g_info ("Wrote digested summary at %s for %s", path, name);
  return g_steal_pointer (&digest);
}

static gboolean
flatpak_repo_save_digested_summary_delta (OstreeRepo   *repo,
                                          const char   *from_digest,
                                          const char   *to_digest,
                                          GBytes       *delta,
                                          GCancellable *cancellable,
                                          GError      **error)
{
  int repo_dfd = ostree_repo_get_dfd (repo);
  g_autofree char *path = NULL;
  g_autofree char *filename = g_strconcat (from_digest, "-", to_digest, ".delta", NULL);
  struct stat stbuf;

  if (!glnx_shutil_mkdir_p_at (repo_dfd, "summaries",
                               0775,
                               cancellable,
                               error))
    return FALSE;

  path = g_build_filename ("summaries", filename, NULL);

  /* Check for pre-existing copy of same size and avoid re-writing it */
  if (fstatat (repo_dfd, path, &stbuf, 0) == 0 &&
      stbuf.st_size == g_bytes_get_size (delta))
    {
      g_info ("Reusing digested summary-diff for %s", filename);
      return TRUE;
    }

  if (!glnx_file_replace_contents_at (repo_dfd, path,
                                      g_bytes_get_data (delta, NULL),
                                      g_bytes_get_size (delta),
                                      ostree_repo_get_disable_fsync (repo) ? GLNX_FILE_REPLACE_NODATASYNC : GLNX_FILE_REPLACE_DATASYNC_NEW,
                                      cancellable, error))
    return FALSE;

  g_info ("Wrote digested summary delta at %s", path);
  return TRUE;
}


typedef struct
{
  guint64    installed_size;
  guint64    download_size;
  char      *metadata_contents;
  GPtrArray *subsets;
  GVariant  *sparse_data;
  gsize      commit_size;
  guint64    commit_timestamp;
} CommitData;

static void
commit_data_free (gpointer data)
{
  CommitData *rev_data = data;

  if (rev_data->subsets)
    g_ptr_array_unref (rev_data->subsets);
  g_free (rev_data->metadata_contents);
  if (rev_data->sparse_data)
    g_variant_unref (rev_data->sparse_data);
  g_free (rev_data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CommitData, commit_data_free);

static GHashTable *
commit_data_cache_new (void)
{
  return g_hash_table_new_full (g_str_hash, g_str_equal, g_free, commit_data_free);
}

static GHashTable *
populate_commit_data_cache (OstreeRepo *repo,
                            GVariant *index_v)
{

  VarSummaryIndexRef index = var_summary_index_from_gvariant (index_v);
  VarMetadataRef index_metadata = var_summary_index_get_metadata (index);
  VarSummaryIndexSubsummariesRef subsummaries = var_summary_index_get_subsummaries (index);
  gsize n_subsummaries = var_summary_index_subsummaries_get_length (subsummaries);
  guint32 cache_version;
  g_autoptr(GHashTable) commit_data_cache = commit_data_cache_new ();

  cache_version = GUINT32_FROM_LE (var_metadata_lookup_uint32 (index_metadata, "xa.cache-version", 0));
  if (cache_version < FLATPAK_XA_CACHE_VERSION)
    {
      /* Need to re-index to get all data */
      g_info ("Old summary cache version %d, not using cache", cache_version);
      return NULL;
    }

  for (gsize i = 0; i < n_subsummaries; i++)
    {
      VarSummaryIndexSubsummariesEntryRef entry = var_summary_index_subsummaries_get_at (subsummaries, i);
      const char *name = var_summary_index_subsummaries_entry_get_key (entry);
      const char *s;
      g_autofree char *subset = NULL;
      VarSubsummaryRef subsummary = var_summary_index_subsummaries_entry_get_value (entry);
      gsize checksum_bytes_len;
      const guchar *checksum_bytes;
      g_autofree char *digest = NULL;
      g_autoptr(GVariant) summary_v = NULL;
      VarSummaryRef summary;
      VarRefMapRef ref_map;
      gsize n_refs;

      checksum_bytes = var_subsummary_peek_checksum (subsummary, &checksum_bytes_len);
      if (G_UNLIKELY (checksum_bytes_len != OSTREE_SHA256_DIGEST_LEN))
        {
          g_info ("Invalid checksum for digested summary, not using cache");
          return NULL;
        }
      digest = ostree_checksum_from_bytes (checksum_bytes);

      s = strrchr (name, '-');
      if (s != NULL)
        subset = g_strndup (name, s - name);
      else
        subset = g_strdup ("");

      summary_v = flatpak_repo_load_digested_summary (repo, digest, NULL);
      if (summary_v == NULL)
        {
          g_info ("Failed to load digested summary %s, not using cache", digest);
          return NULL;
        }

      /* Note that all summaries refered to by the index is in new format */
      summary = var_summary_from_gvariant (summary_v);
      ref_map = var_summary_get_ref_map (summary);
      n_refs = var_ref_map_get_length (ref_map);
      for (gsize j = 0; j < n_refs; j++)
        {
          VarRefMapEntryRef e = var_ref_map_get_at (ref_map, j);
          const char *ref = var_ref_map_entry_get_ref (e);
          VarRefInfoRef info = var_ref_map_entry_get_info (e);
          VarMetadataRef commit_metadata = var_ref_info_get_metadata (info);
          guint64 commit_size = var_ref_info_get_commit_size (info);
          const guchar *commit_bytes;
          gsize commit_bytes_len;
          g_autofree char *rev = NULL;
          CommitData *rev_data;
          VarVariantRef xa_data_v;
          VarCacheDataRef xa_data;

          if (!flatpak_is_app_runtime_or_appstream_ref (ref))
            continue;

          commit_bytes = var_ref_info_peek_checksum (info, &commit_bytes_len);
          if (G_UNLIKELY (commit_bytes_len != OSTREE_SHA256_DIGEST_LEN))
            continue;

          if (!var_metadata_lookup (commit_metadata, "xa.data", NULL, &xa_data_v) ||
              !var_variant_is_type (xa_data_v, G_VARIANT_TYPE ("(tts)")))
            {
              g_info ("Missing xa.data for ref %s, not using cache", ref);
              return NULL;
            }

          xa_data = var_cache_data_from_variant (xa_data_v);

          rev = ostree_checksum_from_bytes (commit_bytes);
          rev_data = g_hash_table_lookup (commit_data_cache, rev);
          if (rev_data == NULL)
            {
              g_auto(GVariantBuilder) sparse_builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;
              g_variant_builder_init (&sparse_builder, G_VARIANT_TYPE_VARDICT);
              gboolean has_sparse = FALSE;

              rev_data = g_new0 (CommitData, 1);
              rev_data->installed_size = var_cache_data_get_installed_size (xa_data);
              rev_data->download_size = var_cache_data_get_download_size (xa_data);
              rev_data->metadata_contents = g_strdup (var_cache_data_get_metadata (xa_data));
              rev_data->commit_size = commit_size;
              rev_data->commit_timestamp = GUINT64_FROM_BE (var_metadata_lookup_uint64 (commit_metadata, OSTREE_COMMIT_TIMESTAMP2, 0));

              /* Get sparse data */
              gsize len = var_metadata_get_length (commit_metadata);
              for (gsize k = 0; k < len; k++)
                {
                  VarMetadataEntryRef m = var_metadata_get_at (commit_metadata, k);
                  const char *m_key = var_metadata_entry_get_key (m);
                  if (!g_str_has_prefix (m_key, "ot.") &&
                      !g_str_has_prefix (m_key, "ostree.") &&
                      strcmp (m_key, "xa.data") != 0)
                    {
                      VarVariantRef v = var_metadata_entry_get_value (m);
                      g_autoptr(GVariant) vv = g_variant_ref_sink (var_variant_dup_to_gvariant (v));
                      g_autoptr(GVariant) child = g_variant_get_child_value (vv, 0);
                      g_variant_builder_add (&sparse_builder, "{sv}", m_key, child);
                      has_sparse = TRUE;
                    }
                }

              if (has_sparse)
                rev_data->sparse_data = g_variant_ref_sink (g_variant_builder_end (&sparse_builder));

              g_hash_table_insert (commit_data_cache, g_strdup (rev), (CommitData *)rev_data);
            }

          if (*subset != 0)
            {
              if (rev_data->subsets == NULL)
                rev_data->subsets = g_ptr_array_new_with_free_func (g_free);

              if (!flatpak_g_ptr_array_contains_string (rev_data->subsets, subset))
                g_ptr_array_add (rev_data->subsets, g_strdup (subset));
            }
        }
    }

  return g_steal_pointer (&commit_data_cache);
}

static CommitData *
read_commit_data (OstreeRepo   *repo,
                  const char   *ref,
                  const char   *rev,
                  GCancellable *cancellable,
                  GError      **error)
{
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) metadata = NULL;
  guint64 installed_size = 0;
  guint64 download_size = 0;
  g_autofree char *metadata_contents = NULL;
  g_autofree char *commit = NULL;
  g_autoptr(GVariant) commit_v = NULL;
  g_autoptr(GVariant) commit_metadata = NULL;
  g_autoptr(GPtrArray) subsets = NULL;
  CommitData *rev_data;
  const char *eol = NULL;
  const char *eol_rebase = NULL;
  int token_type = -1;
  g_autoptr(GVariant) extra_data_sources = NULL;
  guint32 n_extra_data = 0;
  guint64 total_extra_data_download_size = 0;
  g_autoptr(GVariantIter) subsets_iter = NULL;

  if (!ostree_repo_read_commit (repo, rev, &root, &commit, NULL, error))
    return NULL;

  if (!ostree_repo_load_commit (repo, commit, &commit_v, NULL, error))
    return NULL;

  commit_metadata = g_variant_get_child_value (commit_v, 0);
  if (!g_variant_lookup (commit_metadata, "xa.metadata", "s", &metadata_contents))
    {
      metadata = g_file_get_child (root, "metadata");
      if (!g_file_load_contents (metadata, cancellable, &metadata_contents, NULL, NULL, NULL))
        metadata_contents = g_strdup ("");
    }

  if (g_variant_lookup (commit_metadata, "xa.installed-size", "t", &installed_size) &&
      g_variant_lookup (commit_metadata, "xa.download-size", "t", &download_size))
    {
      installed_size = GUINT64_FROM_BE (installed_size);
      download_size = GUINT64_FROM_BE (download_size);
    }
  else
    {
      if (!flatpak_repo_collect_sizes (repo, root, &installed_size, &download_size, cancellable, error))
        return NULL;
    }

  if (g_variant_lookup (commit_metadata, "xa.subsets", "as", &subsets_iter))
    {
      const char *subset;
      subsets = g_ptr_array_new_with_free_func (g_free);
      while (g_variant_iter_next (subsets_iter, "&s", &subset))
        g_ptr_array_add (subsets, g_strdup (subset));
    }

  flatpak_repo_collect_extra_data_sizes (repo, rev, &installed_size, &download_size);

  rev_data = g_new0 (CommitData, 1);
  rev_data->installed_size = installed_size;
  rev_data->download_size = download_size;
  rev_data->metadata_contents = g_steal_pointer (&metadata_contents);
  rev_data->subsets = g_steal_pointer (&subsets);
  rev_data->commit_size = g_variant_get_size (commit_v);
  rev_data->commit_timestamp = ostree_commit_get_timestamp (commit_v);

  g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE, "&s", &eol);
  g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE_REBASE, "&s", &eol_rebase);
  if (g_variant_lookup (commit_metadata, "xa.token-type", "i", &token_type))
    token_type = GINT32_FROM_LE(token_type);

  extra_data_sources = flatpak_commit_get_extra_data_sources (commit_v, NULL);
  if (extra_data_sources)
    {
      n_extra_data = g_variant_n_children (extra_data_sources);
      for (int i = 0; i < n_extra_data; i++)
        {
          guint64 extra_download_size;
          flatpak_repo_parse_extra_data_sources (extra_data_sources, i,
                                                 NULL,
                                                 &extra_download_size,
                                                 NULL,
                                                 NULL,
                                                 NULL);
          total_extra_data_download_size += extra_download_size;
        }
    }

  if (eol || eol_rebase || token_type >= 0 || n_extra_data > 0)
    {
      g_auto(GVariantBuilder) sparse_builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;
      g_variant_builder_init (&sparse_builder, G_VARIANT_TYPE_VARDICT);
      if (eol)
        g_variant_builder_add (&sparse_builder, "{sv}", FLATPAK_SPARSE_CACHE_KEY_ENDOFLINE, g_variant_new_string (eol));
      if (eol_rebase)
        g_variant_builder_add (&sparse_builder, "{sv}", FLATPAK_SPARSE_CACHE_KEY_ENDOFLINE_REBASE, g_variant_new_string (eol_rebase));
      if (token_type >= 0)
        g_variant_builder_add (&sparse_builder, "{sv}", FLATPAK_SPARSE_CACHE_KEY_TOKEN_TYPE, g_variant_new_int32 (GINT32_TO_LE(token_type)));
      if (n_extra_data > 0)
        g_variant_builder_add (&sparse_builder, "{sv}", FLATPAK_SPARSE_CACHE_KEY_EXTRA_DATA_SIZE,
                               g_variant_new ("(ut)", GUINT32_TO_LE(n_extra_data), GUINT64_TO_LE(total_extra_data_download_size)));

      rev_data->sparse_data = g_variant_ref_sink (g_variant_builder_end (&sparse_builder));
    }

  return rev_data;
}

static void
_ostree_parse_delta_name (const char *delta_name,
                          char      **out_from,
                          char      **out_to)
{
  g_auto(GStrv) parts = g_strsplit (delta_name, "-", 2);

  if (parts[0] && parts[1])
    {
      *out_from = g_steal_pointer (&parts[0]);
      *out_to = g_steal_pointer (&parts[1]);
    }
  else
    {
      *out_from = NULL;
      *out_to = g_steal_pointer (&parts[0]);
    }
}

static GString *
static_delta_path_base (const char *dir,
                        const char *from,
                        const char *to)
{
  guint8 csum_to[OSTREE_SHA256_DIGEST_LEN];
  char to_b64[44];
  guint8 csum_to_copy[OSTREE_SHA256_DIGEST_LEN];
  GString *ret = g_string_new (dir);

  ostree_checksum_inplace_to_bytes (to, csum_to);
  ostree_checksum_b64_inplace_from_bytes (csum_to, to_b64);
  ostree_checksum_b64_inplace_to_bytes (to_b64, csum_to_copy);

  g_assert (memcmp (csum_to, csum_to_copy, OSTREE_SHA256_DIGEST_LEN) == 0);

  if (from != NULL)
    {
      guint8 csum_from[OSTREE_SHA256_DIGEST_LEN];
      char from_b64[44];

      ostree_checksum_inplace_to_bytes (from, csum_from);
      ostree_checksum_b64_inplace_from_bytes (csum_from, from_b64);

      g_string_append_c (ret, from_b64[0]);
      g_string_append_c (ret, from_b64[1]);
      g_string_append_c (ret, '/');
      g_string_append (ret, from_b64 + 2);
      g_string_append_c (ret, '-');
    }

  g_string_append_c (ret, to_b64[0]);
  g_string_append_c (ret, to_b64[1]);
  if (from == NULL)
    g_string_append_c (ret, '/');
  g_string_append (ret, to_b64 + 2);

  return ret;
}

static char *
_ostree_get_relative_static_delta_path (const char *from,
                                        const char *to,
                                        const char *target)
{
  GString *ret = static_delta_path_base ("deltas/", from, to);

  if (target != NULL)
    {
      g_string_append_c (ret, '/');
      g_string_append (ret, target);
    }

  return g_string_free (ret, FALSE);
}

static char *
_ostree_get_relative_static_delta_superblock_path (const char        *from,
                                                   const char        *to)
{
  return _ostree_get_relative_static_delta_path (from, to, "superblock");
}

static GVariant *
_ostree_repo_static_delta_superblock_digest (OstreeRepo    *repo,
                                             const char    *from,
                                             const char    *to,
                                             GCancellable  *cancellable,
                                             GError       **error)
{
  g_autofree char *superblock = _ostree_get_relative_static_delta_superblock_path ((from && from[0]) ? from : NULL, to);
  glnx_autofd int fd = -1;
  guint8 digest[OSTREE_SHA256_DIGEST_LEN];
  gsize len;
  gpointer data = NULL;

  if (!glnx_openat_rdonly (ostree_repo_get_dfd (repo), superblock, TRUE, &fd, error))
    return NULL;

  g_autoptr(GBytes) superblock_content = glnx_fd_readall_bytes (fd, cancellable, error);
  if (!superblock_content)
    return NULL;

  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  g_checksum_update (checksum, g_bytes_get_data (superblock_content, NULL), g_bytes_get_size (superblock_content));
  len = sizeof digest;
  g_checksum_get_digest (checksum, digest, &len);

  data = g_memdup2 (digest, len);
  return g_variant_new_from_data (G_VARIANT_TYPE ("ay"),
                                  data, len,
                                  FALSE, g_free, data);
}

static char *
appstream_ref_get_subset (const char *ref)
{
  if (!g_str_has_prefix (ref, "appstream2/"))
    return NULL;

  const char *rest = ref + strlen ("appstream2/");
  const char *dash = strrchr (rest, '-');
  if (dash == NULL)
    return NULL;

  return g_strndup (rest, dash - rest);
}

char *
flatpak_get_arch_for_ref (const char *ref)
{
  if (g_str_has_prefix (ref, "appstream/") ||
      g_str_has_prefix (ref, "appstream2/"))
    {
      const char *rest = strchr (ref, '/') + 1; /* Guaranteed to exist per above check */
      const char *dash = strrchr (rest, '-'); /*  Subset appstream refs are appstream2/$subset-$arch */
      if (dash != NULL)
        rest = dash + 1;
      return g_strdup (rest);
    }
    else if (g_str_has_prefix (ref, "app/") ||
      g_str_has_prefix (ref, "runtime/"))
    {
      const char *slash;
      const char *arch;

      slash = strchr (ref, '/') + 1; /* Guaranteed to exist per above check */
      slash = strchr (slash, '/'); /* Skip id */
      if (slash == NULL)
        return NULL;
      arch = slash + 1;

      slash = strchr (arch, '/'); /* skip to end arch */
      if (slash == NULL)
        return NULL;

      return g_strndup (arch, slash - arch);
    }

  return NULL;
}

typedef enum {
  DIFF_OP_KIND_RESUSE_OLD,
  DIFF_OP_KIND_SKIP_OLD,
  DIFF_OP_KIND_DATA,
} DiffOpKind;

typedef struct {
  DiffOpKind kind;
  gsize size;
} DiffOp;

typedef struct {
  const guchar *old_data;
  const guchar *new_data;

  GArray *ops;
  GArray *data;

  gsize last_old_offset;
  gsize last_new_offset;
} DiffData;

static gsize
match_bytes_at_start (const guchar *data1,
                      gsize data1_len,
                      const guchar *data2,
                      gsize data2_len)
{
  gsize len = 0;
  gsize max_len = MIN (data1_len, data2_len);

  while (len < max_len)
    {
      if (*data1 != *data2)
        break;
      data1++;
      data2++;
      len++;
    }
  return len;
}

static gsize
match_bytes_at_end (const guchar *data1,
                    gsize data1_len,
                    const guchar *data2,
                    gsize data2_len)
{
  gsize len = 0;
  gsize max_len = MIN (data1_len, data2_len);

  data1 += data1_len - 1;
  data2 += data2_len - 1;

  while (len < max_len)
    {
      if (*data1 != *data2)
        break;
      data1--;
      data2--;
      len++;
    }
  return len;
}

static DiffOp *
diff_ensure_op (DiffData *data,
                DiffOpKind kind)
{
  if (data->ops->len == 0 ||
      g_array_index (data->ops, DiffOp, data->ops->len-1).kind != kind)
    {
      DiffOp op = {kind, 0};
      g_array_append_val (data->ops, op);
    }

  return &g_array_index (data->ops, DiffOp, data->ops->len-1);
}

static void
diff_emit_reuse (DiffData *data,
                 gsize size)
{
  DiffOp *op;

  if (size == 0)
    return;

  op = diff_ensure_op (data, DIFF_OP_KIND_RESUSE_OLD);
  op->size += size;
}

static void
diff_emit_skip (DiffData *data,
                gsize size)
{
  DiffOp *op;

  if (size == 0)
    return;

  op = diff_ensure_op (data, DIFF_OP_KIND_SKIP_OLD);
  op->size += size;
}

static void
diff_emit_data (DiffData *data,
                gsize size,
                const guchar *new_data)
{
  DiffOp *op;

  if (size == 0)
    return;

  op = diff_ensure_op (data, DIFF_OP_KIND_DATA);
  op->size += size;

  g_array_append_vals (data->data, new_data, size);
}

static GBytes *
diff_encode (DiffData *data, GError **error)
{
  g_autoptr(GOutputStream) mem = g_memory_output_stream_new_resizable ();
  g_autoptr(GDataOutputStream) out = g_data_output_stream_new (mem);
  gsize ops_count = 0;

  g_data_output_stream_set_byte_order (out, G_DATA_STREAM_BYTE_ORDER_LITTLE_ENDIAN);

  /* Header */
  if (!g_output_stream_write_all (G_OUTPUT_STREAM (out),
                                  FLATPAK_SUMMARY_DIFF_HEADER, 4,
                                  NULL, NULL, error))
    return NULL;

  /* Write the ops count placeholder */
  if (!g_data_output_stream_put_uint32 (out, 0, NULL, error))
    return NULL;

  for (gsize i = 0; i < data->ops->len; i++)
    {
      DiffOp *op = &g_array_index (data->ops, DiffOp, i);
      gsize size = op->size;

      while (size > 0)
        {
          /* We leave a nibble at the top for the op */
          guint32 opdata = (guint64)size & 0x0fffffff;
          size -= opdata;

          opdata = opdata | ((0xf & op->kind) << 28);

          if (!g_data_output_stream_put_uint32 (out, opdata, NULL, error))
            return NULL;
          ops_count++;
        }
    }

  /* Then add the data */
  if (data->data->len > 0 &&
      !g_output_stream_write_all (G_OUTPUT_STREAM (out),
                                  data->data->data, data->data->len,
                                  NULL, NULL, error))
    return NULL;

  /* Back-patch in the ops count */
  if (!g_seekable_seek (G_SEEKABLE(out), 4, G_SEEK_SET, NULL, error))
    return NULL;

  if (!g_data_output_stream_put_uint32 (out, ops_count, NULL, error))
    return NULL;

  if (!g_output_stream_close (G_OUTPUT_STREAM (out), NULL, error))
    return NULL;

  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (mem));
}

static void
diff_consume_block2 (DiffData *data,
                     gsize consume_old_offset,
                     gsize consume_old_size,
                     gsize produce_new_offset,
                     gsize produce_new_size)
{
  /* We consumed $consume_old_size bytes from $consume_old_offset to
     produce $produce_new_size bytes at $produce_new_size */

  /* First we copy old data for any matching prefix of the block */

  gsize prefix_len = match_bytes_at_start (data->old_data + consume_old_offset, consume_old_size,
                                           data->new_data + produce_new_offset, produce_new_size);
  diff_emit_reuse (data, prefix_len);

  consume_old_size -= prefix_len;
  consume_old_offset += prefix_len;

  produce_new_size -= prefix_len;
  produce_new_offset += prefix_len;

  /* Then we find the matching suffix for the rest */
  gsize suffix_len = match_bytes_at_end (data->old_data + consume_old_offset, consume_old_size,
                                         data->new_data + produce_new_offset, produce_new_size);

  /* Skip source data until suffix match */
  diff_emit_skip (data, consume_old_size - suffix_len);

  /* Copy new data until suffix match */
  diff_emit_data (data, produce_new_size - suffix_len, data->new_data + produce_new_offset);

  diff_emit_reuse (data, suffix_len);
}

static void
diff_consume_block (DiffData *data,
                    gssize consume_old_offset,
                    gsize consume_old_size,
                    gssize produce_new_offset,
                    gsize produce_new_size)
{
  if (consume_old_offset == -1)
    consume_old_offset = data->last_old_offset;
  if (produce_new_offset == -1)
    produce_new_offset = data->last_new_offset;

  /* We consumed $consume_old_size bytes from $consume_old_offset to
   * produce $produce_new_size bytes at $produce_new_size, however
   * while the emitted blocks are in order they may not cover the
   * every byte, so we emit the inbetwen blocks separately. */

  if (consume_old_offset != data->last_old_offset ||
      produce_new_offset != data->last_new_offset)
    diff_consume_block2 (data,
                         data->last_old_offset, consume_old_offset - data->last_old_offset ,
                         data->last_new_offset, produce_new_offset - data->last_new_offset);

  diff_consume_block2 (data,
                       consume_old_offset, consume_old_size,
                       produce_new_offset, produce_new_size);

  data->last_old_offset = consume_old_offset + consume_old_size;
  data->last_new_offset = produce_new_offset + produce_new_size;
}

GBytes *
flatpak_summary_apply_diff (GBytes *old,
                            GBytes *diff,
                            GError **error)
{
  g_autoptr(GBytes) uncompressed = NULL;
  const guchar *diffdata;
  gsize diff_size;
  guint32 *ops;
  guint32 n_ops;
  gsize data_offset;
  gsize data_size;
  const guchar *data;
  const guchar *old_data = g_bytes_get_data (old, NULL);
  gsize old_size = g_bytes_get_size (old);
  g_autoptr(GByteArray) res = g_byte_array_new ();

  uncompressed = flatpak_zlib_decompress_bytes (diff, error);
  if (uncompressed == NULL)
    {
      g_prefix_error (error, "Invalid summary diff: ");
      return NULL;
    }

  diffdata = g_bytes_get_data (uncompressed, NULL);
  diff_size = g_bytes_get_size (uncompressed);

  if (diff_size < 8 ||
      memcmp (diffdata, FLATPAK_SUMMARY_DIFF_HEADER, 4) != 0)
    {
      flatpak_fail (error, "Invalid summary diff");
      return NULL;
    }

  n_ops = GUINT32_FROM_LE (*(guint32 *)(diffdata+4));
  ops = (guint32 *)(diffdata+8);

  data_offset = 4 + 4 + 4 * n_ops;

  /* All ops must fit in diff, and avoid wrapping the multiply */
  if (data_offset > diff_size ||
      (data_offset - 4 - 4) / 4 != n_ops)
    {
      flatpak_fail (error, "Invalid summary diff");
      return NULL;
    }

  data = diffdata + data_offset;
  data_size = diff_size - data_offset;

  for (gsize i = 0; i < n_ops; i++)
    {
      guint32 opdata = GUINT32_FROM_LE (ops[i]);
      guint32 kind = (opdata & 0xf0000000) >> 28;
      guint32 size = opdata & 0x0fffffff;

      switch (kind)
        {
        case DIFF_OP_KIND_RESUSE_OLD:
          if (size > old_size)
            {
              flatpak_fail (error, "Invalid summary diff");
              return NULL;
            }
          g_byte_array_append (res, old_data, size);
          old_data += size;
          old_size -= size;
          break;
        case DIFF_OP_KIND_SKIP_OLD:
          if (size > old_size)
            {
              flatpak_fail (error, "Invalid summary diff");
              return NULL;
            }
          old_data += size;
          old_size -= size;
          break;
        case DIFF_OP_KIND_DATA:
          if (size > data_size)
            {
              flatpak_fail (error, "Invalid summary diff");
              return NULL;
            }
          g_byte_array_append (res, data, size);
          data += size;
          data_size -= size;
          break;
        default:
          flatpak_fail (error, "Invalid summary diff");
          return NULL;
        }
    }

  return g_byte_array_free_to_bytes (g_steal_pointer (&res));
}


static GBytes *
flatpak_summary_generate_diff (GVariant *old_v,
                               GVariant *new_v,
                               GError **error)
{
  VarSummaryRef new, old;
  VarRefMapRef new_refs, old_refs;
  VarRefMapEntryRef new_entry, old_entry;
  gsize new_len, old_len;
  int new_i, old_i;
  const char *old_ref, *new_ref;
  g_autoptr(GArray) ops = g_array_new (FALSE, TRUE, sizeof (DiffOp));
  g_autoptr(GArray) data_bytes = g_array_new (FALSE, TRUE, 1);
  g_autoptr(GBytes) diff_uncompressed = NULL;
  g_autoptr(GBytes) diff_compressed = NULL;
  DiffData data = {
    g_variant_get_data (old_v),
    g_variant_get_data (new_v),
    ops,
    data_bytes,
  };

  new = var_summary_from_gvariant (new_v);
  old = var_summary_from_gvariant (old_v);

  new_refs = var_summary_get_ref_map (new);
  old_refs = var_summary_get_ref_map (old);

  new_len = var_ref_map_get_length (new_refs);
  old_len = var_ref_map_get_length (old_refs);

  new_i = old_i = 0;
  while (new_i < new_len && old_i < old_len)
    {
      if (new_i == new_len)
        {
          /* Just old left */
          old_entry = var_ref_map_get_at (old_refs, old_i);
          old_ref = var_ref_map_entry_get_ref (old_entry);
          old_i++;
          diff_consume_block (&data,
                              -1, 0,
                              (const guchar *)new_entry.base - (const guchar *)new.base, new_entry.size);
        }
      else if (old_i == old_len)
        {
          /* Just new left */
          new_entry = var_ref_map_get_at (new_refs, new_i);
          new_ref = var_ref_map_entry_get_ref (new_entry);
          diff_consume_block (&data,
                              (const guchar *)old_entry.base - (const guchar *)old.base, old_entry.size,
                              -1, 0);

          new_i++;
        }
      else
        {
          new_entry = var_ref_map_get_at (new_refs, new_i);
          new_ref = var_ref_map_entry_get_ref (new_entry);

          old_entry = var_ref_map_get_at (old_refs, old_i);
          old_ref = var_ref_map_entry_get_ref (old_entry);

          int cmp = strcmp (new_ref, old_ref);
          if (cmp == 0)
            {
              /* same ref */
              diff_consume_block (&data,
                                  (const guchar *)old_entry.base - (const guchar *)old.base, old_entry.size,
                                  (const guchar *)new_entry.base - (const guchar *)new.base, new_entry.size);
              old_i++;
              new_i++;
            }
          else if (cmp < 0)
            {
              /* new added */
              diff_consume_block (&data,
                                  -1, 0,
                                  (const guchar *)new_entry.base - (const guchar *)new.base, new_entry.size);
              new_i++;
            }
          else
            {
              /* old removed */
              diff_consume_block (&data,
                                  (const guchar *)old_entry.base - (const guchar *)old.base, old_entry.size,
                                  -1, 0);
              old_i++;
            }
        }
    }

  /* Flush till the end */
  diff_consume_block2 (&data,
                       data.last_old_offset, old.size - data.last_old_offset,
                       data.last_new_offset, new.size - data.last_new_offset);

  diff_uncompressed = diff_encode (&data, error);
  if (diff_uncompressed == NULL)
    return NULL;

  diff_compressed = flatpak_zlib_compress_bytes (diff_uncompressed, 9, error);
  if (diff_compressed == NULL)
    return NULL;

#ifdef VALIDATE_DIFF
  {
    g_autoptr(GError) apply_error = NULL;
    g_autoptr(GBytes) old_bytes = g_variant_get_data_as_bytes (old_v);
    g_autoptr(GBytes) new_bytes = g_variant_get_data_as_bytes (new_v);
    g_autoptr(GBytes) applied = flatpak_summary_apply_diff (old_bytes, diff_compressed, &apply_error);
    g_assert (applied != NULL);
    g_assert (g_bytes_equal (applied, new_bytes));
  }
#endif

  return g_steal_pointer (&diff_compressed);
}

static void
variant_dict_merge (GVariantDict *dict,
                    GVariant *to_merge)
{
  GVariantIter iter;
  gchar *key;
  GVariant *value;

  if (to_merge)
    {
      g_variant_iter_init (&iter, to_merge);
      while (g_variant_iter_next (&iter, "{sv}", &key, &value))
        {
          g_variant_dict_insert_value (dict, key, value);
          g_variant_unref (value);
          g_free (key);
        }
    }
}

static void
add_summary_metadata (OstreeRepo   *repo,
                      GVariantBuilder *metadata_builder)
{
  GKeyFile *config;
  g_autofree char *title = NULL;
  g_autofree char *comment = NULL;
  g_autofree char *description = NULL;
  g_autofree char *homepage = NULL;
  g_autofree char *icon = NULL;
  g_autofree char *redirect_url = NULL;
  g_autofree char *default_branch = NULL;
  g_autofree char *remote_mode_str = NULL;
  g_autofree char *authenticator_name = NULL;
  g_autofree char *gpg_keys = NULL;
  g_auto(GStrv) config_keys = NULL;
  int authenticator_install = -1;
  const char *collection_id;
  gboolean deploy_collection_id = FALSE;
  gboolean deploy_sideload_collection_id = FALSE;
  gboolean tombstone_commits = FALSE;

  config = ostree_repo_get_config (repo);

  if (config)
    {
      remote_mode_str = g_key_file_get_string (config, "core", "mode", NULL);
      tombstone_commits = g_key_file_get_boolean (config, "core", "tombstone-commits", NULL);

      title = g_key_file_get_string (config, "flatpak", "title", NULL);
      comment = g_key_file_get_string (config, "flatpak", "comment", NULL);
      description = g_key_file_get_string (config, "flatpak", "description", NULL);
      homepage = g_key_file_get_string (config, "flatpak", "homepage", NULL);
      icon = g_key_file_get_string (config, "flatpak", "icon", NULL);
      default_branch = g_key_file_get_string (config, "flatpak", "default-branch", NULL);
      gpg_keys = g_key_file_get_string (config, "flatpak", "gpg-keys", NULL);
      redirect_url = g_key_file_get_string (config, "flatpak", "redirect-url", NULL);
      deploy_sideload_collection_id = g_key_file_get_boolean (config, "flatpak", "deploy-sideload-collection-id", NULL);
      deploy_collection_id = g_key_file_get_boolean (config, "flatpak", "deploy-collection-id", NULL);
      authenticator_name = g_key_file_get_string (config, "flatpak", "authenticator-name", NULL);
      if (g_key_file_has_key (config, "flatpak", "authenticator-install", NULL))
        authenticator_install = g_key_file_get_boolean (config, "flatpak", "authenticator-install", NULL);

      config_keys = g_key_file_get_keys (config, "flatpak", NULL, NULL);
    }

  collection_id = ostree_repo_get_collection_id (repo);

  g_variant_builder_add (metadata_builder, "{sv}", "ostree.summary.mode",
                         g_variant_new_string (remote_mode_str ? remote_mode_str : "bare"));
  g_variant_builder_add (metadata_builder, "{sv}", "ostree.summary.tombstone-commits",
                         g_variant_new_boolean (tombstone_commits));
  g_variant_builder_add (metadata_builder, "{sv}", "ostree.summary.indexed-deltas",
                         g_variant_new_boolean (TRUE));
  g_variant_builder_add (metadata_builder, "{sv}", "ostree.summary.last-modified",
                         g_variant_new_uint64 (GUINT64_TO_BE (g_get_real_time () / G_USEC_PER_SEC)));

  if (collection_id)
    g_variant_builder_add (metadata_builder, "{sv}", "ostree.summary.collection-id",
                           g_variant_new_string (collection_id));

  if (title)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.title",
                           g_variant_new_string (title));

  if (comment)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.comment",
                           g_variant_new_string (comment));

  if (description)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.description",
                           g_variant_new_string (description));

  if (homepage)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.homepage",
                           g_variant_new_string (homepage));

  if (icon)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.icon",
                           g_variant_new_string (icon));

  if (redirect_url)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.redirect-url",
                           g_variant_new_string (redirect_url));

  if (default_branch)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.default-branch",
                           g_variant_new_string (default_branch));

  if (deploy_collection_id && collection_id != NULL)
    g_variant_builder_add (metadata_builder, "{sv}", OSTREE_META_KEY_DEPLOY_COLLECTION_ID,
                           g_variant_new_string (collection_id));
  else if (deploy_sideload_collection_id && collection_id != NULL)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.deploy-collection-id",
                           g_variant_new_string (collection_id));
  else if (deploy_collection_id)
    g_info ("Ignoring deploy-collection-id=true because no collection ID is set.");

  if (authenticator_name)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.authenticator-name",
                           g_variant_new_string (authenticator_name));

  if (authenticator_install != -1)
    g_variant_builder_add (metadata_builder, "{sv}", "xa.authenticator-install",
                           g_variant_new_boolean (authenticator_install));

  g_variant_builder_add (metadata_builder, "{sv}", "xa.cache-version",
                         g_variant_new_uint32 (GUINT32_TO_LE (FLATPAK_XA_CACHE_VERSION)));

  if (config_keys != NULL)
    {
      for (int i = 0; config_keys[i] != NULL; i++)
        {
          const char *key = config_keys[i];
          g_autofree char *xa_key = NULL;
          g_autofree char *value = NULL;

          if (!g_str_has_prefix (key, "authenticator-options."))
            continue;

          value = g_key_file_get_string (config, "flatpak", key, NULL);
          if (value == NULL)
            continue;

          xa_key = g_strconcat ("xa.", key, NULL);
          g_variant_builder_add (metadata_builder, "{sv}", xa_key,
                                 g_variant_new_string (value));
        }
    }

  if (gpg_keys)
    {
      guchar *decoded;
      gsize decoded_len;

      gpg_keys = g_strstrip (gpg_keys);
      decoded = g_base64_decode (gpg_keys, &decoded_len);

      g_variant_builder_add (metadata_builder, "{sv}", "xa.gpg-keys",
                             g_variant_new_from_data (G_VARIANT_TYPE ("ay"), decoded, decoded_len,
                                                      TRUE, (GDestroyNotify) g_free, decoded));
    }
}

static GVariant *
generate_summary (OstreeRepo   *repo,
                  gboolean      compat_format,
                  GHashTable   *refs,
                  GHashTable   *commit_data_cache,
                  GPtrArray    *delta_names,
                  const char   *subset,
                  const char  **summary_arches,
                  GCancellable *cancellable,
                  GError      **error)
{
  g_autoptr(GVariantBuilder) metadata_builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariantBuilder) ref_data_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{s(tts)}"));
  g_autoptr(GVariantBuilder) ref_sparse_data_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sa{sv}}"));
  g_autoptr(GVariantBuilder) refs_builder = g_variant_builder_new (G_VARIANT_TYPE ("a(s(taya{sv}))"));
  g_autoptr(GVariantBuilder) summary_builder = g_variant_builder_new (OSTREE_SUMMARY_GVARIANT_FORMAT);
  g_autoptr(GHashTable) summary_arches_ht = NULL;
  g_autoptr(GHashTable) commits = NULL;
  g_autoptr(GList) ordered_keys = NULL;
  GList *l = NULL;

  /* In the new format this goes in the summary index instead */
  if (compat_format)
    add_summary_metadata (repo, metadata_builder);

  ordered_keys = g_hash_table_get_keys (refs);
  ordered_keys = g_list_sort (ordered_keys, (GCompareFunc) strcmp);

  if (summary_arches)
    {
      summary_arches_ht = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);
      for (int i = 0; summary_arches[i] != NULL; i++)
        {
          const char *arch = summary_arches[i];

          g_hash_table_add (summary_arches_ht, (char *)arch);
        }
    }

  /* Compute which commits to keep */
  commits = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL); /* strings owned by ref */
  for (l = ordered_keys; l; l = l->next)
    {
      const char *ref = l->data;
      const char *rev = g_hash_table_lookup (refs, ref);
      g_autofree char *arch = NULL;
      const CommitData *rev_data = NULL;

      if (summary_arches)
        {
          /* NOTE: Non-arched (unknown) refs get into all summary versions */
          arch = flatpak_get_arch_for_ref (ref);
          if (arch != NULL && !g_hash_table_contains (summary_arches_ht, arch))
            continue; /* Filter this ref by arch */
        }

      rev_data = g_hash_table_lookup (commit_data_cache, rev);
      if (*subset != 0)
        {
          /* Subset summaries keep the appstream2/$subset-$arch, and have no appstream/ compat branch */

          if (g_str_has_prefix (ref, "appstream/"))
            {
              continue; /* No compat branch in subsets */
            }
          else if (g_str_has_prefix (ref, "appstream2/"))
            {
              g_autofree char *ref_subset = appstream_ref_get_subset (ref);
              if (ref_subset == NULL)
                continue; /* Non-subset, ignore */

              if (strcmp (subset, ref_subset) != 0)
                continue; /* Different subset, ignore */

              /* Otherwise, keep */
            }
          else if (rev_data)
            {
              if (rev_data->subsets == NULL ||
                  !flatpak_g_ptr_array_contains_string (rev_data->subsets, subset))
                continue; /* Ref is not in this subset */
            }
        }
      else
        {
          /* non-subset, keep everything but subset appstream refs */

          g_autofree char *ref_subset = appstream_ref_get_subset (ref);
          if (ref_subset != NULL)
            continue; /* Subset appstream ref, ignore */
        }

      g_hash_table_add (commits, (char *)rev);
    }

  /* Create refs list, metadata and sparse_data */
  for (l = ordered_keys; l; l = l->next)
    {
      const char *ref = l->data;
      const char *rev = g_hash_table_lookup (refs, ref);
      const CommitData *rev_data = NULL;
      g_auto(GVariantDict) commit_metadata_builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;
      guint64 commit_size;
      guint64 commit_timestamp;

      if (!g_hash_table_contains (commits, rev))
        continue; /* Filter out commit (by arch & subset) */

      if (flatpak_is_app_runtime_or_appstream_ref (ref))
        rev_data = g_hash_table_lookup (commit_data_cache, rev);

      if (rev_data != NULL)
        {
          commit_size = rev_data->commit_size;
          commit_timestamp = rev_data->commit_timestamp;
        }
      else
        {
          g_autoptr(GVariant) commit_obj = NULL;
          if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, rev, &commit_obj, error))
            return NULL;
          commit_size = g_variant_get_size (commit_obj);
          commit_timestamp = ostree_commit_get_timestamp (commit_obj);
        }

      g_variant_dict_init (&commit_metadata_builder, NULL);
      if (!compat_format && rev_data)
        {
          g_variant_dict_insert (&commit_metadata_builder, "xa.data", "(tts)",
                                 GUINT64_TO_BE (rev_data->installed_size),
                                 GUINT64_TO_BE (rev_data->download_size),
                                 rev_data->metadata_contents);
          variant_dict_merge (&commit_metadata_builder, rev_data->sparse_data);
        }

      /* For the new format summary we use a shorter name for the timestamp to save space */
      g_variant_dict_insert_value (&commit_metadata_builder,
                                   compat_format ? OSTREE_COMMIT_TIMESTAMP  : OSTREE_COMMIT_TIMESTAMP2,
                                   g_variant_new_uint64 (GUINT64_TO_BE (commit_timestamp)));

      g_variant_builder_add_value (refs_builder,
                                   g_variant_new ("(s(t@ay@a{sv}))", ref,
                                                  commit_size,
                                                  ostree_checksum_to_bytes_v (rev),
                                                  g_variant_dict_end (&commit_metadata_builder)));

      if (compat_format && rev_data)
        {
          g_variant_builder_add (ref_data_builder, "{s(tts)}",
                                 ref,
                                 GUINT64_TO_BE (rev_data->installed_size),
                                 GUINT64_TO_BE (rev_data->download_size),
                                 rev_data->metadata_contents);
          if (rev_data->sparse_data)
            g_variant_builder_add (ref_sparse_data_builder, "{s@a{sv}}",
                                   ref, rev_data->sparse_data);
        }
    }

  if (delta_names)
    {
      g_auto(GVariantDict) deltas_builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;

      g_variant_dict_init (&deltas_builder, NULL);
      for (guint i = 0; i < delta_names->len; i++)
        {
          g_autofree char *from = NULL;
          g_autofree char *to = NULL;
          GVariant *digest;

          _ostree_parse_delta_name (delta_names->pdata[i], &from, &to);

          /* Only keep deltas going to a ref that is in the summary
           * (i.e. not arch filtered or random) */
          if (!g_hash_table_contains (commits, to))
            continue;

          digest = _ostree_repo_static_delta_superblock_digest (repo,
                                                                (from && from[0]) ? from : NULL,
                                                                to, cancellable, error);
          if (digest == NULL)
            return FALSE;

          g_variant_dict_insert_value (&deltas_builder, delta_names->pdata[i], digest);
        }

      if (delta_names->len > 0)
        g_variant_builder_add (metadata_builder, "{sv}", "ostree.static-deltas", g_variant_dict_end (&deltas_builder));
    }

  if (compat_format)
    {
      /* Note: xa.cache doesn’t need to support collection IDs for the refs listed
       * in it, because the xa.cache metadata is stored on the ostree-metadata ref,
       * which is itself strongly bound to a collection ID — so that collection ID
       * is bound to all the refs in xa.cache. If a client is using the xa.cache
       * data from a summary file (rather than an ostree-metadata branch), they are
       * too old to care about collection IDs anyway. */
      g_variant_builder_add (metadata_builder, "{sv}", "xa.cache",
                             g_variant_new_variant (g_variant_builder_end (ref_data_builder)));
      g_variant_builder_add (metadata_builder, "{sv}", "xa.sparse-cache",
                             g_variant_builder_end (ref_sparse_data_builder));
    }
  else
    {
      g_variant_builder_add (metadata_builder, "{sv}", "xa.summary-version",
                             g_variant_new_uint32 (GUINT32_TO_LE (FLATPAK_XA_SUMMARY_VERSION)));
    }

  g_variant_builder_add_value (summary_builder, g_variant_builder_end (refs_builder));
  g_variant_builder_add_value (summary_builder, g_variant_builder_end (metadata_builder));

  return g_variant_ref_sink (g_variant_builder_end (summary_builder));
}

static GVariant *
read_digested_summary (OstreeRepo   *repo,
                       const char   *digest,
                       GHashTable   *digested_summary_cache,
                       GCancellable *cancellable,
                       GError      **error)
{
  GVariant *cached;
  g_autoptr(GVariant) loaded = NULL;

  cached = g_hash_table_lookup (digested_summary_cache, digest);
  if (cached)
    return g_variant_ref (cached);

  loaded = flatpak_repo_load_digested_summary (repo, digest, error);
  if (loaded == NULL)
    return NULL;

  g_hash_table_insert (digested_summary_cache, g_strdup (digest), g_variant_ref (loaded));

  return g_steal_pointer (&loaded);
}

static gboolean
add_to_history (OstreeRepo      *repo,
                GVariantBuilder *history_builder,
                VarChecksumRef   old_digest_vv,
                GVariant        *current_digest_v,
                GVariant        *current_content,
                GHashTable      *digested_summary_cache,
                guint           *history_len,
                guint            max_history_length,
                GCancellable    *cancellable,
                GError         **error)
{
  g_autoptr(GVariant) old_digest_v = g_variant_ref_sink (var_checksum_dup_to_gvariant (old_digest_vv));
  g_autofree char *old_digest = NULL;
  g_autoptr(GVariant) old_content = NULL;
  g_autofree char *current_digest = NULL;
  g_autoptr(GBytes) subsummary_diff = NULL;

  /* Limit history length */
  if (*history_len >= max_history_length)
    return TRUE;

  /* Avoid repeats in the history (in case nothing changed in subsummary) */
  if (g_variant_equal (old_digest_v, current_digest_v))
    return TRUE;

  old_digest = ostree_checksum_from_bytes_v (old_digest_v);
  old_content = read_digested_summary (repo, old_digest, digested_summary_cache, cancellable, NULL);
  if  (old_content == NULL)
    return TRUE; /* Only add parents that still exist */

  subsummary_diff = flatpak_summary_generate_diff (old_content, current_content, error);
  if  (subsummary_diff == NULL)
    return FALSE;

  current_digest = ostree_checksum_from_bytes_v (current_digest_v);

  if (!flatpak_repo_save_digested_summary_delta (repo, old_digest, current_digest,
                                                 subsummary_diff, cancellable, error))
    return FALSE;

  *history_len += 1;
  g_variant_builder_add_value (history_builder, old_digest_v);

  return TRUE;
}

static GVariant *
generate_summary_index (OstreeRepo   *repo,
                        GVariant     *old_index_v,
                        GHashTable   *summaries,
                        GHashTable   *digested_summaries,
                        GHashTable   *digested_summary_cache,
                        const char  **gpg_key_ids,
                        const char   *gpg_homedir,
                        GCancellable *cancellable,
                        GError      **error)
{
  g_autoptr(GVariantBuilder) metadata_builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);
  g_autoptr(GVariantBuilder) subsummary_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{s(ayaaya{sv})}"));
  g_autoptr(GVariantBuilder) index_builder = g_variant_builder_new (FLATPAK_SUMMARY_INDEX_GVARIANT_FORMAT);
  g_autoptr(GVariant) index = NULL;
  g_autoptr(GList) ordered_summaries = NULL;
  guint max_history_length = flatpak_repo_get_summary_history_length (repo);
  GList *l;

  add_summary_metadata (repo, metadata_builder);

  ordered_summaries = g_hash_table_get_keys (summaries);
  ordered_summaries = g_list_sort (ordered_summaries, (GCompareFunc) strcmp);
  for (l = ordered_summaries; l; l = l->next)
    {
      g_auto(GVariantDict) subsummary_metadata_builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;
      const char *subsummary = l->data;
      const char *digest = g_hash_table_lookup (summaries, subsummary);
      g_autoptr(GVariant) digest_v = g_variant_ref_sink (ostree_checksum_to_bytes_v (digest));
      g_autoptr(GVariantBuilder) history_builder = g_variant_builder_new (G_VARIANT_TYPE ("aay"));
      g_autoptr(GVariant) subsummary_content = NULL;

      subsummary_content = read_digested_summary (repo, digest, digested_summary_cache, cancellable, error);
      if  (subsummary_content == NULL)
        return NULL;  /* This really should always be there as we're supposed to index it */

      if (old_index_v)
        {
          VarSummaryIndexRef old_index = var_summary_index_from_gvariant (old_index_v);
          VarSummaryIndexSubsummariesRef old_subsummaries = var_summary_index_get_subsummaries (old_index);
          VarSubsummaryRef old_subsummary;
          guint history_len = 0;

          if (var_summary_index_subsummaries_lookup (old_subsummaries, subsummary, NULL, &old_subsummary))
            {
              VarChecksumRef parent = var_subsummary_get_checksum (old_subsummary);

              /* Add current as first in history */
              if (!add_to_history (repo, history_builder, parent, digest_v, subsummary_content, digested_summary_cache,
                                   &history_len, max_history_length, cancellable, error))
                return FALSE;

              /* Add previous history */
              VarArrayofChecksumRef history = var_subsummary_get_history (old_subsummary);
              gsize len = var_arrayof_checksum_get_length (history);
              for (gsize i = 0; i < len; i++)
                {
                  VarChecksumRef c = var_arrayof_checksum_get_at (history, i);
                  if (!add_to_history (repo, history_builder, c, digest_v, subsummary_content, digested_summary_cache,
                                       &history_len, max_history_length, cancellable, error))
                    return FALSE;
                }
            }
        }

      g_variant_dict_init (&subsummary_metadata_builder, NULL);
      g_variant_builder_add (subsummary_builder, "{s(@ay@aay@a{sv})}",
                             subsummary,
                             digest_v,
                             g_variant_builder_end (history_builder),
                             g_variant_dict_end (&subsummary_metadata_builder));
    }

  g_variant_builder_add_value (index_builder, g_variant_builder_end (subsummary_builder));
  g_variant_builder_add_value (index_builder, g_variant_builder_end (metadata_builder));

  index = g_variant_ref_sink (g_variant_builder_end (index_builder));

  return g_steal_pointer (&index);
}

static gboolean
flatpak_repo_gc_digested_summaries (OstreeRepo *repo,
                                    const char *index_digest,           /* The digest of the current (new) index (if any) */
                                    const char *old_index_digest,       /* The digest of the previous index (if any) */
                                    GHashTable *digested_summaries,     /* generated */
                                    GHashTable *digested_summary_cache, /* generated + referenced */
                                    GCancellable *cancellable,
                                    GError **error)
{
  g_auto(GLnxDirFdIterator) iter = {0};
  int repo_fd = ostree_repo_get_dfd (repo);
  struct dirent *dent;
  const char *ext;
  g_autoptr(GError) local_error = NULL;

  if (!glnx_dirfd_iterator_init_at (repo_fd, "summaries", FALSE, &iter, &local_error))
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return TRUE;

      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  while (TRUE)
    {
      gboolean remove = FALSE;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&iter, &dent, cancellable, error))
        return FALSE;

      if (dent == NULL)
        break;

      if (dent->d_type != DT_REG)
        continue;

      /* Keep it if its an unexpected type */
      ext = strchr (dent->d_name, '.');
      if (ext != NULL)
        {
          if (strcmp (ext, ".gz") == 0 && strlen (dent->d_name) == 64 + 3)
            {
              g_autofree char *sha256 = g_strndup (dent->d_name, 64);

              /* Keep all the referenced summaries */
              if (g_hash_table_contains (digested_summary_cache, sha256))
                {
                  g_info ("Keeping referenced summary %s", dent->d_name);
                  continue;
                }
              /* Remove rest */
              remove = TRUE;
            }
          else if (strcmp (ext, ".delta") == 0)
            {
              const char *dash = strchr (dent->d_name, '-');
              if (dash != NULL && dash < ext && (ext - dash) == 1 + 64)
                {
                  g_autofree char *to_sha256 = g_strndup (dash + 1, 64);

                  /* Only keep deltas going to a generated summary */
                  if (g_hash_table_contains (digested_summaries, to_sha256))
                    {
                      g_info ("Keeping delta to generated summary %s", dent->d_name);
                      continue;
                    }
                  /* Remove rest */
                  remove = TRUE;
                }
            }
          else if (strcmp (ext, ".idx.sig") == 0)
            {
              g_autofree char *digest = g_strndup (dent->d_name, strlen (dent->d_name) - strlen (".idx.sig"));

              if (g_strcmp0 (digest, index_digest) == 0)
                continue; /* Always keep current */

              if (g_strcmp0 (digest, old_index_digest) == 0)
                continue; /* Always keep previous one, to avoid some races */

              /* Remove the rest */
              remove = TRUE;
            }
        }

      if (remove)
        {
          g_info ("Removing old digested summary file %s", dent->d_name);
          if (unlinkat (iter.fd, dent->d_name, 0) != 0)
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }
        }
      else
        g_info ("Keeping unexpected summary file %s", dent->d_name);
    }

  return TRUE;
}


/* Update the metadata in the summary file for @repo, and then re-sign the file.
 * If the repo has a collection ID set, additionally store the metadata on a
 * contentless commit in a well-known branch, which is the preferred way of
 * broadcasting per-repo metadata (putting it in the summary file is deprecated,
 * but kept for backwards compatibility).
 *
 * Note that there are two keys for the collection ID: collection-id, and
 * ostree.deploy-collection-id. If a client does not currently have a
 * collection ID configured for this remote, it will *only* update its
 * configuration from ostree.deploy-collection-id.  This allows phased
 * deployment of collection-based repositories. Clients will only update their
 * configuration from an unset to a set collection ID once (otherwise the
 * security properties of collection IDs are broken). */
gboolean
flatpak_repo_update (OstreeRepo   *repo,
                     FlatpakRepoUpdateFlags flags,
                     const char  **gpg_key_ids,
                     const char   *gpg_homedir,
                     GCancellable *cancellable,
                     GError      **error)
{
  g_autoptr(GHashTable) commit_data_cache = NULL;
  g_autoptr(GVariant) compat_summary = NULL;
  g_autoptr(GVariant) summary_index = NULL;
  g_autoptr(GVariant) old_index = NULL;
  g_autoptr(GPtrArray) delta_names = NULL;
  g_auto(GStrv) summary_arches = NULL;
  g_autoptr(GHashTable) refs = NULL;
  g_autoptr(GHashTable) arches = NULL;
  g_autoptr(GHashTable) subsets = NULL;
  g_autoptr(GHashTable) summaries = NULL;
  g_autoptr(GHashTable) digested_summaries = NULL;
  g_autoptr(GHashTable) digested_summary_cache = NULL;
  g_autoptr(GBytes) index_sig = NULL;
  time_t old_compat_sig_mtime;
  GKeyFile *config;
  gboolean disable_index = (flags & FLATPAK_REPO_UPDATE_FLAG_DISABLE_INDEX) != 0;
  g_autofree char *index_digest = NULL;
  g_autofree char *old_index_digest = NULL;

  config = ostree_repo_get_config (repo);

  if (!ostree_repo_list_refs_ext (repo, NULL, &refs,
                                  OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_REMOTES | OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_MIRRORS,
                                  cancellable, error))
    return FALSE;

  old_index = flatpak_repo_load_summary_index (repo, NULL);
  if (old_index)
    commit_data_cache = populate_commit_data_cache (repo, old_index);

  if (commit_data_cache == NULL) /* No index or failed to load it */
    commit_data_cache = commit_data_cache_new ();

  if (!ostree_repo_list_static_delta_names (repo, &delta_names, cancellable, error))
    return FALSE;

  if (config)
    summary_arches = g_key_file_get_string_list (config, "flatpak", "summary-arches", NULL, NULL);

  summaries = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  /* These are the ones we generated */
  digested_summaries = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
  /* These are the ones generated or references */
  digested_summary_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);

  arches = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  subsets = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_hash_table_add (subsets, g_strdup ("")); /* Always have everything subset */

  GLNX_HASH_TABLE_FOREACH_KV (refs, const char *, ref, const char *, rev)
    {
      g_autofree char *arch = flatpak_get_arch_for_ref (ref);
      CommitData *rev_data = NULL;

      if (arch != NULL &&
          !g_hash_table_contains (arches, arch))
        g_hash_table_add (arches, g_steal_pointer (&arch));

      /* Add CommitData for flatpak refs that we didn't already pre-populate */
      if (flatpak_is_app_runtime_or_appstream_ref (ref))
        {
          rev_data = g_hash_table_lookup (commit_data_cache, rev);
          if (rev_data == NULL)
            {
              rev_data = read_commit_data (repo, ref, rev, cancellable, error);
              if (rev_data == NULL)
                return FALSE;

              g_hash_table_insert (commit_data_cache, g_strdup (rev), (CommitData *)rev_data);
            }

          for (int i = 0; rev_data->subsets != NULL && i < rev_data->subsets->len; i++)
            {
              const char *subset = g_ptr_array_index (rev_data->subsets, i);
              if (!g_hash_table_contains (subsets, subset))
                g_hash_table_add (subsets, g_strdup (subset));
            }
        }
    }

  compat_summary = generate_summary (repo, TRUE, refs, commit_data_cache, delta_names,
                                     "", (const char **)summary_arches,
                                     cancellable, error);
  if (compat_summary == NULL)
    return FALSE;

  if (!disable_index)
    {
      GLNX_HASH_TABLE_FOREACH (subsets, const char *, subset)
        {
          GLNX_HASH_TABLE_FOREACH (arches, const char *, arch)
            {
              const char *arch_v[] = { arch, NULL };
              g_autofree char *name = NULL;
              g_autofree char *digest = NULL;

              if (*subset == 0)
                name = g_strdup (arch);
              else
                name = g_strconcat (subset, "-", arch, NULL);

              g_autoptr(GVariant) arch_summary = generate_summary (repo, FALSE, refs, commit_data_cache, NULL, subset, arch_v,
                                                                   cancellable, error);
              if (arch_summary == NULL)
                return FALSE;

              digest = flatpak_repo_save_digested_summary (repo, name, arch_summary, cancellable, error);
              if (digest == NULL)
                return FALSE;

              g_hash_table_insert (digested_summaries, g_strdup (digest), g_variant_ref (arch_summary));
              /* Prime summary cache with generated summaries */
              g_hash_table_insert (digested_summary_cache, g_strdup (digest), g_variant_ref (arch_summary));
              g_hash_table_insert (summaries, g_steal_pointer (&name), g_steal_pointer (&digest));
            }
        }

      summary_index = generate_summary_index (repo, old_index, summaries, digested_summaries, digested_summary_cache,
                                              gpg_key_ids, gpg_homedir,
                                              cancellable, error);
      if (summary_index == NULL)
        return FALSE;
    }

  if (!ostree_repo_static_delta_reindex (repo, 0, NULL, cancellable, error))
    return FALSE;

  if (summary_index && gpg_key_ids)
    {
      g_autoptr(GBytes) index_bytes = g_variant_get_data_as_bytes (summary_index);

      if (!ostree_repo_gpg_sign_data (repo, index_bytes,
                                      NULL,
                                      gpg_key_ids,
                                      gpg_homedir,
                                      &index_sig,
                                      cancellable,
                                      error))
        return FALSE;
    }

  if (summary_index)
    index_digest = g_compute_checksum_for_data (G_CHECKSUM_SHA256,
                                                g_variant_get_data (summary_index),
                                                g_variant_get_size (summary_index));
  if (old_index)
    old_index_digest = g_compute_checksum_for_data (G_CHECKSUM_SHA256,
                                                    g_variant_get_data (old_index),
                                                    g_variant_get_size (old_index));

  /* Release the memory-mapped summary index file before replacing it,
     to avoid failure on filesystems like cifs */
  g_clear_pointer (&old_index, g_variant_unref);

  if (!flatpak_repo_save_summary_index (repo, summary_index, index_digest, index_sig, cancellable, error))
    return FALSE;

  if (!flatpak_repo_save_compat_summary (repo, compat_summary, &old_compat_sig_mtime, cancellable, error))
    return FALSE;

  if (gpg_key_ids)
    {
      if (!ostree_repo_add_gpg_signature_summary (repo,
                                                  gpg_key_ids,
                                                  gpg_homedir,
                                                  cancellable,
                                                  error))
        return FALSE;


      if (old_compat_sig_mtime != 0)
        {
          int repo_dfd = ostree_repo_get_dfd (repo);
          struct stat stbuf;

          /* Ensure we increase (in sec precision) */
          if (fstatat (repo_dfd, "summary.sig", &stbuf, AT_SYMLINK_NOFOLLOW) == 0 &&
              stbuf.st_mtime <= old_compat_sig_mtime)
            {
              struct timespec ts[2] = { {0, UTIME_OMIT}, {old_compat_sig_mtime + 1, 0} };
              (void) utimensat (repo_dfd, "summary.sig", ts, AT_SYMLINK_NOFOLLOW);
            }
        }
    }

  if (!disable_index &&
      !flatpak_repo_gc_digested_summaries (repo, index_digest, old_index_digest, digested_summaries, digested_summary_cache, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_mtree_create_dir (OstreeRepo         *repo,
                          OstreeMutableTree  *parent,
                          const char         *name,
                          OstreeMutableTree **dir_out,
                          GError            **error)
{
  g_autoptr(OstreeMutableTree) dir = NULL;

  if (!ostree_mutable_tree_ensure_dir (parent, name, &dir, error))
    return FALSE;

  if (!flatpak_mtree_ensure_dir_metadata (repo, dir, NULL, error))
    return FALSE;

  *dir_out = g_steal_pointer (&dir);
  return TRUE;
}

gboolean
flatpak_mtree_create_symlink (OstreeRepo         *repo,
                              OstreeMutableTree  *parent,
                              const char         *filename,
                              const char         *target,
                              GError            **error)
{
  g_autoptr(GFileInfo) file_info = g_file_info_new ();
  g_autoptr(GInputStream) content_stream = NULL;
  g_autofree guchar *raw_checksum = NULL;
  g_autofree char *checksum = NULL;
  guint64 length;

  g_file_info_set_name (file_info, filename);
  g_file_info_set_file_type (file_info, G_FILE_TYPE_SYMBOLIC_LINK);
  g_file_info_set_size (file_info, 0);
  g_file_info_set_attribute_uint32 (file_info, "unix::uid", 0);
  g_file_info_set_attribute_uint32 (file_info, "unix::gid", 0);
  g_file_info_set_attribute_uint32 (file_info, "unix::mode", S_IFLNK | 0777);

  g_file_info_set_attribute_boolean (file_info, "standard::is-symlink", TRUE);
  g_file_info_set_attribute_byte_string (file_info, "standard::symlink-target", target);

  if (!ostree_raw_file_to_content_stream (NULL, file_info, NULL,
                                          &content_stream, &length,
                                          NULL, error))
    return FALSE;

  if (!ostree_repo_write_content (repo, NULL, content_stream, length,
                                  &raw_checksum, NULL, error))
    return FALSE;

  checksum = ostree_checksum_from_bytes (raw_checksum);

  if (!ostree_mutable_tree_replace_file (parent, filename, checksum, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_mtree_add_file_from_bytes (OstreeRepo *repo,
                                   GBytes *bytes,
                                   OstreeMutableTree *parent,
                                   const char *filename,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  g_autoptr(GFileInfo) info = g_file_info_new ();
  g_autoptr(GInputStream) memstream = NULL;
  g_autoptr(GInputStream) content_stream = NULL;
  g_autofree guchar *raw_checksum = NULL;
  g_autofree char *checksum = NULL;
  guint64 length;

  g_file_info_set_attribute_uint32 (info, "standard::type", G_FILE_TYPE_REGULAR);
  g_file_info_set_attribute_uint64 (info, "standard::size", g_bytes_get_size (bytes));
  g_file_info_set_attribute_uint32 (info, "unix::uid", 0);
  g_file_info_set_attribute_uint32 (info, "unix::gid", 0);
  g_file_info_set_attribute_uint32 (info, "unix::mode", S_IFREG | 0644);

  memstream = g_memory_input_stream_new_from_bytes (bytes);

  if (!ostree_raw_file_to_content_stream (memstream, info, NULL,
                                          &content_stream, &length,
                                          cancellable, error))
    return FALSE;

  if (!ostree_repo_write_content (repo, NULL, content_stream, length,
                                  &raw_checksum, cancellable, error))
    return FALSE;

  checksum = ostree_checksum_from_bytes (raw_checksum);

  if (!ostree_mutable_tree_replace_file (parent, filename, checksum, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_mtree_ensure_dir_metadata (OstreeRepo        *repo,
                                   OstreeMutableTree *mtree,
                                   GCancellable      *cancellable,
                                   GError           **error)
{
  g_autoptr(GVariant) dirmeta = NULL;
  g_autoptr(GFileInfo) file_info = g_file_info_new ();
  g_autofree guchar *csum = NULL;
  g_autofree char *checksum = NULL;

  g_file_info_set_name (file_info, "/");
  g_file_info_set_file_type (file_info, G_FILE_TYPE_DIRECTORY);
  g_file_info_set_attribute_uint32 (file_info, "unix::uid", 0);
  g_file_info_set_attribute_uint32 (file_info, "unix::gid", 0);
  g_file_info_set_attribute_uint32 (file_info, "unix::mode", 040755);

  dirmeta = ostree_create_directory_metadata (file_info, NULL);
  if (!ostree_repo_write_metadata (repo, OSTREE_OBJECT_TYPE_DIR_META, NULL,
                                   dirmeta, &csum, cancellable, error))
    return FALSE;

  checksum = ostree_checksum_from_bytes (csum);
  ostree_mutable_tree_set_metadata_checksum (mtree, checksum);

  return TRUE;
}

static gboolean
copy_icon (const char        *id,
           GFile             *icons_dir,
           OstreeRepo        *repo,
           OstreeMutableTree *size_mtree,
           const char        *size,
           GError           **error)
{
  g_autofree char *icon_name = g_strconcat (id, ".png", NULL);
  g_autoptr(GFile) size_dir = g_file_get_child (icons_dir, size);
  g_autoptr(GFile) icon_file = g_file_get_child (size_dir, icon_name);
  const char *checksum;

  if (!ostree_repo_file_ensure_resolved (OSTREE_REPO_FILE(icon_file), NULL))
    {
      g_info ("No icon at size %s for %s", size, id);
      return TRUE;
    }

  checksum = ostree_repo_file_get_checksum (OSTREE_REPO_FILE(icon_file));
  if (!ostree_mutable_tree_replace_file (size_mtree, icon_name, checksum, error))
    return FALSE;

  return TRUE;
}

static gboolean
extract_appstream (OstreeRepo        *repo,
                   FlatpakXml        *appstream_root,
                   FlatpakDecomposed *ref,
                   const char        *id,
                   OstreeMutableTree *size1_mtree,
                   OstreeMutableTree *size2_mtree,
                   GCancellable       *cancellable,
                   GError            **error)
{
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) app_info_dir = NULL;
  g_autoptr(GFile) xmls_dir = NULL;
  g_autoptr(GFile) icons_dir = NULL;
  g_autoptr(GFile) appstream_file = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_autofree char *appstream_basename = NULL;
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(FlatpakXml) xml_root = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;

  if (!ostree_repo_read_commit (repo, flatpak_decomposed_get_ref (ref), &root, NULL, NULL, error))
    return FALSE;

  keyfile = g_key_file_new ();
  metadata = g_file_get_child (root, "metadata");
  if (g_file_query_exists (metadata, cancellable))
    {
      g_autofree char *content = NULL;
      gsize len;

      if (!g_file_load_contents (metadata, cancellable, &content, &len, NULL, error))
        return FALSE;

      if (!g_key_file_load_from_data (keyfile, content, len, G_KEY_FILE_NONE, error))
        return FALSE;
    }

  app_info_dir = g_file_resolve_relative_path (root, "files/share/app-info");

  xmls_dir = g_file_resolve_relative_path (app_info_dir, "xmls");
  icons_dir = g_file_resolve_relative_path (app_info_dir, "icons/flatpak");

  appstream_basename = g_strconcat (id, ".xml.gz", NULL);
  appstream_file = g_file_get_child (xmls_dir, appstream_basename);

  in = (GInputStream *) g_file_read (appstream_file, cancellable, error);
  if (!in)
    return FALSE;

  xml_root = flatpak_xml_parse (in, TRUE, cancellable, error);
  if (xml_root == NULL)
    return FALSE;

  if (flatpak_appstream_xml_migrate (xml_root, appstream_root,
                                     flatpak_decomposed_get_ref (ref), id, keyfile))
    {
      g_autoptr(GError) my_error = NULL;
      FlatpakXml *components = appstream_root->first_child;
      FlatpakXml *component = components->first_child;

      while (component != NULL)
        {
          FlatpakXml *component_id, *component_id_text_node;
          g_autofree char *component_id_text = NULL;
          char *component_id_suffix;

          if (g_strcmp0 (component->element_name, "component") != 0)
            {
              component = component->next_sibling;
              continue;
            }

          component_id = flatpak_xml_find (component, "id", NULL);
          component_id_text_node = flatpak_xml_find (component_id, NULL, NULL);

          component_id_text = g_strstrip (g_strdup (component_id_text_node->text));

          /* We're looking for a component that matches the app-id (id), but it
             may have some further elements (separated by dot) and can also have
             ".desktop" at the end which we need to strip out. Further complicating
             things, some actual app ids ends in .desktop, such as org.telegram.desktop. */

          component_id_suffix = component_id_text + strlen (id); /* Don't deref before we check for prefix match! */
          if (!g_str_has_prefix (component_id_text, id) ||
              (component_id_suffix[0] != 0 && component_id_suffix[0] != '.'))
            {
              component = component->next_sibling;
              continue;
            }

          if (g_str_has_suffix (component_id_suffix, ".desktop"))
            component_id_suffix[strlen (component_id_suffix) - strlen (".desktop")] = 0;

          if (!copy_icon (component_id_text, icons_dir, repo, size1_mtree, "64x64", &my_error))
            {
              g_print (_("Error copying 64x64 icon for component %s: %s\n"), component_id_text, my_error->message);
              g_clear_error (&my_error);
            }

          if (!copy_icon (component_id_text, icons_dir, repo, size2_mtree, "128x128", &my_error))
             {
               g_print (_("Error copying 128x128 icon for component %s: %s\n"), component_id_text, my_error->message);
               g_clear_error (&my_error);
             }


          /* We might match other prefixes, so keep on going */
          component = component->next_sibling;
        }
    }

  return TRUE;
}

/* This is similar to ostree_repo_list_refs(), but returns only valid flatpak
 * refs, as FlatpakDecomposed. */
static GHashTable *
flatpak_repo_list_flatpak_refs (OstreeRepo   *repo,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_autoptr(GHashTable) refspecs = NULL;
  g_autoptr(GHashTable) refs = NULL;
  GHashTableIter iter;
  gpointer key, value;

  if (!ostree_repo_list_refs_ext (repo, NULL, &refspecs,
                                  OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_REMOTES | OSTREE_REPO_LIST_REFS_EXT_EXCLUDE_MIRRORS,
                                  cancellable, error))
    return NULL;

  refs = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash, (GEqualFunc)flatpak_decomposed_equal, (GDestroyNotify)flatpak_decomposed_unref, g_free);

  g_hash_table_iter_init (&iter, refspecs);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *refstr = key;
      const char *checksum = value;
      FlatpakDecomposed *ref = NULL;

      ref = flatpak_decomposed_new_from_ref_take ((char *)refstr, NULL);
      if (ref)
        {
          g_hash_table_iter_steal (&iter);
          g_hash_table_insert (refs, ref, (char *)checksum);
        }
    }

  return g_steal_pointer (&refs);
}

static gboolean
_flatpak_repo_generate_appstream (OstreeRepo   *repo,
                                  const char  **gpg_key_ids,
                                  const char   *gpg_homedir,
                                  FlatpakDecomposed **all_refs_keys,
                                  guint         n_keys,
                                  GHashTable   *all_commits,
                                  const char   *arch,
                                  const char   *subset,
                                  guint64       timestamp,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  g_autoptr(FlatpakXml) appstream_root = NULL;
  g_autoptr(GBytes) xml_data = NULL;
  g_autoptr(GBytes) xml_gz_data = NULL;
  g_autoptr(OstreeMutableTree) mtree = ostree_mutable_tree_new ();
  g_autoptr(OstreeMutableTree) icons_mtree = NULL;
  g_autoptr(OstreeMutableTree) icons_flatpak_mtree = NULL;
  g_autoptr(OstreeMutableTree) size1_mtree = NULL;
  g_autoptr(OstreeMutableTree) size2_mtree = NULL;
  const char *compat_arch;
  compat_arch = flatpak_get_compat_arch (arch);
  const char *branch_names[] = { "appstream", "appstream2" };
  const char *collection_id;

  if (subset != NULL && *subset != 0)
    g_info ("Generating appstream for %s, subset %s", arch, subset);
  else
    g_info ("Generating appstream for %s", arch);

  collection_id = ostree_repo_get_collection_id (repo);

  if (!flatpak_mtree_ensure_dir_metadata (repo, mtree, cancellable, error))
    return FALSE;

  if (!flatpak_mtree_create_dir (repo, mtree, "icons", &icons_mtree, error))
    return FALSE;

  if (!flatpak_mtree_create_dir (repo, icons_mtree, "64x64", &size1_mtree, error))
    return FALSE;

  if (!flatpak_mtree_create_dir (repo, icons_mtree, "128x128", &size2_mtree, error))
    return FALSE;

  /* For compatibility with libappstream we create a $origin ("flatpak") subdirectory with symlinks
   * to the size directories thus matching the standard merged appstream layout if we assume the
   * appstream has origin=flatpak, which flatpak-builder creates.
   *
   * See https://github.com/ximion/appstream/pull/224 for details.
   */
  if (!flatpak_mtree_create_dir (repo, icons_mtree, "flatpak", &icons_flatpak_mtree, error))
    return FALSE;
  if (!flatpak_mtree_create_symlink (repo, icons_flatpak_mtree, "64x64", "../64x64", error))
    return FALSE;
  if (!flatpak_mtree_create_symlink (repo, icons_flatpak_mtree, "128x128", "../128x128", error))
    return FALSE;

  appstream_root = flatpak_appstream_xml_new ();

  for (int i = 0; i < n_keys; i++)
    {
      FlatpakDecomposed *ref = all_refs_keys[i];
      GVariant *commit_v = NULL;
      VarMetadataRef commit_metadata;
      g_autoptr(GError) my_error = NULL;
      g_autofree char *id = NULL;

      if (!flatpak_decomposed_is_arch (ref, arch))
        {
          g_autoptr(FlatpakDecomposed) main_ref = NULL;

          /* Include refs that don't match the main arch (e.g. x86_64), if they match
             the compat arch (e.g. i386) and the main arch version is not in the repo */
          if (compat_arch != NULL && flatpak_decomposed_is_arch (ref, compat_arch))
            main_ref = flatpak_decomposed_new_from_decomposed (ref, 0, NULL, compat_arch, NULL, NULL);

          if (main_ref == NULL ||
              g_hash_table_lookup (all_commits, main_ref))
            continue;
        }

      commit_v = g_hash_table_lookup (all_commits, ref);
      g_assert (commit_v != NULL);

      commit_metadata = var_commit_get_metadata (var_commit_from_gvariant (commit_v));
      if (var_metadata_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE, NULL, NULL) ||
          var_metadata_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE_REBASE, NULL, NULL))
        {
          g_info (_("%s is end-of-life, ignoring for appstream"), flatpak_decomposed_get_ref (ref));
          continue;
        }

      if (*subset != 0)
        {
          VarVariantRef xa_subsets_v;
          gboolean in_subset = FALSE;

          if (var_metadata_lookup (commit_metadata, "xa.subsets", NULL, &xa_subsets_v))
            {
              VarArrayofstringRef xa_subsets = var_arrayofstring_from_variant (xa_subsets_v);
              gsize len = var_arrayofstring_get_length (xa_subsets);

              for (gsize j = 0; j < len; j++)
                {
                  const char *xa_subset = var_arrayofstring_get_at (xa_subsets, j);
                  if (strcmp (subset, xa_subset) == 0)
                    {
                      in_subset = TRUE;
                      break;
                    }
                }
            }

          if (!in_subset)
            continue;
        }

      id = flatpak_decomposed_dup_id (ref);
      if (!extract_appstream (repo, appstream_root,
                              ref, id, size1_mtree, size2_mtree,
                              cancellable, &my_error))
        {
          if (flatpak_decomposed_is_app (ref))
            g_print (_("No appstream data for %s: %s\n"), flatpak_decomposed_get_ref (ref), my_error->message);
          continue;
        }
    }

  if (!flatpak_appstream_xml_root_to_data (appstream_root, &xml_data, &xml_gz_data, error))
    return FALSE;

  for (int i = 0; i < G_N_ELEMENTS (branch_names); i++)
    {
      gboolean skip_commit = FALSE;
      const char *branch_prefix = branch_names[i];
      g_autoptr(GFile) root = NULL;
      g_autofree char *branch = NULL;
      g_autofree char *parent = NULL;
      g_autofree char *commit_checksum = NULL;

      if (*subset != 0 && i == 0)
        continue; /* No old-style branch for subsets */

      if (*subset != 0)
        branch = g_strdup_printf ("%s/%s-%s", branch_prefix, subset, arch);
      else
        branch = g_strdup_printf ("%s/%s", branch_prefix, arch);

      if (!flatpak_repo_resolve_rev (repo, collection_id, NULL, branch, TRUE,
                                     &parent, cancellable, error))
        return FALSE;

      if (i == 0)
        {
          if (!flatpak_mtree_add_file_from_bytes (repo, xml_gz_data, mtree, "appstream.xml.gz", cancellable, error))
            return FALSE;
        }
      else
        {
          if (!ostree_mutable_tree_remove (mtree, "appstream.xml.gz", TRUE, error))
            return FALSE;

          if (!flatpak_mtree_add_file_from_bytes (repo, xml_data, mtree, "appstream.xml", cancellable, error))
            return FALSE;
        }

      if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
        return FALSE;

      /* No need to commit if nothing changed */
      if (parent)
        {
          g_autoptr(GFile) parent_root = NULL;

          if (!ostree_repo_read_commit (repo, parent, &parent_root, NULL, cancellable, error))
            return FALSE;

          if (g_file_equal (root, parent_root))
            {
              skip_commit = TRUE;
              g_info ("Not updating %s, no change", branch);
            }
        }

      if (!skip_commit)
        {
          g_autoptr(GVariantDict) metadata_dict = NULL;
          g_autoptr(GVariant) metadata = NULL;

          /* Add bindings to the metadata. Do this even if P2P support is not
           * enabled, as it might be enable for other flatpak builds. */
          metadata_dict = g_variant_dict_new (NULL);
          g_variant_dict_insert (metadata_dict, "ostree.collection-binding",
                                 "s", (collection_id != NULL) ? collection_id : "");
          g_variant_dict_insert_value (metadata_dict, "ostree.ref-binding",
                                       g_variant_new_strv ((const gchar * const *) &branch, 1));
          metadata = g_variant_ref_sink (g_variant_dict_end (metadata_dict));

          if (timestamp > 0)
            {
              if (!ostree_repo_write_commit_with_time (repo, parent, "Update", NULL, metadata,
                                                       OSTREE_REPO_FILE (root),
                                                       timestamp,
                                                       &commit_checksum,
                                                       cancellable, error))
                return FALSE;
            }
          else
            {
              if (!ostree_repo_write_commit (repo, parent, "Update", NULL, metadata,
                                             OSTREE_REPO_FILE (root),
                                             &commit_checksum, cancellable, error))
                return FALSE;
            }

          if (gpg_key_ids)
            {
              for (int j = 0; gpg_key_ids[j] != NULL; j++)
                {
                  const char *keyid = gpg_key_ids[j];

                  if (!ostree_repo_sign_commit (repo,
                                                commit_checksum,
                                                keyid,
                                                gpg_homedir,
                                                cancellable,
                                                error))
                    return FALSE;
                }
            }

          g_info ("Creating appstream branch %s", branch);
          if (collection_id != NULL)
            {
              const OstreeCollectionRef collection_ref = { (char *) collection_id, branch };
              ostree_repo_transaction_set_collection_ref (repo, &collection_ref, commit_checksum);
            }
          else
            {
              ostree_repo_transaction_set_ref (repo, NULL, branch, commit_checksum);
            }
        }
    }

  return TRUE;
}


gboolean
flatpak_repo_generate_appstream (OstreeRepo   *repo,
                                 const char  **gpg_key_ids,
                                 const char   *gpg_homedir,
                                 guint64       timestamp,
                                 GCancellable *cancellable,
                                 GError      **error)
{
  g_autoptr(GHashTable) all_refs = NULL;
  g_autoptr(GHashTable) all_commits = NULL;
  g_autofree FlatpakDecomposed **all_refs_keys = NULL;
  guint n_keys;
  g_autoptr(GPtrArray) arches = NULL;  /* (element-type utf8 utf8) */
  g_autoptr(GPtrArray) subsets = NULL;  /* (element-type utf8 utf8) */
  g_autoptr(FlatpakRepoTransaction) transaction = NULL;
  OstreeRepoTransactionStats stats;

  arches = g_ptr_array_new_with_free_func (g_free);
  subsets = g_ptr_array_new_with_free_func (g_free);

  g_ptr_array_add (subsets, g_strdup (""));

  all_refs = flatpak_repo_list_flatpak_refs (repo, cancellable, error);
  if (all_refs == NULL)
    return FALSE;

  all_commits = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash, (GEqualFunc)flatpak_decomposed_equal, (GDestroyNotify)flatpak_decomposed_unref, (GDestroyNotify)g_variant_unref);

  GLNX_HASH_TABLE_FOREACH_KV (all_refs, FlatpakDecomposed *, ref, const char *, commit)
    {
      VarMetadataRef commit_metadata;
      VarVariantRef xa_subsets_v;
      const char *reverse_compat_arch;
      char *new_arch = NULL;
      g_autoptr(GVariant) commit_v = NULL;

      if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit, &commit_v, NULL))
        {
          g_warning ("Couldn't load commit %s (ref %s)", commit, flatpak_decomposed_get_ref (ref));
          continue;
        }

      g_hash_table_insert (all_commits, flatpak_decomposed_ref (ref), g_variant_ref (commit_v));

      /* Compute list of subsets */
      commit_metadata = var_commit_get_metadata (var_commit_from_gvariant (commit_v));
      if (var_metadata_lookup (commit_metadata, "xa.subsets", NULL, &xa_subsets_v))
        {
          VarArrayofstringRef xa_subsets = var_arrayofstring_from_variant (xa_subsets_v);
          gsize len = var_arrayofstring_get_length (xa_subsets);
          for (gsize j = 0; j < len; j++)
            {
              const char *subset = var_arrayofstring_get_at (xa_subsets, j);

              if (!flatpak_g_ptr_array_contains_string (subsets, subset))
                g_ptr_array_add (subsets, g_strdup (subset));
            }
        }

      /* Compute list of arches */
      if (!flatpak_decomposed_is_arches (ref, arches->len, (const char **) arches->pdata))
        {
          new_arch = flatpak_decomposed_dup_arch (ref);
          g_ptr_array_add (arches, new_arch);

          /* If repo contains e.g. i386, also generated x86-64 appdata */
          reverse_compat_arch = flatpak_get_compat_arch_reverse (new_arch);
          if (reverse_compat_arch != NULL &&
              !flatpak_g_ptr_array_contains_string (arches, reverse_compat_arch))
            g_ptr_array_add (arches, g_strdup (reverse_compat_arch));
        }
    }

  g_ptr_array_sort (subsets, flatpak_strcmp0_ptr);
  g_ptr_array_sort (arches, flatpak_strcmp0_ptr);

  all_refs_keys = (FlatpakDecomposed **) g_hash_table_get_keys_as_array (all_refs, &n_keys);

  /* Sort refs so that appdata order is stable for e.g. deltas */
  g_qsort_with_data (all_refs_keys, n_keys, sizeof (FlatpakDecomposed *), (GCompareDataFunc) flatpak_decomposed_strcmp_p, NULL);

  transaction = flatpak_repo_transaction_start (repo, cancellable, error);
  if (transaction == NULL)
    return FALSE;

  for (int l = 0; l < subsets->len; l++)
    {
      const char *subset = g_ptr_array_index (subsets, l);

      for (int k = 0; k < arches->len; k++)
        {
          const char *arch = g_ptr_array_index (arches, k);

          if (!_flatpak_repo_generate_appstream (repo,
                                                 gpg_key_ids,
                                                 gpg_homedir,
                                                 all_refs_keys,
                                                 n_keys,
                                                 all_commits,
                                                 arch,
                                                 subset,
                                                 timestamp,
                                                 cancellable,
                                                 error))
            return FALSE;
        }
    }

  if (!ostree_repo_commit_transaction (repo, &stats, cancellable, error))
    return FALSE;

  return TRUE;
}

void
flatpak_extension_free (FlatpakExtension *extension)
{
  g_free (extension->id);
  g_free (extension->installed_id);
  g_free (extension->commit);
  flatpak_decomposed_unref (extension->ref);
  g_free (extension->directory);
  g_free (extension->files_path);
  g_free (extension->add_ld_path);
  g_free (extension->subdir_suffix);
  g_strfreev (extension->merge_dirs);
  g_free (extension);
}

static int
flatpak_extension_compare (gconstpointer _a,
                           gconstpointer _b)
{
  const FlatpakExtension *a = _a;
  const FlatpakExtension *b = _b;

  return b->priority - a->priority;
}

static FlatpakExtension *
flatpak_extension_new (const char        *id,
                       const char        *extension,
                       FlatpakDecomposed *ref,
                       const char        *directory,
                       const char        *add_ld_path,
                       const char        *subdir_suffix,
                       char             **merge_dirs,
                       GFile             *files,
                       GFile             *deploy_dir,
                       gboolean           is_unmaintained,
                       OstreeRepo        *repo)
{
  FlatpakExtension *ext = g_new0 (FlatpakExtension, 1);
  g_autoptr(GBytes) deploy_data = NULL;

  ext->id = g_strdup (id);
  ext->installed_id = g_strdup (extension);
  ext->ref = flatpak_decomposed_ref (ref);
  ext->directory = g_strdup (directory);
  ext->files_path = g_file_get_path (files);
  ext->add_ld_path = g_strdup (add_ld_path);
  ext->subdir_suffix = g_strdup (subdir_suffix);
  ext->merge_dirs = g_strdupv (merge_dirs);
  ext->is_unmaintained = is_unmaintained;

  /* Unmaintained extensions won't have a deploy or commit; see
   * https://github.com/flatpak/flatpak/issues/167 */
  if (deploy_dir && !is_unmaintained)
    {
      deploy_data = flatpak_load_deploy_data (deploy_dir, ref, repo, FLATPAK_DEPLOY_VERSION_ANY, NULL, NULL);
      if (deploy_data)
        ext->commit = g_strdup (flatpak_deploy_data_get_commit (deploy_data));
    }

  if (is_unmaintained)
    ext->priority = 1000;
  else
    {
      g_autoptr(GKeyFile) keyfile = g_key_file_new ();
      g_autofree char *metadata_path = g_build_filename (ext->files_path, "../metadata", NULL);

      if (g_key_file_load_from_file (keyfile, metadata_path, G_KEY_FILE_NONE, NULL))
        ext->priority = g_key_file_get_integer (keyfile,
                                                FLATPAK_METADATA_GROUP_EXTENSION_OF,
                                                FLATPAK_METADATA_KEY_PRIORITY,
                                                NULL);
    }

  return ext;
}

gboolean
flatpak_extension_matches_reason (const char *extension_id,
                                  const char *reasons,
                                  gboolean    default_value)
{
  const char *extension_basename;
  g_auto(GStrv) reason_list = NULL;
  size_t i;

  if (reasons == NULL || *reasons == 0)
    return default_value;

  extension_basename = strrchr (extension_id, '.');
  if (extension_basename == NULL)
    return FALSE;
  extension_basename += 1;

  reason_list = g_strsplit (reasons, ";", -1);

  for (i = 0; reason_list[i]; ++i)
    {
      const char *reason = reason_list[i];

      if (strcmp (reason, "active-gl-driver") == 0)
        {
          /* handled below */
          const char **gl_drivers = flatpak_get_gl_drivers ();
          size_t j;

          for (j = 0; gl_drivers[j]; j++)
            {
              if (strcmp (gl_drivers[j], extension_basename) == 0)
                return TRUE;
            }
        }
      else if (strcmp (reason, "active-gtk-theme") == 0)
        {
          const char *gtk_theme = flatpak_get_gtk_theme ();
          if (strcmp (gtk_theme, extension_basename) == 0)
            return TRUE;
        }
      else if (strcmp (reason, "have-intel-gpu") == 0)
        {
          /* Used for Intel VAAPI driver extension */
          if (flatpak_get_have_intel_gpu ())
            return TRUE;
        }
      else if (g_str_has_prefix (reason, "have-kernel-module-"))
        {
          const char *module_name = reason + strlen ("have-kernel-module-");

          if (flatpak_get_have_kernel_module (module_name))
            return TRUE;
        }
      else if (g_str_has_prefix (reason, "on-xdg-desktop-"))
        {
          const char *desktop_name = reason + strlen ("on-xdg-desktop-");
          const char *current_desktop_var = g_getenv ("XDG_CURRENT_DESKTOP");
          g_auto(GStrv) current_desktop_names = NULL;
          size_t j;

          if (!current_desktop_var)
            continue;

          current_desktop_names = g_strsplit (current_desktop_var, ":", -1);

          for (j = 0; current_desktop_names[j]; ++j)
            {
              if (g_ascii_strcasecmp (desktop_name, current_desktop_names[j]) == 0)
                return TRUE;
            }
        }
    }

  return FALSE;
}

static GList *
add_extension (GKeyFile   *metakey,
               const char *group,
               const char *extension,
               const char *arch,
               const char *branch,
               GList      *res)
{
  FlatpakExtension *ext;
  g_autofree char *directory = g_key_file_get_string (metakey, group,
                                                      FLATPAK_METADATA_KEY_DIRECTORY,
                                                      NULL);
  g_autofree char *add_ld_path = g_key_file_get_string (metakey, group,
                                                        FLATPAK_METADATA_KEY_ADD_LD_PATH,
                                                        NULL);
  g_auto(GStrv) merge_dirs = g_key_file_get_string_list (metakey, group,
                                                         FLATPAK_METADATA_KEY_MERGE_DIRS,
                                                         NULL, NULL);
  g_autofree char *enable_if = g_key_file_get_string (metakey, group,
                                                      FLATPAK_METADATA_KEY_ENABLE_IF,
                                                      NULL);
  g_autofree char *subdir_suffix = g_key_file_get_string (metakey, group,
                                                          FLATPAK_METADATA_KEY_SUBDIRECTORY_SUFFIX,
                                                          NULL);
  g_autoptr(FlatpakDecomposed) ref = NULL;
  gboolean is_unmaintained = FALSE;
  g_autoptr(GFile) files = NULL;
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(FlatpakDir) dir = NULL;

  if (directory == NULL)
    return res;

  ref = flatpak_decomposed_new_from_parts (FLATPAK_KINDS_RUNTIME, extension, arch, branch, NULL);
  if (ref == NULL)
    return res;

  files = flatpak_find_unmaintained_extension_dir_if_exists (extension, arch, branch, NULL);

  if (files == NULL)
    {
      deploy_dir = flatpak_find_deploy_dir_for_ref (ref, &dir, NULL, NULL);
      if (deploy_dir)
        files = g_file_get_child (deploy_dir, "files");
    }
  else
    is_unmaintained = TRUE;

  /* Prefer a full extension (org.freedesktop.Locale) over subdirectory ones (org.freedesktop.Locale.sv) */
  if (files != NULL)
    {
      if (flatpak_extension_matches_reason (extension, enable_if, TRUE))
        {
          ext = flatpak_extension_new (extension, extension, ref, directory,
                                       add_ld_path, subdir_suffix, merge_dirs,
                                       files, deploy_dir, is_unmaintained,
                                       is_unmaintained ? NULL : flatpak_dir_get_repo (dir));
          res = g_list_prepend (res, ext);
        }
    }
  else if (g_key_file_get_boolean (metakey, group,
                                   FLATPAK_METADATA_KEY_SUBDIRECTORIES, NULL))
    {
      g_autofree char *prefix = g_strconcat (extension, ".", NULL);
      g_auto(GStrv) ids = NULL;
      g_auto(GStrv) unmaintained_refs = NULL;
      int j;

      ids = flatpak_list_deployed_refs ("runtime", prefix, arch, branch,
                                        NULL, NULL);
      for (j = 0; ids != NULL && ids[j] != NULL; j++)
        {
          const char *id = ids[j];
          g_autofree char *extended_dir = g_build_filename (directory, id + strlen (prefix), NULL);
          g_autoptr(FlatpakDecomposed) dir_ref = NULL;
          g_autoptr(GFile) subdir_deploy_dir = NULL;
          g_autoptr(GFile) subdir_files = NULL;
          g_autoptr(FlatpakDir) subdir_dir = NULL;

          dir_ref = flatpak_decomposed_new_from_parts (FLATPAK_KINDS_RUNTIME, id, arch, branch, NULL);
          if (dir_ref == NULL)
            continue;

          subdir_deploy_dir = flatpak_find_deploy_dir_for_ref (dir_ref, &subdir_dir, NULL, NULL);
          if (subdir_deploy_dir)
            subdir_files = g_file_get_child (subdir_deploy_dir, "files");

          if (subdir_files && flatpak_extension_matches_reason (id, enable_if, TRUE))
            {
              ext = flatpak_extension_new (extension, id, dir_ref, extended_dir,
                                           add_ld_path, subdir_suffix, merge_dirs,
                                           subdir_files, subdir_deploy_dir, FALSE,
                                           flatpak_dir_get_repo (subdir_dir));
              ext->needs_tmpfs = TRUE;
              res = g_list_prepend (res, ext);
            }
        }

      unmaintained_refs = flatpak_list_unmaintained_refs (prefix, arch, branch,
                                                          NULL, NULL);
      for (j = 0; unmaintained_refs != NULL && unmaintained_refs[j] != NULL; j++)
        {
          g_autofree char *extended_dir = g_build_filename (directory, unmaintained_refs[j] + strlen (prefix), NULL);
          g_autoptr(FlatpakDecomposed) dir_ref = NULL;
          g_autoptr(GFile) subdir_files = flatpak_find_unmaintained_extension_dir_if_exists (unmaintained_refs[j], arch, branch, NULL);

          dir_ref = flatpak_decomposed_new_from_parts (FLATPAK_KINDS_RUNTIME, unmaintained_refs[j], arch, branch, NULL);
          if (dir_ref == NULL)
            continue;

          if (subdir_files && flatpak_extension_matches_reason (unmaintained_refs[j], enable_if, TRUE))
            {
              ext = flatpak_extension_new (extension, unmaintained_refs[j], dir_ref,
                                           extended_dir, add_ld_path, subdir_suffix,
                                           merge_dirs, subdir_files, NULL, TRUE, NULL);
              ext->needs_tmpfs = TRUE;
              res = g_list_prepend (res, ext);
            }
        }
    }

  return res;
}

void
flatpak_parse_extension_with_tag (const char *extension,
                                  char      **name,
                                  char      **tag)
{
  const char *tag_chr = strchr (extension, '@');

  if (tag_chr)
    {
      if (name != NULL)
        *name = g_strndup (extension, tag_chr - extension);

      /* Everything after the @ */
      if (tag != NULL)
        *tag = g_strdup (tag_chr + 1);

      return;
    }

  if (name != NULL)
    *name = g_strdup (extension);

  if (tag != NULL)
    *tag = NULL;
}

GList *
flatpak_list_extensions (GKeyFile   *metakey,
                         const char *arch,
                         const char *default_branch)
{
  g_auto(GStrv) groups = NULL;
  int i, j;
  GList *res;

  res = NULL;

  if (arch == NULL)
    arch = flatpak_get_arch ();

  groups = g_key_file_get_groups (metakey, NULL);
  for (i = 0; groups[i] != NULL; i++)
    {
      char *extension;

      if (g_str_has_prefix (groups[i], FLATPAK_METADATA_GROUP_PREFIX_EXTENSION) &&
          *(extension = (groups[i] + strlen (FLATPAK_METADATA_GROUP_PREFIX_EXTENSION))) != 0)
        {
          g_autofree char *version = g_key_file_get_string (metakey, groups[i],
                                                            FLATPAK_METADATA_KEY_VERSION,
                                                            NULL);
          g_auto(GStrv) versions = g_key_file_get_string_list (metakey, groups[i],
                                                               FLATPAK_METADATA_KEY_VERSIONS,
                                                               NULL, NULL);
          g_autofree char *name = NULL;
          const char *default_branches[] = { default_branch, NULL};
          const char **branches;

          flatpak_parse_extension_with_tag (extension, &name, NULL);

          if (versions)
            branches = (const char **) versions;
          else
            {
              if (version)
                default_branches[0] = version;
              branches = default_branches;
            }

          for (j = 0; branches[j] != NULL; j++)
            res = add_extension (metakey, groups[i], name, arch, branches[j], res);
        }
    }

  return g_list_sort (g_list_reverse (res), flatpak_extension_compare);
}

#define OSTREE_STATIC_DELTA_META_ENTRY_FORMAT "(uayttay)"
#define OSTREE_STATIC_DELTA_FALLBACK_FORMAT "(yaytt)"
#define OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT "(a{sv}tayay" OSTREE_COMMIT_GVARIANT_STRING "aya" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT "a" OSTREE_STATIC_DELTA_FALLBACK_FORMAT ")"

static inline guint64
maybe_swap_endian_u64 (gboolean swap,
                       guint64  v)
{
  if (!swap)
    return v;
  return GUINT64_SWAP_LE_BE (v);
}

static guint64
flatpak_bundle_get_installed_size (GVariant *bundle, gboolean byte_swap)
{
  guint64 total_usize = 0;
  g_autoptr(GVariant) meta_entries = NULL;
  guint i, n_parts;

  g_variant_get_child (bundle, 6, "@a" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT, &meta_entries);
  n_parts = g_variant_n_children (meta_entries);

  for (i = 0; i < n_parts; i++)
    {
      guint32 version;
      guint64 size, usize;
      g_autoptr(GVariant) objects = NULL;

      g_variant_get_child (meta_entries, i, "(u@aytt@ay)",
                           &version, NULL, &size, &usize, &objects);

      total_usize += maybe_swap_endian_u64 (byte_swap, usize);
    }

  return total_usize;
}

GVariant *
flatpak_bundle_load (GFile              *file,
                     char              **commit,
                     FlatpakDecomposed **ref,
                     char              **origin,
                     char              **runtime_repo,
                     char              **app_metadata,
                     guint64            *installed_size,
                     GBytes            **gpg_keys,
                     char              **collection_id,
                     GError             **error)
{
  g_autoptr(GVariant) delta = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GBytes) copy = NULL;
  g_autoptr(GVariant) to_csum_v = NULL;

  guint8 endianness_char;
  gboolean byte_swap = FALSE;

  GMappedFile *mfile = g_mapped_file_new (flatpak_file_get_path_cached (file), FALSE, error);

  if (mfile == NULL)
    return NULL;

  bytes = g_mapped_file_get_bytes (mfile);
  g_mapped_file_unref (mfile);

  delta = g_variant_new_from_bytes (G_VARIANT_TYPE (OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT), bytes, FALSE);
  g_variant_ref_sink (delta);

  to_csum_v = g_variant_get_child_value (delta, 3);
  if (!ostree_validate_structureof_csum_v (to_csum_v, error))
    return NULL;

  metadata = g_variant_get_child_value (delta, 0);

  if (g_variant_lookup (metadata, "ostree.endianness", "y", &endianness_char))
    {
      int file_byte_order = G_BYTE_ORDER;
      switch (endianness_char)
        {
        case 'l':
          file_byte_order = G_LITTLE_ENDIAN;
          break;

        case 'B':
          file_byte_order = G_BIG_ENDIAN;
          break;

        default:
          break;
        }
      byte_swap = (G_BYTE_ORDER != file_byte_order);
    }

  if (commit)
    *commit = ostree_checksum_from_bytes_v (to_csum_v);

  if (installed_size)
    *installed_size = flatpak_bundle_get_installed_size (delta, byte_swap);

  if (ref != NULL)
    {
      FlatpakDecomposed *the_ref = NULL;
      g_autofree char *ref_str = NULL;

      if (!g_variant_lookup (metadata, "ref", "s", &ref_str))
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid bundle, no ref in metadata"));
          return NULL;
        }

      the_ref = flatpak_decomposed_new_from_ref (ref_str, error);
      if (the_ref == NULL)
        return NULL;

      g_clear_pointer (ref, flatpak_decomposed_unref);
      *ref = the_ref;
    }

  if (origin != NULL)
    {
      if (!g_variant_lookup (metadata, "origin", "s", origin))
        *origin = NULL;
    }

  if (runtime_repo != NULL)
    {
      if (!g_variant_lookup (metadata, "runtime-repo", "s", runtime_repo))
        *runtime_repo = NULL;
    }

  if (collection_id != NULL)
    {
      if (!g_variant_lookup (metadata, "collection-id", "s", collection_id))
        {
          *collection_id = NULL;
        }
      else if (**collection_id == '\0')
        {
          g_free (*collection_id);
          *collection_id = NULL;
        }
    }

  if (app_metadata != NULL)
    {
      if (!g_variant_lookup (metadata, "metadata", "s", app_metadata))
        *app_metadata = NULL;
    }

  if (gpg_keys != NULL)
    {
      g_autoptr(GVariant) gpg_value = g_variant_lookup_value (metadata, "gpg-keys",
                                                              G_VARIANT_TYPE ("ay"));
      if (gpg_value)
        {
          gsize n_elements;
          const char *data = g_variant_get_fixed_array (gpg_value, &n_elements, 1);
          *gpg_keys = g_bytes_new (data, n_elements);
        }
      else
        {
          *gpg_keys = NULL;
        }
    }

  /* Make a copy of the data so we can return it after freeing the file */
  copy = g_bytes_new (g_variant_get_data (metadata),
                      g_variant_get_size (metadata));
  return g_variant_ref_sink (g_variant_new_from_bytes (g_variant_get_type (metadata),
                                                       copy,
                                                       FALSE));
}

gboolean
flatpak_pull_from_bundle (OstreeRepo   *repo,
                          GFile        *file,
                          const char   *remote,
                          const char   *ref,
                          gboolean      require_gpg_signature,
                          GCancellable *cancellable,
                          GError      **error)
{
  gsize metadata_size = 0;
  g_autofree char *metadata_contents = NULL;
  g_autofree char *to_checksum = NULL;
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) metadata_file = NULL;
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(OstreeGpgVerifyResult) gpg_result = NULL;
  g_autoptr(GError) my_error = NULL;
  g_autoptr(GVariant) metadata = NULL;
  gboolean metadata_valid;
  g_autofree char *remote_collection_id = NULL;
  g_autofree char *collection_id = NULL;

  metadata = flatpak_bundle_load (file, &to_checksum, NULL, NULL, NULL, &metadata_contents, NULL, NULL, &collection_id, error);
  if (metadata == NULL)
    return FALSE;

  if (metadata_contents != NULL)
    metadata_size = strlen (metadata_contents);

  if (!ostree_repo_get_remote_option (repo, remote, "collection-id", NULL,
                                      &remote_collection_id, NULL))
    remote_collection_id = NULL;

  if (remote_collection_id != NULL && collection_id != NULL &&
      strcmp (remote_collection_id, collection_id) != 0)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Collection ‘%s’ of bundle doesn’t match collection ‘%s’ of remote"),
                               collection_id, remote_collection_id);

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return FALSE;

  /* Don’t need to set the collection ID here, since the remote binds this ref to the collection. */
  ostree_repo_transaction_set_ref (repo, remote, ref, to_checksum);

  if (!ostree_repo_static_delta_execute_offline (repo,
                                                 file,
                                                 FALSE,
                                                 cancellable,
                                                 error))
    return FALSE;

  gpg_result = ostree_repo_verify_commit_ext (repo, to_checksum,
                                              NULL, NULL, cancellable, &my_error);
  if (gpg_result == NULL)
    {
      /* no gpg signature, we ignore this *if* there is no gpg key
       * specified in the bundle or by the user */
      if (g_error_matches (my_error, OSTREE_GPG_ERROR, OSTREE_GPG_ERROR_NO_SIGNATURE) &&
          !require_gpg_signature)
        {
          g_clear_error (&my_error);
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return FALSE;
        }
    }
  else
    {
      /* If there is no valid gpg signature we fail, unless there is no gpg
         key specified (on the command line or in the file) because then we
         trust the source bundle. */
      if (ostree_gpg_verify_result_count_valid (gpg_result) == 0  &&
          require_gpg_signature)
        return flatpak_fail_error (error, FLATPAK_ERROR_UNTRUSTED, _("GPG signatures found, but none are in trusted keyring"));
    }

  if (!ostree_repo_read_commit (repo, to_checksum, &root, NULL, NULL, error))
    return FALSE;

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    return FALSE;

  /* We ensure that the actual installed metadata matches the one in the
     header, because you may have made decisions on whether to install it or not
     based on that data. */
  metadata_file = g_file_resolve_relative_path (root, "metadata");
  in = (GInputStream *) g_file_read (metadata_file, cancellable, NULL);
  if (in != NULL)
    {
      g_autoptr(GMemoryOutputStream) data_stream = (GMemoryOutputStream *) g_memory_output_stream_new_resizable ();

      if (g_output_stream_splice (G_OUTPUT_STREAM (data_stream), in,
                                  G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                  cancellable, error) < 0)
        return FALSE;

      metadata_valid =
        metadata_contents != NULL &&
        metadata_size == g_memory_output_stream_get_data_size (data_stream) &&
        memcmp (metadata_contents, g_memory_output_stream_get_data (data_stream), metadata_size) == 0;
    }
  else
    {
      metadata_valid = (metadata_contents == NULL);
    }

  if (!metadata_valid)
    {
      /* Immediately remove this broken commit */
      ostree_repo_set_ref_immediate (repo, remote, ref, NULL, cancellable, error);
      return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Metadata in header and app are inconsistent"));
    }

  return TRUE;
}

typedef struct
{
  FlatpakOciPullProgress progress_cb;
  gpointer               progress_user_data;
  guint64                total_size;
  guint64                previous_layers_size;
  guint32                n_layers;
  guint32                pulled_layers;
} FlatpakOciPullProgressData;

static void
oci_layer_progress (guint64  downloaded_bytes,
                    gpointer user_data)
{
  FlatpakOciPullProgressData *progress_data = user_data;

  if (progress_data->progress_cb)
    progress_data->progress_cb (progress_data->total_size, progress_data->previous_layers_size + downloaded_bytes,
                                progress_data->n_layers, progress_data->pulled_layers,
                                progress_data->progress_user_data);
}

gboolean
flatpak_mirror_image_from_oci (FlatpakOciRegistry    *dst_registry,
                               FlatpakOciRegistry    *registry,
                               const char            *oci_repository,
                               const char            *digest,
                               const char            *remote,
                               const char            *ref,
                               const char            *delta_url,
                               OstreeRepo            *repo,
                               FlatpakOciPullProgress progress_cb,
                               gpointer               progress_user_data,
                               GCancellable          *cancellable,
                               GError               **error)
{
  FlatpakOciPullProgressData progress_data = { progress_cb, progress_user_data };
  g_autoptr(FlatpakOciVersioned) versioned = NULL;
  FlatpakOciManifest *manifest = NULL;
  g_autoptr(FlatpakOciDescriptor) manifest_desc = NULL;
  g_autoptr(FlatpakOciManifest) delta_manifest = NULL;
  g_autofree char *old_checksum = NULL;
  g_autoptr(GVariant) old_commit = NULL;
  g_autoptr(GFile) old_root = NULL;
  OstreeRepoCommitState old_state = 0;
  g_autofree char *old_diffid = NULL;
  gsize versioned_size;
  g_autoptr(FlatpakOciIndex) index = NULL;
  g_autoptr(FlatpakOciImage) image_config = NULL;
  int n_layers;
  int i;

  if (!flatpak_oci_registry_mirror_blob (dst_registry, registry, oci_repository, TRUE, digest, NULL, NULL, NULL, cancellable, error))
    return FALSE;

  versioned = flatpak_oci_registry_load_versioned (dst_registry, NULL, digest, NULL, &versioned_size, cancellable, error);
  if (versioned == NULL)
    return FALSE;

  if (!FLATPAK_IS_OCI_MANIFEST (versioned))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Image is not a manifest"));

  manifest = FLATPAK_OCI_MANIFEST (versioned);

  if (manifest->config.digest == NULL)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Image is not a manifest"));

  if (!flatpak_oci_registry_mirror_blob (dst_registry, registry, oci_repository, FALSE, manifest->config.digest, (const char **)manifest->config.urls, NULL, NULL, cancellable, error))
    return FALSE;

  image_config = flatpak_oci_registry_load_image_config (dst_registry, NULL,
                                                         manifest->config.digest, NULL,
                                                         NULL, cancellable, error);
  if (image_config == NULL)
    return FALSE;

  /* For deltas we ensure that the diffid and regular layers exists and match up */
  n_layers = flatpak_oci_manifest_get_n_layers (manifest);
  if (n_layers == 0 || n_layers != flatpak_oci_image_get_n_layers (image_config))
    return flatpak_fail (error, _("Invalid OCI image config"));

  /* Look for delta manifest, and if it exists, the current (old) commit and its recorded diffid */
  if (flatpak_repo_resolve_rev (repo, NULL, remote, ref, FALSE, &old_checksum, NULL, NULL) &&
      ostree_repo_load_commit (repo, old_checksum, &old_commit, &old_state, NULL) &&
      (old_state == OSTREE_REPO_COMMIT_STATE_NORMAL) &&
      ostree_repo_read_commit (repo, old_checksum, &old_root, NULL, NULL, NULL))
    {
      delta_manifest = flatpak_oci_registry_find_delta_manifest (registry, oci_repository, digest, delta_url, cancellable);
      if (delta_manifest)
        {
          VarMetadataRef commit_metadata = var_commit_get_metadata (var_commit_from_gvariant (old_commit));
          const char *raw_old_diffid = var_metadata_lookup_string (commit_metadata, "xa.diff-id", NULL);
          if (raw_old_diffid != NULL)
            old_diffid = g_strconcat ("sha256:", raw_old_diffid, NULL);
        }
    }

  for (i = 0; manifest->layers[i] != NULL; i++)
    {
      FlatpakOciDescriptor *layer = manifest->layers[i];
      FlatpakOciDescriptor *delta_layer = NULL;

      if (delta_manifest)
        delta_layer = flatpak_oci_manifest_find_delta_for (delta_manifest, old_diffid, image_config->rootfs.diff_ids[i]);

      if (delta_layer)
        progress_data.total_size += delta_layer->size;
      else
        progress_data.total_size += layer->size;
      progress_data.n_layers++;
    }

  if (progress_cb)
    progress_cb (progress_data.total_size, 0,
                 progress_data.n_layers, progress_data.pulled_layers,
                 progress_user_data);

  for (i = 0; manifest->layers[i] != NULL; i++)
    {
      FlatpakOciDescriptor *layer = manifest->layers[i];
      FlatpakOciDescriptor *delta_layer = NULL;

      if (delta_manifest)
        delta_layer = flatpak_oci_manifest_find_delta_for (delta_manifest, old_diffid, image_config->rootfs.diff_ids[i]);

      if (delta_layer)
        {
          g_info ("Using OCI delta %s for layer %s", delta_layer->digest, layer->digest);
          g_autofree char *delta_digest = NULL;
          glnx_autofd int delta_fd = flatpak_oci_registry_download_blob (registry, oci_repository, FALSE,
                                                                         delta_layer->digest, (const char **)delta_layer->urls,
                                                                         oci_layer_progress, &progress_data,
                                                                         cancellable, error);
          if (delta_fd == -1)
            return FALSE;

          delta_digest = flatpak_oci_registry_apply_delta_to_blob (dst_registry, delta_fd, old_root, cancellable, error);
          if (delta_digest == NULL)
            return FALSE;

          if (g_strcmp0 (delta_digest, image_config->rootfs.diff_ids[i]) != 0)
            return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Wrong layer checksum, expected %s, was %s"), image_config->rootfs.diff_ids[i], delta_digest);
        }
      else
        {
          if (!flatpak_oci_registry_mirror_blob (dst_registry, registry, oci_repository, FALSE, layer->digest, (const char **)layer->urls,
                                                 oci_layer_progress, &progress_data,
                                                 cancellable, error))
            return FALSE;
        }

      progress_data.pulled_layers++;
      progress_data.previous_layers_size += delta_layer ? delta_layer->size : layer->size;
    }

  index = flatpak_oci_registry_load_index (dst_registry, NULL, NULL);
  if (index == NULL)
    index = flatpak_oci_index_new ();

  manifest_desc = flatpak_oci_descriptor_new (versioned->mediatype, digest, versioned_size);

  flatpak_oci_index_add_manifest (index, ref, manifest_desc);

  if (!flatpak_oci_registry_save_index (dst_registry, index, cancellable, error))
    return FALSE;

  return TRUE;
}

char *
flatpak_pull_from_oci (OstreeRepo            *repo,
                       FlatpakOciRegistry    *registry,
                       const char            *oci_repository,
                       const char            *digest,
                       const char            *delta_url,
                       FlatpakOciManifest    *manifest,
                       FlatpakOciImage       *image_config,
                       const char            *remote,
                       const char            *ref,
                       FlatpakPullFlags       flags,
                       FlatpakOciPullProgress progress_cb,
                       gpointer               progress_user_data,
                       GCancellable          *cancellable,
                       GError               **error)
{
  gboolean force_disable_deltas = (flags & FLATPAK_PULL_FLAGS_NO_STATIC_DELTAS) != 0;
  g_autoptr(OstreeMutableTree) archive_mtree = NULL;
  g_autoptr(GFile) archive_root = NULL;
  g_autoptr(FlatpakOciManifest) delta_manifest = NULL;
  g_autofree char *old_checksum = NULL;
  g_autoptr(GVariant) old_commit = NULL;
  g_autoptr(GFile) old_root = NULL;
  OstreeRepoCommitState old_state = 0;
  g_autofree char *old_diffid = NULL;
  g_autofree char *commit_checksum = NULL;
  const char *parent = NULL;
  g_autofree char *subject = NULL;
  g_autofree char *body = NULL;
  g_autofree char *manifest_ref = NULL;
  g_autofree char *full_ref = NULL;
  const char *diffid;
  guint64 timestamp = 0;
  FlatpakOciPullProgressData progress_data = { progress_cb, progress_user_data };
  g_autoptr(GVariantBuilder) metadata_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
  g_autoptr(GVariant) metadata = NULL;
  GHashTable *labels;
  int n_layers;
  int i;

  g_assert (ref != NULL);
  g_assert (g_str_has_prefix (digest, "sha256:"));

  labels = flatpak_oci_image_get_labels (image_config);
  if (labels)
    flatpak_oci_parse_commit_labels (labels, &timestamp,
                                     &subject, &body,
                                     &manifest_ref, NULL, NULL,
                                     metadata_builder);

  if (manifest_ref == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("No ref specified for OCI image %s"), digest);
      return NULL;
    }

  if (strcmp (manifest_ref, ref) != 0)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Wrong ref (%s) specified for OCI image %s, expected %s"), manifest_ref, digest, ref);
      return NULL;
    }

  g_variant_builder_add (metadata_builder, "{s@v}", "xa.alt-id",
                         g_variant_new_variant (g_variant_new_string (digest + strlen ("sha256:"))));

  /* For deltas we ensure that the diffid and regular layers exists and match up */
  n_layers = flatpak_oci_manifest_get_n_layers (manifest);
  if (n_layers == 0 || n_layers != flatpak_oci_image_get_n_layers (image_config))
    {
      flatpak_fail (error, _("Invalid OCI image config"));
      return NULL;
    }

  /* Assuming everyting looks good, we record the uncompressed checksum (the diff-id) of the last layer,
     because that is what we can read back easily from the deploy dir, and thus is easy to use for applying deltas */
  diffid = image_config->rootfs.diff_ids[n_layers-1];
  if (diffid != NULL && g_str_has_prefix (diffid, "sha256:"))
    g_variant_builder_add (metadata_builder, "{s@v}", "xa.diff-id",
                           g_variant_new_variant (g_variant_new_string (diffid + strlen ("sha256:"))));

  /* Look for delta manifest, and if it exists, the current (old) commit and its recorded diffid */
  if (!force_disable_deltas &&
      !flatpak_oci_registry_is_local (registry) &&
      flatpak_repo_resolve_rev (repo, NULL, remote, ref, FALSE, &old_checksum, NULL, NULL) &&
      ostree_repo_load_commit (repo, old_checksum, &old_commit, &old_state, NULL) &&
      (old_state == OSTREE_REPO_COMMIT_STATE_NORMAL) &&
      ostree_repo_read_commit (repo, old_checksum, &old_root, NULL, NULL, NULL))
    {
      delta_manifest = flatpak_oci_registry_find_delta_manifest (registry, oci_repository, digest, delta_url, cancellable);
      if (delta_manifest)
        {
          VarMetadataRef commit_metadata = var_commit_get_metadata (var_commit_from_gvariant (old_commit));
          const char *raw_old_diffid = var_metadata_lookup_string (commit_metadata, "xa.diff-id", NULL);
          if (raw_old_diffid != NULL)
            old_diffid = g_strconcat ("sha256:", raw_old_diffid, NULL);
        }
    }

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return NULL;

  /* There is no way to write a subset of the archive to a mtree, so instead
     we write all of it and then build a new mtree with the subset */
  archive_mtree = ostree_mutable_tree_new ();

  for (i = 0; manifest->layers[i] != NULL; i++)
    {
      FlatpakOciDescriptor *layer = manifest->layers[i];
      FlatpakOciDescriptor *delta_layer = NULL;

      if (delta_manifest)
        delta_layer = flatpak_oci_manifest_find_delta_for (delta_manifest, old_diffid, image_config->rootfs.diff_ids[i]);

      if (delta_layer)
        progress_data.total_size += delta_layer->size;
      else
        progress_data.total_size += layer->size;

      progress_data.n_layers++;
    }

  if (progress_cb)
    progress_cb (progress_data.total_size, 0,
                 progress_data.n_layers, progress_data.pulled_layers,
                 progress_user_data);

  for (i = 0; manifest->layers[i] != NULL; i++)
    {
      FlatpakOciDescriptor *layer = manifest->layers[i];
      FlatpakOciDescriptor *delta_layer = NULL;
      OstreeRepoImportArchiveOptions opts = { 0, };
      g_autoptr(FlatpakAutoArchiveRead) a = NULL;
      glnx_autofd int layer_fd = -1;
      glnx_autofd int blob_fd = -1;
      g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
      g_autoptr(GError) local_error = NULL;
      const char *layer_checksum;
      const char *expected_digest;

      if (delta_manifest)
        delta_layer = flatpak_oci_manifest_find_delta_for (delta_manifest, old_diffid, image_config->rootfs.diff_ids[i]);

      opts.autocreate_parents = TRUE;
      opts.ignore_unsupported_content = TRUE;

      if (delta_layer)
        {
          g_info ("Using OCI delta %s for layer %s", delta_layer->digest, layer->digest);
          expected_digest = image_config->rootfs.diff_ids[i]; /* The delta recreates the uncompressed tar so use that digest */
        }
      else
        {
          layer_fd = g_steal_fd (&blob_fd);
          expected_digest = layer->digest;
        }

      blob_fd = flatpak_oci_registry_download_blob (registry, oci_repository, FALSE,
                                                    delta_layer ? delta_layer->digest : layer->digest,
                                                    (const char **)(delta_layer ? delta_layer->urls : layer->urls),
                                                    oci_layer_progress, &progress_data,
                                                    cancellable, &local_error);

      if (blob_fd == -1 && delta_layer == NULL &&
          flatpak_oci_registry_is_local (registry) &&
          g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          /* Pulling regular layer from local repo and its not there, try the uncompressed version.
           * This happens when we deploy via system helper using oci deltas */
          expected_digest = image_config->rootfs.diff_ids[i];
          blob_fd = flatpak_oci_registry_download_blob (registry, oci_repository, FALSE,
                                                        image_config->rootfs.diff_ids[i], NULL,
                                                        oci_layer_progress, &progress_data,
                                                        cancellable, NULL); /* No error here, we report the first error if this failes */
        }

      if (blob_fd == -1)
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          goto error;
        }

      g_clear_error (&local_error);

      if (delta_layer)
        {
          layer_fd = flatpak_oci_registry_apply_delta (registry, blob_fd, old_root, cancellable, error);
          if (layer_fd == -1)
            goto error;
        }
      else
        {
          layer_fd = g_steal_fd (&blob_fd);
        }

      a = archive_read_new ();
#ifdef HAVE_ARCHIVE_READ_SUPPORT_FILTER_ALL
      archive_read_support_filter_all (a);
#else
      archive_read_support_compression_all (a);
#endif
      archive_read_support_format_all (a);

      if (!flatpak_archive_read_open_fd_with_checksum (a, layer_fd, checksum, error))
        goto error;

      if (!ostree_repo_import_archive_to_mtree (repo, &opts, a, archive_mtree, NULL, cancellable, error))
        goto error;

      if (archive_read_close (a) != ARCHIVE_OK)
        {
          propagate_libarchive_error (error, a);
          goto error;
        }

      layer_checksum = g_checksum_get_string (checksum);
      if (!g_str_has_prefix (expected_digest, "sha256:") ||
          strcmp (expected_digest + strlen ("sha256:"), layer_checksum) != 0)
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Wrong layer checksum, expected %s, was %s"), expected_digest, layer_checksum);
          goto error;
        }

      progress_data.pulled_layers++;
      progress_data.previous_layers_size += delta_layer ? delta_layer->size : layer->size;
    }

  if (!ostree_repo_write_mtree (repo, archive_mtree, &archive_root, cancellable, error))
    goto error;

  if (!ostree_repo_file_ensure_resolved ((OstreeRepoFile *) archive_root, error))
    goto error;

  metadata = g_variant_ref_sink (g_variant_builder_end (metadata_builder));
  if (!ostree_repo_write_commit_with_time (repo,
                                           parent,
                                           subject,
                                           body,
                                           metadata,
                                           OSTREE_REPO_FILE (archive_root),
                                           timestamp,
                                           &commit_checksum,
                                           cancellable, error))
    goto error;

  if (remote)
    full_ref = g_strdup_printf ("%s:%s", remote, ref);
  else
    full_ref = g_strdup (ref);

  /* Don’t need to set the collection ID here, since the ref is bound to a
   * collection via its remote. */
  ostree_repo_transaction_set_ref (repo, NULL, full_ref, commit_checksum);

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    return NULL;

  return g_steal_pointer (&commit_checksum);

error:

  ostree_repo_abort_transaction (repo, cancellable, NULL);
  return NULL;
}

/* This allocates and locks a subdir of the tmp dir, using an existing
 * one with the same prefix if it is not in use already. */
gboolean
flatpak_allocate_tmpdir (int           tmpdir_dfd,
                         const char   *tmpdir_relpath,
                         const char   *tmpdir_prefix,
                         char        **tmpdir_name_out,
                         int          *tmpdir_fd_out,
                         GLnxLockFile *file_lock_out,
                         gboolean     *reusing_dir_out,
                         GCancellable *cancellable,
                         GError      **error)
{
  gboolean reusing_dir = FALSE;
  g_autofree char *tmpdir_name = NULL;
  glnx_autofd int tmpdir_fd = -1;
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

  /* Look for existing tmpdir (with same prefix) to reuse */
  if (!glnx_dirfd_iterator_init_at (tmpdir_dfd, tmpdir_relpath ? tmpdir_relpath : ".", FALSE, &dfd_iter, error))
    return FALSE;

  while (tmpdir_name == NULL)
    {
      struct dirent *dent;
      glnx_autofd int existing_tmpdir_fd = -1;
      g_autoptr(GError) local_error = NULL;
      g_autofree char *lock_name = NULL;

      if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, cancellable, error))
        return FALSE;

      if (dent == NULL)
        break;

      if (!g_str_has_prefix (dent->d_name, tmpdir_prefix))
        continue;

      /* Quickly skip non-dirs, if unknown we ignore ENOTDIR when opening instead */
      if (dent->d_type != DT_UNKNOWN &&
          dent->d_type != DT_DIR)
        continue;

      if (!glnx_opendirat (dfd_iter.fd, dent->d_name, FALSE,
                           &existing_tmpdir_fd, &local_error))
        {
          if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY))
            {
              continue;
            }
          else
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }
        }

      lock_name = g_strconcat (dent->d_name, "-lock", NULL);

      /* We put the lock outside the dir, so we can hold the lock
       * until the directory is fully removed */
      if (!glnx_make_lock_file (dfd_iter.fd, lock_name, LOCK_EX | LOCK_NB,
                                file_lock_out, &local_error))
        {
          if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
            {
              continue;
            }
          else
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }
        }

      /* Touch the reused directory so that we don't accidentally
       *   remove it due to being old when cleaning up the tmpdir
       */
      (void) futimens (existing_tmpdir_fd, NULL);

      /* We found an existing tmpdir which we managed to lock */
      tmpdir_name = g_strdup (dent->d_name);
      tmpdir_fd = g_steal_fd (&existing_tmpdir_fd);
      reusing_dir = TRUE;
    }

  while (tmpdir_name == NULL)
    {
      g_autofree char *tmpdir_name_template = g_strconcat (tmpdir_prefix, "XXXXXX", NULL);
      g_autoptr(GError) local_error = NULL;
      g_autofree char *lock_name = NULL;
      g_auto(GLnxTmpDir) new_tmpdir = { 0, };
      /* No existing tmpdir found, create a new */

      if (!glnx_mkdtempat (dfd_iter.fd, tmpdir_name_template, 0777,
                           &new_tmpdir, error))
        return FALSE;

      lock_name = g_strconcat (new_tmpdir.path, "-lock", NULL);

      /* Note, at this point we can race with another process that picks up this
       * new directory. If that happens we need to retry, making a new directory. */
      if (!glnx_make_lock_file (dfd_iter.fd, lock_name, LOCK_EX | LOCK_NB,
                                file_lock_out, &local_error))
        {
          if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
            {
              glnx_tmpdir_unset (&new_tmpdir); /* Don't delete */
              continue;
            }
          else
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }
        }

      tmpdir_name = g_strdup (new_tmpdir.path);
      tmpdir_fd = dup (new_tmpdir.fd);
      glnx_tmpdir_unset (&new_tmpdir); /* Don't delete */
    }

  if (tmpdir_name_out)
    *tmpdir_name_out = g_steal_pointer (&tmpdir_name);

  if (tmpdir_fd_out)
    *tmpdir_fd_out = g_steal_fd (&tmpdir_fd);

  if (reusing_dir_out)
    *reusing_dir_out = reusing_dir;

  return TRUE;
}

static gint
string_length_compare_func (gconstpointer a,
                            gconstpointer b)
{
  return strlen (*(char * const *) a) - strlen (*(char * const *) b);
}

/* Sort a string array by decreasing length */
char **
flatpak_strv_sort_by_length (const char * const *strv)
{
  GPtrArray *array;
  int i;

  if (strv == NULL)
    return NULL;

  /* Combine both */
  array = g_ptr_array_new ();

  for (i = 0; strv[i] != NULL; i++)
    g_ptr_array_add (array, g_strdup (strv[i]));

  g_ptr_array_sort (array, string_length_compare_func);

  g_ptr_array_add (array, NULL);
  return (char **) g_ptr_array_free (array, FALSE);
}

char **
flatpak_strv_merge (char   **strv1,
                    char   **strv2)
{
  GPtrArray *array;
  int i;

  /* Maybe either (or both) is unspecified */
  if (strv1 == NULL)
    return g_strdupv (strv2);
  if (strv2 == NULL)
    return g_strdupv (strv1);

  /* Combine both */
  array = g_ptr_array_new ();

  for (i = 0; strv1[i] != NULL; i++)
    {
      if (!flatpak_g_ptr_array_contains_string (array, strv1[i]))
        g_ptr_array_add (array, g_strdup (strv1[i]));
    }

  for (i = 0; strv2[i] != NULL; i++)
    {
      if (!flatpak_g_ptr_array_contains_string (array, strv2[i]))
        g_ptr_array_add (array, g_strdup (strv2[i]));
    }

  g_ptr_array_add (array, NULL);
  return (char **) g_ptr_array_free (array, FALSE);
}

/* In this NULL means don't care about these paths, while
   an empty array means match anything */
char **
flatpak_subpaths_merge (char **subpaths1,
                        char **subpaths2)
{
  char **res;

  if (subpaths1 != NULL && subpaths1[0] == NULL)
    return g_strdupv (subpaths1);
  if (subpaths2 != NULL && subpaths2[0] == NULL)
    return g_strdupv (subpaths2);

  res = flatpak_strv_merge (subpaths1, subpaths2);
  if (res)
    qsort (res, g_strv_length (res), sizeof (const char *), flatpak_strcmp0_ptr);

  return res;
}

gboolean
flatpak_g_ptr_array_contains_string (GPtrArray *array, const char *str)
{
  int i;

  for (i = 0; i < array->len; i++)
    {
      if (strcmp (g_ptr_array_index (array, i), str) == 0)
        return TRUE;
    }
  return FALSE;
}

void
flatpak_log_dir_access (FlatpakDir *dir)
{
  if (dir != NULL)
    {
      GFile *dir_path = NULL;
      g_autofree char *dir_path_str = NULL;
      g_autofree char *dir_name = NULL;

      dir_path = flatpak_dir_get_path (dir);
      if (dir_path != NULL)
        dir_path_str = g_file_get_path (dir_path);
      dir_name = flatpak_dir_get_name (dir);
      g_info ("Opening %s flatpak installation at path %s", dir_name, dir_path_str);
    }
}

gboolean
flatpak_check_required_version (const char *ref,
                                GKeyFile   *metakey,
                                GError    **error)
{
  g_auto(GStrv) required_versions = NULL;
  const char *group;
  int max_required_major = 0, max_required_minor = 0;
  const char *max_required_version = "0.0";
  int i;

  if (g_str_has_prefix (ref, "app/"))
    group = "Application";
  else
    group = "Runtime";

  /* We handle handle multiple version requirements here. Each requirement must
   * be in the form major.minor.micro, and if the flatpak version matches the
   * major.minor part, t must be equal or later in the micro. If the major.minor part
   * doesn't exactly match any of the specified requirements it must be larger
   * than the maximum specified requirement.
   *
   * For example, specifying
   *   required-flatpak=1.6.2;1.4.2;1.0.2;
   * would allow flatpak versions:
   *  1.7.0, 1.6.2, 1.6.3, 1.4.2, 1.4.3, 1.0.2, 1.0.3
   * but not:
   *  1.6.1, 1.4.1 or 1.2.100.
   *
   * The goal here is to be able to specify a version (like 1.6.2 above) where a feature
   * was introduced, but also allow backports of said feature to earlier version series.
   *
   * Earlier versions that only support specifying one version will only look at the first
   * element in the list, so put the largest version first.
   */
  required_versions = g_key_file_get_string_list (metakey, group, "required-flatpak", NULL, NULL);
  if (required_versions == 0 || required_versions[0] == NULL)
    return TRUE;

  for (i = 0; required_versions[i] != NULL; i++)
    {
      int required_major, required_minor, required_micro;
      const char *required_version = required_versions[i];

      if (sscanf (required_version, "%d.%d.%d", &required_major, &required_minor, &required_micro) != 3)
        return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA,
                                   _("Invalid require-flatpak argument %s"), required_version);
      else
        {
          /* If flatpak is in the same major.minor series as the requirement, do a micro check */
          if (required_major == PACKAGE_MAJOR_VERSION && required_minor == PACKAGE_MINOR_VERSION)
            {
              if (required_micro <= PACKAGE_MICRO_VERSION)
                return TRUE;
              else
                return flatpak_fail_error (error, FLATPAK_ERROR_NEED_NEW_FLATPAK,
                                           _("%s needs a later flatpak version (%s)"),
                                           ref, required_version);
            }

          /* Otherwise, keep track of the largest major.minor that is required */
          if ((required_major > max_required_major) ||
              (required_major == max_required_major &&
               required_minor > max_required_minor))
            {
              max_required_major = required_major;
              max_required_minor = required_minor;
              max_required_version = required_version;
            }
        }
    }

  if (max_required_major > PACKAGE_MAJOR_VERSION ||
      (max_required_major == PACKAGE_MAJOR_VERSION && max_required_minor > PACKAGE_MINOR_VERSION))
    return flatpak_fail_error (error, FLATPAK_ERROR_NEED_NEW_FLATPAK,
                               _("%s needs a later flatpak version (%s)"),
                               ref, max_required_version);

  return TRUE;
}

static int
dist (const char *s, int ls, const char *t, int lt, int i, int j, int *d)
{
  int x, y;

  if (d[i * (lt + 1) + j] >= 0)
    return d[i * (lt + 1) + j];

  if (i == ls)
    x = lt - j;
  else if (j == lt)
    x = ls - i;
  else if (s[i] == t[j])
    x = dist (s, ls, t, lt, i + 1, j + 1, d);
  else
    {
      x = dist (s, ls, t, lt, i + 1, j + 1, d);
      y = dist (s, ls, t, lt, i, j + 1, d);
      if (y < x)
        x = y;
      y = dist (s, ls, t, lt, i + 1, j, d);
      if (y < x)
        x = y;
      x++;
    }

  d[i * (lt + 1) + j] = x;

  return x;
}

int
flatpak_levenshtein_distance (const char *s,
                              gssize ls,
                              const char *t,
                              gssize lt)
{
  int i, j;
  int *d;

  if (ls < 0)
    ls = strlen (s);

  if (lt < 0)
    lt = strlen (t);

  d = alloca (sizeof (int) * (ls + 1) * (lt + 1));

  for (i = 0; i <= ls; i++)
    for (j = 0; j <= lt; j++)
      d[i * (lt + 1) + j] = -1;

  return dist (s, ls, t, lt, 0, 0, d);
}

/* Wrapper that uses ostree_repo_resolve_collection_ref() and on failure falls
 * back to using ostree_repo_resolve_rev() for backwards compatibility. This
 * means we support refs/heads/, refs/remotes/, and refs/mirrors/. */
gboolean
flatpak_repo_resolve_rev (OstreeRepo    *repo,
                          const char    *collection_id, /* nullable */
                          const char    *remote_name, /* nullable */
                          const char    *ref_name,
                          gboolean       allow_noent,
                          char         **out_rev,
                          GCancellable  *cancellable,
                          GError       **error)
{
  g_autoptr(GError) local_error = NULL;

  if (collection_id != NULL)
    {
      /* Do a version check to ensure we have these:
       * https://github.com/ostreedev/ostree/pull/1821
       * https://github.com/ostreedev/ostree/pull/1825 */
#if OSTREE_CHECK_VERSION (2019, 2)
      const OstreeCollectionRef c_r =
        {
          .collection_id = (char *) collection_id,
          .ref_name = (char *) ref_name,
        };
      OstreeRepoResolveRevExtFlags flags = remote_name == NULL ?
                                           OSTREE_REPO_RESOLVE_REV_EXT_LOCAL_ONLY :
                                           OSTREE_REPO_RESOLVE_REV_EXT_NONE;
      if (ostree_repo_resolve_collection_ref (repo, &c_r,
                                              allow_noent,
                                              flags,
                                              out_rev,
                                              cancellable, NULL))
        return TRUE;
#endif
    }

  /* There may be several remotes with the same branch (if we for
   * instance changed the origin) so prepend the current origin to
   * make sure we get the right one */
  if (remote_name != NULL)
    {
      g_autofree char *refspec = g_strdup_printf ("%s:%s", remote_name, ref_name);
      ostree_repo_resolve_rev (repo, refspec, allow_noent, out_rev, &local_error);
    }
  else
    ostree_repo_resolve_rev_ext (repo, ref_name, allow_noent,
                                 OSTREE_REPO_RESOLVE_REV_EXT_NONE, out_rev, &local_error);

  if (local_error != NULL)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        flatpak_fail_error (error, FLATPAK_ERROR_REF_NOT_FOUND, "%s", local_error->message);
      else
        g_propagate_error (error, g_steal_pointer (&local_error));

      return FALSE;
    }

  return TRUE;
}

/* Convert an app id to a dconf path in the obvious way.
 */
char *
flatpak_dconf_path_for_app_id (const char *app_id)
{
  GString *s;
  const char *p;

  s = g_string_new ("");

  g_string_append_c (s, '/');
  for (p = app_id; *p; p++)
    {
      if (*p == '.')
        g_string_append_c (s, '/');
      else
        g_string_append_c (s, *p);
    }
  g_string_append_c (s, '/');

  return g_string_free (s, FALSE);
}

/* Check if two dconf paths are 'similar enough', which
 * for now is defined as equal except case differences
 * and -/_
 */
gboolean
flatpak_dconf_path_is_similar (const char *path1,
                               const char *path2)
{
  int i1, i2;
  int num_components = -1;

  for (i1 = i2 = 0; path1[i1] != '\0'; i1++, i2++)
    {
      if (path2[i2] == '\0')
        break;

      if (isupper(path2[i2]) &&
          (path1[i1] == '-' || path1[i1] == '_'))
        {
          i1++;
          if (path1[i1] == '\0')
            break;
        }

      if (isupper(path1[i1]) &&
          (path2[i2] == '-' || path2[i2] == '_'))
        {
          i2++;
          if (path2[i2] == '\0')
            break;
        }

      if (tolower (path1[i1]) == tolower (path2[i2]))
        {
          if (path1[i1] == '/')
            num_components++;
          continue;
        }

      if ((path1[i1] == '-' || path1[i1] == '_') &&
          (path2[i2] == '-' || path2[i2] == '_'))
        continue;

      break;
    }

  /* Skip over any versioning if we have at least a TLD and
   * domain name, so 2 components */
  /* We need at least TLD, and domain name, so 2 components */
  if (num_components >= 2)
    {
      while (isdigit (path1[i1]))
        i1++;
      while (isdigit (path2[i2]))
        i2++;
    }

  if (path1[i1] != path2[i2])
    return FALSE;

  /* Both strings finished? */
  if (path1[i1] == '\0')
    return TRUE;

  /* Maybe a trailing slash in both strings */
  if (path1[i1] == '/')
    {
      i1++;
      i2++;
    }

  if (path1[i1] != path2[i2])
    return FALSE;

  return (path1[i1] == '\0');
}

GStrv
flatpak_parse_env_block (const char  *data,
                         gsize        length,
                         GError     **error)
{
  g_autoptr(GPtrArray) env_vars = g_ptr_array_new_with_free_func (g_free);
  const char *p = data;
  gsize remaining = length;

  /* env_block might not be \0-terminated */
  while (remaining > 0)
    {
      size_t len = strnlen (p, remaining);
      const char *equals;

      g_assert (len <= remaining);

      equals = memchr (p, '=', len);

      if (equals == NULL || equals == p)
        return glnx_null_throw (error,
                                "Environment variable must be in the form VARIABLE=VALUE, not %.*s", (int) len, p);

      g_ptr_array_add (env_vars,
                       g_strndup (p, len));

      p += len;
      remaining -= len;

      if (remaining > 0)
        {
          g_assert (*p == '\0');
          p += 1;
          remaining -= 1;
        }
    }

  g_ptr_array_add (env_vars, NULL);

  return (GStrv) g_ptr_array_free (g_steal_pointer (&env_vars), FALSE);
}

/**
 * flatpak_envp_cmp:
 * @p1: a `const char * const *`
 * @p2: a `const char * const *`
 *
 * Compare two environment variables, given as pointers to pointers
 * to the actual `KEY=value` string.
 *
 * In particular this is suitable for sorting a #GStrv using `qsort`.
 *
 * Returns: negative, 0 or positive if `*p1` compares before, equal to
 *  or after `*p2`
 */
int
flatpak_envp_cmp (const void *p1,
                  const void *p2)
{
  const char * const * s1 = p1;
  const char * const * s2 = p2;
  size_t l1 = strlen (*s1);
  size_t l2 = strlen (*s2);
  size_t min;
  const char *tmp;
  int ret;

  tmp = strchr (*s1, '=');

  if (tmp != NULL)
    l1 = tmp - *s1;

  tmp = strchr (*s2, '=');

  if (tmp != NULL)
    l2 = tmp - *s2;

  min = MIN (l1, l2);
  ret = strncmp (*s1, *s2, min);

  /* If they differ before the first '=' (if any) in either s1 or s2,
   * then they are certainly different */
  if (ret != 0)
    return ret;

  ret = strcmp (*s1, *s2);

  /* If they do not differ at all, then they are equal */
  if (ret == 0)
    return ret;

  /* FOO < FOO=..., and FOO < FOOBAR */
  if ((*s1)[min] == '\0')
    return -1;

  /* FOO=... > FOO, and FOOBAR > FOO */
  if ((*s2)[min] == '\0')
    return 1;

  /* FOO= < FOOBAR */
  if ((*s1)[min] == '=' && (*s2)[min] != '=')
    return -1;

  /* FOOBAR > FOO= */
  if ((*s2)[min] == '=' && (*s1)[min] != '=')
    return 1;

  /* Fall back to plain string comparison */
  return ret;
}

/*
 * Return %TRUE if @s consists of one or more digits.
 * This is the same as Python bytes.isdigit().
 */
gboolean
flatpak_str_is_integer (const char *s)
{
  if (s == NULL || *s == '\0')
    return FALSE;

  for (; *s != '\0'; s++)
    {
      if (!g_ascii_isdigit (*s))
        return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_uri_equal (const char *uri1,
                   const char *uri2)
{
  g_autofree char *uri1_norm = NULL;
  g_autofree char *uri2_norm = NULL;
  gsize uri1_len = strlen (uri1);
  gsize uri2_len = strlen (uri2);

  /* URIs handled by libostree are equivalent with or without a trailing slash,
   * but this isn't otherwise guaranteed to be the case.
   */
  if (g_str_has_prefix (uri1, "oci+") || g_str_has_prefix (uri2, "oci+"))
    return g_strcmp0 (uri1, uri2) == 0;

  if (g_str_has_suffix (uri1, "/"))
    uri1_norm = g_strndup (uri1, uri1_len - 1);
  else
    uri1_norm = g_strdup (uri1);

  if (g_str_has_suffix (uri2, "/"))
    uri2_norm = g_strndup (uri2, uri2_len - 1);
  else
    uri2_norm = g_strdup (uri2);

  return g_strcmp0 (uri1_norm, uri2_norm) == 0;
}

static gboolean
is_char_safe (gunichar c)
{
  return g_unichar_isgraph (c) || c == ' ';
}

static gboolean
should_hex_escape (gunichar           c,
                   FlatpakEscapeFlags flags)
{
  if ((flags & FLATPAK_ESCAPE_ALLOW_NEWLINES) && c == '\n')
    return FALSE;

  return !is_char_safe (c);
}

static void
append_hex_escaped_character (GString *result,
                              gunichar c)
{
  if (c <= 0xFF)
    g_string_append_printf (result, "\\x%02X", c);
  else if (c <= 0xFFFF)
    g_string_append_printf (result, "\\u%04X", c);
  else
    g_string_append_printf (result, "\\U%08X", c);
}

static char *
escape_character (gunichar c)
{
  g_autoptr(GString) res = g_string_new ("");
  append_hex_escaped_character (res, c);
  return g_string_free (g_steal_pointer (&res), FALSE);
}

char *
flatpak_escape_string (const char        *s,
                       FlatpakEscapeFlags flags)
{
  g_autoptr(GString) res = g_string_new ("");
  gboolean did_escape = FALSE;

  while (*s)
    {
      gunichar c = g_utf8_get_char_validated (s, -1);
      if (c == (gunichar)-2 || c == (gunichar)-1)
        {
          /* Need to convert to unsigned first, to avoid negative chars becoming
             huge gunichars. */
          append_hex_escaped_character (res, (unsigned char)*s++);
          did_escape = TRUE;
          continue;
        }
      else if (should_hex_escape (c, flags))
        {
          append_hex_escaped_character (res, c);
          did_escape = TRUE;
        }
      else if (c == '\\' || (!(flags & FLATPAK_ESCAPE_DO_NOT_QUOTE) && c == '\''))
        {
          g_string_append_printf (res, "\\%c", (char) c);
          did_escape = TRUE;
        }
      else
        g_string_append_unichar (res, c);

      s = g_utf8_find_next_char (s, NULL);
    }

  if (did_escape && !(flags & FLATPAK_ESCAPE_DO_NOT_QUOTE))
    {
      g_string_prepend_c (res, '\'');
      g_string_append_c (res, '\'');
    }

  return g_string_free (g_steal_pointer (&res), FALSE);
}

gboolean
flatpak_validate_path_characters (const char *path,
                                  GError    **error)
{
  while (*path)
    {
      gunichar c = g_utf8_get_char_validated (path, -1);
      if (c == (gunichar)-1 || c == (gunichar)-2)
        {
          /* Need to convert to unsigned first, to avoid negative chars becoming
             huge gunichars. */
          g_autofree char *escaped_char = escape_character ((unsigned char)*path);
          g_autofree char *escaped = flatpak_escape_string (path, FLATPAK_ESCAPE_DEFAULT);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "Non-UTF8 byte %s in path %s", escaped_char, escaped);
          return FALSE;
        }
      else if (!is_char_safe (c))
        {
          g_autofree char *escaped_char = escape_character (c);
          g_autofree char *escaped = flatpak_escape_string (path, FLATPAK_ESCAPE_DEFAULT);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "Non-graphical character %s in path %s", escaped_char, escaped);
          return FALSE;
        }

      path = g_utf8_find_next_char (path, NULL);
    }

  return TRUE;
}

gboolean
running_under_sudo (void)
{
  const char *sudo_command_env = g_getenv ("SUDO_COMMAND");
  g_auto(GStrv) split_command = NULL;

  if (!sudo_command_env)
    return FALSE;

  /* SUDO_COMMAND could be a value like `/usr/bin/flatpak run foo` */
  split_command = g_strsplit (sudo_command_env, " ", 2);
  if (g_str_has_suffix (split_command[0], "flatpak"))
    return TRUE;

  return FALSE;
}
