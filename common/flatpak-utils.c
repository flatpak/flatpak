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
#include "libglnx/libglnx.h"
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

void
flatpak_debug2 (const char *format, ...)
{
  va_list var_args;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
  va_start (var_args, format);
  g_logv (G_LOG_DOMAIN "2",
          G_LOG_LEVEL_DEBUG,
          format, var_args);
  va_end (var_args);
#pragma GCC diagnostic pop
}

gboolean
flatpak_write_update_checksum (GOutputStream *out,
                               gconstpointer  data,
                               gsize          len,
                               gsize         *out_bytes_written,
                               GChecksum     *checksum,
                               GCancellable  *cancellable,
                               GError       **error)
{
  if (out)
    {
      if (!g_output_stream_write_all (out, data, len, out_bytes_written,
                                      cancellable, error))
        return FALSE;
    }
  else if (out_bytes_written)
    {
      *out_bytes_written = len;
    }

  if (checksum)
    g_checksum_update (checksum, data, len);

  return TRUE;
}

gboolean
flatpak_splice_update_checksum (GOutputStream         *out,
                                GInputStream          *in,
                                GChecksum             *checksum,
                                FlatpakLoadUriProgress progress,
                                gpointer               progress_data,
                                GCancellable          *cancellable,
                                GError               **error)
{
  gsize bytes_read, bytes_written;
  char buf[32 * 1024];
  guint64 downloaded_bytes = 0;
  gint64 progress_start;

  progress_start = g_get_monotonic_time ();
  do
    {
      if (!g_input_stream_read_all (in, buf, sizeof buf, &bytes_read, cancellable, error))
        return FALSE;

      if (!flatpak_write_update_checksum (out, buf, bytes_read, &bytes_written, checksum,
                                          cancellable, error))
        return FALSE;

      downloaded_bytes += bytes_read;

      if (progress &&
          g_get_monotonic_time () - progress_start >  5 * 1000000)
        {
          progress (downloaded_bytes, progress_data);
          progress_start = g_get_monotonic_time ();
        }
    }
  while (bytes_read > 0);

  if (progress)
    progress (downloaded_bytes, progress_data);

  return TRUE;
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
              char *tmp = strchr (string, '/');
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

static const char *
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
                                                    "org.gnome.desktop.interface", FALSE);

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

static int fancy_output = -1;

void
flatpak_disable_fancy_output (void)
{
  fancy_output = FALSE;
}

void
flatpak_enable_fancy_output (void)
{
  fancy_output = TRUE;
}

gboolean
flatpak_fancy_output (void)
{
  if (fancy_output != -1)
    return fancy_output;

  if (g_strcmp0 (g_getenv ("FLATPAK_FANCY_OUTPUT"), "0") == 0)
    return FALSE;

  return isatty (STDOUT_FILENO);
}

const char *
flatpak_get_bwrap (void)
{
  const char *e = g_getenv ("FLATPAK_BWRAP");

  if (e != NULL)
    return e;
  return HELPER;
}

static gboolean
is_valid_initial_name_character (gint c, gboolean allow_dash)
{
  return
    (c >= 'A' && c <= 'Z') ||
    (c >= 'a' && c <= 'z') ||
    (c == '_') || (allow_dash && c == '-');
}

static gboolean
is_valid_name_character (gint c, gboolean allow_dash)
{
  return
    is_valid_initial_name_character (c, allow_dash) ||
    (c >= '0' && c <= '9');
}

/**
 * flatpak_is_valid_name:
 * @string: The string to check
 * @error: Return location for an error
 *
 * Checks if @string is a valid application name.
 *
 * App names are composed of 3 or more elements separated by a period
 * ('.') character. All elements must contain at least one character.
 *
 * Each element must only contain the ASCII characters
 * "[A-Z][a-z][0-9]_-". Elements may not begin with a digit.
 * Additionally "-" is only allowed in the last element.
 *
 * App names must not begin with a '.' (period) character.
 *
 * App names must not exceed 255 characters in length.
 *
 * The above means that any app name is also a valid DBus well known
 * bus name, but not all DBus names are valid app names. The difference are:
 * 1) DBus name elements may contain '-' in the non-last element.
 * 2) DBus names require only two elements
 *
 * Returns: %TRUE if valid, %FALSE otherwise.
 *
 * Since: 2.26
 */
gboolean
flatpak_is_valid_name (const char *string,
                       GError    **error)
{
  guint len;
  gboolean ret;
  const gchar *s;
  const gchar *end;
  const gchar *last_dot;
  int dot_count;
  gboolean last_element;

  g_return_val_if_fail (string != NULL, FALSE);

  ret = FALSE;

  len = strlen (string);
  if (G_UNLIKELY (len == 0))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                          _("Name can't be empty"));
      goto out;
    }

  if (G_UNLIKELY (len > 255))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                          _("Name can't be longer than 255 characters"));
      goto out;
    }

  end = string + len;

  last_dot = strrchr (string, '.');
  last_element = FALSE;

  s = string;
  if (G_UNLIKELY (*s == '.'))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                          _("Name can't start with a period"));
      goto out;
    }
  else if (G_UNLIKELY (!is_valid_initial_name_character (*s, last_element)))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                          _("Name can't start with %c"), *s);
      goto out;
    }

  s += 1;
  dot_count = 0;
  while (s != end)
    {
      if (*s == '.')
        {
          if (s == last_dot)
            last_element = TRUE;
          s += 1;
          if (G_UNLIKELY (s == end))
            {
              flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                                  _("Name can't end with a period"));
              goto out;
            }
          if (!is_valid_initial_name_character (*s, last_element))
            {
              if (*s == '-')
                flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                                    _("Only last name segment can contain -"));
              else
                flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                                    _("Name segment can't start with %c"), *s);
              goto out;
            }
          dot_count++;
        }
      else if (G_UNLIKELY (!is_valid_name_character (*s, last_element)))
        {
          if (*s == '-')
            flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                                _("Only last name segment can contain -"));
          else
            flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                                _("Name can't contain %c"), *s);
          goto out;
        }
      s += 1;
    }

  if (G_UNLIKELY (dot_count < 2))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                          _("Names must contain at least 2 periods"));
      goto out;
    }

  ret = TRUE;

out:
  return ret;
}

gboolean
flatpak_has_name_prefix (const char *string,
                         const char *name)
{
  const char *rest;

  if (!g_str_has_prefix (string, name))
    return FALSE;

  rest = string + strlen (name);
  return
    *rest == 0 ||
    *rest == '.' ||
    !is_valid_name_character (*rest, FALSE);
}

gboolean
flatpak_name_matches_one_wildcard_prefix (const char         *name,
                                          const char * const *wildcarded_prefixes,
                                          gboolean            require_exact_match)
{
  const char * const *iter = wildcarded_prefixes;
  const char *remainder;
  gsize longest_match_len = 0;

  /* Find longest valid match */
  for (; *iter != NULL; ++iter)
    {
      const char *prefix = *iter;
      gsize prefix_len = strlen (prefix);
      gsize match_len = strlen (prefix);
      gboolean has_wildcard = FALSE;
      const char *end_of_match;

      if (g_str_has_suffix (prefix, ".*"))
        {
          has_wildcard = TRUE;
          prefix_len -= 2;
        }

      if (strncmp (name, prefix, prefix_len) != 0)
        continue;

      end_of_match = name + prefix_len;

      if (has_wildcard &&
          end_of_match[0] == '.' &&
          is_valid_initial_name_character (end_of_match[1], TRUE))
        {
          end_of_match += 2;
          while (*end_of_match != 0 &&
                 (is_valid_name_character (*end_of_match, TRUE) ||
                  (end_of_match[0] == '.' &&
                   is_valid_initial_name_character (end_of_match[1], TRUE))))
            end_of_match++;
        }

      match_len = end_of_match - name;

      if (match_len > longest_match_len)
        longest_match_len = match_len;
    }

  if (longest_match_len == 0)
    return FALSE;

  if (require_exact_match)
    return name[longest_match_len] == 0;

  /* non-exact matches can be exact, or can be followed by characters that would make
   * not be part of the last element in the matched prefix, due to being invalid or
   * a new element. As a special case we explicitly disallow dash here, even though
   * it iss typically allowed in the final element of a name, this allows you too sloppily
   * match org.the.App with org.the.App-symbolic[.png] or org.the.App-settings[.desktop].
   */
  remainder = name + longest_match_len;
  return
    *remainder == 0 ||
    *remainder == '.' ||
    !is_valid_name_character (*remainder, FALSE);
}

gboolean
flatpak_get_allowed_exports (const char     *source_path,
                             const char     *app_id,
                             FlatpakContext *context,
                             char         ***allowed_extensions_out,
                             char         ***allowed_prefixes_out,
                             gboolean       *require_exact_match_out)
{
  g_autoptr(GPtrArray) allowed_extensions = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) allowed_prefixes = g_ptr_array_new_with_free_func (g_free);
  gboolean require_exact_match = FALSE;

  g_ptr_array_add (allowed_prefixes, g_strdup_printf ("%s.*", app_id));

  if (strcmp (source_path, "share/applications") == 0)
    {
      g_ptr_array_add (allowed_extensions, g_strdup (".desktop"));
    }
  else if (flatpak_has_path_prefix (source_path, "share/icons"))
    {
      g_ptr_array_add (allowed_extensions, g_strdup (".svgz"));
      g_ptr_array_add (allowed_extensions, g_strdup (".png"));
      g_ptr_array_add (allowed_extensions, g_strdup (".svg"));
      g_ptr_array_add (allowed_extensions, g_strdup (".ico"));
    }
  else if (strcmp (source_path, "share/dbus-1/services") == 0)
    {
      g_auto(GStrv) owned_dbus_names =  flatpak_context_get_session_bus_policy_allowed_own_names (context);

      g_ptr_array_add (allowed_extensions, g_strdup (".service"));

      for (GStrv iter = owned_dbus_names; *iter != NULL; ++iter)
        g_ptr_array_add (allowed_prefixes, g_strdup (*iter));

      /* We need an exact match with no extra garbage, because the filename refers to busnames
       * and we can *only* match exactly these */
      require_exact_match = TRUE;
    }
  else if (strcmp (source_path, "share/gnome-shell/search-providers") == 0)
    {
      g_ptr_array_add (allowed_extensions, g_strdup (".ini"));
    }
  else if (strcmp (source_path, "share/mime/packages") == 0)
    {
      g_ptr_array_add (allowed_extensions, g_strdup (".xml"));
    }
  else
    return FALSE;

  g_ptr_array_add (allowed_extensions, NULL);
  g_ptr_array_add (allowed_prefixes, NULL);

  if (allowed_extensions_out)
    *allowed_extensions_out = (char **) g_ptr_array_free (g_steal_pointer (&allowed_extensions), FALSE);

  if (allowed_prefixes_out)
    *allowed_prefixes_out = (char **) g_ptr_array_free (g_steal_pointer (&allowed_prefixes), FALSE);

  if (require_exact_match_out)
    *require_exact_match_out = require_exact_match;

  return TRUE;
}

static gboolean
is_valid_initial_branch_character (gint c)
{
  return
    (c >= '0' && c <= '9') ||
    (c >= 'A' && c <= 'Z') ||
    (c >= 'a' && c <= 'z') ||
    (c == '_') ||
    (c == '-');
}

static gboolean
is_valid_branch_character (gint c)
{
  return
    is_valid_initial_branch_character (c) ||
    (c == '.');
}

/**
 * flatpak_is_valid_branch:
 * @string: The string to check
 * @error: return location for an error
 *
 * Checks if @string is a valid branch name.
 *
 * Branch names must only contain the ASCII characters
 * "[A-Z][a-z][0-9]_-.".
 * Branch names may not begin with a period.
 * Branch names must contain at least one character.
 *
 * Returns: %TRUE if valid, %FALSE otherwise.
 *
 * Since: 2.26
 */
gboolean
flatpak_is_valid_branch (const char *string,
                         GError    **error)
{
  guint len;
  gboolean ret;
  const gchar *s;
  const gchar *end;

  g_return_val_if_fail (string != NULL, FALSE);

  ret = FALSE;

  len = strlen (string);
  if (G_UNLIKELY (len == 0))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                          _("Branch can't be empty"));
      goto out;
    }

  end = string + len;

  s = string;
  if (G_UNLIKELY (!is_valid_initial_branch_character (*s)))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                          _("Branch can't start with %c"), *s);
      goto out;
    }

  s += 1;
  while (s != end)
    {
      if (G_UNLIKELY (!is_valid_branch_character (*s)))
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_NAME,
                              _("Branch can't contain %c"), *s);
          goto out;
        }
      s += 1;
    }

  ret = TRUE;

out:
  return ret;
}

/* Dashes are only valid in the last part of the app id, so
   we replace them with underscore so we can suffix the id */
char *
flatpak_make_valid_id_prefix (const char *orig_id)
{
  char *id, *t;

  id = g_strdup (orig_id);
  t = id;
  while (*t != 0 && *t != '/')
    {
      if (*t == '-')
        *t = '_';

      t++;
    }

  return id;
}

gboolean
flatpak_id_has_subref_suffix (const char *id)
{
  return
    g_str_has_suffix (id, ".Locale") ||
    g_str_has_suffix (id, ".Debug") ||
    g_str_has_suffix (id, ".Sources");
}


static const char *
skip_segment (const char *s)
{
  const char *slash;

  slash = strchr (s, '/');
  if (slash)
    return slash + 1;
  return s + strlen (s);
}

static int
compare_segment (const char *s1, const char *s2)
{
  gint c1, c2;

  while (*s1 && *s1 != '/' &&
         *s2 && *s2 != '/')
    {
      c1 = *s1;
      c2 = *s2;
      if (c1 != c2)
        return c1 - c2;
      s1++;
      s2++;
    }

  c1 = *s1;
  if (c1 == '/')
    c1 = 0;
  c2 = *s2;
  if (c2 == '/')
    c2 = 0;

  return c1 - c2;
}

