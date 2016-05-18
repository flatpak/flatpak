/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#include "flatpak-utils.h"
#include "lib/flatpak-error.h"
#include "flatpak-dir.h"
#include "flatpak-portal-error.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>

#include <glib.h>
#include "libgsystem.h"
#include "libglnx/libglnx.h"
#include <libsoup/soup.h>

/* This is also here so the common code can report these errors to the lib */
static const GDBusErrorEntry flatpak_error_entries[] = {
  {FLATPAK_ERROR_ALREADY_INSTALLED,     "org.freedesktop.Flatpak.Error.AlreadyInstalled"},
  {FLATPAK_ERROR_NOT_INSTALLED,         "org.freedesktop.Flatpak.Error.NotInstalled"},
};

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

gboolean
flatpak_fail (GError **error, const char *format, ...)
{
  g_autofree char *message = NULL;
  va_list args;

  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);

  g_set_error_literal (error,
                       G_IO_ERROR, G_IO_ERROR_FAILED,
                       message);

  return FALSE;
}

const char *
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
 * at least try to execute a 386, wheras an arm binary would not.
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

const char *
flatpak_get_bwrap (void)
{
  const char *e = g_getenv ("FLATPAK_BWRAP");

  if (e != NULL)
    return e;
  return HELPER;
}

/* We only migrate the user dir, because thats what most people used with xdg-app,
 * and its where all per-user state/config are stored.
 */
void
flatpak_migrate_from_xdg_app (void)
{
  g_autofree char *source = g_build_filename (g_get_user_data_dir (), "xdg-app", NULL);
  g_autofree char *dest = g_build_filename (g_get_user_data_dir (), "flatpak", NULL);

  if (!g_file_test (dest, G_FILE_TEST_EXISTS) &&
      g_file_test (source, G_FILE_TEST_EXISTS))
    {
      g_print ("Migrating %s to %s\n", source, dest);
      if (rename (source, dest) != 0)
        {
          if (errno != ENOENT &&
              errno != ENOTEMPTY &&
              errno != EEXIST)
            g_print ("Error during migration: %s\n", strerror (errno));
        }
    }
}

static gboolean
is_valid_initial_name_character (gint c)
{
  return
    (c >= 'A' && c <= 'Z') ||
    (c >= 'a' && c <= 'z') ||
    (c == '_');
}

static gboolean
is_valid_name_character (gint c)
{
  return
    is_valid_initial_name_character (c) ||
    (c >= '0' && c <= '9');
}

/** flatpak_is_name:
 * @string: The string to check
 *
 * Checks if @string is a valid application name.
 *
 * App names are composed of 3 or more elements separated by a period
 * ('.') character. All elements must contain at least one character.
 *
 * Each element must only contain the ASCII characters
 * "[A-Z][a-z][0-9]_". Elements may not begin with a digit.
 *
 * App names must not begin with a '.' (period) character.
 *
 * App names must not exceed 255 characters in length.
 *
 * The above means that any app name is also a valid DBus well known
 * bus name, but not all DBus names are valid app names. The difference are:
 * 1) DBus name elements may contain '-'
 * 2) DBus names require only two elements
 *
 * Returns: %TRUE if valid, %FALSE otherwise.
 *
 * Since: 2.26
 */
