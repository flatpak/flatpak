/*
 * Copyright © 2014 Red Hat, Inc
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

#include "flatpak-utils.h"
#include "lib/flatpak-error.h"
#include "flatpak-dir.h"
#include "flatpak-portal-error.h"
#include "flatpak-run.h"

#include <glib/gi18n.h>

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
#include "libglnx/libglnx.h"
#include <libsoup/soup.h>
#include <gio/gunixoutputstream.h>

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

/* Get all compatible arches for this host in order of priority */
const char **
flatpak_get_arches (void)
{
  static gsize arches = 0;
  static struct {
    const char *kernel_arch;
    const char *compat_arch;
  } compat_arches[] = {
    { "x86_64", "i386" },
    { "aarch64", "arm" },
  };

  if (g_once_init_enter (&arches))
    {
      gsize new_arches = 0;
      const char *main_arch = flatpak_get_arch ();
      const char *kernel_arch = flatpak_get_kernel_arch ();
      GPtrArray *array = g_ptr_array_new ();
      int i;

      /* This is the userspace arch, i.e. the one flatpak itself was
         build for. It's always first. */
      g_ptr_array_add (array, (char *)main_arch);

      /* Also add all other arches that are compatible with the kernel arch */
      for (i = 0; i < G_N_ELEMENTS(compat_arches); i++)
        {
          if ((strcmp (compat_arches[i].kernel_arch, kernel_arch) == 0) &&
              /* Don't re-add the main arch */
              (strcmp (compat_arches[i].compat_arch, main_arch) != 0))
            g_ptr_array_add (array, (char *)compat_arches[i].compat_arch);
        }

      g_ptr_array_add (array, NULL);
      new_arches = (gsize)g_ptr_array_free (array, FALSE);

      g_once_init_leave (&arches, new_arches);
 }

  return (const char **)arches;
}