int
flatpak_compare_ref (const char *ref1, const char *ref2)
{
  int res;
  int i;

  /* Skip first element and do per-segment compares for rest */
  for (i = 0; i < 3; i++)
    {
      ref1 = skip_segment (ref1);
      ref2 = skip_segment (ref2);

      res = compare_segment (ref1, ref2);
      if (res != 0)
        return res;
    }
  return 0;
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
flatpak_filter_glob_to_regexp (const char *glob, GError **error)
{
  g_autoptr(GString) regexp = g_string_new ("");
  int parts = 1;
  gboolean empty_part;

  if (g_str_has_prefix (glob, "app/"))
    {
      glob += strlen ("app/");
      g_string_append (regexp, "app/");
    }
  else if (g_str_has_prefix (glob, "runtime/"))
    {
      glob += strlen ("runtime/");
      g_string_append (regexp, "runtime/");
    }
  else
    g_string_append (regexp, "(app|runtime)/");

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

          ref_regexp = flatpak_filter_glob_to_regexp (glob, error);
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
flatpak_decompose_ref (const char *full_ref,
                       GError    **error)
{
  g_auto(GStrv) parts = NULL;
  g_autoptr(GError) local_error = NULL;

  parts = g_strsplit (full_ref, "/", 0);
  if (g_strv_length (parts) != 4)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Wrong number of components in %s"), full_ref);
      return NULL;
    }

  if (strcmp (parts[0], "app") != 0 && strcmp (parts[0], "runtime") != 0)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("%s is not application or runtime"), full_ref);
      return NULL;
    }

  if (!flatpak_is_valid_name (parts[1], &local_error))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Invalid name %s: %s"), parts[1], local_error->message);
      return NULL;
    }

  if (strlen (parts[2]) == 0)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Invalid arch %s"), parts[2]);
      return NULL;
    }

  if (!flatpak_is_valid_branch (parts[3], &local_error))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Invalid branch %s: %s"), parts[3], local_error->message);
      return NULL;
    }

  return g_steal_pointer (&parts);
}

static const char *
next_element (const char **partial_ref)
{
  const char *slash;
  const char *end;

  slash = (const char *) strchr (*partial_ref, '/');
  if (slash != NULL)
    {
      end = slash;
      *partial_ref = slash + 1;
    }
  else
    {
      end = *partial_ref + strlen (*partial_ref);
      *partial_ref = end;
    }

  return end;
}

FlatpakKinds
flatpak_kinds_from_bools (gboolean app, gboolean runtime)
{
  FlatpakKinds kinds = 0;

  if (app)
    kinds |= FLATPAK_KINDS_APP;

  if (runtime)
    kinds |= FLATPAK_KINDS_RUNTIME;

  if (kinds == 0)
    kinds = FLATPAK_KINDS_APP | FLATPAK_KINDS_RUNTIME;

  return kinds;
}

static gboolean
_flatpak_split_partial_ref_arg (const char   *partial_ref,
                                gboolean      validate,
                                FlatpakKinds  default_kinds,
                                const char   *default_arch,
                                const char   *default_branch,
                                FlatpakKinds *out_kinds,
                                char        **out_id,
                                char        **out_arch,
                                char        **out_branch,
                                GError      **error)
{
  const char *id_start = NULL;
  const char *id_end = NULL;
  g_autofree char *id = NULL;
  const char *arch_start = NULL;
  const char *arch_end = NULL;
  g_autofree char *arch = NULL;
  const char *branch_start = NULL;
  const char *branch_end = NULL;
  g_autofree char *branch = NULL;
  g_autoptr(GError) local_error = NULL;
  FlatpakKinds kinds = 0;

  if (g_str_has_prefix (partial_ref, "app/"))
    {
      partial_ref += strlen ("app/");
      kinds = FLATPAK_KINDS_APP;
    }
  else if (g_str_has_prefix (partial_ref, "runtime/"))
    {
      partial_ref += strlen ("runtime/");
      kinds = FLATPAK_KINDS_RUNTIME;
    }
  else
    kinds = default_kinds;

  id_start = partial_ref;
  id_end = next_element (&partial_ref);
  id = g_strndup (id_start, id_end - id_start);

  if (validate && !flatpak_is_valid_name (id, &local_error))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Invalid id %s: %s"), id, local_error->message);

  arch_start = partial_ref;
  arch_end = next_element (&partial_ref);
  if (arch_end != arch_start)
    arch = g_strndup (arch_start, arch_end - arch_start);
  else
    arch = g_strdup (default_arch);

  branch_start = partial_ref;
  branch_end = next_element (&partial_ref);
  if (branch_end != branch_start)
    branch = g_strndup (branch_start, branch_end - branch_start);
  else
    branch = g_strdup (default_branch);

  if (validate && branch != NULL && !flatpak_is_valid_branch (branch, &local_error))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Invalid branch %s: %s"), branch, local_error->message);

  if (out_kinds)
    *out_kinds = kinds;
  if (out_id != NULL)
    *out_id = g_steal_pointer (&id);
  if (out_arch != NULL)
    *out_arch = g_steal_pointer (&arch);
  if (out_branch != NULL)
    *out_branch = g_steal_pointer (&branch);

  return TRUE;
}

gboolean
flatpak_split_partial_ref_arg (const char   *partial_ref,
                               FlatpakKinds  default_kinds,
                               const char   *default_arch,
                               const char   *default_branch,
                               FlatpakKinds *out_kinds,
                               char        **out_id,
                               char        **out_arch,
                               char        **out_branch,
                               GError      **error)
{
  return _flatpak_split_partial_ref_arg (partial_ref,
                                         TRUE,
                                         default_kinds,
                                         default_arch,
                                         default_branch,
                                         out_kinds,
                                         out_id,
                                         out_arch,
                                         out_branch,
                                         error);
}

gboolean
flatpak_split_partial_ref_arg_novalidate (const char   *partial_ref,
                                          FlatpakKinds  default_kinds,
                                          const char   *default_arch,
                                          const char   *default_branch,
                                          FlatpakKinds *out_kinds,
                                          char        **out_id,
                                          char        **out_arch,
                                          char        **out_branch)
{
  return _flatpak_split_partial_ref_arg (partial_ref,
                                         FALSE,
                                         default_kinds,
                                         default_arch,
                                         default_branch,
                                         out_kinds,
                                         out_id,
                                         out_arch,
                                         out_branch,
                                         NULL);
}


char *
flatpak_compose_ref (gboolean    app,
                     const char *name,
                     const char *branch,
                     const char *arch,
                     GError    **error)
{
  g_autoptr(GError) local_error = NULL;

  if (!flatpak_is_valid_name (name, &local_error))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("'%s' is not a valid name: %s"), name, local_error->message);
      return NULL;
    }

  if (branch && !flatpak_is_valid_branch (branch, &local_error))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("'%s' is not a valid branch name: %s"), branch, local_error->message);
      return NULL;
    }

  if (app)
    return flatpak_build_app_ref (name, branch, arch);
  else
    return flatpak_build_runtime_ref (name, branch, arch);
}

char *
flatpak_build_untyped_ref (const char *runtime,
                           const char *branch,
                           const char *arch)
{
  if (arch == NULL)
    arch = flatpak_get_arch ();

  return g_build_filename (runtime, arch, branch, NULL);
}

char *
flatpak_build_runtime_ref (const char *runtime,
                           const char *branch,
                           const char *arch)
{
  if (branch == NULL)
    branch = "master";

  if (arch == NULL)
    arch = flatpak_get_arch ();

  return g_build_filename ("runtime", runtime, arch, branch, NULL);
}

char *
flatpak_build_app_ref (const char *app,
                       const char *branch,
                       const char *arch)
{
  if (branch == NULL)
    branch = "master";

  if (arch == NULL)
    arch = flatpak_get_arch ();

  return g_build_filename ("app", app, arch, branch, NULL);
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
  const char *key;
  GHashTableIter iter;
  int i;

  hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

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
  g_hash_table_iter_init (&iter, hash);
  while (g_hash_table_iter_next (&iter, (gpointer *) &key, NULL))
    g_ptr_array_add (names, g_strdup (key));

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
flatpak_find_deploy_dir_for_ref (const char   *ref,
                                 FlatpakDir  **dir_out,
                                 GCancellable *cancellable,
                                 GError      **error)
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
      flatpak_fail_error (error, FLATPAK_ERROR_NOT_INSTALLED, _("%s not installed"), ref);
      return NULL;
    }

  if (dir_out)
    *dir_out = g_object_ref (dir);
  return g_steal_pointer (&deploy);
}

GFile *
flatpak_find_files_dir_for_ref (const char   *ref,
                                GCancellable *cancellable,
                                GError      **error)
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