gboolean
flatpak_is_valid_name (const char *string)
{
  guint len;
  gboolean ret;
  const gchar *s;
  const gchar *end;
  int dot_count;

  g_return_val_if_fail (string != NULL, FALSE);

  ret = FALSE;

  len = strlen (string);
  if (G_UNLIKELY (len == 0 || len > 255))
    goto out;

  end = string + len;

  s = string;
  if (G_UNLIKELY (*s == '.'))
    /* can't start with a . */
    goto out;
  else if (G_UNLIKELY (!is_valid_initial_name_character (*s)))
    goto out;

  s += 1;
  dot_count = 0;
  while (s != end)
    {
      if (*s == '.')
        {
          s += 1;
          if (G_UNLIKELY (s == end || !is_valid_initial_name_character (*s)))
            goto out;
          dot_count++;
        }
      else if (G_UNLIKELY (!is_valid_name_character (*s)))
        {
          goto out;
        }
      s += 1;
    }

  if (G_UNLIKELY (dot_count < 2))
    goto out;

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
    !is_valid_name_character (*rest);
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

/** flatpak_is_valid_branch:
 * @string: The string to check
 *
 * Checks if @string is a valid branch name.
 *
 * Branch names must only contain the ASCII characters
 * "[A-Z][a-z][0-9]_-.".
 * Branch names may not begin with a digit.
 * Branch names must contain at least one character.
 *
 * Returns: %TRUE if valid, %FALSE otherwise.
 *
 * Since: 2.26
 */
gboolean
flatpak_is_valid_branch (const char *string)
{
  guint len;
  gboolean ret;
  const gchar *s;
  const gchar *end;

  g_return_val_if_fail (string != NULL, FALSE);

  ret = FALSE;

  len = strlen (string);
  if (G_UNLIKELY (len == 0))
    goto out;

  end = string + len;

  s = string;
  if (G_UNLIKELY (!is_valid_initial_branch_character (*s)))
    goto out;

  s += 1;
  while (s != end)
    {
      if (G_UNLIKELY (!is_valid_branch_character (*s)))
        goto out;
      s += 1;
    }

  ret = TRUE;

out:
  return ret;
}

char **
flatpak_decompose_ref (const char *full_ref,
                       GError    **error)
{
  g_auto(GStrv) parts = NULL;

  parts = g_strsplit (full_ref, "/", 0);
  if (g_strv_length (parts) != 4)
    {
      flatpak_fail (error, "Wrong number of components in %s", full_ref);
      return NULL;
    }

  if (strcmp (parts[0], "app") != 0 && strcmp (parts[0], "runtime") != 0)
    {
      flatpak_fail (error, "Not application or runtime");
      return NULL;
    }

  if (!flatpak_is_valid_name (parts[1]))
    {
      flatpak_fail (error, "Invalid name %s", parts[1]);
      return NULL;
    }

  if (strlen (parts[2]) == 0)
    {
      flatpak_fail (error, "Invalid arch %s", parts[2]);
      return NULL;
    }

  if (!flatpak_is_valid_branch (parts[3]))
    {
      flatpak_fail (error, "Invalid branch %s", parts[3]);
      return NULL;
    }

  return g_steal_pointer (&parts);
}

char *
flatpak_compose_ref (gboolean    app,
                     const char *name,
                     const char *branch,
                     const char *arch,
                     GError    **error)
{
  if (!flatpak_is_valid_name (name))
    {
      flatpak_fail (error, "'%s' is not a valid name", name);
      return NULL;
    }

  if (branch && !flatpak_is_valid_branch (branch))
    {
      flatpak_fail (error, "'%s' is not a valid branch name", branch);
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
                            const char   *branch,
                            const char   *arch,
                            GCancellable *cancellable,
                            GError      **error)
{
  gchar **ret = NULL;

  g_autoptr(GPtrArray) names = NULL;
  g_autoptr(GHashTable) hash = NULL;
  g_autoptr(FlatpakDir) user_dir = NULL;
  g_autoptr(FlatpakDir) system_dir = NULL;
  const char *key;
  GHashTableIter iter;

  hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  user_dir = flatpak_dir_get_user ();
  system_dir = flatpak_dir_get_system ();

  if (!flatpak_dir_collect_deployed_refs (user_dir, type, name_prefix,
                                          branch, arch, hash, cancellable,
                                          error))
    goto out;

  if (!flatpak_dir_collect_deployed_refs (system_dir, type, name_prefix,
                                          branch, arch, hash, cancellable,
                                          error))
    goto out;

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

GFile *
flatpak_find_deploy_dir_for_ref (const char   *ref,
                                 GCancellable *cancellable,
                                 GError      **error)
{
  g_autoptr(FlatpakDir) user_dir = NULL;
  g_autoptr(FlatpakDir) system_dir = NULL;
  GFile *deploy = NULL;

  user_dir = flatpak_dir_get_user ();
  system_dir = flatpak_dir_get_system ();

  deploy = flatpak_dir_get_if_deployed (user_dir, ref, NULL, cancellable);
  if (deploy == NULL)
    deploy = flatpak_dir_get_if_deployed (system_dir, ref, NULL, cancellable);
  if (deploy == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s not installed", ref);
      return NULL;
    }

  return deploy;

}

FlatpakDeploy *
flatpak_find_deploy_for_ref (const char   *ref,
                             GCancellable *cancellable,
                             GError      **error)
{
  g_autoptr(FlatpakDir) user_dir = NULL;
  g_autoptr(FlatpakDir) system_dir = NULL;
  g_autoptr(FlatpakDeploy) deploy = NULL;
  g_autoptr(GError) my_error = NULL;

  user_dir = flatpak_dir_get_user ();
  system_dir = flatpak_dir_get_system ();

  deploy = flatpak_dir_load_deployed (user_dir, ref, NULL, cancellable, &my_error);
  if (deploy == NULL && g_error_matches (my_error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED))
    {
      g_clear_error (&my_error);
      deploy = flatpak_dir_load_deployed (system_dir, ref, NULL, cancellable, &my_error);
    }
  if (deploy == NULL)
    g_propagate_error (error, g_steal_pointer (&my_error));

  return g_steal_pointer (&deploy);
}


static gboolean
overlay_symlink_tree_dir (int           source_parent_fd,
                          const char   *source_name,
                          const char   *source_symlink_prefix,
                          int           destination_parent_fd,
                          const char   *destination_name,
                          GCancellable *cancellable,
                          GError      **error)
{
  gboolean ret = FALSE;
  int res;

  g_auto(GLnxDirFdIterator) source_iter = { 0 };
  glnx_fd_close int destination_dfd = -1;
  struct dirent *dent;

  if (!glnx_dirfd_iterator_init_at (source_parent_fd, source_name, FALSE, &source_iter, error))
    goto out;

  do
    res = mkdirat (destination_parent_fd, destination_name, 0777);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      if (errno != EEXIST)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  if (!gs_file_open_dir_fd_at (destination_parent_fd, destination_name,
                               &destination_dfd,
                               cancellable, error))
    goto out;

  while (TRUE)
    {

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&source_iter, &dent, cancellable, error))
        goto out;

      if (dent == NULL)
        break;

      if (dent->d_type == DT_DIR)
        {
          g_autofree gchar *target = g_build_filename ("..", source_symlink_prefix, dent->d_name, NULL);
          if (!overlay_symlink_tree_dir (source_iter.fd, dent->d_name, target, destination_dfd, dent->d_name,
                                         cancellable, error))
            goto out;
        }
      else
        {
          g_autofree gchar *target = g_build_filename (source_symlink_prefix, dent->d_name, NULL);

          if (unlinkat (destination_dfd, dent->d_name, 0) != 0 && errno != ENOENT)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }

          if (symlinkat (target, destination_dfd, dent->d_name) != 0)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }
    }

  ret = TRUE;
out:

  return ret;
}

gboolean
flatpak_overlay_symlink_tree (GFile        *source,
                              GFile        *destination,
                              const char   *symlink_prefix,
                              GCancellable *cancellable,
                              GError      **error)
{
  gboolean ret = FALSE;

  if (!gs_file_ensure_directory (destination, TRUE, cancellable, error))
    goto out;

  /* The fds are closed by this call */
  if (!overlay_symlink_tree_dir (AT_FDCWD, gs_file_get_path_cached (source),
                                 symlink_prefix,
                                 AT_FDCWD, gs_file_get_path_cached (destination),
                                 cancellable, error))
    goto out;

  ret = TRUE;

out:
  return ret;
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
  if (!remove_dangling_symlinks (AT_FDCWD, gs_file_get_path_cached (dir),
                                 cancellable, error))
    goto out;

  ret = TRUE;

out:
  return ret;
}

/* Based on g_mkstemp from glib */