gboolean
flatpak_is_in_sandbox (void)
{
  static gsize in_sandbox = 0;

  if (g_once_init_enter (&in_sandbox))
    {
      g_autofree char *path = g_build_filename (g_get_user_runtime_dir (), "flatpak-info", NULL);
      gsize new_in_sandbox;

      new_in_sandbox = 2;
      if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
        new_in_sandbox = 1;

      g_once_init_leave (&in_sandbox, new_in_sandbox);
 }

  return in_sandbox == 1;
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

gboolean
flatpak_split_partial_ref_arg (char *partial_ref,
                               char **inout_arch,
                               char **inout_branch,
                               GError    **error)
{
  char *slash;
  char *arch = NULL;
  char *branch = NULL;

  if (partial_ref == NULL)
    return TRUE;

  slash = strchr (partial_ref, '/');
  if (slash != NULL)
    *slash = 0;

  if (!flatpak_is_valid_name (partial_ref))
    return flatpak_fail (error, "Invalid name %s", partial_ref);

  if (slash == NULL)
    goto out;

  arch = slash + 1;
  slash = strchr (arch, '/');
  if (slash != NULL)
    *slash = 0;

  if (strlen (arch) == 0)
    arch = NULL;

  if (slash == NULL)
    goto out;

  branch = slash + 1;
  if (strlen (branch) > 0)
    {
      if (!flatpak_is_valid_branch (branch))
        return flatpak_fail (error, "Invalid branch %s", branch);
    }
  else
    branch = NULL;

 out:

  if (*inout_arch == NULL)
    *inout_arch = arch;

  if (*inout_branch == NULL)
    *inout_branch = branch;

  return TRUE;
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
flatpak_find_files_dir_for_ref (const char   *ref,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_autoptr(FlatpakDir) user_dir = NULL;
  g_autoptr(FlatpakDir) system_dir = NULL;
  g_autoptr(GFile) deploy = NULL;

  user_dir = flatpak_dir_get_user ();
  system_dir = flatpak_dir_get_system ();

  deploy = flatpak_dir_get_if_deployed (user_dir, ref, NULL, cancellable);
  if (deploy == NULL)
    deploy = flatpak_dir_get_if_deployed (system_dir, ref, NULL, cancellable);
  if (deploy == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("%s not installed"), ref);
      return NULL;
    }

  return g_file_get_child (deploy, "files");
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

  if (!glnx_opendirat (destination_parent_fd, destination_name, TRUE,
                       &destination_dfd, error))
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

  if (!flatpak_mkdir_p (destination, cancellable, error))
    goto out;

  /* The fds are closed by this call */
  if (!overlay_symlink_tree_dir (AT_FDCWD, flatpak_file_get_path_cached (source),
                                 symlink_prefix,
                                 AT_FDCWD, flatpak_file_get_path_cached (destination),
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
  if (!remove_dangling_symlinks (AT_FDCWD, flatpak_file_get_path_cached (dir),
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
  char     *name;
  GKeyFile *app_info;
  gboolean  exited;
  GList    *pending;
} AppInfo;

static void
app_info_free (AppInfo *info)
{
  g_free (info->name);
  g_key_file_unref (info->app_info);
  g_free (info);
}

static void
ensure_app_ids (void)
{
  if (app_ids == NULL)
    app_ids = g_hash_table_new_full (g_str_hash, g_str_equal,
                                     NULL, (GDestroyNotify) app_info_free);
}

/* Returns NULL on failure, keyfile with name "" if not sandboxed, and full app-info otherwise */
static GKeyFile *
parse_app_id_from_fileinfo (int pid)
{
  g_autofree char *root_path = NULL;
  g_autofree char *path = NULL;
  g_autofree char *content = NULL;
  g_autofree char *app_id = NULL;
  glnx_fd_close int root_fd = -1;
  glnx_fd_close int info_fd = -1;
  struct stat stat_buf;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GMappedFile) mapped = NULL;
  g_autoptr(GKeyFile) metadata = NULL;

  root_path = g_strdup_printf ("/proc/%u/root", pid);
  root_fd = openat (AT_FDCWD, root_path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (root_fd == -1)
    {
      /* Not able to open the root dir shouldn't happen. Probably the app died and
         we're failing due to /proc/$pid not existing. In that case fail instead
         of treating this as privileged. */
      g_debug ("Unable to open %s", root_path);
      return NULL;
    }

  metadata = g_key_file_new ();

  info_fd = openat (root_fd, ".flatpak-info", O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (info_fd == -1)
    {
      if (errno == ENOENT)
        {
          /* No file => on the host */
          g_key_file_set_string (metadata, "Application", "name", "");
          return g_steal_pointer (&metadata);
        }

      return NULL; /* Some weird error => failure */
    }

  if (fstat (info_fd, &stat_buf) != 0 || !S_ISREG (stat_buf.st_mode))
    return NULL; /* Some weird fd => failure */

  mapped = g_mapped_file_new_from_fd  (info_fd, FALSE, &local_error);
  if (mapped == NULL)
    {
      g_warning ("Can't map .flatpak-info file: %s", local_error->message);
      return NULL;
    }

  if (!g_key_file_load_from_data (metadata,
                                  g_mapped_file_get_contents (mapped),
                                  g_mapped_file_get_length (mapped),
                                  G_KEY_FILE_NONE, &local_error))
    {
      g_warning ("Can't load .flatpak-info file: %s", local_error->message);
      return NULL;
    }

  return g_steal_pointer (&metadata);
}

static void
got_credentials_cb (GObject      *source_object,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  AppInfo *info = user_data;

  g_autoptr(GDBusMessage) reply = NULL;
  g_autoptr(GError) error = NULL;
  GList *l;

  reply = g_dbus_connection_send_message_with_reply_finish (G_DBUS_CONNECTION (source_object),
                                                            res, &error);

  if (!info->exited && reply != NULL)
    {
      GVariant *body = g_dbus_message_get_body (reply);
      guint32 pid;

      g_variant_get (body, "(u)", &pid);

      info->app_info = parse_app_id_from_fileinfo (pid);
    }

  for (l = info->pending; l != NULL; l = l->next)
    {
      GTask *task = l->data;

      if (info->app_info == NULL)
        g_task_return_new_error (task, FLATPAK_PORTAL_ERROR, FLATPAK_PORTAL_ERROR_FAILED,
                                 "Can't find app id");
      else
        g_task_return_pointer (task, g_key_file_ref (info->app_info), (GDestroyNotify)g_key_file_unref);
    }

  g_list_free_full (info->pending, g_object_unref);
  info->pending = NULL;

  if (info->app_info == NULL)
    g_hash_table_remove (app_ids, info->name);
}

void
flatpak_invocation_lookup_app_info (GDBusMethodInvocation *invocation,
                                    GCancellable          *cancellable,
                                    GAsyncReadyCallback    callback,
                                    gpointer               user_data)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);

  g_autoptr(GTask) task = NULL;
  AppInfo *info;

  task = g_task_new (invocation, cancellable, callback, user_data);

  ensure_app_ids ();

  info = g_hash_table_lookup (app_ids, sender);

  if (info == NULL)
    {
      info = g_new0 (AppInfo, 1);
      info->name = g_strdup (sender);
      g_hash_table_insert (app_ids, info->name, info);
    }

  if (info->app_info)
    {
      g_task_return_pointer (task, g_key_file_ref (info->app_info), (GDestroyNotify)g_key_file_unref);
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

GKeyFile *
flatpak_invocation_lookup_app_info_finish (GDBusMethodInvocation *invocation,
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
      AppInfo *info = g_hash_table_lookup (app_ids, name);

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
  GPtrArray *args;
  const gchar *arg;
  gboolean res;

  args = g_ptr_array_new ();
  g_ptr_array_add (args, (gchar *) argv0);
  while ((arg = va_arg (ap, const gchar *)))
    g_ptr_array_add (args, (gchar *) arg);
  g_ptr_array_add (args, NULL);

  res = flatpak_spawnv (dir, output, error, (const gchar * const *) args->pdata);

  g_ptr_array_free (args, TRUE);

  return res;
}

gboolean
flatpak_spawnv (GFile                *dir,
                char                **output,
                GError              **error,
                const gchar * const  *argv)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GSubprocess) subp = NULL;
  GInputStream *in;
  g_autoptr(GOutputStream) out = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  SpawnData data = {0};
  g_autofree gchar *commandline = NULL;

  launcher = g_subprocess_launcher_new (0);

  if (output)
    g_subprocess_launcher_set_flags (launcher, G_SUBPROCESS_FLAGS_STDOUT_PIPE);

  if (dir)
    {
      g_autofree char *path = g_file_get_path (dir);
      g_subprocess_launcher_set_cwd (launcher, path);
    }

  commandline = g_strjoinv (" ", (gchar **) argv);
  g_debug ("Running '%s'", commandline);

  subp = g_subprocess_launcher_spawnv (launcher, argv, error);

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

const char *
flatpak_file_get_path_cached (GFile *file)
{
  const char *path;
  static GQuark _file_path_quark = 0;

  if (G_UNLIKELY (_file_path_quark) == 0)
    _file_path_quark = g_quark_from_static_string ("flatpak-file-path");

  do
    {
      path = g_object_get_qdata ((GObject*)file, _file_path_quark);
      if (path == NULL)
        {
          g_autofree char *new_path = NULL;
          new_path = g_file_get_path (file);
          if (new_path == NULL)
            return NULL;

          if (g_object_replace_qdata ((GObject*)file, _file_path_quark,
                                      NULL, new_path, g_free, NULL))
            path = g_steal_pointer (&new_path);
        }
    }
  while (path == NULL);

  return path;
}

gboolean
flatpak_openat_noatime (int            dfd,
                        const char    *name,
                        int           *ret_fd,
                        GCancellable  *cancellable,
                        GError       **error)
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

gboolean
flatpak_zero_mtime (int parent_dfd,
                    const char *rel_path,
                    GCancellable  *cancellable,
                    GError       **error)
{
  struct stat stbuf;

  if (TEMP_FAILURE_RETRY (fstatat (parent_dfd, rel_path, &stbuf, AT_SYMLINK_NOFOLLOW)) != 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  if (S_ISDIR (stbuf.st_mode))
    {
      g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

      glnx_dirfd_iterator_init_at (parent_dfd, rel_path, FALSE, &dfd_iter, NULL);

      while (TRUE)
        {
          struct dirent *dent;

          if (!glnx_dirfd_iterator_next_dent (&dfd_iter, &dent, NULL, NULL) || dent == NULL)
            break;

          if (!flatpak_zero_mtime (dfd_iter.fd, dent->d_name,
                                   cancellable, error))
            return FALSE;
        }

      /* Update stbuf */
      if (TEMP_FAILURE_RETRY (fstat (dfd_iter.fd, &stbuf)) != 0)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }

  /* OSTree checks out to mtime 1, so we do the same */
  if (stbuf.st_mtime != 1)
    {
      const struct timespec times[2] = { { 0, UTIME_OMIT }, { 1, } };

      if (TEMP_FAILURE_RETRY (utimensat (parent_dfd, rel_path, times, AT_SYMLINK_NOFOLLOW)) != 0)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }
    }

  return TRUE;
}

/* Make a directory, and its parent. Don't error if it already exists.
 * If you want a failure mode with EEXIST, use g_file_make_directory_with_parents. */
gboolean
flatpak_mkdir_p (GFile         *dir,
                 GCancellable  *cancellable,
                 GError       **error)
{
  return glnx_shutil_mkdir_p_at (AT_FDCWD,
                                 flatpak_file_get_path_cached (dir),
                                 0777,
                                 cancellable,
                                 error);
}

gboolean
flatpak_rm_rf (GFile         *dir,
               GCancellable  *cancellable,
               GError       **error)
{
  return glnx_shutil_rm_rf_at (AT_FDCWD,
                               flatpak_file_get_path_cached (dir),
                               cancellable, error);
}

gboolean flatpak_file_rename (GFile *from,
                              GFile *to,
                              GCancellable  *cancellable,
                              GError       **error)
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

gboolean
flatpak_open_in_tmpdir_at (int                tmpdir_fd,
                           int                mode,
                           char              *tmpl,
                           GOutputStream    **out_stream,
                           GCancellable      *cancellable,
                           GError           **error)
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

/* This matches all refs that have ref, followed by '.'  as prefix */
char **
flatpak_summary_match_subrefs (GVariant *summary, const char *ref)
{
  g_autoptr(GVariant) refs = g_variant_get_child_value (summary, 0);
  GPtrArray *res = g_ptr_array_new ();
  gsize n, i;
  g_auto(GStrv) parts = NULL;
  g_autofree char *parts_prefix = NULL;

  parts = g_strsplit (ref, "/", 0);
  parts_prefix = g_strconcat (parts[1], ".", NULL);

  n = g_variant_n_children (refs);
  for (i = 0; i < n; i++)
    {
      g_autoptr(GVariant) child = NULL;
      g_auto(GStrv) cur_parts = NULL;
      const char *cur;

      child = g_variant_get_child_value (refs, i);
      g_variant_get_child (child, 0, "&s", &cur, NULL);

      cur_parts = g_strsplit (cur, "/", 0);

      /* Must match type, arch, branch */
      if (strcmp (parts[0], cur_parts[0]) != 0 ||
          strcmp (parts[2], cur_parts[2]) != 0 ||
          strcmp (parts[3], cur_parts[3]) != 0)
        continue;

      /* But only prefix of id */
      if (!g_str_has_prefix (cur_parts[1], parts_prefix))
        continue;

      g_ptr_array_add (res, g_strdup (cur));
    }

  g_ptr_array_add (res, NULL);
  return (char **)g_ptr_array_free (res, FALSE);
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

/* Loads a summary file from a local repo */
GVariant *
flatpak_repo_load_summary (OstreeRepo *repo,
                           GError **error)
{
  glnx_fd_close int fd = -1;
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

typedef struct {
  guint64 installed_size;
  guint64 download_size;
  char *metadata_contents;
} CommitData;

static void
commit_data_free (gpointer data)
{
  CommitData *rev_data = data;
  g_free (rev_data->metadata_contents);
  g_free (rev_data);
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
  g_autoptr(GVariant) old_summary = NULL;
  g_autoptr(GHashTable) refs = NULL;
  const char *prefixes[] = { "appstream", "app", "runtime", NULL };
  const char **prefix;
  g_autoptr(GList) ordered_keys = NULL;
  GList *l = NULL;
  g_autoptr(GHashTable) commit_data_cache = NULL;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);

  config = ostree_repo_get_config (repo);

  if (config)
    title = g_key_file_get_string (config, "flatpak", "title", NULL);

  if (title)
    g_variant_builder_add (&builder, "{sv}", "xa.title",
                           g_variant_new_string (title));


  g_variant_builder_init (&ref_data_builder, G_VARIANT_TYPE ("a{s(tts)}"));

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
  if (old_summary != NULL)
    {
      g_autoptr(GVariant) extensions = g_variant_get_child_value (old_summary, 1);
      g_autoptr(GVariant) cache_v = g_variant_lookup_value (extensions, "xa.cache", NULL);
      g_autoptr(GVariant) cache = NULL;
      if (cache_v != NULL)
        {
          cache = g_variant_get_child_value (cache_v, 0);
          gsize n, i;

          n = g_variant_n_children (cache);
          for (i = 0; i < n; i++)
            {
              g_autoptr(GVariant) old_element = g_variant_get_child_value (cache, i);
              g_autoptr(GVariant) old_ref_v = g_variant_get_child_value (old_element, 0);
              const char *old_ref = g_variant_get_string (old_ref_v, NULL);
              g_autofree char *old_rev = NULL;
              g_autoptr(GVariant) old_commit_data_v = g_variant_get_child_value (old_element, 1);
              CommitData *old_rev_data;

              if (flatpak_summary_lookup_ref (old_summary, old_ref, &old_rev))
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

                  old_rev_data = g_new (CommitData, 1);
                  old_rev_data->installed_size = old_installed_size;
                  old_rev_data->download_size = old_download_size;
                  old_rev_data->metadata_contents = g_steal_pointer (&old_metadata);

                  g_hash_table_insert (commit_data_cache, g_steal_pointer (&old_rev), old_rev_data);
                }
            }
        }
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
      CommitData *rev_data;

      /* See if we already have the info on this revision */
      if (g_hash_table_lookup (commit_data_cache, rev))
        continue;

      if (!ostree_repo_read_commit (repo, rev, &root, NULL, NULL, error))
        return FALSE;

      if (!flatpak_repo_collect_sizes (repo, root, &installed_size, &download_size, cancellable, error))
        return FALSE;

      metadata = g_file_get_child (root, "metadata");
      if (!g_file_load_contents (metadata, cancellable, &metadata_contents, NULL, NULL, NULL))
        metadata_contents = g_strdup ("");

      rev_data = g_new (CommitData, 1);
      rev_data->installed_size = installed_size;
      rev_data->download_size = download_size;
      rev_data->metadata_contents = g_strdup (metadata_contents);

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
  g_autoptr(GError) my_error = NULL;
  gssize n_bytes_written;

  in = (GInputStream *) g_file_read (icon_file, NULL, &my_error);
  if (!in)
    {
      if (g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_debug ("No icon at size %s", size);
          return TRUE;
        }

      g_propagate_error (error, g_steal_pointer (&my_error));
      return FALSE;
    }

  if (!flatpak_mkdir_p (dest_size_dir, NULL, error))
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
          if (!g_str_has_prefix (component_id_text, id) ||
              !g_str_has_suffix (component_id_text, ".desktop"))
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

          /* We updated icons for our component, so we're done */
          break;
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
              if (g_str_has_prefix (ref, "app/"))
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
  g_free (extension->files_path);
  g_free (extension);
}

static FlatpakExtension *
flatpak_extension_new (const char *id,
                       const char *extension,
                       const char *ref,
                       const char *directory,
                       GFile *files)
{
  FlatpakExtension *ext = g_new0 (FlatpakExtension, 1);

  ext->id = g_strdup (id);
  ext->installed_id = g_strdup (extension);
  ext->ref = g_strdup (ref);
  ext->directory = g_strdup (directory);
  ext->files_path = g_file_get_path (files);
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
          g_autoptr(GFile) files = NULL;

          if (directory == NULL)
            continue;

          if (version)
            branch = version;
          else
            branch = default_branch;

          ref = g_build_filename ("runtime", extension, arch, branch, NULL);

          files = flatpak_find_files_dir_for_ref (ref, NULL, NULL);
          /* Prefer a full extension (org.freedesktop.Locale) over subdirectory ones (org.freedesktop.Locale.sv) */
          if (files != NULL)
            {
              ext = flatpak_extension_new (extension, extension, ref, directory, files);
              res = g_list_prepend (res, ext);
            }
          else if (g_key_file_get_boolean (metakey, groups[i],
                                           "subdirectories", NULL))
            {
              g_autofree char *prefix = g_strconcat (extension, ".", NULL);
              g_auto(GStrv) refs = NULL;
              int j;
              gboolean needs_tmpfs = TRUE;

              refs = flatpak_list_deployed_refs ("runtime", prefix, arch, branch,
                                                 NULL, NULL);
              for (j = 0; refs != NULL && refs[j] != NULL; j++)
                {
                  g_autofree char *extended_dir = g_build_filename (directory, refs[j] + strlen (prefix), NULL);
                  g_autofree char *dir_ref = g_build_filename ("runtime", refs[j], arch, branch, NULL);
                  g_autoptr(GFile) subdir_files = flatpak_find_files_dir_for_ref (dir_ref, NULL, NULL);

                  if (subdir_files)
                    {
                      ext = flatpak_extension_new (extension, refs[j], dir_ref, extended_dir, subdir_files);
                      ext->needs_tmpfs = needs_tmpfs;
                      needs_tmpfs = FALSE; /* Only first subdir needs a tmpfs */
                      res = g_list_prepend (res, ext);
                    }
                }
            }
        }
    }

  return g_list_reverse (res);
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

      if (!glnx_mkdtempat (dfd_iter.fd, tmpdir_name_template, 0777, error))
        return FALSE;

      if (!glnx_opendirat (dfd_iter.fd, tmpdir_name_template, FALSE,
                           &new_tmpdir_fd, error))
        return FALSE;

      lock_name = g_strconcat (tmpdir_name_template, "-lock", NULL);

      /* Note, at this point we can race with another process that picks up this
       * new directory. If that happens we need to retry, making a new directory. */
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

/* Uncomment to get debug traces in /tmp/flatpak-completion-debug.txt (nice
 * to not have it interfere with stdout/stderr)
 */
#if 0
void
flatpak_completion_debug (const gchar *format, ...)
{
  va_list var_args;
  gchar *s;
  static FILE *f = NULL;

  va_start (var_args, format);
  s = g_strdup_vprintf (format, var_args);
  if (f == NULL)
    f = fopen ("/tmp/flatpak-completion-debug.txt", "a+");
  fprintf (f, "%s\n", s);
  fflush (f);
  g_free (s);
}
#else
void
flatpak_completion_debug (const gchar *format, ...)
{
}
#endif

static gboolean
is_word_separator (char c)
{
  return g_ascii_isspace (c);
}

void
flatpak_complete_file (FlatpakCompletion *completion)
{
  flatpak_completion_debug ("completing FILE");
  g_print ("%s\n", "__FLATPAK_FILE");
}

void
flatpak_complete_dir (FlatpakCompletion *completion)
{
  flatpak_completion_debug ("completing DIR");
  g_print ("%s\n", "__FLATPAK_DIR");
}

void
flatpak_complete_word (FlatpakCompletion *completion,
                       char *format, ...)
{
  va_list args;
  const char *rest;
  g_autofree char *string = NULL;

  g_return_if_fail (format != NULL);

  va_start (args, format);
  string = g_strdup_vprintf (format, args);
  va_end (args);

  if (!g_str_has_prefix (string, completion->cur))
    return;

  /* I'm not sure exactly what bash is doing here, but this seems to work... */
  if (strcmp (completion->shell_cur, "=") == 0)
    rest = string + strlen (completion->cur) - strlen (completion->shell_cur) + 1;
  else
    rest = string + strlen (completion->cur) - strlen (completion->shell_cur);

  flatpak_completion_debug ("completing word: '%s' (%s)", string, rest);

  g_print ("%s\n", rest);
}

void
flatpak_complete_ref (FlatpakCompletion *completion,
                      OstreeRepo *repo)
{
  g_autoptr(GHashTable) refs = NULL;
  flatpak_completion_debug ("completing REF");

  if (ostree_repo_list_refs (repo,
                             NULL,
                             &refs, NULL, NULL))
    {
      GHashTableIter hashiter;
      gpointer hashkey, hashvalue;

      g_hash_table_iter_init (&hashiter, refs);
      while ((g_hash_table_iter_next (&hashiter, &hashkey, &hashvalue)))
        {
          const char *ref = (const char *)hashkey;
          if (!(g_str_has_prefix (ref, "runtime/") ||
                g_str_has_prefix (ref, "app/")))
            continue;
          flatpak_complete_word (completion, "%s", ref);
        }
    }
}

static gboolean
switch_already_in_line (FlatpakCompletion *completion,
                        GOptionEntry      *entry)
{
  guint i = 0;
  guint line_part_len = 0;

  for (; i < completion->original_argc; ++i)
    {
      line_part_len = strlen (completion->original_argv[i]);
      if (line_part_len > 2 &&
          g_strcmp0 (&completion->original_argv[i][2], entry->long_name) == 0)
        return TRUE;

      if (line_part_len == 2 &&
          completion->original_argv[i][1] == entry->short_name)
        return TRUE;
    }

  return FALSE;
}

static gboolean
should_filter_out_option_from_completion (FlatpakCompletion *completion,
                                          GOptionEntry      *entry)
{
  switch (entry->arg)
    {
      case G_OPTION_ARG_NONE:
      case G_OPTION_ARG_STRING:
      case G_OPTION_ARG_INT:
      case G_OPTION_ARG_FILENAME:
      case G_OPTION_ARG_DOUBLE:
      case G_OPTION_ARG_INT64:
        return switch_already_in_line (completion, entry);
      default:
        return FALSE;
    }
}

void
flatpak_complete_options (FlatpakCompletion *completion,
                          GOptionEntry *entries)
{
  GOptionEntry *e = entries;
  int i;

  while (e->long_name != NULL)
    {
      if (e->arg_description)
        {
          g_autofree char *prefix = g_strdup_printf ("--%s=", e->long_name);

          if (g_str_has_prefix (completion->cur, prefix))
            {
              if (strcmp (e->arg_description, "ARCH") == 0)
                {
                  const char *arches[] = {"i386", "x86_64", "aarch64", "arm"};
                  for (i = 0; i < G_N_ELEMENTS (arches); i++)
                    flatpak_complete_word (completion, "%s%s ", prefix, arches[i]);
                }
              else if (strcmp (e->arg_description, "SHARE") == 0)
                {
                  for (i = 0; flatpak_context_shares[i] != NULL; i++)
                    flatpak_complete_word (completion, "%s%s ", prefix, flatpak_context_shares[i]);
                }
              else if (strcmp (e->arg_description, "DEVICE") == 0)
                {
                  for (i = 0; flatpak_context_devices[i] != NULL; i++)
                    flatpak_complete_word (completion, "%s%s ", prefix, flatpak_context_devices[i]);
                }
              else if (strcmp (e->arg_description, "FEATURE") == 0)
                {
                  for (i = 0; flatpak_context_features[i] != NULL; i++)
                    flatpak_complete_word (completion, "%s%s ", prefix, flatpak_context_features[i]);
                }
              else if (strcmp (e->arg_description, "SOCKET") == 0)
                {
                  for (i = 0; flatpak_context_sockets[i] != NULL; i++)
                    flatpak_complete_word (completion, "%s%s ", prefix, flatpak_context_sockets[i]);
                }
              else if (strcmp (e->arg_description, "FILE") == 0)
                {
                  flatpak_complete_file (completion);
                }
              else
                flatpak_complete_word (completion, "%s", prefix);
            }
          else
            flatpak_complete_word (completion, "%s", prefix);
        }
      else
        {
          /* If this is just a switch, then don't add it multiple
           * times */
          if (!should_filter_out_option_from_completion (completion, e)) {
            flatpak_complete_word (completion, "--%s ", e->long_name);
          }  else {
            flatpak_completion_debug ("switch --%s is already in line %s", e->long_name, completion->line);
          }
        }

      /* We may end up checking switch_already_in_line twice, but this is
       * for simplicity's sake - the alternative solution would be to
       * continue the loop early and have to increment e. */
      if (e->short_name != 0)
        {
          /* This is a switch, we may not want to add it */
          if (!e->arg_description)
            {
              if (!should_filter_out_option_from_completion (completion, e)) {
                flatpak_complete_word (completion, "-%c ", e->short_name);
              } else {
                flatpak_completion_debug ("switch -%c is already in line %s", e->short_name, completion->line);
              }
            }
          else
            {
              flatpak_complete_word (completion, "-%c ", e->short_name);
            }
        }
      e++;
    }
}

static gchar *
pick_word_at (const char  *s,
              int          cursor,
              int         *out_word_begins_at)
{
  int begin, end;

  if (s[0] == '\0')
    {
      if (out_word_begins_at != NULL)
        *out_word_begins_at = -1;
      return NULL;
    }

  if (is_word_separator (s[cursor]) && ((cursor > 0 && is_word_separator(s[cursor-1])) || cursor == 0))
    {
      if (out_word_begins_at != NULL)
        *out_word_begins_at = cursor;
      return g_strdup ("");
    }

  while (!is_word_separator (s[cursor - 1]) && cursor > 0)
    cursor--;
  begin = cursor;

  end = begin;
  while (!is_word_separator (s[end]) && s[end] != '\0')
    end++;

  if (out_word_begins_at != NULL)
    *out_word_begins_at = begin;

  return g_strndup (s + begin, end - begin);
}

static gboolean
parse_completion_line_to_argv (const char        *initial_completion_line,
                               FlatpakCompletion *completion)
{
  gboolean parse_result = g_shell_parse_argv (initial_completion_line,
                                              &completion->original_argc,
                                              &completion->original_argv,
                                              NULL);

  /* Make a shallow copy of argv, which will be our "working set" */
  completion->argc = completion->original_argc;
  completion->argv = g_memdup (completion->original_argv,
                               sizeof (gchar *) * (completion->original_argc + 1));

  return parse_result;
}

FlatpakCompletion *
flatpak_completion_new (const char *arg_line,
                        const char *arg_point,
                        const char *arg_cur)
{
  FlatpakCompletion *completion;
  g_autofree char *initial_completion_line = NULL;
  int _point;
  char *endp;
  int cur_begin;
  int i;

  _point = strtol (arg_point, &endp, 10);
  if (endp == arg_point || *endp != '\0')
    return NULL;

  completion = g_new0 (FlatpakCompletion, 1);
  completion->line = g_strdup (arg_line);
  completion->shell_cur = g_strdup (arg_cur);
  completion->point = _point;

  flatpak_completion_debug ("========================================");
  flatpak_completion_debug ("completion_point=%d", completion->point);
  flatpak_completion_debug ("completion_shell_cur='%s'", completion->shell_cur);
  flatpak_completion_debug ("----");
  flatpak_completion_debug (" 0123456789012345678901234567890123456789012345678901234567890123456789");
  flatpak_completion_debug ("'%s'", completion->line);
  flatpak_completion_debug (" %*s^", completion->point, "");

  /* compute cur and prev */
  completion->prev = NULL;
  completion->cur = pick_word_at (completion->line, completion->point, &cur_begin);
  if (cur_begin > 0)
    {
      gint prev_end;
      for (prev_end = cur_begin - 1; prev_end >= 0; prev_end--)
        {
          if (!is_word_separator (completion->line[prev_end]))
            {
              completion->prev = pick_word_at (completion->line, prev_end, NULL);
              break;
            }
        }

      initial_completion_line = g_strndup (completion->line, cur_begin);
    }
  else
    initial_completion_line = g_strdup ("");

  flatpak_completion_debug ("'%s'", initial_completion_line);
  flatpak_completion_debug ("----");

  flatpak_completion_debug (" cur='%s'", completion->cur);
  flatpak_completion_debug ("prev='%s'", completion->prev);

  if (!parse_completion_line_to_argv (initial_completion_line,
                                      completion))
    {
      /* it's very possible the command line can't be parsed (for
       * example, missing quotes etc) - in that case, we just
       * don't autocomplete at all
       */
      flatpak_completion_free (completion);
      return NULL;
    }

  flatpak_completion_debug ("completion_argv %i:", completion->original_argc);
  for (i = 0; i < completion->original_argc; i++)
    flatpak_completion_debug (completion->original_argv[i]);

  flatpak_completion_debug ("----");

  return completion;
}

void
flatpak_completion_free (FlatpakCompletion *completion)
{
  g_free (completion->cur);
  g_free (completion->prev);
  g_free (completion->line);
  g_free (completion->argv);
  g_strfreev (completion->original_argv);
  g_free (completion);
}

char **
flatpak_get_current_locale_subpaths (void)
{
  const gchar * const *langs = g_get_language_names ();
  GPtrArray *subpaths = g_ptr_array_new ();
  int i;

  for (i = 0; langs[i] != NULL; i++)
    {
      g_autofree char *dir = g_strconcat ("/", langs[i], NULL);
      char *c;

      c = strchr (dir, '@');
      if (c != NULL)
        *c = 0;
      c = strchr (dir, '_');
      if (c != NULL)
        *c = 0;
      c = strchr (dir, '.');
      if (c != NULL)
        *c = 0;

      if (strcmp (dir, "/C") == 0)
        continue;

      g_ptr_array_add (subpaths, g_steal_pointer (&dir));
    }

  g_ptr_array_add (subpaths, NULL);

  return (char **)g_ptr_array_free (subpaths, FALSE);
}