char *
flatpak_find_current_ref (const char   *app_id,
                          GCancellable *cancellable,
                          GError      **error)
{
  g_autofree char *current_ref = NULL;
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
                                const char   *ref,
                                const char   *commit,
                                GCancellable *cancellable,
                                GError      **error)
{
  FlatpakDeploy *deploy = NULL;
  int i;
  g_autoptr(GError) my_error = NULL;

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
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(GPtrArray) dirs = NULL;

  dirs = flatpak_dir_get_system_list (cancellable, error);
  if (dirs == NULL)
    return NULL;

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
 * existing symlink target, if any. This is atomic in the sense that
 * we're guaranteed to remove any existing symlink target (once),
 * independent of how many processes do the same operation in
 * parallele. However, it is still possible that we remove the old and
 * then fail to create the new symlink for some reason, ending up with
 * neither the old or the new target. That is fine if the reason for
 * the symlink is keeping a cache though.
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
          g_autofree char *old_target = flatpak_resolve_link (tmp_path, error);
          if (old_target == NULL)
            return FALSE;
          unlink (old_target);
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

static gboolean
needs_quoting (const char *arg)
{
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

      if (needs_quoting (argv[i]))
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
      tmpf->fd = glnx_steal_fd (&memfd);
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

gboolean
flatpak_variant_bsearch_str (GVariant   *array,
                             const char *str,
                             int        *out_pos)
{
  gsize imax, imin;
  gsize imid;
  gsize n;

  g_return_val_if_fail (out_pos != NULL, FALSE);

  n = g_variant_n_children (array);
  if (n == 0)
    return FALSE;

  imax = n - 1;
  imin = 0;
  while (imax >= imin)
    {
      g_autoptr(GVariant) child = NULL;
      g_autoptr(GVariant) cur_v = NULL;
      const char *cur;
      int cmp;

      imid = (imin + imax) / 2;

      child = g_variant_get_child_value (array, imid);
      cur_v = g_variant_get_child_value (child, 0);
      cur = g_variant_get_data (cur_v);

      cmp = strcmp (cur, str);
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
          *out_pos = imid;
          return TRUE;
        }
    }

  *out_pos = imid;
  return FALSE;
}

/* Find the list of refs which belong to the given @collection_id in @summary.
 * If @collection_id is %NULL, the main refs list from the summary will be
 * returned. If @collection_id doesn’t match any collection IDs in the summary
 * file, %NULL will be returned. */
static GVariant *
summary_find_refs_list (GVariant   *summary,
                        const char *collection_id)
{
  g_autoptr(GVariant) refs = NULL;
  g_autoptr(GVariant) metadata = g_variant_get_child_value (summary, 1);
  const char *summary_collection_id;

  if (!g_variant_lookup (metadata, "ostree.summary.collection-id", "&s", &summary_collection_id))
    summary_collection_id = NULL;

  if (collection_id == NULL || g_strcmp0 (collection_id, summary_collection_id) == 0)
    {
      refs = g_variant_get_child_value (summary, 0);
    }
  else if (collection_id != NULL)
    {
      g_autoptr(GVariant) collection_map = NULL;

      collection_map = g_variant_lookup_value (metadata, "ostree.summary.collection-map",
                                               G_VARIANT_TYPE ("a{sa(s(taya{sv}))}"));
      if (collection_map != NULL)
        refs = g_variant_lookup_value (collection_map, collection_id, G_VARIANT_TYPE ("a(s(taya{sv}))"));
    }

  return g_steal_pointer (&refs);
}

/* This matches all refs from @collection_id that have ref, followed by '.'  as prefix */
char **
flatpak_summary_match_subrefs (GVariant   *summary,
                               const char *collection_id,
                               const char *ref)
{
  g_autoptr(GVariant) refs = NULL;
  GPtrArray *res = g_ptr_array_new ();
  gsize n, i;
  g_auto(GStrv) parts = NULL;
  g_autofree char *parts_prefix = NULL;
  g_autofree char *ref_prefix = NULL;
  g_autofree char *ref_suffix = NULL;

  /* Work out which refs list to use, based on the @collection_id. */
  refs = summary_find_refs_list (summary, collection_id);

  if (refs != NULL)
    {
      /* Match against the refs. */
      parts = g_strsplit (ref, "/", 0);
      parts_prefix = g_strconcat (parts[1], ".", NULL);

      ref_prefix = g_strconcat (parts[0], "/", NULL);
      ref_suffix = g_strconcat ("/", parts[2], "/", parts[3], NULL);

      n = g_variant_n_children (refs);
      for (i = 0; i < n; i++)
        {
          g_autoptr(GVariant) child = NULL;
          g_autoptr(GVariant) cur_v = NULL;
          const char *cur;
          const char *id_start;

          child = g_variant_get_child_value (refs, i);
          cur_v = g_variant_get_child_value (child, 0);
          cur = g_variant_get_data (cur_v);

          /* Must match type */
          if (!g_str_has_prefix (cur, ref_prefix))
            continue;

          /* Must match arch & branch */
          if (!g_str_has_suffix (cur, ref_suffix))
            continue;

          id_start = strchr (cur, '/');
          if (id_start == NULL)
            continue;

          /* But only prefix of id */
          if (!g_str_has_prefix (id_start + 1, parts_prefix))
            continue;

          g_ptr_array_add (res, g_strdup (cur));
        }
    }

  g_ptr_array_add (res, NULL);
  return (char **) g_ptr_array_free (res, FALSE);
}

gboolean
flatpak_summary_lookup_ref (GVariant   *summary,
                            const char *collection_id,
                            const char *ref,
                            char      **out_checksum,
                            GVariant  **out_variant)
{
  g_autoptr(GVariant) refs = NULL;
  int pos;
  g_autoptr(GVariant) refdata = NULL;
  g_autoptr(GVariant) reftargetdata = NULL;
  guint64 commit_size;
  g_autoptr(GVariant) commit_csum_v = NULL;

  refs = summary_find_refs_list (summary, collection_id);
  if (refs == NULL)
    return FALSE;

  if (!flatpak_variant_bsearch_str (refs, ref, &pos))
    return FALSE;

  refdata = g_variant_get_child_value (refs, pos);
  reftargetdata = g_variant_get_child_value (refdata, 1);
  g_variant_get (reftargetdata, "(t@ay@a{sv})", &commit_size, &commit_csum_v, NULL);

  if (!ostree_validate_structureof_csum_v (commit_csum_v, NULL))
    return FALSE;

  if (out_checksum)
    *out_checksum = ostree_checksum_from_bytes_v (commit_csum_v);

  if (out_variant)
    *out_variant = g_steal_pointer (&reftargetdata);

  return TRUE;
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

  title = g_key_file_get_locale_string (keyfile, source_group,
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

  collection_id = g_key_file_get_string (keyfile, source_group,
                                         FLATPAK_REPO_DEPLOY_COLLECTION_ID_KEY, NULL);
  if (collection_id == NULL || *collection_id == '\0')
    collection_id = g_key_file_get_string (keyfile, source_group,
                                           FLATPAK_REPO_COLLECTION_ID_KEY, NULL);
  if (collection_id != NULL)
    {
      if (gpg_key == NULL)
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Collection ID requires GPG key to be provided"));
          return NULL;
        }

      g_key_file_set_string (config, group, "collection-id", collection_id);
    }

  /* If a collection ID is set, refs are verified from commit metadata rather
   * than the summary file. */
  g_key_file_set_boolean (config, group, "gpg-verify-summary",
                          (gpg_key != NULL && collection_id == NULL));

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

/* Loads a summary file from a local repo */
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

typedef struct
{
  guint64   installed_size;
  guint64   download_size;
  char     *metadata_contents;
  GVariant *sparse_data;
} CommitData;

static void
commit_data_free (gpointer data)
{
  CommitData *rev_data = data;

  g_free (rev_data->metadata_contents);
  if (rev_data->sparse_data)
    g_variant_unref (rev_data->sparse_data);
  g_free (rev_data);
}

/* For all the refs listed in @cache_v (an xa.cache value) which exist in the
 * @summary, insert their data into @commit_data_cache if it isn’t already there. */
static void
populate_commit_data_cache (GVariant   *metadata,
                            GVariant   *summary,
                            GHashTable *commit_data_cache /* (element-type utf8 CommitData) */)
{
  g_autoptr(GVariant) cache_v = NULL;
  g_autoptr(GVariant) cache = NULL;
  g_autoptr(GVariant) sparse_cache = NULL;
  gsize n, i;
  const char *old_collection_id;

  if (!g_variant_lookup (metadata, "ostree.summary.collection-id", "&s", &old_collection_id))
    old_collection_id = NULL;

  cache_v = g_variant_lookup_value (metadata, "xa.cache", NULL);

  if (cache_v == NULL)
    return;

  cache = g_variant_get_child_value (cache_v, 0);

  sparse_cache = g_variant_lookup_value (metadata, "xa.sparse-cache", NULL);

  n = g_variant_n_children (cache);
  for (i = 0; i < n; i++)
    {
      g_autoptr(GVariant) old_element = g_variant_get_child_value (cache, i);
      g_autoptr(GVariant) old_ref_v = g_variant_get_child_value (old_element, 0);
      const char *old_ref = g_variant_get_string (old_ref_v, NULL);
      g_autofree char *old_rev = NULL;
      g_autoptr(GVariant) old_commit_data_v = g_variant_get_child_value (old_element, 1);
      CommitData *old_rev_data;

      if (flatpak_summary_lookup_ref (summary, old_collection_id, old_ref, &old_rev, NULL))
        {
          guint64 old_installed_size, old_download_size;
          g_autofree char *old_metadata = NULL;

          /* See if we already have the info on this revision */
          if (g_hash_table_lookup (commit_data_cache, old_rev))
            continue;

          g_variant_get_child (old_commit_data_v, 0, "t", &old_installed_size);
          old_installed_size = GUINT64_FROM_BE (old_installed_size);
          g_variant_get_child (old_commit_data_v, 1, "t", &old_download_size);
          old_download_size = GUINT64_FROM_BE (old_download_size);
          g_variant_get_child (old_commit_data_v, 2, "s", &old_metadata);

          old_rev_data = g_new0 (CommitData, 1);
          old_rev_data->installed_size = old_installed_size;
          old_rev_data->download_size = old_download_size;
          old_rev_data->metadata_contents = g_steal_pointer (&old_metadata);

          if (sparse_cache)
            old_rev_data->sparse_data = g_variant_lookup_value (sparse_cache, old_ref, G_VARIANT_TYPE_VARDICT);

          g_hash_table_insert (commit_data_cache, g_steal_pointer (&old_rev), old_rev_data);
        }
    }
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
                     const char  **gpg_key_ids,
                     const char   *gpg_homedir,
                     GCancellable *cancellable,
                     GError      **error)
{
  g_auto(GVariantBuilder) builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_auto(GVariantBuilder) commits_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("aay"));
  g_auto(GVariantBuilder) ref_data_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a{s(tts)}"));
  g_auto(GVariantBuilder) ref_sparse_data_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a{sa{sv}}"));
  GKeyFile *config;
  g_autofree char *title = NULL;
  g_autofree char *comment = NULL;
  g_autofree char *description = NULL;
  g_autofree char *homepage = NULL;
  g_autofree char *icon = NULL;
  g_autofree char *redirect_url = NULL;
  g_autofree char *default_branch = NULL;
  g_autofree char *authenticator_name = NULL;
  g_autofree char *gpg_keys = NULL;
  g_auto(GStrv) config_keys = NULL;
  int authenticator_install = -1;
  g_autoptr(GVariant) old_summary = NULL;
  g_autoptr(GVariant) new_summary = NULL;
  g_autoptr(GHashTable) refs = NULL;
  const char *prefixes[] = { "appstream", "appstream2", "app", "runtime", NULL };
  const char **prefix;
  g_autoptr(GList) ordered_keys = NULL;
  GList *l = NULL;
  g_autoptr(GHashTable) commit_data_cache = NULL;
  const char *collection_id;
  g_autofree char *old_ostree_metadata_checksum = NULL;
  g_autoptr(GVariant) old_ostree_metadata_v = NULL;
  gboolean deploy_collection_id = FALSE;

  config = ostree_repo_get_config (repo);

  if (config)
    {
      title = g_key_file_get_string (config, "flatpak", "title", NULL);
      comment = g_key_file_get_string (config, "flatpak", "comment", NULL);
      description = g_key_file_get_string (config, "flatpak", "description", NULL);
      homepage = g_key_file_get_string (config, "flatpak", "homepage", NULL);
      icon = g_key_file_get_string (config, "flatpak", "icon", NULL);
      default_branch = g_key_file_get_string (config, "flatpak", "default-branch", NULL);
      gpg_keys = g_key_file_get_string (config, "flatpak", "gpg-keys", NULL);
      redirect_url = g_key_file_get_string (config, "flatpak", "redirect-url", NULL);
      deploy_collection_id = g_key_file_get_boolean (config, "flatpak", "deploy-collection-id", NULL);
      authenticator_name = g_key_file_get_string (config, "flatpak", "authenticator-name", NULL);
      if (g_key_file_has_key (config, "flatpak", "authenticator-install", NULL))
        authenticator_install = g_key_file_get_boolean (config, "flatpak", "authenticator-install", NULL);

      config_keys = g_key_file_get_keys (config, "flatpak", NULL, NULL);
    }

  collection_id = ostree_repo_get_collection_id (repo);

  if (title)
    g_variant_builder_add (&builder, "{sv}", "xa.title",
                           g_variant_new_string (title));

  if (comment)
    g_variant_builder_add (&builder, "{sv}", "xa.comment",
                           g_variant_new_string (comment));

  if (description)
    g_variant_builder_add (&builder, "{sv}", "xa.description",
                           g_variant_new_string (description));

  if (homepage)
    g_variant_builder_add (&builder, "{sv}", "xa.homepage",
                           g_variant_new_string (homepage));

  if (icon)
    g_variant_builder_add (&builder, "{sv}", "xa.icon",
                           g_variant_new_string (icon));

  if (redirect_url)
    g_variant_builder_add (&builder, "{sv}", "xa.redirect-url",
                           g_variant_new_string (redirect_url));

  if (default_branch)
    g_variant_builder_add (&builder, "{sv}", "xa.default-branch",
                           g_variant_new_string (default_branch));

  if (deploy_collection_id && collection_id != NULL)
    g_variant_builder_add (&builder, "{sv}", OSTREE_META_KEY_DEPLOY_COLLECTION_ID,
                           g_variant_new_string (collection_id));
  else if (deploy_collection_id)
    g_debug ("Ignoring deploy-collection-id=true because no collection ID is set.");

  if (authenticator_name)
    g_variant_builder_add (&builder, "{sv}", "xa.authenticator-name",
                           g_variant_new_string (authenticator_name));

  if (authenticator_install != -1)
    g_variant_builder_add (&builder, "{sv}", "xa.authenticator-install",
                           g_variant_new_boolean (authenticator_install));

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
          g_variant_builder_add (&builder, "{sv}", xa_key,
                                 g_variant_new_string (value));
        }
    }

  if (gpg_keys)
    {
      guchar *decoded;
      gsize decoded_len;

      gpg_keys = g_strstrip (gpg_keys);
      decoded = g_base64_decode (gpg_keys, &decoded_len);

      g_variant_builder_add (&builder, "{sv}", "xa.gpg-keys",
                             g_variant_new_from_data (G_VARIANT_TYPE ("ay"), decoded, decoded_len,
                                                      TRUE, (GDestroyNotify) g_free, decoded));
    }

  /* Only operate on flatpak relevant refs */
  refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  for (prefix = prefixes; *prefix != NULL; prefix++)
    {
      g_autoptr(GHashTable) prefix_refs = NULL;
      GHashTableIter hashiter;
      gpointer key, value;

      if (!ostree_repo_list_refs_ext (repo, *prefix, &prefix_refs,
                                      OSTREE_REPO_LIST_REFS_EXT_NONE,
                                      cancellable, error))
        return FALSE;

      /* Merge the prefix refs to the full refs table */
      g_hash_table_iter_init (&hashiter, prefix_refs);
      while (g_hash_table_iter_next (&hashiter, &key, &value))
        {
          char *ref = g_strdup (key);
          char *rev = g_strdup (value);
          g_hash_table_replace (refs, ref, rev);
        }
    }

  commit_data_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, commit_data_free);

  old_summary = flatpak_repo_load_summary (repo, NULL);

  if (!flatpak_repo_resolve_rev (repo, collection_id, NULL, OSTREE_REPO_METADATA_REF,
                                 TRUE, &old_ostree_metadata_checksum, cancellable, error))
    return FALSE;

  if (old_summary != NULL &&
      old_ostree_metadata_checksum != NULL &&
      ostree_repo_load_commit (repo, old_ostree_metadata_checksum, &old_ostree_metadata_v, NULL, NULL))
    {
      g_autoptr(GVariant) metadata = g_variant_get_child_value (old_ostree_metadata_v, 0);

      populate_commit_data_cache (metadata, old_summary, commit_data_cache);
    }
  else if (old_summary != NULL)
    {
      g_autoptr(GVariant) extensions = g_variant_get_child_value (old_summary, 1);

      populate_commit_data_cache (extensions, old_summary, commit_data_cache);
    }

  ordered_keys = g_hash_table_get_keys (refs);
  ordered_keys = g_list_sort (ordered_keys, (GCompareFunc) strcmp);
  for (l = ordered_keys; l; l = l->next)
    {
      const char *ref = l->data;
      const char *rev = g_hash_table_lookup (refs, ref);
      g_autoptr(GFile) root = NULL;
      g_autoptr(GFile) metadata = NULL;
      guint64 installed_size = 0;
      guint64 download_size = 0;
      g_autofree char *metadata_contents = NULL;
      g_autofree char *commit = NULL;
      g_autoptr(GVariant) commit_v = NULL;
      g_autoptr(GVariant) commit_metadata = NULL;
      CommitData *rev_data;
      const char *eol = NULL;
      const char *eol_rebase = NULL;
      int token_type = -1;

      /* See if we already have the info on this revision */
      if (g_hash_table_lookup (commit_data_cache, rev))
        continue;

      if (!ostree_repo_read_commit (repo, rev, &root, &commit, NULL, error))
        return FALSE;

      if (!ostree_repo_load_commit (repo, commit, &commit_v, NULL, error))
        return FALSE;

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
            return FALSE;
        }

      flatpak_repo_collect_extra_data_sizes (repo, rev, &installed_size, &download_size);

      rev_data = g_new0 (CommitData, 1);
      rev_data->installed_size = installed_size;
      rev_data->download_size = download_size;
      rev_data->metadata_contents = g_steal_pointer (&metadata_contents);

      g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE, "&s", &eol);
      g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE_REBASE, "&s", &eol_rebase);
      g_variant_lookup (commit_metadata, "xa.token-type", "i", &token_type);
      if (eol || eol_rebase || token_type >= 0)
        {
          g_auto(GVariantBuilder) sparse_builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;
          g_variant_builder_init (&sparse_builder, G_VARIANT_TYPE_VARDICT);
          if (eol)
            g_variant_builder_add (&sparse_builder, "{sv}", FLATPAK_SPARSE_CACHE_KEY_ENDOFLINE, g_variant_new_string (eol));
          if (eol_rebase)
            g_variant_builder_add (&sparse_builder, "{sv}", FLATPAK_SPARSE_CACHE_KEY_ENDOFLINE_REBASE, g_variant_new_string (eol_rebase));
          if (token_type >= 0)
            g_variant_builder_add (&sparse_builder, "{sv}", FLATPAK_SPARSE_CACHE_KEY_TOKEN_TYPE, g_variant_new_int32 (token_type));

          rev_data->sparse_data = g_variant_ref_sink (g_variant_builder_end (&sparse_builder));
        }

      g_hash_table_insert (commit_data_cache, g_strdup (rev), rev_data);
    }

  for (l = ordered_keys; l; l = l->next)
    {
      const char *ref = l->data;
      const char *rev = g_hash_table_lookup (refs, ref);
      const CommitData *rev_data = g_hash_table_lookup (commit_data_cache,
                                                        rev);

      g_variant_builder_add (&ref_data_builder, "{s(tts)}",
                             ref,
                             GUINT64_TO_BE (rev_data->installed_size),
                             GUINT64_TO_BE (rev_data->download_size),
                             rev_data->metadata_contents);
      if (rev_data->sparse_data)
        g_variant_builder_add (&ref_sparse_data_builder, "{s@a{sv}}",
                               ref, rev_data->sparse_data);
      g_variant_builder_add (&commits_builder, "@ay", ostree_checksum_to_bytes_v (rev));
    }

  /* Note: xa.cache doesn’t need to support collection IDs for the refs listed
   * in it, because the xa.cache metadata is stored on the ostree-metadata ref,
   * which is itself strongly bound to a collection ID — so that collection ID
   * is bound to all the refs in xa.cache. If a client is using the xa.cache
   * data from a summary file (rather than an ostree-metadata branch), they are
   * too old to care about collection IDs anyway. */
  g_variant_builder_add (&builder, "{sv}", "xa.cache",
                         g_variant_new_variant (g_variant_builder_end (&ref_data_builder)));

  g_variant_builder_add (&builder, "{sv}", "xa.sparse-cache",
                         g_variant_builder_end (&ref_sparse_data_builder));

  new_summary = g_variant_ref_sink (g_variant_builder_end (&builder));

  /* Write out a new metadata commit for the repository. */
  if (collection_id != NULL)
    {
      OstreeCollectionRef collection_ref = { (gchar *) collection_id, (gchar *) OSTREE_REPO_METADATA_REF };
      g_autofree gchar *new_ostree_metadata_checksum = NULL;
      g_autoptr(OstreeMutableTree) mtree = NULL;
      g_autoptr(OstreeRepoFile) repo_file = NULL;
      g_autoptr(GVariantDict) new_summary_commit_dict = NULL;
      g_autoptr(GVariant) new_summary_commit = NULL;

      /* Add bindings to the metadata. */
      new_summary_commit_dict = g_variant_dict_new (new_summary);
      g_variant_dict_insert_value (new_summary_commit_dict, "xa.commits",
                                   g_variant_builder_end (&commits_builder));
      g_variant_dict_insert (new_summary_commit_dict, "ostree.collection-binding",
                             "s", collection_ref.collection_id);
      g_variant_dict_insert_value (new_summary_commit_dict, "ostree.ref-binding",
                                   g_variant_new_strv ((const gchar * const *) &collection_ref.ref_name, 1));
      new_summary_commit = g_variant_ref_sink (g_variant_dict_end (new_summary_commit_dict));

      if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
        goto out;

      /* Set up an empty mtree. */
      mtree = ostree_mutable_tree_new ();
      if (!flatpak_mtree_ensure_dir_metadata (repo, mtree, cancellable, error))
        goto out;
      if (!ostree_repo_write_mtree (repo, mtree, (GFile **) &repo_file, NULL, error))
        goto out;

      if (!ostree_repo_write_commit (repo, old_ostree_metadata_checksum,
                                     NULL /* subject */, NULL /* body */,
                                     new_summary_commit, repo_file, &new_ostree_metadata_checksum,
                                     NULL, error))
        goto out;

      if (gpg_key_ids != NULL)
        {
          const char * const *iter;

          for (iter = gpg_key_ids; iter != NULL && *iter != NULL; iter++)
            {
              const char *key_id = *iter;

              if (!ostree_repo_sign_commit (repo,
                                            new_ostree_metadata_checksum,
                                            key_id,
                                            gpg_homedir,
                                            cancellable,
                                            error))
                goto out;
            }
        }

      ostree_repo_transaction_set_collection_ref (repo, &collection_ref,
                                                  new_ostree_metadata_checksum);

      if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
        goto out;
    }

  /* Regenerate and re-sign the summary file. */
  if (!ostree_repo_regenerate_summary (repo, new_summary, cancellable, error))
    return FALSE;

  if (gpg_key_ids)
    {
      if (!ostree_repo_add_gpg_signature_summary (repo,
                                                  gpg_key_ids,
                                                  gpg_homedir,
                                                  cancellable,
                                                  error))
        return FALSE;
    }

  return TRUE;