gint
flatpak_mkstempat (int    dir_fd,
                   gchar *tmpl,
                   int    flags,
                   int    mode)
{
  char *XXXXXX;
  int count, fd;
  static const char letters[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  static const int NLETTERS = sizeof (letters) - 1;
  glong value;
  GTimeVal tv;
  static int counter = 0;

  g_return_val_if_fail (tmpl != NULL, -1);

  /* find the last occurrence of "XXXXXX" */
  XXXXXX = g_strrstr (tmpl, "XXXXXX");

  if (!XXXXXX || strncmp (XXXXXX, "XXXXXX", 6))
    {
      errno = EINVAL;
      return -1;
    }

  /* Get some more or less random data.  */
  g_get_current_time (&tv);
  value = (tv.tv_usec ^ tv.tv_sec) + counter++;

  for (count = 0; count < 100; value += 7777, ++count)
    {
      glong v = value;

      /* Fill in the random bits.  */
      XXXXXX[0] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[1] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[2] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[3] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[4] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[5] = letters[v % NLETTERS];

      fd = openat (dir_fd, tmpl, flags | O_CREAT | O_EXCL, mode);

      if (fd >= 0)
        return fd;
      else if (errno != EEXIST)
        /* Any other error will apply also to other names we might
         *  try, and there are 2^32 or so of them, so give up now.
         */
        return -1;
    }

  /* We got out of the loop because we ran out of combinations to try.  */
  errno = EEXIST;
  return -1;
}

struct FlatpakTablePrinter
{
  GPtrArray *rows;
  GPtrArray *current;
  int        n_columns;
};

FlatpakTablePrinter *
flatpak_table_printer_new (void)
{
  FlatpakTablePrinter *printer = g_new0 (FlatpakTablePrinter, 1);

  printer->rows = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
  printer->current = g_ptr_array_new_with_free_func (g_free);

  return printer;
}

void
flatpak_table_printer_free (FlatpakTablePrinter *printer)
{
  g_ptr_array_free (printer->rows, TRUE);
  g_ptr_array_free (printer->current, TRUE);
  g_free (printer);
}

void
flatpak_table_printer_add_column (FlatpakTablePrinter *printer,
                                  const char          *text)
{
  g_ptr_array_add (printer->current, text ? g_strdup (text) : g_strdup (""));
}

void
flatpak_table_printer_append_with_comma (FlatpakTablePrinter *printer,
                                         const char          *text)
{
  char *old, *new;

  g_assert (printer->current->len > 0);

  old = g_ptr_array_index (printer->current, printer->current->len - 1);

  if (old[0] != 0)
    new = g_strconcat (old, ",", text, NULL);
  else
    new = g_strdup (text);

  g_ptr_array_index (printer->current, printer->current->len - 1) = new;
  g_free (old);
}


void
flatpak_table_printer_finish_row (FlatpakTablePrinter *printer)
{
  if (printer->current->len == 0)
    return; /* Ignore empty rows */

  printer->n_columns = MAX (printer->n_columns, printer->current->len);
  g_ptr_array_add (printer->current, NULL);
  g_ptr_array_add (printer->rows,
                   g_ptr_array_free (printer->current, FALSE));
  printer->current = g_ptr_array_new_with_free_func (g_free);
}

void
flatpak_table_printer_print (FlatpakTablePrinter *printer)
{
  g_autofree int *widths = NULL;
  int i, j;

  if (printer->current->len != 0)
    flatpak_table_printer_finish_row (printer);

  widths = g_new0 (int, printer->n_columns);

  for (i = 0; i < printer->rows->len; i++)
    {
      char **row = g_ptr_array_index (printer->rows, i);

      for (j = 0; row[j] != NULL; j++)
        widths[j] = MAX (widths[j], strlen (row[j]));
    }

  for (i = 0; i < printer->rows->len; i++)
    {
      char **row = g_ptr_array_index (printer->rows, i);

      for (j = 0; row[j] != NULL; j++)
        g_print ("%s%-*s", (j == 0) ? "" : " ", widths[j], row[j]);
      g_print ("\n");
    }
}


static GHashTable *app_ids;

typedef struct
{
  char    *name;
  char    *app_id;
  gboolean exited;
  GList   *pending;
} AppIdInfo;

static void
app_id_info_free (AppIdInfo *info)
{
  g_free (info->name);
  g_free (info->app_id);
  g_free (info);
}

static void
ensure_app_ids (void)
{
  if (app_ids == NULL)
    app_ids = g_hash_table_new_full (g_str_hash, g_str_equal,
                                     NULL, (GDestroyNotify) app_id_info_free);
}

static void
got_credentials_cb (GObject      *source_object,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  AppIdInfo *info = user_data;

  g_autoptr(GDBusMessage) reply = NULL;
  g_autoptr(GError) error = NULL;
  GList *l;

  reply = g_dbus_connection_send_message_with_reply_finish (G_DBUS_CONNECTION (source_object),
                                                            res, &error);

  if (!info->exited && reply != NULL)
    {
      GVariant *body = g_dbus_message_get_body (reply);
      guint32 pid;
      g_autofree char *path = NULL;
      g_autofree char *content = NULL;

      g_variant_get (body, "(u)", &pid);

      path = g_strdup_printf ("/proc/%u/cgroup", pid);

      if (g_file_get_contents (path, &content, NULL, NULL))
        {
          gchar **lines =  g_strsplit (content, "\n", -1);
          int i;

          for (i = 0; lines[i] != NULL; i++)
            {
              if (g_str_has_prefix (lines[i], "1:name=systemd:"))
                {
                  const char *unit = lines[i] + strlen ("1:name=systemd:");
                  g_autofree char *scope = g_path_get_basename (unit);

                  if (g_str_has_prefix (scope, "flatpak-") &&
                      g_str_has_suffix (scope, ".scope"))
                    {
                      const char *name = scope + strlen ("flatpak-");
                      char *dash = strchr (name, '-');
                      if (dash != NULL)
                        {
                          *dash = 0;
                          info->app_id = g_strdup (name);
                        }
                    }
                  else
                    {
                      info->app_id = g_strdup ("");
                    }
                }
            }
          g_strfreev (lines);
        }
    }

  for (l = info->pending; l != NULL; l = l->next)
    {
      GTask *task = l->data;

      if (info->app_id == NULL)
        g_task_return_new_error (task, FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_FAILED,
                                 "Can't find app id");
      else
        g_task_return_pointer (task, g_strdup (info->app_id), g_free);
    }

  g_list_free_full (info->pending, g_object_unref);
  info->pending = NULL;

  if (info->app_id == NULL)
    g_hash_table_remove (app_ids, info->name);
}

void
flatpak_invocation_lookup_app_id (GDBusMethodInvocation *invocation,
                                  GCancellable          *cancellable,
                                  GAsyncReadyCallback    callback,
                                  gpointer               user_data)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);

  g_autoptr(GTask) task = NULL;
  AppIdInfo *info;

  task = g_task_new (invocation, cancellable, callback, user_data);

  ensure_app_ids ();

  info = g_hash_table_lookup (app_ids, sender);

  if (info == NULL)
    {
      info = g_new0 (AppIdInfo, 1);
      info->name = g_strdup (sender);
      g_hash_table_insert (app_ids, info->name, info);
    }

  if (info->app_id)
    {
      g_task_return_pointer (task, g_strdup (info->app_id), g_free);
    }
  else
    {
      if (info->pending == NULL)
        {
          g_autoptr(GDBusMessage) msg = g_dbus_message_new_method_call ("org.freedesktop.DBus",
                                                                        "/org/freedesktop/DBus",
                                                                        "org.freedesktop.DBus",
                                                                        "GetConnectionUnixProcessID");
          g_dbus_message_set_body (msg, g_variant_new ("(s)", sender));

          g_dbus_connection_send_message_with_reply (connection, msg,
                                                     G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                     30000,
                                                     NULL,
                                                     cancellable,
                                                     got_credentials_cb,
                                                     info);
        }

      info->pending = g_list_prepend (info->pending, g_object_ref (task));
    }
}