out:
  if (repo != NULL)
    ostree_repo_abort_transaction (repo, cancellable, NULL);
  return FALSE;
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
validate_component (FlatpakXml *component,
                    const char *ref,
                    const char *id,
                    char      **tags,
                    const char *runtime,
                    const char *sdk)
{
  FlatpakXml *bundle, *text, *prev, *id_node, *id_text_node, *metadata, *value;
  g_autofree char *id_text = NULL;
  int i;

  if (g_strcmp0 (component->element_name, "component") != 0)
    return FALSE;

  id_node = flatpak_xml_find (component, "id", NULL);
  if (id_node == NULL)
    return FALSE;

  id_text_node = flatpak_xml_find (id_node, NULL, NULL);
  if (id_text_node == NULL || id_text_node->text == NULL)
    return FALSE;

  id_text = g_strstrip (g_strdup (id_text_node->text));

  /* Drop .desktop file suffix (unless the actual app id ends with .desktop) */
  if (g_str_has_suffix (id_text, ".desktop") &&
      !g_str_has_suffix (id, ".desktop"))
    id_text[strlen (id_text) - strlen (".desktop")] = 0;

  if (!g_str_has_prefix (id_text, id))
    {
      g_warning ("Invalid id %s", id_text);
      return FALSE;
    }

  while ((bundle = flatpak_xml_find (component, "bundle", &prev)) != NULL)
    flatpak_xml_free (flatpak_xml_unlink (component, bundle));

  bundle = flatpak_xml_new ("bundle");
  bundle->attribute_names = g_new0 (char *, 2 * 4);
  bundle->attribute_values = g_new0 (char *, 2 * 4);
  bundle->attribute_names[0] = g_strdup ("type");
  bundle->attribute_values[0] = g_strdup ("flatpak");

  i = 1;
  if (runtime && !g_str_has_prefix (runtime, "runtime/"))
    {
      bundle->attribute_names[i] = g_strdup ("runtime");
      bundle->attribute_values[i] = g_strdup (runtime);
      i++;
    }

  if (sdk)
    {
      bundle->attribute_names[i] = g_strdup ("sdk");
      bundle->attribute_values[i] = g_strdup (sdk);
      i++;
    }

  text = flatpak_xml_new (NULL);
  text->text = g_strdup (ref);
  flatpak_xml_add (bundle, text);

  flatpak_xml_add (component, flatpak_xml_new_text ("  "));
  flatpak_xml_add (component, bundle);
  flatpak_xml_add (component, flatpak_xml_new_text ("\n  "));

  if (tags != NULL && tags[0] != NULL)
    {
      metadata = flatpak_xml_find (component, "metadata", NULL);
      if (metadata == NULL)
        {
          metadata = flatpak_xml_new ("metadata");
          metadata->attribute_names = g_new0 (char *, 1);
          metadata->attribute_values = g_new0 (char *, 1);

          flatpak_xml_add (component, flatpak_xml_new_text ("  "));
          flatpak_xml_add (component, metadata);
          flatpak_xml_add (component, flatpak_xml_new_text ("\n  "));
        }

      value = flatpak_xml_new ("value");
      value->attribute_names = g_new0 (char *, 2);
      value->attribute_values = g_new0 (char *, 2);
      value->attribute_names[0] = g_strdup ("key");
      value->attribute_values[0] = g_strdup ("X-Flatpak-Tags");
      flatpak_xml_add (metadata, flatpak_xml_new_text ("\n       "));
      flatpak_xml_add (metadata, value);
      flatpak_xml_add (metadata, flatpak_xml_new_text ("\n    "));

      text = flatpak_xml_new (NULL);
      text->text = g_strjoinv (",", tags);
      flatpak_xml_add (value, text);
    }

  return TRUE;
}

gboolean
flatpak_appstream_xml_migrate (FlatpakXml *source,
                               FlatpakXml *dest,
                               const char *ref,
                               const char *id,
                               GKeyFile   *metadata)
{
  FlatpakXml *source_components;
  FlatpakXml *dest_components;
  FlatpakXml *component;
  FlatpakXml *prev_component;
  gboolean migrated = FALSE;
  g_auto(GStrv) tags = NULL;
  g_autofree const char *runtime = NULL;
  g_autofree const char *sdk = NULL;
  const char *group;

  if (source->first_child == NULL ||
      source->first_child->next_sibling != NULL ||
      g_strcmp0 (source->first_child->element_name, "components") != 0)
    return FALSE;

  if (g_str_has_prefix (ref, "app/"))
    group = FLATPAK_METADATA_GROUP_APPLICATION;
  else
    group = FLATPAK_METADATA_GROUP_RUNTIME;

  tags = g_key_file_get_string_list (metadata, group, FLATPAK_METADATA_KEY_TAGS,
                                     NULL, NULL);
  runtime = g_key_file_get_string (metadata, group,
                                   FLATPAK_METADATA_KEY_RUNTIME, NULL);
  sdk = g_key_file_get_string (metadata, group, FLATPAK_METADATA_KEY_SDK, NULL);

  source_components = source->first_child;
  dest_components = dest->first_child;

  component = source_components->first_child;
  prev_component = NULL;
  while (component != NULL)
    {
      FlatpakXml *next = component->next_sibling;

      if (validate_component (component, ref, id, tags, runtime, sdk))
        {
          flatpak_xml_add (dest_components,
                           flatpak_xml_unlink (component, prev_component));
          migrated = TRUE;
        }
      else
        {
          prev_component = component;
        }

      component = next;
    }

  return migrated;
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
      g_debug ("No icon at size %s for %s", size, id);
      return TRUE;
    }

  checksum = ostree_repo_file_get_checksum (OSTREE_REPO_FILE(icon_file));
  if (!ostree_mutable_tree_replace_file (size_mtree, icon_name, checksum, error))
    return FALSE;

  return TRUE;
}

static gboolean
extract_appstream (OstreeRepo   *repo,
                   FlatpakXml   *appstream_root,
                   const char   *ref,
                   const char   *id,
                   OstreeMutableTree *size1_mtree,
                   OstreeMutableTree *size2_mtree,
                   GCancellable *cancellable,
                   GError      **error)
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

  if (!ostree_repo_read_commit (repo, ref, &root, NULL, NULL, error))
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
                                     ref, id, keyfile))
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

FlatpakXml *
flatpak_appstream_xml_new (void)
{
  FlatpakXml *appstream_root = NULL;
  FlatpakXml *appstream_components;

  appstream_root = flatpak_xml_new ("root");
  appstream_components = flatpak_xml_new ("components");
  flatpak_xml_add (appstream_root, appstream_components);
  flatpak_xml_add (appstream_components, flatpak_xml_new_text ("\n  "));

  appstream_components->attribute_names = g_new0 (char *, 3);
  appstream_components->attribute_values = g_new0 (char *, 3);
  appstream_components->attribute_names[0] = g_strdup ("version");
  appstream_components->attribute_values[0] = g_strdup ("0.8");
  appstream_components->attribute_names[1] = g_strdup ("origin");
  appstream_components->attribute_values[1] = g_strdup ("flatpak");

  return appstream_root;
}

gboolean
flatpak_appstream_xml_root_to_data (FlatpakXml *appstream_root,
                                    GBytes    **uncompressed,
                                    GBytes    **compressed,
                                    GError    **error)
{
  g_autoptr(GString) xml = NULL;
  g_autoptr(GZlibCompressor) compressor = NULL;
  g_autoptr(GOutputStream) out2 = NULL;
  g_autoptr(GOutputStream) out = NULL;

  flatpak_xml_add (appstream_root->first_child, flatpak_xml_new_text ("\n"));

  xml = g_string_new ("");
  flatpak_xml_to_string (appstream_root, xml);

  if (compressed)
    {
      compressor = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1);
      out = g_memory_output_stream_new_resizable ();
      out2 = g_converter_output_stream_new (out, G_CONVERTER (compressor));
      if (!g_output_stream_write_all (out2, xml->str, xml->len,
                                      NULL, NULL, error))
        return FALSE;
      if (!g_output_stream_close (out2, NULL, error))
        return FALSE;
    }

  if (uncompressed)
    *uncompressed = g_string_free_to_bytes (g_steal_pointer (&xml));

  if (compressed)
    *compressed = g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out));

  return TRUE;
}

void
flatpak_appstream_xml_filter (FlatpakXml *appstream,
                              GRegex *allow_refs,
                              GRegex *deny_refs)
{
  FlatpakXml *components;
  FlatpakXml *component;
  FlatpakXml *prev_component, *old;

  for (components = appstream->first_child;
       components != NULL;
       components = components->next_sibling)
    {
      if (g_strcmp0 (components->element_name, "components") != 0)
        continue;


      prev_component = NULL;
      component = components->first_child;
      while (component != NULL)
        {
          FlatpakXml *bundle;
          gboolean allow = FALSE;

          if (g_strcmp0 (component->element_name, "component") == 0)
            {
              bundle = flatpak_xml_find (component, "bundle", NULL);
              if (bundle && bundle->first_child && bundle->first_child->text)
                allow = flatpak_filters_allow_ref (allow_refs, deny_refs, bundle->first_child->text);
            }

          if (allow)
            {
              prev_component = component;
              component = component->next_sibling;
            }
          else
            {
              old = component;

              /* prev_component is same as before */
              component = component->next_sibling;

              flatpak_xml_unlink (old, prev_component);
              flatpak_xml_free (old);
            }
        }
    }
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
  g_autofree const char **all_refs_keys = NULL;
  guint n_keys;
  gsize i;
  g_autoptr(GHashTable) arches = NULL;  /* (element-type utf8 utf8) */
  const char *collection_id;

  arches = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  collection_id = ostree_repo_get_collection_id (repo);

  if (!ostree_repo_list_refs (repo,
                              NULL,
                              &all_refs,
                              cancellable,
                              error))
    return FALSE;

  all_refs_keys = (const char **) g_hash_table_get_keys_as_array (all_refs, &n_keys);

  /* Sort refs so that appdata order is stable for e.g. deltas */
  g_qsort_with_data (all_refs_keys, n_keys, sizeof (char *), (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);

  for (i = 0; i < n_keys; i++)
    {
      const char *ref = all_refs_keys[i];
      g_auto(GStrv) split = NULL;
      const char *arch;

      split = flatpak_decompose_ref (ref, NULL);
      if (!split)
        continue;

      arch = split[2];
      if (!g_hash_table_contains (arches, arch))
        {
          const char *reverse_compat_arch;
          g_hash_table_add (arches, g_strdup (arch));

          /* If repo contains e.g. i386, also generated x86-64 appdata */
          reverse_compat_arch = flatpak_get_compat_arch_reverse (arch);
          if (reverse_compat_arch)
            g_hash_table_add (arches, g_strdup (reverse_compat_arch));
        }
    }

  GLNX_HASH_TABLE_FOREACH (arches, const char *, arch)
  {
    OstreeRepoTransactionStats stats;
    g_autoptr(FlatpakXml) appstream_root = NULL;
    g_autoptr(GBytes) xml_data = NULL;
    g_autoptr(GBytes) xml_gz_data = NULL;
    g_autoptr(OstreeMutableTree) mtree = ostree_mutable_tree_new ();
    g_autoptr(OstreeMutableTree) icons_mtree = NULL;
    g_autoptr(OstreeMutableTree) icons_flatpak_mtree = NULL;
    g_autoptr(OstreeMutableTree) size1_mtree = NULL;
    g_autoptr(OstreeMutableTree) size2_mtree = NULL;
    const char *compat_arch;
    g_autoptr(FlatpakRepoTransaction) transaction = NULL;
    compat_arch = flatpak_get_compat_arch (arch);
    const char *branch_names[] = { "appstream", "appstream2" };

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

    for (i = 0; i < n_keys; i++)
      {
        const char *ref = all_refs_keys[i];
        const char *commit;
        g_autoptr(GVariant) commit_v = NULL;
        g_autoptr(GVariant) commit_metadata = NULL;
        g_auto(GStrv) split = NULL;
        g_autoptr(GError) my_error = NULL;
        const char *eol = NULL;
        const char *eol_rebase = NULL;

        split = flatpak_decompose_ref (ref, NULL);
        if (!split)
          continue;

        if (strcmp (split[2], arch) != 0)
          {
            g_autofree char *main_ref = NULL;
            /* Include refs that don't match the main arch (e.g. x86_64), if they match
               the compat arch (e.g. i386) and the main arch version is not in the repo */
            if (g_strcmp0 (split[2], compat_arch) == 0)
              main_ref = g_strdup_printf ("%s/%s/%s/%s",
                                          split[0], split[1], arch, split[3]);
            if (main_ref == NULL ||
                g_hash_table_lookup (all_refs, main_ref))
              continue;
          }

        commit = g_hash_table_lookup (all_refs, ref);

        if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit,
                                       &commit_v, NULL))
          {
            g_warning ("Couldn't load commit %s (ref %s)", commit, ref);
            continue;
          }

        commit_metadata = g_variant_get_child_value (commit_v, 0);
        g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE, "&s", &eol);
        g_variant_lookup (commit_metadata, OSTREE_COMMIT_META_KEY_ENDOFLIFE_REBASE, "&s", &eol_rebase);
        if (eol || eol_rebase)
          {
            g_print (_("%s is end-of-life, ignoring\n"), ref);
            continue;
          }

        if (!extract_appstream (repo, appstream_root,
                                ref, split[1], size1_mtree, size2_mtree,
                                cancellable, &my_error))
          {
            if (g_str_has_prefix (ref, "app/"))
              g_print (_("No appstream data for %s: %s\n"), ref, my_error->message);
            continue;
          }
      }

    if (!flatpak_appstream_xml_root_to_data (appstream_root, &xml_data, &xml_gz_data, error))
      return FALSE;

    transaction = flatpak_repo_transaction_start (repo, cancellable, error);
    if (transaction == NULL)
      return FALSE;

    for (i = 0; i < G_N_ELEMENTS (branch_names); i++)
      {
        gboolean skip_commit = FALSE;
        const char *branch_prefix = branch_names[i];
        g_autoptr(GFile) root = NULL;
        g_autofree char *branch = NULL;
        g_autofree char *parent = NULL;
        g_autofree char *commit_checksum = NULL;

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
                g_debug ("Not updating %s, no change", branch);
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
                int i;

                for (i = 0; gpg_key_ids[i] != NULL; i++)
                  {
                    const char *keyid = gpg_key_ids[i];

                    if (!ostree_repo_sign_commit (repo,
                                                  commit_checksum,
                                                  keyid,
                                                  gpg_homedir,
                                                  cancellable,
                                                  error))
                      return FALSE;
                  }
              }

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

    if (!ostree_repo_commit_transaction (repo, &stats, cancellable, error))
      return FALSE;
  }

  return TRUE;
}