char *
flatpak_invocation_lookup_app_id_finish (GDBusMethodInvocation *invocation,
                                         GAsyncResult          *result,
                                         GError               **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
name_owner_changed (GDBusConnection *connection,
                    const gchar     *sender_name,
                    const gchar     *object_path,
                    const gchar     *interface_name,
                    const gchar     *signal_name,
                    GVariant        *parameters,
                    gpointer         user_data)
{
  const char *name, *from, *to;

  g_variant_get (parameters, "(sss)", &name, &from, &to);

  ensure_app_ids ();

  if (name[0] == ':' &&
      strcmp (name, from) == 0 &&
      strcmp (to, "") == 0)
    {
      AppIdInfo *info = g_hash_table_lookup (app_ids, name);

      if (info != NULL)
        {
          info->exited = TRUE;
          if (info->pending == NULL)
            g_hash_table_remove (app_ids, name);
        }
    }
}

void
flatpak_connection_track_name_owners (GDBusConnection *connection)
{
  g_dbus_connection_signal_subscribe (connection,
                                      "org.freedesktop.DBus",
                                      "org.freedesktop.DBus",
                                      "NameOwnerChanged",
                                      "/org/freedesktop/DBus",
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      name_owner_changed,
                                      NULL, NULL);
}

typedef struct
{
  GError    *error;
  GError    *splice_error;
  GMainLoop *loop;
  int        refs;
} SpawnData;

static void
spawn_data_exit (SpawnData *data)
{
  data->refs--;
  if (data->refs == 0)
    g_main_loop_quit (data->loop);
}

static void
spawn_output_spliced_cb (GObject      *obj,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  SpawnData *data = user_data;

  g_output_stream_splice_finish (G_OUTPUT_STREAM (obj), result, &data->splice_error);
  spawn_data_exit (data);
}

static void
spawn_exit_cb (GObject      *obj,
               GAsyncResult *result,
               gpointer      user_data)
{
  SpawnData *data = user_data;

  g_subprocess_wait_check_finish (G_SUBPROCESS (obj), result, &data->error);
  spawn_data_exit (data);
}

gboolean
flatpak_spawn (GFile       *dir,
               char       **output,
               GError     **error,
               const gchar *argv0,
               va_list      ap)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GSubprocess) subp = NULL;
  GPtrArray *args;
  const gchar *arg;
  GInputStream *in;
  g_autoptr(GOutputStream) out = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  SpawnData data = {0};

  args = g_ptr_array_new ();
  g_ptr_array_add (args, (gchar *) argv0);
  while ((arg = va_arg (ap, const gchar *)))
    g_ptr_array_add (args, (gchar *) arg);
  g_ptr_array_add (args, NULL);

  launcher = g_subprocess_launcher_new (0);

  if (output)
    g_subprocess_launcher_set_flags (launcher, G_SUBPROCESS_FLAGS_STDOUT_PIPE);

  if (dir)
    {
      g_autofree char *path = g_file_get_path (dir);
      g_subprocess_launcher_set_cwd (launcher, path);
    }

  subp = g_subprocess_launcher_spawnv (launcher, (const gchar * const *) args->pdata, error);
  g_ptr_array_free (args, TRUE);

  if (subp == NULL)
    return FALSE;

  loop = g_main_loop_new (NULL, FALSE);

  data.loop = loop;
  data.refs = 1;

  if (output)
    {
      data.refs++;
      in = g_subprocess_get_stdout_pipe (subp);
      out = g_memory_output_stream_new_resizable ();
      g_output_stream_splice_async (out,
                                    in,
                                    G_OUTPUT_STREAM_SPLICE_NONE,
                                    0,
                                    NULL,
                                    spawn_output_spliced_cb,
                                    &data);
    }

  g_subprocess_wait_async (subp, NULL, spawn_exit_cb, &data);

  g_main_loop_run (loop);

  if (data.error)
    {
      g_propagate_error (error, data.error);
      g_clear_error (&data.splice_error);
      return FALSE;
    }

  if (out)
    {
      if (data.splice_error)
        {
          g_propagate_error (error, data.splice_error);
          return FALSE;
        }

      /* Null terminate */
      g_output_stream_write (out, "\0", 1, NULL, NULL);
      g_output_stream_close (out, NULL, NULL);
      *output = g_memory_output_stream_steal_data (G_MEMORY_OUTPUT_STREAM (out));
    }

  return TRUE;
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
    r = mkdir (gs_file_get_path_cached (dest), 0755);
  while (G_UNLIKELY (r == -1 && errno == EINTR));
  if (r == -1 &&
      (!merge || errno != EEXIST))
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (!gs_file_open_dir_fd (dest, &dest_dfd,
                            cancellable, error))
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

  while (TRUE)
    {
      GFileInfo *file_info = NULL;
      GFile *src_child = NULL;

      if (!gs_file_enumerator_iterate (enumerator, &file_info, &src_child,
                                       cancellable, error))
        goto out;
      if (!file_info)
        break;

      if (dest_child)
        g_object_unref (dest_child);
      dest_child = g_file_get_child (dest, g_file_info_get_name (file_info));

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!flatpak_cp_a (src_child, dest_child, flags,
                             cancellable, error))
            goto out;
        }
      else
        {
          (void) unlink (gs_file_get_path_cached (dest_child));
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

  n = g_variant_n_children (array);
  if (n == 0)
    return FALSE;

  imax = n - 1;
  imin = 0;
  while (imax >= imin)
    {
      g_autoptr(GVariant) child = NULL;
      const char *cur;
      int cmp;

      imid = (imin + imax) / 2;

      child = g_variant_get_child_value (array, imid);
      g_variant_get_child (child, 0, "&s", &cur, NULL);

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

gboolean
flatpak_summary_lookup_ref (GVariant *summary, const char *ref, char **out_checksum)
{
  g_autoptr(GVariant) refs = g_variant_get_child_value (summary, 0);
  int pos;
  g_autoptr(GVariant) refdata = NULL;
  g_autoptr(GVariant) reftargetdata = NULL;
  guint64 commit_size;
  g_autoptr(GVariant) commit_csum_v = NULL;

  if (!flatpak_variant_bsearch_str (refs, ref, &pos))
    return FALSE;

  refdata = g_variant_get_child_value (refs, pos);
  reftargetdata = g_variant_get_child_value (refdata, 1);
  g_variant_get (reftargetdata, "(t@ay@a{sv})", &commit_size, &commit_csum_v, NULL);

  if (!ostree_validate_structureof_csum_v (commit_csum_v, NULL))
    return FALSE;

  if (out_checksum)
    *out_checksum = ostree_checksum_from_bytes_v (commit_csum_v);

  return TRUE;
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
  GError *temp_error = NULL;

  if (file_info != NULL && g_file_info_get_file_type (file_info) == G_FILE_TYPE_REGULAR)
    {
      const char *checksum = ostree_repo_file_get_checksum (OSTREE_REPO_FILE (file));
      guint64 obj_size;
      guint64 file_size = g_file_info_get_size (file_info);

      if (installed_size)
        *installed_size += ((file_size + 511) / 512) * 512;

      if (download_size)
        {
          if (!ostree_repo_query_object_storage_size (repo,
                                                      OSTREE_OBJECT_TYPE_FILE, checksum,
                                                      &obj_size, cancellable, error))
            return FALSE;

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

gboolean
flatpak_repo_update (OstreeRepo   *repo,
                     const char  **gpg_key_ids,
                     const char   *gpg_homedir,
                     GCancellable *cancellable,
                     GError      **error)
{
  GVariantBuilder builder;
  GVariantBuilder ref_data_builder;
  GKeyFile *config;
  g_autofree char *title = NULL;

  g_autoptr(GHashTable) refs = NULL;
  GList *ordered_keys = NULL;
  GList *l = NULL;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);

  config = ostree_repo_get_config (repo);

  if (config)
    title = g_key_file_get_string (config, "flatpak", "title", NULL);

  if (title)
    g_variant_builder_add (&builder, "{sv}", "xa.title",
                           g_variant_new_string (title));


  g_variant_builder_init (&ref_data_builder, G_VARIANT_TYPE ("a{s(tts)}"));

  if (!ostree_repo_list_refs (repo, NULL, &refs, cancellable, error))
    return FALSE;

  ordered_keys = g_hash_table_get_keys (refs);
  ordered_keys = g_list_sort (ordered_keys, (GCompareFunc) strcmp);

  for (l = ordered_keys; l; l = l->next)
    {
      const char *ref = l->data;
      g_autoptr(GFile) root = NULL;
      g_autoptr(GFile) metadata = NULL;
      guint64 installed_size = 0;
      guint64 download_size = 0;
      g_autofree char *metadata_contents = NULL;

      if (!ostree_repo_read_commit (repo, ref, &root, NULL, NULL, error))
        return FALSE;

      if (!flatpak_repo_collect_sizes (repo, root, &installed_size, &download_size, cancellable, error))
        return FALSE;

      metadata = g_file_get_child (root, "metadata");
      if (!g_file_load_contents (metadata, cancellable, &metadata_contents, NULL, NULL, NULL))
        metadata_contents = g_strdup ("");

      g_variant_builder_add (&ref_data_builder, "{s(tts)}",
                             ref,
                             GUINT64_TO_BE (installed_size),
                             GUINT64_TO_BE (download_size),
                             metadata_contents);
    }

  g_variant_builder_add (&builder, "{sv}", "xa.cache",
                         g_variant_new_variant (g_variant_builder_end (&ref_data_builder)));

  if (!ostree_repo_regenerate_summary (repo, g_variant_builder_end (&builder),
                                       cancellable, error))
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
}

gboolean
flatpak_mtree_create_root (OstreeRepo        *repo,
                           OstreeMutableTree *mtree,
                           GCancellable      *cancellable,
                           GError           **error)
{
  g_autoptr(GVariant) dirmeta = NULL;
  g_autoptr(GFileInfo) file_info = g_file_info_new ();
  g_autofree guchar *csum;
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

static OstreeRepoCommitFilterResult
commit_filter (OstreeRepo *repo,
               const char *path,
               GFileInfo  *file_info,
               gpointer    user_data)
{
  guint current_mode;

  /* No user info */
  g_file_info_set_attribute_uint32 (file_info, "unix::uid", 0);
  g_file_info_set_attribute_uint32 (file_info, "unix::gid", 0);

  /* No setuid */
  current_mode = g_file_info_get_attribute_uint32 (file_info, "unix::mode");
  g_file_info_set_attribute_uint32 (file_info, "unix::mode", current_mode & ~07000);

  return OSTREE_REPO_COMMIT_FILTER_ALLOW;
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
  if (!g_str_has_prefix (id_text, id) ||
      !g_str_has_suffix (id_text, ".desktop"))
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
  if (runtime)
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
    group = "Application";
  else
    group = "Runtime";

  tags = g_key_file_get_string_list (metadata, group, "tags", NULL, NULL);
  runtime = g_key_file_get_string (metadata, group, "runtime", NULL);
  sdk = g_key_file_get_string (metadata, group, "sdk", NULL);

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
copy_icon (const char *id,
           GFile      *root,
           GFile      *dest,
           const char *size,
           GError    **error)
{
  g_autofree char *icon_name = g_strconcat (id, ".png", NULL);

  g_autoptr(GFile) icons_dir =
    g_file_resolve_relative_path (root,
                                  "files/share/app-info/icons/flatpak");
  g_autoptr(GFile) size_dir = g_file_get_child (icons_dir, size);
  g_autoptr(GFile) icon_file = g_file_get_child (size_dir, icon_name);
  g_autoptr(GFile) dest_dir = g_file_get_child (dest, "icons");
  g_autoptr(GFile) dest_size_dir = g_file_get_child (dest_dir, size);
  g_autoptr(GFile) dest_file = g_file_get_child (dest_size_dir, icon_name);
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(GOutputStream) out = NULL;
  gssize n_bytes_written;

  in = (GInputStream *) g_file_read (icon_file, NULL, error);
  if (!in)
    return FALSE;

  if (!gs_file_ensure_directory (dest_size_dir, TRUE, NULL, error))
    return FALSE;

  out = (GOutputStream *) g_file_replace (dest_file, NULL, FALSE,
                                          G_FILE_CREATE_REPLACE_DESTINATION,
                                          NULL, error);
  if (!out)
    return FALSE;

  n_bytes_written = g_output_stream_splice (out, in,
                                            G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                            NULL, error);
  if (n_bytes_written < 0)
    return FALSE;

  return TRUE;
}

static gboolean
extract_appstream (OstreeRepo   *repo,
                   FlatpakXml   *appstream_root,
                   const char   *ref,
                   const char   *id,
                   GFile        *dest,
                   GCancellable *cancellable,
                   GError      **error)
{
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) xmls_dir = NULL;
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

  xmls_dir = g_file_resolve_relative_path (root, "files/share/app-info/xmls");
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

          if (g_strcmp0 (component->element_name, "component") != 0)
            {
              component = component->next_sibling;
              continue;
            }

          component_id = flatpak_xml_find (component, "id", NULL);
          component_id_text_node = flatpak_xml_find (component_id, NULL, NULL);

          component_id_text = g_strstrip (g_strdup (component_id_text_node->text));
          if (!g_str_has_suffix (component_id_text, ".desktop"))
            {
              component = component->next_sibling;
              continue;
            }

          g_print ("Extracting icons for component %s\n", component_id_text);
          component_id_text[strlen (component_id_text) - strlen (".desktop")] = 0;

          if (!copy_icon (component_id_text, root, dest, "64x64", &my_error))
            {
              g_print ("Error copying 64x64 icon: %s\n", my_error->message);
              g_clear_error (&my_error);
            }
          if (!copy_icon (component_id_text, root, dest, "128x128", &my_error))
            {
              g_print ("Error copying 128x128 icon: %s\n", my_error->message);
              g_clear_error (&my_error);
            }

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

GBytes *
flatpak_appstream_xml_root_to_data (FlatpakXml *appstream_root,
                                    GError    **error)
{
  g_autoptr(GString) xml = NULL;
  g_autoptr(GZlibCompressor) compressor = NULL;
  g_autoptr(GOutputStream) out2 = NULL;
  g_autoptr(GOutputStream) out = NULL;

  flatpak_xml_add (appstream_root->first_child, flatpak_xml_new_text ("\n"));

  xml = g_string_new ("");
  flatpak_xml_to_string (appstream_root, xml);

  compressor = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1);
  out = g_memory_output_stream_new_resizable ();
  out2 = g_converter_output_stream_new (out, G_CONVERTER (compressor));
  if (!g_output_stream_write_all (out2, xml->str, xml->len,
                                  NULL, NULL, error))
    return NULL;
  if (!g_output_stream_close (out2, NULL, error))
    return NULL;

  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out));
}

gboolean
flatpak_repo_generate_appstream (OstreeRepo   *repo,
                                 const char  **gpg_key_ids,
                                 const char   *gpg_homedir,
                                 GCancellable *cancellable,
                                 GError      **error)
{
  g_autoptr(GHashTable) all_refs = NULL;
  g_autoptr(GHashTable) arches = NULL;
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  gboolean skip_commit = FALSE;

  arches = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  if (!ostree_repo_list_refs (repo,
                              NULL,
                              &all_refs,
                              cancellable,
                              error))
    return FALSE;

  g_hash_table_iter_init (&iter, all_refs);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *ref = key;
      const char *arch;
      g_auto(GStrv) split = NULL;

      split = flatpak_decompose_ref (ref, NULL);
      if (!split)
        continue;

      arch = split[2];
      if (!g_hash_table_contains (arches, arch))
        g_hash_table_insert (arches, g_strdup (arch), GINT_TO_POINTER (1));
    }

  g_hash_table_iter_init (&iter, arches);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      GHashTableIter iter2;
      const char *arch = key;
      g_autofree char *tmpdir = g_strdup ("/tmp/flatpak-appstream-XXXXXX");
      g_autoptr(FlatpakTempDir) tmpdir_file = NULL;
      g_autoptr(GFile) appstream_file = NULL;
      g_autoptr(GFile) root = NULL;
      g_autoptr(OstreeMutableTree) mtree = NULL;
      g_autofree char *commit_checksum = NULL;
      OstreeRepoTransactionStats stats;
      g_autoptr(OstreeRepoCommitModifier) modifier = NULL;
      g_autofree char *parent = NULL;
      g_autofree char *branch = NULL;
      g_autoptr(FlatpakXml) appstream_root = NULL;
      g_autoptr(GBytes) xml_data = NULL;

      if (g_mkdtemp_full (tmpdir, 0755) == NULL)
        return flatpak_fail (error, "Can't create temporary directory");

      tmpdir_file = g_file_new_for_path (tmpdir);

      appstream_root = flatpak_appstream_xml_new ();

      g_hash_table_iter_init (&iter2, all_refs);
      while (g_hash_table_iter_next (&iter2, &key, &value))
        {
          const char *ref = key;
          g_auto(GStrv) split = NULL;
          g_autoptr(GError) my_error = NULL;

          split = flatpak_decompose_ref (ref, NULL);
          if (!split)
            continue;

          if (strcmp (split[2], arch) != 0)
            continue;

          if (!extract_appstream (repo, appstream_root,
                                  ref, split[1], tmpdir_file,
                                  cancellable, &my_error))
            {
              g_print ("No appstream data for %s: %s\n", ref, my_error->message);
              continue;
            }
        }

      xml_data = flatpak_appstream_xml_root_to_data (appstream_root, error);
      if (xml_data == NULL)
        return FALSE;

      appstream_file = g_file_get_child (tmpdir_file, "appstream.xml.gz");

      if (!g_file_replace_contents (appstream_file,
                                    g_bytes_get_data (xml_data, NULL),
                                    g_bytes_get_size (xml_data),
                                    NULL,
                                    FALSE,
                                    G_FILE_CREATE_NONE,
                                    NULL,
                                    cancellable,
                                    error))
        return FALSE;

      if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
        return FALSE;

      branch = g_strdup_printf ("appstream/%s", arch);

      if (!ostree_repo_resolve_rev (repo, branch, TRUE, &parent, error))
        goto out;

      mtree = ostree_mutable_tree_new ();

      modifier = ostree_repo_commit_modifier_new (OSTREE_REPO_COMMIT_MODIFIER_FLAGS_SKIP_XATTRS,
                                                  (OstreeRepoCommitFilter) commit_filter, NULL, NULL);

      if (!ostree_repo_write_directory_to_mtree (repo, G_FILE (tmpdir_file), mtree, modifier, cancellable, error))
        goto out;

      if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
        goto out;


      /* No need to commit if nothing changed */
      if (parent)
        {
          g_autoptr(GFile) parent_root;

          if (!ostree_repo_read_commit (repo, parent, &parent_root, NULL, cancellable, error))
            goto out;

          if (g_file_equal (root, parent_root))
            skip_commit = TRUE;
        }

      if (!skip_commit)
        {
          if (!ostree_repo_write_commit (repo, parent, "Update", NULL, NULL,
                                         OSTREE_REPO_FILE (root),
                                         &commit_checksum, cancellable, error))
            goto out;

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
                    goto out;
                }
            }

          ostree_repo_transaction_set_ref (repo, NULL, branch, commit_checksum);

          if (!ostree_repo_commit_transaction (repo, &stats, cancellable, error))
            goto out;
        }
      else
        {
          ostree_repo_abort_transaction (repo, cancellable, NULL);
        }
    }

  return TRUE;

out:
  ostree_repo_abort_transaction (repo, cancellable, NULL);
  return FALSE;
}

void
flatpak_extension_free (FlatpakExtension *extension)
{
  g_free (extension->id);
  g_free (extension->installed_id);
  g_free (extension->ref);
  g_free (extension->directory);
  g_free (extension);
}

static FlatpakExtension *
flatpak_extension_new (const char *id,
                       const char *extension,
                       const char *arch,
                       const char *branch,
                       const char *directory)
{
  FlatpakExtension *ext = g_new0 (FlatpakExtension, 1);

  ext->id = g_strdup (id);
  ext->installed_id = g_strdup (extension);
  ext->ref = g_build_filename ("runtime", extension, arch, branch, NULL);
  ext->directory = g_strdup (directory);
  return ext;
}

GList *
flatpak_list_extensions (GKeyFile   *metakey,
                         const char *arch,
                         const char *default_branch)
{
  g_auto(GStrv) groups = NULL;
  int i;
  GList *res;

  res = NULL;

  if (arch == NULL)
    arch = flatpak_get_arch ();

  groups = g_key_file_get_groups (metakey, NULL);
  for (i = 0; groups[i] != NULL; i++)
    {
      FlatpakExtension *ext;
      char *extension;

      if (g_str_has_prefix (groups[i], "Extension ") &&
          *(extension = (groups[i] + strlen ("Extension "))) != 0)
        {
          g_autofree char *directory = g_key_file_get_string (metakey, groups[i], "directory", NULL);
          g_autofree char *version = g_key_file_get_string (metakey, groups[i], "version", NULL);
          g_autofree char *ref = NULL;
          const char *branch;
          g_autoptr(GFile) deploy = NULL;

          if (directory == NULL)
            continue;

          if (version)
            branch = version;
          else
            branch = default_branch;

          ref = g_build_filename ("runtime", extension, arch, branch, NULL);

          deploy = flatpak_find_deploy_dir_for_ref (ref, NULL, NULL);
          /* Prefer a full extension (org.freedesktop.Locale) over subdirectory ones (org.freedesktop.Locale.sv) */
          if (deploy != NULL)
            {
              ext = flatpak_extension_new (extension, extension, arch, branch, directory);
              res = g_list_prepend (res, ext);
            }
          else if (g_key_file_get_boolean (metakey, groups[i],
                                           "subdirectories", NULL))
            {
              g_autofree char *prefix = g_strconcat (extension, ".", NULL);
              g_auto(GStrv) refs = NULL;
              int j;

              refs = flatpak_list_deployed_refs ("runtime", prefix, arch, branch,
                                                 NULL, NULL);
              for (j = 0; refs != NULL && refs[j] != NULL; j++)
                {
                  g_autofree char *extended_dir = g_build_filename (directory, refs[j] + strlen (prefix), NULL);

                  ext = flatpak_extension_new (extension, refs[j], arch, branch, extended_dir);
                  res = g_list_prepend (res, ext);
                }
            }
        }
    }

  return res;
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
  guint64 total_size = 0, total_usize = 0;

  g_autoptr(GVariant) meta_entries = NULL;
  guint i, n_parts;

  g_variant_get_child (bundle, 6, "@a" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT, &meta_entries);
  n_parts = g_variant_n_children (meta_entries);
  g_print ("Number of parts: %u\n", n_parts);

  for (i = 0; i < n_parts; i++)
    {
      guint32 version;
      guint64 size, usize;
      g_autoptr(GVariant) objects = NULL;

      g_variant_get_child (meta_entries, i, "(u@aytt@ay)",
                           &version, NULL, &size, &usize, &objects);

      total_size += maybe_swap_endian_u64 (byte_swap, size);
      total_usize += maybe_swap_endian_u64 (byte_swap, usize);
    }

  return total_usize;
}