void
flatpak_extension_free (FlatpakExtension *extension)
{
  g_free (extension->id);
  g_free (extension->installed_id);
  g_free (extension->commit);
  g_free (extension->ref);
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
flatpak_extension_new (const char *id,
                       const char *extension,
                       const char *ref,
                       const char *directory,
                       const char *add_ld_path,
                       const char *subdir_suffix,
                       char      **merge_dirs,
                       GFile      *files,
                       GFile      *deploy_dir,
                       gboolean    is_unmaintained)
{
  FlatpakExtension *ext = g_new0 (FlatpakExtension, 1);
  g_autoptr(GVariant) deploy_data = NULL;

  ext->id = g_strdup (id);
  ext->installed_id = g_strdup (extension);
  ext->ref = g_strdup (ref);
  ext->directory = g_strdup (directory);
  ext->files_path = g_file_get_path (files);
  ext->add_ld_path = g_strdup (add_ld_path);
  ext->subdir_suffix = g_strdup (subdir_suffix);
  ext->merge_dirs = g_strdupv (merge_dirs);
  ext->is_unmaintained = is_unmaintained;

  if (deploy_dir)
    {
      deploy_data = flatpak_load_deploy_data (deploy_dir, ref, FLATPAK_DEPLOY_VERSION_ANY, NULL, NULL);
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
                                  const char *reason,
                                  gboolean    default_value)
{
  const char *extension_basename;
  g_auto(GStrv) reason_list = NULL;
  size_t i;

  if (reason == NULL || *reason == 0)
    return default_value;

  extension_basename = strrchr (extension_id, '.');
  if (extension_basename == NULL)
    return FALSE;
  extension_basename += 1;

  reason_list = g_strsplit (reason, ";", -1);

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
  g_autofree char *ref = NULL;
  gboolean is_unmaintained = FALSE;
  g_autoptr(GFile) files = NULL;
  g_autoptr(GFile) deploy_dir = NULL;

  if (directory == NULL)
    return res;

  ref = g_build_filename ("runtime", extension, arch, branch, NULL);

  files = flatpak_find_unmaintained_extension_dir_if_exists (extension, arch, branch, NULL);

  if (files == NULL)
    {
      deploy_dir = flatpak_find_deploy_dir_for_ref (ref, NULL, NULL, NULL);
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
          ext = flatpak_extension_new (extension, extension, ref, directory, add_ld_path, subdir_suffix, merge_dirs, files, deploy_dir, is_unmaintained);
          res = g_list_prepend (res, ext);
        }
    }
  else if (g_key_file_get_boolean (metakey, group,
                                   FLATPAK_METADATA_KEY_SUBDIRECTORIES, NULL))
    {
      g_autofree char *prefix = g_strconcat (extension, ".", NULL);
      g_auto(GStrv) refs = NULL;
      g_auto(GStrv) unmaintained_refs = NULL;
      int j;

      refs = flatpak_list_deployed_refs ("runtime", prefix, arch, branch,
                                         NULL, NULL);
      for (j = 0; refs != NULL && refs[j] != NULL; j++)
        {
          g_autofree char *extended_dir = g_build_filename (directory, refs[j] + strlen (prefix), NULL);
          g_autofree char *dir_ref = g_build_filename ("runtime", refs[j], arch, branch, NULL);
          g_autoptr(GFile) subdir_deploy_dir = NULL;
          g_autoptr(GFile) subdir_files = NULL;
          subdir_deploy_dir = flatpak_find_deploy_dir_for_ref (dir_ref, NULL, NULL, NULL);
          if (subdir_deploy_dir)
            subdir_files = g_file_get_child (subdir_deploy_dir, "files");

          if (subdir_files && flatpak_extension_matches_reason (refs[j], enable_if, TRUE))
            {
              ext = flatpak_extension_new (extension, refs[j], dir_ref, extended_dir, add_ld_path, subdir_suffix, merge_dirs, subdir_files, subdir_deploy_dir, FALSE);
              ext->needs_tmpfs = TRUE;
              res = g_list_prepend (res, ext);
            }
        }

      unmaintained_refs = flatpak_list_unmaintained_refs (prefix, arch, branch,
                                                          NULL, NULL);
      for (j = 0; unmaintained_refs != NULL && unmaintained_refs[j] != NULL; j++)
        {
          g_autofree char *extended_dir = g_build_filename (directory, unmaintained_refs[j] + strlen (prefix), NULL);
          g_autofree char *dir_ref = g_build_filename ("runtime", unmaintained_refs[j], arch, branch, NULL);
          g_autoptr(GFile) subdir_files = flatpak_find_unmaintained_extension_dir_if_exists (unmaintained_refs[j], arch, branch, NULL);

          if (subdir_files && flatpak_extension_matches_reason (unmaintained_refs[j], enable_if, TRUE))
            {
              ext = flatpak_extension_new (extension, unmaintained_refs[j], dir_ref, extended_dir, add_ld_path, subdir_suffix, merge_dirs, subdir_files, NULL, TRUE);
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

typedef struct
{
  FlatpakXml *current;
} XmlData;

FlatpakXml *
flatpak_xml_new (const gchar *element_name)
{
  FlatpakXml *node = g_new0 (FlatpakXml, 1);

  node->element_name = g_strdup (element_name);
  return node;
}

FlatpakXml *
flatpak_xml_new_text (const gchar *text)
{
  FlatpakXml *node = g_new0 (FlatpakXml, 1);

  node->text = g_strdup (text);
  return node;
}

void
flatpak_xml_add (FlatpakXml *parent, FlatpakXml *node)
{
  node->parent = parent;

  if (parent->first_child == NULL)
    parent->first_child = node;
  else
    parent->last_child->next_sibling = node;
  parent->last_child = node;
}

static void
xml_start_element (GMarkupParseContext *context,
                   const gchar         *element_name,
                   const gchar        **attribute_names,
                   const gchar        **attribute_values,
                   gpointer             user_data,
                   GError             **error)
{
  XmlData *data = user_data;
  FlatpakXml *node;

  node = flatpak_xml_new (element_name);
  node->attribute_names = g_strdupv ((char **) attribute_names);
  node->attribute_values = g_strdupv ((char **) attribute_values);

  flatpak_xml_add (data->current, node);
  data->current = node;
}

static void
xml_end_element (GMarkupParseContext *context,
                 const gchar         *element_name,
                 gpointer             user_data,
                 GError             **error)
{
  XmlData *data = user_data;

  data->current = data->current->parent;
}

static void
xml_text (GMarkupParseContext *context,
          const gchar         *text,
          gsize                text_len,
          gpointer             user_data,
          GError             **error)
{
  XmlData *data = user_data;
  FlatpakXml *node;

  node = flatpak_xml_new (NULL);
  node->text = g_strndup (text, text_len);
  flatpak_xml_add (data->current, node);
}

static void
xml_passthrough (GMarkupParseContext *context,
                 const gchar         *passthrough_text,
                 gsize                text_len,
                 gpointer             user_data,
                 GError             **error)
{
}

static GMarkupParser xml_parser = {
  xml_start_element,
  xml_end_element,
  xml_text,
  xml_passthrough,
  NULL
};

void
flatpak_xml_free (FlatpakXml *node)
{
  FlatpakXml *child;

  if (node == NULL)
    return;

  child = node->first_child;
  while (child != NULL)
    {
      FlatpakXml *next = child->next_sibling;
      flatpak_xml_free (child);
      child = next;
    }

  g_free (node->element_name);
  g_free (node->text);
  g_strfreev (node->attribute_names);
  g_strfreev (node->attribute_values);
  g_free (node);
}


void
flatpak_xml_to_string (FlatpakXml *node, GString *res)
{
  int i;
  FlatpakXml *child;

  if (node->parent == NULL)
    g_string_append (res, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");

  if (node->element_name)
    {
      if (node->parent != NULL)
        {
          g_string_append (res, "<");
          g_string_append (res, node->element_name);
          if (node->attribute_names)
            {
              for (i = 0; node->attribute_names[i] != NULL; i++)
                {
                  g_string_append_printf (res, " %s=\"%s\"",
                                          node->attribute_names[i],
                                          node->attribute_values[i]);
                }
            }
          if (node->first_child == NULL)
            g_string_append (res, "/>");
          else
            g_string_append (res, ">");
        }

      child = node->first_child;
      while (child != NULL)
        {
          flatpak_xml_to_string (child, res);
          child = child->next_sibling;
        }
      if (node->parent != NULL)
        {
          if (node->first_child != NULL)
            g_string_append_printf (res, "</%s>", node->element_name);
        }
    }
  else if (node->text)
    {
      g_autofree char *escaped = g_markup_escape_text (node->text, -1);
      g_string_append (res, escaped);
    }
}

FlatpakXml *
flatpak_xml_unlink (FlatpakXml *node,
                    FlatpakXml *prev_sibling)
{
  FlatpakXml *parent = node->parent;

  if (parent == NULL)
    return node;

  if (parent->first_child == node)
    parent->first_child = node->next_sibling;

  if (parent->last_child == node)
    parent->last_child = prev_sibling;

  if (prev_sibling)
    prev_sibling->next_sibling = node->next_sibling;

  node->parent = NULL;
  node->next_sibling = NULL;

  return node;
}

FlatpakXml *
flatpak_xml_find (FlatpakXml  *node,
                  const char  *type,
                  FlatpakXml **prev_child_out)
{
  FlatpakXml *child = NULL;
  FlatpakXml *prev_child = NULL;

  child = node->first_child;
  prev_child = NULL;
  while (child != NULL)
    {
      FlatpakXml *next = child->next_sibling;

      if (g_strcmp0 (child->element_name, type) == 0)
        {
          if (prev_child_out)
            *prev_child_out = prev_child;
          return child;
        }

      prev_child = child;
      child = next;
    }

  return NULL;
}


FlatpakXml *
flatpak_xml_parse (GInputStream *in,
                   gboolean      compressed,
                   GCancellable *cancellable,
                   GError      **error)
{
  g_autoptr(GInputStream) real_in = NULL;
  g_autoptr(FlatpakXml) xml_root = NULL;
  XmlData data = { 0 };
  char buffer[32 * 1024];
  gssize len;
  g_autoptr(GMarkupParseContext) ctx = NULL;

  if (compressed)
    {
      g_autoptr(GZlibDecompressor) decompressor = NULL;
      decompressor = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
      real_in = g_converter_input_stream_new (in, G_CONVERTER (decompressor));
    }
  else
    {
      real_in = g_object_ref (in);
    }

  xml_root = flatpak_xml_new ("root");
  data.current = xml_root;

  ctx = g_markup_parse_context_new (&xml_parser,
                                    G_MARKUP_PREFIX_ERROR_POSITION,
                                    &data,
                                    NULL);

  while ((len = g_input_stream_read (real_in, buffer, sizeof (buffer),
                                     cancellable, error)) > 0)
    {
      if (!g_markup_parse_context_parse (ctx, buffer, len, error))
        return NULL;
    }

  if (len < 0)
    return NULL;

  return g_steal_pointer (&xml_root);
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
flatpak_bundle_load (GFile   *file,
                     char   **commit,
                     char   **ref,
                     char   **origin,
                     char   **runtime_repo,
                     char   **app_metadata,
                     guint64 *installed_size,
                     GBytes **gpg_keys,
                     char   **collection_id,
                     GError **error)
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
      if (!g_variant_lookup (metadata, "ref", "s", ref))
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Invalid bundle, no ref in metadata"));
          return NULL;
        }
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

      /* Null terminate */
      g_output_stream_write (G_OUTPUT_STREAM (data_stream), "\0", 1, NULL, NULL);

      metadata_valid =
        metadata_contents != NULL &&
        strcmp (metadata_contents, g_memory_output_stream_get_data (data_stream)) == 0;
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
                               const char            *ref,
                               FlatpakOciPullProgress progress_cb,
                               gpointer               progress_user_data,
                               GCancellable          *cancellable,
                               GError               **error)
{
  FlatpakOciPullProgressData progress_data = { progress_cb, progress_user_data };
  g_autoptr(FlatpakOciVersioned) versioned = NULL;
  FlatpakOciManifest *manifest = NULL;
  g_autoptr(FlatpakOciDescriptor) manifest_desc = NULL;
  gsize versioned_size;
  g_autoptr(FlatpakOciIndex) index = NULL;
  int i;

  if (!flatpak_oci_registry_mirror_blob (dst_registry, registry, oci_repository, TRUE, digest, NULL, NULL, cancellable, error))
    return FALSE;

  versioned = flatpak_oci_registry_load_versioned (dst_registry, NULL, digest, &versioned_size, cancellable, error);
  if (versioned == NULL)
    return FALSE;

  if (!FLATPAK_IS_OCI_MANIFEST (versioned))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Image is not a manifest"));

  manifest = FLATPAK_OCI_MANIFEST (versioned);

  if (manifest->config.digest != NULL)
    {
      if (!flatpak_oci_registry_mirror_blob (dst_registry, registry, oci_repository, FALSE, manifest->config.digest, NULL, NULL, cancellable, error))
        return FALSE;
    }

  for (i = 0; manifest->layers[i] != NULL; i++)
    {
      FlatpakOciDescriptor *layer = manifest->layers[i];
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

      if (!flatpak_oci_registry_mirror_blob (dst_registry, registry, oci_repository, FALSE, layer->digest,
                                             oci_layer_progress, &progress_data,
                                             cancellable, error))
        return FALSE;

      progress_data.pulled_layers++;
      progress_data.previous_layers_size += layer->size;
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
                       FlatpakOciManifest    *manifest,
                       FlatpakOciImage       *image_config,
                       const char            *remote,
                       const char            *ref,
                       FlatpakOciPullProgress progress_cb,
                       gpointer               progress_user_data,
                       GCancellable          *cancellable,
                       GError               **error)
{
  g_autoptr(OstreeMutableTree) archive_mtree = NULL;
  g_autoptr(GFile) archive_root = NULL;
  g_autofree char *commit_checksum = NULL;
  const char *parent = NULL;
  g_autofree char *subject = NULL;
  g_autofree char *body = NULL;
  g_autofree char *manifest_ref = NULL;
  g_autofree char *full_ref = NULL;
  guint64 timestamp = 0;
  FlatpakOciPullProgressData progress_data = { progress_cb, progress_user_data };
  g_autoptr(GVariantBuilder) metadata_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
  g_autoptr(GVariant) metadata = NULL;
  GHashTable *labels;
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

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return NULL;

  /* There is no way to write a subset of the archive to a mtree, so instead
     we write all of it and then build a new mtree with the subset */
  archive_mtree = ostree_mutable_tree_new ();

  for (i = 0; manifest->layers[i] != NULL; i++)
    {
      FlatpakOciDescriptor *layer = manifest->layers[i];
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
      OstreeRepoImportArchiveOptions opts = { 0, };
      g_autoptr(FlatpakAutoArchiveRead) a = NULL;
      glnx_autofd int layer_fd = -1;
      g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
      const char *layer_checksum;

      opts.autocreate_parents = TRUE;
      opts.ignore_unsupported_content = TRUE;

      layer_fd = flatpak_oci_registry_download_blob (registry, oci_repository, FALSE,
                                                     layer->digest,
                                                     oci_layer_progress, &progress_data,
                                                     cancellable, error);
      if (layer_fd == -1)
        goto error;

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
      if (!g_str_has_prefix (layer->digest, "sha256:") ||
          strcmp (layer->digest + strlen ("sha256:"), layer_checksum) != 0)
        {
          flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Wrong layer checksum, expected %s, was %s"), layer->digest, layer_checksum);
          goto error;
        }

      progress_data.pulled_layers++;
      progress_data.previous_layers_size += layer->size;
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
      tmpdir_fd = glnx_steal_fd (&existing_tmpdir_fd);
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
    *tmpdir_fd_out = glnx_steal_fd (&tmpdir_fd);

  if (reusing_dir_out)
    *reusing_dir_out = reusing_dir;

  return TRUE;
}

char *
flatpak_prompt (gboolean allow_empty,
                const char *prompt, ...)
{
  char buf[512];
  va_list var_args;
  g_autofree char *s = NULL;


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
  va_start (var_args, prompt);
  s = g_strdup_vprintf (prompt, var_args);
  va_end (var_args);
#pragma GCC diagnostic pop

  while (TRUE)
    {
      g_print ("%s: ", s);

      if (!isatty (STDIN_FILENO) || !isatty (STDOUT_FILENO))
        {
          g_print ("n\n");
          return NULL;
        }

      if (fgets (buf, sizeof (buf), stdin) == NULL)
        return NULL;

      g_strstrip (buf);

      if (buf[0] != 0 || allow_empty)
        return g_strdup (buf);
    }
}

char *
flatpak_password_prompt (const char *prompt, ...)
{
  char buf[512];
  va_list var_args;
  g_autofree char *s = NULL;
  gboolean was_echo;


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
  va_start (var_args, prompt);
  s = g_strdup_vprintf (prompt, var_args);
  va_end (var_args);
#pragma GCC diagnostic pop

  while (TRUE)
    {
      g_print ("%s: ", s);

      if (!isatty (STDIN_FILENO) || !isatty (STDOUT_FILENO))
        return NULL;

      was_echo = flatpak_set_tty_echo (FALSE);

      if (fgets (buf, sizeof (buf), stdin) == NULL)
        return NULL;

      flatpak_set_tty_echo (was_echo);

      g_strstrip (buf);

      /* We stole the return, so manual new line */
      g_print ("\n");
      return g_strdup (buf);
    }
}


gboolean
flatpak_yes_no_prompt (gboolean default_yes, const char *prompt, ...)
{
  char buf[512];
  va_list var_args;
  g_autofree char *s = NULL;


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
  va_start (var_args, prompt);
  s = g_strdup_vprintf (prompt, var_args);
  va_end (var_args);
#pragma GCC diagnostic pop

  while (TRUE)
    {
      g_print ("%s %s: ", s, default_yes ? "[Y/n]" : "[y/n]");

      if (!isatty (STDIN_FILENO) || !isatty (STDOUT_FILENO))
        {
          g_print ("n\n");
          return FALSE;
        }

      if (fgets (buf, sizeof (buf), stdin) == NULL)
        return FALSE;

      g_strstrip (buf);

      if (default_yes && strlen (buf) == 0)
        return TRUE;

      if (g_ascii_strcasecmp (buf, "y") == 0 ||
          g_ascii_strcasecmp (buf, "yes") == 0)
        return TRUE;

      if (g_ascii_strcasecmp (buf, "n") == 0 ||
          g_ascii_strcasecmp (buf, "no") == 0)
        return FALSE;
    }
}

static gboolean
is_number (const char *s)
{
  if (*s == '\0')
    return FALSE;

  while (*s != 0)
    {
      if (!g_ascii_isdigit (*s))
        return FALSE;
      s++;
    }

  return TRUE;
}

long
flatpak_number_prompt (gboolean default_yes, int min, int max, const char *prompt, ...)
{
  char buf[512];
  va_list var_args;
  g_autofree char *s = NULL;

  va_start (var_args, prompt);
  s = g_strdup_vprintf (prompt, var_args);
  va_end (var_args);

  while (TRUE)
    {
      g_print ("%s [%d-%d]: ", s, min, max);

      if (!isatty (STDIN_FILENO) || !isatty (STDOUT_FILENO))
        {
          g_print ("0\n");
          return 0;
        }

      if (fgets (buf, sizeof (buf), stdin) == NULL)
        return 0;

      g_strstrip (buf);

      if (default_yes && strlen (buf) == 0 &&
          max - min == 1 && min == 0)
        return 1;

      if (is_number (buf))
        {
          long res = strtol (buf, NULL, 10);

          if (res >= min && res <= max)
            return res;
        }
    }
}

static gboolean
parse_range (const char *s, int *a, int *b)
{
  char *p;

  p = strchr (s, '-');
  if (!p)
    return FALSE;

  p++;
  p[-1] = '\0';

  if (is_number (s) && is_number (p))
    {
      *a = (int) strtol (s, NULL, 10);
      *b = (int) strtol (p, NULL, 10);
      p[-1] = '-';
      return TRUE;
    }

  p[-1] = '-';
  return FALSE;
}

static void
add_number (GArray *numbers,
            int     num)
{
  int i;

  for (i = 0; i < numbers->len; i++)
    {
      if (g_array_index (numbers, int, i) == num)
        return;
    }

  g_array_append_val (numbers, num);
}

int *
flatpak_parse_numbers (const char *buf,
                       int         min,
                       int         max)
{
  g_autoptr(GArray) numbers = g_array_new (FALSE, FALSE, sizeof (int));
  g_auto(GStrv) parts = g_strsplit_set (buf, " ,", 0);
  int i, j;

  for (i = 0; parts[i]; i++)
    {
      int a, b;

      g_strstrip (parts[i]);

      if (parse_range (parts[i], &a, &b) &&
          min <= a && a <= max &&
          min <= b && b <= max)
        {
          for (j = a; j <= b; j++)
            add_number (numbers, j);
        }
      else if (is_number (parts[i]))
        {
          int res = (int) strtol (parts[i], NULL, 10);
          if (min <= res && res <= max)
            add_number (numbers, res);
          else
            return NULL;
        }
      else
        return NULL;
    }

  j = 0;
  g_array_append_val (numbers, j);

  return (int *) g_array_free (g_steal_pointer (&numbers), FALSE);
}

/* Returns a 0-terminated array of ints. Free with g_free */
int *
flatpak_numbers_prompt (gboolean default_yes, int min, int max, const char *prompt, ...)
{
  char buf[512];
  va_list var_args;
  g_autofree char *s = NULL;
  g_autofree int *choice = g_new0 (int, 2);
  int *numbers;

  va_start (var_args, prompt);
  s = g_strdup_vprintf (prompt, var_args);
  va_end (var_args);

  while (TRUE)
    {
      g_print ("%s [%d-%d]: ", s, min, max);

      if (!isatty (STDIN_FILENO) || !isatty (STDOUT_FILENO))
        {
          g_print ("0\n");
          choice[0] = 0;
          return g_steal_pointer (&choice);
        }

      if (fgets (buf, sizeof (buf), stdin) == NULL)
        {
          choice[0] = 0;
          return g_steal_pointer (&choice);
        }

      g_strstrip (buf);

      if (default_yes && strlen (buf) == 0 &&
          max - min == 1 && min == 0)
        {
          choice[0] = 0;
          return g_steal_pointer (&choice);
        }

      numbers = flatpak_parse_numbers (buf, min, max);
      if (numbers)
        return numbers;
    }
}

void
flatpak_format_choices (const char **choices,
                        const char  *prompt,
                        ...)
{
  va_list var_args;
  g_autofree char *s = NULL;
  int i;

  va_start (var_args, prompt);
  s = g_strdup_vprintf (prompt, var_args);
  va_end (var_args);

  g_print ("%s\n\n", s);
  for (i = 0; choices[i]; i++)
    g_print ("  %2d) %s\n", i + 1, choices[i]);
  g_print ("\n");
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

char *
flatpak_get_lang_from_locale (const char *locale)
{
  g_autofree char *lang = g_strdup (locale);
  char *c;

  c = strchr (lang, '@');
  if (c != NULL)
    *c = 0;
  c = strchr (lang, '_');
  if (c != NULL)
    *c = 0;
  c = strchr (lang, '.');
  if (c != NULL)
    *c = 0;

  if (strcmp (lang, "C") == 0)
    return NULL;

  return g_steal_pointer (&lang);
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

char **
flatpak_get_current_locale_langs (void)
{
  const gchar * const *locales = g_get_language_names ();
  GPtrArray *langs = g_ptr_array_new ();
  int i;

  for (i = 0; locales[i] != NULL; i++)
    {
      g_autofree char *lang = flatpak_get_lang_from_locale (locales[i]);
      if (lang != NULL && !flatpak_g_ptr_array_contains_string (langs, lang))
        g_ptr_array_add (langs, g_steal_pointer (&lang));
    }

  g_ptr_array_sort (langs, flatpak_strcmp0_ptr);
  g_ptr_array_add (langs, NULL);

  return (char **) g_ptr_array_free (langs, FALSE);
}

static inline guint
get_write_progress (guint outstanding_writes)
{
  return outstanding_writes > 0 ? (guint) (3 / (gdouble) outstanding_writes) : 3;
}

static void
progress_cb (OstreeAsyncProgress *progress, gpointer user_data)
{
  gboolean last_was_metadata = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (progress), "last-was-metadata"));
  FlatpakProgressCallback progress_cb = g_object_get_data (G_OBJECT (progress), "callback");
  guint last_progress = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (progress), "last_progress"));
  guint last_total = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (progress), "last_total"));
  GString *buf;
  g_autofree char *status = NULL;
  guint outstanding_fetches;
  guint outstanding_metadata_fetches;
  guint outstanding_writes;
  guint64 bytes_transferred;
  guint64 fetched_delta_part_size;
  guint64 total_delta_part_size;
  guint64 outstanding_extra_data;
  guint64 total_extra_data_bytes;
  guint64 transferred_extra_data_bytes;
  guint64 total = 0;
  guint metadata_fetched;
  guint64 start_time;
  guint64 elapsed_time;
  guint new_progress = 0;
  gboolean estimating = FALSE;
  gboolean downloading_extra_data;
  gboolean scanning;
  guint n_scanned_metadata;
  guint fetched_delta_parts;
  guint total_delta_parts;
  guint fetched_delta_part_fallbacks;
  guint total_delta_part_fallbacks;
  guint fetched;
  guint requested;
  guint64 total_transferred;
  g_autofree gchar *formatted_bytes_total_transferred = NULL;
  g_autoptr(GVariant) outstanding_fetchesv = NULL;
  g_autoptr(GVariant) outstanding_extra_datav = NULL;

  /* We get some extra calls before we've really started due to the initialization of the
     extra data, so ignore those */
  outstanding_fetchesv = ostree_async_progress_get_variant (progress, "outstanding-fetches");
  outstanding_extra_datav = ostree_async_progress_get_variant (progress, "outstanding-extra-data");
  if (outstanding_fetchesv == NULL || outstanding_extra_datav == NULL)
    return;

  buf = g_string_new ("");

  /* The heuristic here goes as follows:
   *  - While fetching metadata, grow up to 5%
   *  - Download goes up to 97%
   *  - Writing objects adds the last 3%
   *
   *
   * Meaning of each variable:
   *
   *   Status:
   *    - status: only sent by OSTree when the pull ends (with or without an error)
   *
   *   Fetches:
   *    - fetched: sum of content + metadata fetches
   *    - requested: sum of requested content and metadata fetches
   *    - bytes_transferred: every and all transferred data (in bytes)
   *    - metadata_fetched: the number of fetched metadata objects
   *    - outstanding_fetches: missing fetches (metadata + content + deltas)
   *    - outstanding_delta_fetches: missing delta-only fetches
   *    - outstanding_metadata_fetches: missing metadata-only fetches
   *    - fetched_content_bytes: the estimated downloaded size of content (in bytes)
   *    - total_content_bytes: the estimated total size of content, based on average bytes per object (in bytes)
   *
   *   Writes:
   *    - outstanding_writes: all missing writes (sum of outstanding content, metadata and delta writes)
   *
   *   Static deltas:
   *    - total_delta_part_size: the total size (in bytes) of static deltas
   *    - fetched_delta_part_size: the size (in bytes) of already fetched static deltas
   *
   *   Extra data:
   *    - total_extra_data_bytes: the sum of all extra data file sizes (in bytes)
   *    - downloading_extra_data: whether extra-data files are being downloaded or not
   */

  /* We request everything in one go to make sure we don't race with the update from
     the async download and get mixed results */
  ostree_async_progress_get (progress,
                             "outstanding-fetches", "u", &outstanding_fetches,
                             "outstanding-metadata-fetches", "u", &outstanding_metadata_fetches,
                             "outstanding-writes", "u", &outstanding_writes,
                             "scanning", "u", &scanning,
                             "scanned-metadata", "u", &n_scanned_metadata,
                             "fetched-delta-parts", "u", &fetched_delta_parts,
                             "total-delta-parts", "u", &total_delta_parts,
                             "fetched-delta-fallbacks", "u", &fetched_delta_part_fallbacks,
                             "total-delta-fallbacks", "u", &total_delta_part_fallbacks,
                             "fetched-delta-part-size", "t", &fetched_delta_part_size,
                             "total-delta-part-size", "t", &total_delta_part_size,
                             "bytes-transferred", "t", &bytes_transferred,
                             "fetched", "u", &fetched,
                             "metadata-fetched", "u", &metadata_fetched,
                             "requested", "u", &requested,
                             "start-time", "t", &start_time,
                             "status", "s", &status,
                             "outstanding-extra-data", "t", &outstanding_extra_data,
                             "total-extra-data-bytes", "t", &total_extra_data_bytes,
                             "transferred-extra-data-bytes", "t", &transferred_extra_data_bytes,
                             "downloading-extra-data", "u", &downloading_extra_data,
                             NULL);

  elapsed_time = (g_get_monotonic_time () - start_time) / G_USEC_PER_SEC;

  /* When we receive the status, it means that the ostree pull operation is
   * finished. We only have to be careful about the extra-data fields. */
  if (status && *status && total_extra_data_bytes == 0)
    {
      g_string_append (buf, status);
      new_progress = 100;
      goto out;
    }

  total_transferred = bytes_transferred + transferred_extra_data_bytes;
  formatted_bytes_total_transferred =  g_format_size_full (total_transferred, 0);

  g_object_set_data (G_OBJECT (progress), "last-was-metadata", GUINT_TO_POINTER (FALSE));

  if (total_delta_parts == 0 &&
      (outstanding_metadata_fetches > 0 || last_was_metadata)  &&
      metadata_fetched < 20)
    {
      /* We need to hit two callbacks with no metadata outstanding, because
         sometimes we get called when we just handled a metadata, but did
         not yet process it and add more metadata */
      if (outstanding_metadata_fetches > 0)
        g_object_set_data (G_OBJECT (progress), "last-was-metadata", GUINT_TO_POINTER (TRUE));

      /* At this point we don't really know how much data there is, so we have to make a guess.
       * Since its really hard to figure out early how much data there is we report 1% until
       * all objects are scanned. */

      estimating = TRUE;

      g_string_append_printf (buf, _("Downloading metadata: %u/(estimating) %s"),
                              fetched, formatted_bytes_total_transferred);

      /* Go up to 5% until the metadata is all fetched */
      new_progress = 0;
      if (requested > 0)
        new_progress = fetched * 5 / requested;
    }
  else
    {
      if (total_delta_parts > 0)
        {
          g_autofree gchar *formatted_bytes_total = NULL;

          /* We're only using deltas, so we can ignore regular objects
           * and get perfect sizes.
           *
           * fetched_delta_part_size is the total size of all the
           * delta parts and fallback objects that were already
           * available at the start and need not be downloaded.
           */
          total = total_delta_part_size - fetched_delta_part_size + total_extra_data_bytes;
          formatted_bytes_total = g_format_size_full (total, 0);

          g_string_append_printf (buf, _("Downloading: %s/%s"),
                                  formatted_bytes_total_transferred,
                                  formatted_bytes_total);
        }
      else
        {
          /* Non-deltas, so we can't know anything other than object
             counts, except the additional extra data which we know
             the byte size of. To be able to compare them with the
             extra data we use the average object size to estimate a
             total size. */
          double average_object_size = 1;
          if (fetched > 0)
            average_object_size = bytes_transferred / (double) fetched;

          total = average_object_size * requested + total_extra_data_bytes;

          if (downloading_extra_data)
            {
              g_autofree gchar *formatted_bytes_total = g_format_size_full (total, 0);
              g_string_append_printf (buf, _("Downloading extra data: %s/%s"),
                                      formatted_bytes_total_transferred,
                                      formatted_bytes_total);
            }
          else
            g_string_append_printf (buf, _("Downloading files: %d/%d %s"),
                                    fetched, requested, formatted_bytes_total_transferred);
        }

      /* The download progress goes up to 97% */
      if (total > 0)
        {
          new_progress = 5 + ((total_transferred / (gdouble) total) * 92);
        }
      else
        {
          new_progress = 97;
        }

      /* And the writing of the objects adds 3% to the progress */
      new_progress += get_write_progress (outstanding_writes);
    }

  if (elapsed_time > 0) // Ignore first second
    {
      g_autofree gchar *formatted_bytes_sec = g_format_size (total_transferred / elapsed_time);
      g_string_append_printf (buf, " (%s/s)", formatted_bytes_sec);
    }

out:
  if (new_progress < last_progress && last_total == total)
    new_progress = last_progress;
  g_object_set_data (G_OBJECT (progress), "last_progress", GUINT_TO_POINTER (new_progress));
  g_object_set_data (G_OBJECT (progress), "last_total", GUINT_TO_POINTER (total));

  progress_cb (buf->str, new_progress, estimating, user_data);

  g_string_free (buf, TRUE);
}

OstreeAsyncProgress *
flatpak_progress_new (FlatpakProgressCallback progress,
                      gpointer                progress_data)
{
  OstreeAsyncProgress *ostree_progress =
    ostree_async_progress_new_and_connect (progress_cb,
                                           progress_data);

  g_object_set_data (G_OBJECT (ostree_progress), "callback", progress);
  g_object_set_data (G_OBJECT (ostree_progress), "callback_data", progress_data);
  g_object_set_data (G_OBJECT (ostree_progress), "last_progress", GUINT_TO_POINTER (0));
  g_object_set_data (G_OBJECT (ostree_progress), "last_total", GUINT_TO_POINTER (0));
  g_object_set_data (G_OBJECT (ostree_progress), "chained_from", NULL);

  return ostree_progress;
}

#ifdef FLATPAK_DO_CHAIN_PROGRESS
static void
progress_trigger_change (OstreeAsyncProgress *progress)
{
  guint chain_count;

  /* Trigger changed signal in original progress by changing *something* */
  chain_count = ostree_async_progress_get_uint (progress, "flatpak-chain-count");
  ostree_async_progress_set_uint (progress, "flatpak-chain-count", chain_count + 1);
}

static void
handle_chained_progress (OstreeAsyncProgress *chained_progress,
                         gpointer             user_data)
{
  OstreeAsyncProgress *original_progress = (OstreeAsyncProgress *) user_data;

  /* Sync the chained progress's state back to the original instance, to take
   * into account any updates received while a different GMainContext was
   * active */
  ostree_async_progress_copy_state (chained_progress, original_progress);
  progress_trigger_change (original_progress);

}

void
flatpak_chained_progress_finish (OstreeAsyncProgress *progress)
{
  /* At this point there might be outstanding idle events with changes in
   * the chained progress, so we need to call ostree_async_progress_finish() to
   * emit the changed signal which will call handle_chained_progress,
   * copying the data to the original progress.
   *
   * Unfortunately it will first mark the chained progress dead
   * which makes ostree_async_progress_copy_state() not actually copy
   * anything. So, to fix this we do a copy ahead of time in case it
   * was needed.
   *
   * We still need to call the regular finish() though to avoid some
   * idle callback hanging around unresolved forever (and to cause
   * the changed signal to be emitted).
   */
  OstreeAsyncProgress *original_progress = OSTREE_ASYNC_PROGRESS (g_object_get_data (G_OBJECT (progress), "chained-from"));

  ostree_async_progress_copy_state (progress, original_progress);
  ostree_async_progress_finish (progress);
}
#endif  /* FLATPAK_DO_CHAIN_PROGRESS */

/*
 * This is necessary when pushing a temporary GMainContext to be the thread
 * default with flatpak_main_context_new_default() in order to call an async
 * operation as if it were sync, if you have an OstreeAsyncProgress object that
 * would otherwise be forwarded into the async operation.
 *
 * This is because the original OstreeAsyncProgress object won't receive any
 * signals while the temporary GMainContext is active, since the GMainContext it
 * was created with won't be iterated.
 *
 * Note that this should only be done when the two GMainContexts are in the same
 * thread. If they are in different threads, then the progress's update callback
 * will be called from the wrong thread.
 *
 * tl;dr instead of this:
 *
 *     my_operation (OstreeAsyncProgress *progress)
 *     {
 *       g_autoptr(GMainContextPopDefault) context = NULL;
 *       context = flatpak_main_context_new_default ();
 *       ostree_some_async_op (progress, some_callback_setting_some_flag, data);
 *       while (wait_for_flag)
 *         g_main_context_iteration (context, TRUE);
 *     }
 *
 * do this:
 *
 *     my_operation (OstreeAsyncProgress *progress)
 *     {
 *       g_autoptr(GMainContextPopDefault) context = NULL;
 *       g_autoptr(FlatpakAsyncProgressChained) chained_progress = NULL;
 *       context = flatpak_main_context_new_default ();
 *       chained_progress = flatpak_progress_chain (progress);
 *       ostree_some_async_op (chained_progress,
 *                             some_callback_setting_some_flag, data);
 *       while (wait_for_flag)
 *         g_main_context_iteration (context, TRUE);
 *     }
 *
 * This is a no-op, preserving the current behaviour where progress events are
 * not fired, if the libostree version isn't new enough.
 */
FlatpakAsyncProgressChained *
flatpak_progress_chain (OstreeAsyncProgress *progress)
{
#ifdef FLATPAK_DO_CHAIN_PROGRESS
  if (progress == NULL)
    return NULL;

  OstreeAsyncProgress *chained_progress = ostree_async_progress_new ();

  /* Copy the OstreeAsyncProgress's state to the chained instance */
  ostree_async_progress_copy_state (progress, chained_progress);

  g_object_set_data (G_OBJECT (chained_progress), "chained-from", progress);

  g_signal_connect_data (chained_progress, "changed",
                         G_CALLBACK (handle_chained_progress),
                         g_object_ref (progress), (GClosureNotify)g_object_unref, 0);

  return chained_progress;
#else /* !FLATPAK_DO_CHAIN_PROGRESS */
  return progress;
#endif
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
      g_debug ("Opening %s flatpak installation at path %s", dir_name, dir_path_str);
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

static gboolean
str_has_sign (const gchar *str)
{
  return str[0] == '-' || str[0] == '+';
}

static gboolean
str_has_hex_prefix (const gchar *str)
{
  return str[0] == '0' && g_ascii_tolower (str[1]) == 'x';
}

/* Copied from glib-2.54.0 to avoid the Glib's version bump.
 * Function name in glib: g_ascii_string_to_unsigned
 * If this is being dropped(migration to g_ascii_string_to_unsigned)
 * make sure to remove str_has_hex_prefix and str_has_sign helpers too.
 */
gboolean
flatpak_utils_ascii_string_to_unsigned (const gchar *str,
                                        guint        base,
                                        guint64      min,
                                        guint64      max,
                                        guint64     *out_num,
                                        GError     **error)
{
  guint64 number;
  const gchar *end_ptr = NULL;
  gint saved_errno = 0;

  g_return_val_if_fail (str != NULL, FALSE);
  g_return_val_if_fail (base >= 2 && base <= 36, FALSE);
  g_return_val_if_fail (min <= max, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (str[0] == '\0')
    {
      g_set_error_literal (error,
                           G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           _("Empty string is not a number"));
      return FALSE;
    }

  errno = 0;
  number = g_ascii_strtoull (str, (gchar **) &end_ptr, base);
  saved_errno = errno;

  if (/* We do not allow leading whitespace, but g_ascii_strtoull
       * accepts it and just skips it, so we need to check for it
       * ourselves.
       */
    g_ascii_isspace (str[0]) ||
    /* Unsigned number should have no sign.
     */
    str_has_sign (str) ||
    /* We don't support hexadecimal numbers prefixed with 0x or
     * 0X.
     */
    (base == 16 && str_has_hex_prefix (str)) ||
    (saved_errno != 0 && saved_errno != ERANGE) ||
    end_ptr == NULL ||
    *end_ptr != '\0')
    {
      g_set_error (error,
                   G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   _("“%s” is not an unsigned number"), str);
      return FALSE;
    }
  if (saved_errno == ERANGE || number < min || number > max)
    {
      gchar *min_str = g_strdup_printf ("%" G_GUINT64_FORMAT, min);
      gchar *max_str = g_strdup_printf ("%" G_GUINT64_FORMAT, max);

      g_set_error (error,
                   G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   _("Number “%s” is out of bounds [%s, %s]"),
                   str, min_str, max_str);
      g_free (min_str);
      g_free (max_str);
      return FALSE;
    }
  if (out_num != NULL)
    *out_num = number;
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
flatpak_levenshtein_distance (const char *s, const char *t)
{
  int ls = strlen (s);
  int lt = strlen (t);
  int i, j;
  int *d;

  d = alloca (sizeof (int) * (ls + 1) * (lt + 1));

  for (i = 0; i <= ls; i++)
    for (j = 0; j <= lt; j++)
      d[i * (lt + 1) + j] = -1;

  return dist (s, ls, t, lt, 0, 0, d);
}

void
flatpak_get_window_size (int *rows, int *cols)
{
  struct winsize w;

  if (ioctl (STDOUT_FILENO, TIOCGWINSZ, &w) == 0)
    {
      /* For whatever reason, in buildbot this returns 0, 0 so add a fallback */
      if (w.ws_row == 0)
        w.ws_row = 24;
      if (w.ws_col == 0)
        w.ws_col = 80;
      *rows = w.ws_row;
      *cols = w.ws_col;
    }
  else
    {
      *rows = 24;
      *cols = 80;
    }
}

gboolean
flatpak_set_tty_echo (gboolean echo)
{
  struct termios term;
  gboolean was;

  tcgetattr (STDIN_FILENO, &term);
  was = (term.c_lflag & ECHO) != 0;

  if (echo)
    term.c_lflag |= ECHO;
  else
    term.c_lflag &= ~ECHO;
  tcsetattr (STDIN_FILENO, TCSANOW, &term);

  return was;
}

gboolean
flatpak_get_cursor_pos (int * row, int *col)
{
  fd_set readset;
  struct timeval time;
  struct termios term, initial_term;
  int res = 0;

  tcgetattr (STDIN_FILENO, &initial_term);
  term = initial_term;
  term.c_lflag &= ~ICANON;
  term.c_lflag &= ~ECHO;
  tcsetattr (STDIN_FILENO, TCSANOW, &term);

  printf ("\033[6n");
  fflush (stdout);

  FD_ZERO (&readset);
  FD_SET (STDIN_FILENO, &readset);
  time.tv_sec = 0;
  time.tv_usec = 100000;

  if (select (STDIN_FILENO + 1, &readset, NULL, NULL, &time) == 1)
    res = scanf ("\033[%d;%dR", row, col);

  tcsetattr (STDIN_FILENO, TCSADRAIN, &initial_term);

  return res == 2;
}

void
flatpak_hide_cursor (void)
{
  write (STDOUT_FILENO, FLATPAK_ANSI_HIDE_CURSOR, strlen (FLATPAK_ANSI_HIDE_CURSOR));
}

void
flatpak_show_cursor (void)
{
  write (STDOUT_FILENO, FLATPAK_ANSI_SHOW_CURSOR, strlen (FLATPAK_ANSI_SHOW_CURSOR));
}

void
flatpak_enable_raw_mode (void)
{
  struct termios raw;

  tcgetattr (STDIN_FILENO, &raw);

  raw.c_lflag &= ~(ECHO | ICANON);

  tcsetattr (STDIN_FILENO, TCSAFLUSH, &raw);
}

void
flatpak_disable_raw_mode (void)
{
  struct termios raw;

  tcgetattr (STDIN_FILENO, &raw);

  raw.c_lflag |= (ECHO | ICANON);

  tcsetattr (STDIN_FILENO, TCSAFLUSH, &raw);
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


#if !GLIB_CHECK_VERSION (2, 56, 0)
/* All this code is backported directly from glib */

static void
g_date_time_get_week_number (GDateTime *datetime,
                             gint      *week_number,
                             gint      *day_of_week,
                             gint      *day_of_year)
{
  gint a, b, c, d, e, f, g, n, s, month, day, year;

  g_date_time_get_ymd (datetime, &year, &month, &day);

  if (month <= 2)
    {
      a = g_date_time_get_year (datetime) - 1;
      b = (a / 4) - (a / 100) + (a / 400);
      c = ((a - 1) / 4) - ((a - 1) / 100) + ((a - 1) / 400);
      s = b - c;
      e = 0;
      f = day - 1 + (31 * (month - 1));
    }
  else
    {
      a = year;
      b = (a / 4) - (a / 100) + (a / 400);
      c = ((a - 1) / 4) - ((a - 1) / 100) + ((a - 1) / 400);
      s = b - c;
      e = s + 1;
      f = day + (((153 * (month - 3)) + 2) / 5) + 58 + s;
    }

  g = (a + b) % 7;
  d = (f + g - e) % 7;
  n = f + 3 - d;

  if (week_number)
    {
      if (n < 0)
        *week_number = 53 - ((g - s) / 5);
      else if (n > 364 + s)
        *week_number = 1;
      else
        *week_number = (n / 7) + 1;
    }

  if (day_of_week)
    *day_of_week = d + 1;

  if (day_of_year)
    *day_of_year = f + 1;
}

#define GREGORIAN_LEAP(y)    ((((y) % 4) == 0) && (!((((y) % 100) == 0) && (((y) % 400) != 0))))

/* Parse integers in the form d (week days), dd (hours etc), ddd (ordinal days) or dddd (years) */
static gboolean
get_iso8601_int (const gchar *text, gsize length, gint *value)
{
  gint i, v = 0;

  if (length < 1 || length > 4)
    return FALSE;

  for (i = 0; i < length; i++)
    {
      const gchar c = text[i];
      if (c < '0' || c > '9')
        return FALSE;
      v = v * 10 + (c - '0');
    }

  *value = v;
  return TRUE;
}

/* Parse seconds in the form ss or ss.sss (variable length decimal) */
static gboolean
get_iso8601_seconds (const gchar *text, gsize length, gdouble *value)
{
  gint i;
  gdouble divisor = 1, v = 0;

  if (length < 2)
    return FALSE;

  for (i = 0; i < 2; i++)
    {
      const gchar c = text[i];
      if (c < '0' || c > '9')
        return FALSE;
      v = v * 10 + (c - '0');
    }

  if (length > 2 && !(text[i] == '.' || text[i] == ','))
    return FALSE;
  i++;
  if (i == length)
    return FALSE;

  for (; i < length; i++)
    {
      const gchar c = text[i];
      if (c < '0' || c > '9')
        return FALSE;
      v = v * 10 + (c - '0');
      divisor *= 10;
    }

  *value = v / divisor;
  return TRUE;
}

static GDateTime *
g_date_time_new_ordinal (GTimeZone *tz, gint year, gint ordinal_day, gint hour, gint minute, gdouble seconds)
{
  GDateTime *dt, *dt2;

  if (ordinal_day < 1 || ordinal_day > (GREGORIAN_LEAP (year) ? 366 : 365))
    return NULL;

  dt = g_date_time_new (tz, year, 1, 1, hour, minute, seconds);
  dt2 = g_date_time_add_days (dt, ordinal_day - 1);
  g_date_time_unref (dt);

  return dt2;
}

static GDateTime *
g_date_time_new_week (GTimeZone *tz, gint year, gint week, gint week_day, gint hour, gint minute, gdouble seconds)
{
  gint64 p;
  gint max_week, jan4_week_day, ordinal_day;
  GDateTime *dt;

  p = (year * 365 + (year / 4) - (year / 100) + (year / 400)) % 7;
  max_week = p == 4 ? 53 : 52;

  if (week < 1 || week > max_week || week_day < 1 || week_day > 7)
    return NULL;

  dt = g_date_time_new (tz, year, 1, 4, 0, 0, 0);
  g_date_time_get_week_number (dt, NULL, &jan4_week_day, NULL);
  g_date_time_unref (dt);

  ordinal_day = (week * 7) + week_day - (jan4_week_day + 3);
  if (ordinal_day < 0)
    {
      year--;
      ordinal_day += GREGORIAN_LEAP (year) ? 366 : 365;
    }
  else if (ordinal_day > (GREGORIAN_LEAP (year) ? 366 : 365))
    {
      ordinal_day -= (GREGORIAN_LEAP (year) ? 366 : 365);
      year++;
    }

  return g_date_time_new_ordinal (tz, year, ordinal_day, hour, minute, seconds);
}

static GDateTime *
parse_iso8601_date (const gchar *text, gsize length,
                    gint hour, gint minute, gdouble seconds, GTimeZone *tz)
{
  /* YYYY-MM-DD */
  if (length == 10 && text[4] == '-' && text[7] == '-')
    {
      int year, month, day;
      if (!get_iso8601_int (text, 4, &year) ||
          !get_iso8601_int (text + 5, 2, &month) ||
          !get_iso8601_int (text + 8, 2, &day))
        return NULL;
      return g_date_time_new (tz, year, month, day, hour, minute, seconds);
    }
  /* YYYY-DDD */
  else if (length == 8 && text[4] == '-')
    {
      gint year, ordinal_day;
      if (!get_iso8601_int (text, 4, &year) ||
          !get_iso8601_int (text + 5, 3, &ordinal_day))
        return NULL;
      return g_date_time_new_ordinal (tz, year, ordinal_day, hour, minute, seconds);
    }
  /* YYYY-Www-D */
  else if (length == 10 && text[4] == '-' && text[5] == 'W' && text[8] == '-')
    {
      gint year, week, week_day;
      if (!get_iso8601_int (text, 4, &year) ||
          !get_iso8601_int (text + 6, 2, &week) ||
          !get_iso8601_int (text + 9, 1, &week_day))
        return NULL;
      return g_date_time_new_week (tz, year, week, week_day, hour, minute, seconds);
    }
  /* YYYYWwwD */
  else if (length == 8 && text[4] == 'W')
    {
      gint year, week, week_day;
      if (!get_iso8601_int (text, 4, &year) ||
          !get_iso8601_int (text + 5, 2, &week) ||
          !get_iso8601_int (text + 7, 1, &week_day))
        return NULL;
      return g_date_time_new_week (tz, year, week, week_day, hour, minute, seconds);
    }
  /* YYYYMMDD */
  else if (length == 8)
    {
      int year, month, day;
      if (!get_iso8601_int (text, 4, &year) ||
          !get_iso8601_int (text + 4, 2, &month) ||
          !get_iso8601_int (text + 6, 2, &day))
        return NULL;
      return g_date_time_new (tz, year, month, day, hour, minute, seconds);
    }
  /* YYYYDDD */
  else if (length == 7)
    {
      gint year, ordinal_day;
      if (!get_iso8601_int (text, 4, &year) ||
          !get_iso8601_int (text + 4, 3, &ordinal_day))
        return NULL;
      return g_date_time_new_ordinal (tz, year, ordinal_day, hour, minute, seconds);
    }
  else
    return FALSE;
}

static GTimeZone *
parse_iso8601_timezone (const gchar *text, gsize length, gssize *tz_offset)
{
  gint i, tz_length, offset_sign = 1, offset_hours, offset_minutes;
  GTimeZone *tz;

  /* UTC uses Z suffix  */
  if (length > 0 && text[length - 1] == 'Z')
    {
      *tz_offset = length - 1;
      return g_time_zone_new_utc ();
    }

  /* Look for '+' or '-' of offset */
  for (i = length - 1; i >= 0; i--)
    if (text[i] == '+' || text[i] == '-')
      {
        offset_sign = text[i] == '-' ? -1 : 1;
        break;
      }
  if (i < 0)
    return NULL;
  tz_length = length - i;

  /* +hh:mm or -hh:mm */
  if (tz_length == 6 && text[i + 3] == ':')
    {
      if (!get_iso8601_int (text + i + 1, 2, &offset_hours) ||
          !get_iso8601_int (text + i + 4, 2, &offset_minutes))
        return NULL;
    }
  /* +hhmm or -hhmm */
  else if (tz_length == 5)
    {
      if (!get_iso8601_int (text + i + 1, 2, &offset_hours) ||
          !get_iso8601_int (text + i + 3, 2, &offset_minutes))
        return NULL;
    }
  /* +hh or -hh */
  else if (tz_length == 3)
    {
      if (!get_iso8601_int (text + i + 1, 2, &offset_hours))
        return NULL;
      offset_minutes = 0;
    }
  else
    return NULL;

  *tz_offset = i;
  tz = g_time_zone_new (text + i);

  /* Double-check that the GTimeZone matches our interpretation of the timezone.
   * Failure would indicate a bug either here of in the GTimeZone code. */
  g_assert (g_time_zone_get_offset (tz, 0) == offset_sign * (offset_hours * 3600 + offset_minutes * 60));

  return tz;
}

static gboolean
parse_iso8601_time (const gchar *text, gsize length,
                    gint *hour, gint *minute, gdouble *seconds, GTimeZone **tz)
{
  gssize tz_offset = -1;

  /* Check for timezone suffix */
  *tz = parse_iso8601_timezone (text, length, &tz_offset);
  if (tz_offset >= 0)
    length = tz_offset;

  /* hh:mm:ss(.sss) */
  if (length >= 8 && text[2] == ':' && text[5] == ':')
    {
      return get_iso8601_int (text, 2, hour) &&
             get_iso8601_int (text + 3, 2, minute) &&
             get_iso8601_seconds (text + 6, length - 6, seconds);
    }
  /* hhmmss(.sss) */
  else if (length >= 6)
    {
      return get_iso8601_int (text, 2, hour) &&
             get_iso8601_int (text + 2, 2, minute) &&
             get_iso8601_seconds (text + 4, length - 4, seconds);
    }
  else
    return FALSE;
}


GDateTime *
flatpak_g_date_time_new_from_iso8601 (const gchar *text, GTimeZone *default_tz)
{
  gint length, date_length = -1;
  gint hour = 0, minute = 0;
  gdouble seconds = 0.0;
  GTimeZone *tz = NULL;
  GDateTime *datetime = NULL;

  g_return_val_if_fail (text != NULL, NULL);

  /* Count length of string and find date / time separator ('T', 't', or ' ') */
  for (length = 0; text[length] != '\0'; length++)
    {
      if (date_length < 0 && (text[length] == 'T' || text[length] == 't' || text[length] == ' '))
        date_length = length;
    }

  if (date_length < 0)
    return NULL;

  if (!parse_iso8601_time (text + date_length + 1, length - (date_length + 1),
                           &hour, &minute, &seconds, &tz))
    goto out;
  if (tz == NULL && default_tz == NULL)
    return NULL;

  datetime = parse_iso8601_date (text, date_length, hour, minute, seconds, tz ? tz : default_tz);

out:
  if (tz != NULL)
    g_time_zone_unref (tz);
  return datetime;
}
#endif

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
  int i;

  for (i = 0; path1[i]; i++)
    {
      if (path2[i] == '\0')
        return FALSE;

      if (tolower (path1[i]) == tolower (path2[i]))
        continue;

      if ((path1[i] == '-' || path1[i] == '_') &&
          (path2[i] == '-' || path2[i] == '_'))
        continue;

      return FALSE;
    }

  if (path2[i] != '\0')
    return FALSE;

  return TRUE;
}