GVariant *
flatpak_bundle_load (GFile   *file,
                     char   **commit,
                     char   **ref,
                     char   **origin,
                     guint64 *installed_size,
                     GBytes **gpg_keys,
                     GError **error)
{
  g_autoptr(GVariant) delta = NULL;
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GVariant) to_csum_v = NULL;
  guint8 endianness_char;
  gboolean byte_swap = FALSE;

  GMappedFile *mfile = g_mapped_file_new (gs_file_get_path_cached (file), FALSE, error);

  if (mfile == NULL)
    return NULL;

  bytes = g_mapped_file_get_bytes (mfile);
  g_mapped_file_unref (mfile);

  delta = g_variant_new_from_bytes (G_VARIANT_TYPE (OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT), bytes, FALSE);
  g_variant_ref_sink (delta);

  to_csum_v = g_variant_get_child_value (delta, 3);
  if (!ostree_validate_structureof_csum_v (to_csum_v, error))
    return NULL;

  if (commit)
    *commit = ostree_checksum_from_bytes_v (to_csum_v);

  if (installed_size)
    *installed_size = flatpak_bundle_get_installed_size (delta, byte_swap);

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


  if (ref != NULL)
    {
      if (!g_variant_lookup (metadata, "ref", "s", ref))
        {
          flatpak_fail (error, "Invalid bundle, no ref in metadata");
          return NULL;
        }
    }

  if (origin != NULL)
    {
      if (!g_variant_lookup (metadata, "origin", "s", origin))
        *origin = NULL;
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
  return g_variant_new_from_bytes (g_variant_get_type (metadata),
                                   g_bytes_new (g_variant_get_data (metadata),
                                                g_variant_get_size (metadata)),
                                   FALSE);
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

  metadata = flatpak_bundle_load (file, &to_checksum, NULL, NULL, NULL, NULL, error);
  if (metadata == NULL)
    return FALSE;

  g_variant_lookup (metadata, "metadata", "s", &metadata_contents);

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return FALSE;

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
      /* NOT_FOUND means no gpg signature, we ignore this *if* there
       * is no gpg key specified in the bundle or by the user */
      if (g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
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
        return flatpak_fail (error, "GPG signatures found, but none are in trusted keyring");
    }

  if (!ostree_repo_read_commit (repo, to_checksum, &root, NULL, NULL, error))
    return FALSE;

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    return FALSE;

  /* We ensure that the actual installed metadata matches the one in the
     header, because you may have made decisions on wheter to install it or not
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
      return flatpak_fail (error, "Metadata in header and app are inconsistent");
    }

  return TRUE;
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
  glnx_fd_close int tmpdir_fd = -1;

  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

  /* Look for existing tmpdir (with same prefix) to reuse */
  if (!glnx_dirfd_iterator_init_at (tmpdir_dfd, tmpdir_relpath ? tmpdir_relpath : ".", FALSE, &dfd_iter, error))
    return FALSE;

  while (tmpdir_name == NULL)
    {
      struct dirent *dent;
      glnx_fd_close int existing_tmpdir_fd = -1;
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
      glnx_fd_close int new_tmpdir_fd = -1;
      g_autoptr(GError) local_error = NULL;
      g_autofree char *lock_name = NULL;

      /* No existing tmpdir found, create a new */

      if (!glnx_mkdtempat (tmpdir_dfd, tmpdir_name_template, 0777, error))
        return FALSE;

      if (!glnx_opendirat (tmpdir_dfd, tmpdir_name_template, FALSE,
                           &new_tmpdir_fd, error))
        return FALSE;

      lock_name = g_strconcat (tmpdir_name_template, "-lock", NULL);

      /* Note, at this point we can race with another process that picks up this
       * new directory. If that happens we need to retry, making a new directory. */
      if (!glnx_make_lock_file (tmpdir_dfd, lock_name, LOCK_EX | LOCK_NB,
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

      tmpdir_name = g_steal_pointer (&tmpdir_name_template);
      tmpdir_fd = glnx_steal_fd (&new_tmpdir_fd);
    }

  if (tmpdir_name_out)
    *tmpdir_name_out = g_steal_pointer (&tmpdir_name);

  if (tmpdir_fd_out)
    *tmpdir_fd_out = glnx_steal_fd (&tmpdir_fd);

  if (reusing_dir_out)
    *reusing_dir_out = reusing_dir;

  return TRUE;
}
