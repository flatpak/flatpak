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

#include "xdg-app-utils.h"
#include "xdg-app-dir.h"
#include "xdg-app-portal-error.h"

#include <string.h>
#include <stdlib.h>
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

gint
xdg_app_strcmp0_ptr (gconstpointer  a,
                     gconstpointer  b)
{
  return g_strcmp0 (* (char * const *) a, * (char * const *) b);
}

/* Returns end of matching path prefix, or NULL if no match */
const char *
xdg_app_path_match_prefix (const char *pattern,
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
              tmp = xdg_app_path_match_prefix (pattern, string);
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
xdg_app_fail (GError **error, const char *format, ...)
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
xdg_app_get_arch (void)
{
  static struct utsname buf;
  static char *arch = NULL;

  if (arch == NULL)
    {
      if (uname (&buf))
        arch = "unknown";
      else
        arch = buf.machine;
    }

  return arch;
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

/** xdg_app_is_name:
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
xdg_app_is_valid_name (const char *string)
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
    {
      /* can't start with a . */
      goto out;
    }
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
        goto out;
      s += 1;
    }

  if (G_UNLIKELY (dot_count < 2))
    goto out;

  ret = TRUE;

 out:
  return ret;
}

gboolean
xdg_app_has_name_prefix (const char *string,
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

/** xdg_app_is_valid_branch:
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
xdg_app_is_valid_branch (const char *string)
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
xdg_app_decompose_ref (const char *full_ref,
                       GError **error)
{
  g_auto(GStrv) parts = NULL;

  parts = g_strsplit (full_ref, "/", 0);
  if (g_strv_length (parts) != 4)
    {
      xdg_app_fail (error, "Wrong number of components in %s", full_ref);
      return NULL;
    }

  if (strcmp (parts[0], "app") != 0 && strcmp (parts[0], "runtime") != 0)
    {
      xdg_app_fail (error, "Not application or runtime");
      return NULL;
    }

  if (!xdg_app_is_valid_name (parts[1]))
    {
      xdg_app_fail (error, "Invalid name %s", parts[1]);
      return NULL;
    }

  if (strlen (parts[2]) == 0)
    {
      xdg_app_fail (error, "Invalid arch %s", parts[2]);
      return NULL;
    }

  if (!xdg_app_is_valid_branch (parts[3]))
    {
      xdg_app_fail (error, "Invalid branch %s", parts[3]);
      return NULL;
    }

  return g_steal_pointer (&parts);
}

char *
xdg_app_compose_ref (gboolean app,
                     const char *name,
                     const char *branch,
                     const char *arch,
                     GError **error)
{
  if (!xdg_app_is_valid_name (name))
    {
      xdg_app_fail (error, "'%s' is not a valid name", name);
      return NULL;
    }

  if (branch && !xdg_app_is_valid_branch (branch))
    {
      xdg_app_fail (error, "'%s' is not a valid branch name", branch);
      return NULL;
    }

  if (app)
    return xdg_app_build_app_ref (name, branch, arch);
  else
    return xdg_app_build_runtime_ref (name, branch, arch);
}

char *
xdg_app_build_untyped_ref (const char *runtime,
                           const char *branch,
                           const char *arch)
{
  if (arch == NULL)
    arch = xdg_app_get_arch ();

  return g_build_filename (runtime, arch, branch, NULL);
}

char *
xdg_app_build_runtime_ref (const char *runtime,
                           const char *branch,
                           const char *arch)
{
  if (branch == NULL)
    branch = "master";

  if (arch == NULL)
    arch = xdg_app_get_arch ();

  return g_build_filename ("runtime", runtime, arch, branch, NULL);
}

char *
xdg_app_build_app_ref (const char *app,
                       const char *branch,
                       const char *arch)
{
  if (branch == NULL)
    branch = "master";

  if (arch == NULL)
    arch = xdg_app_get_arch ();

  return g_build_filename ("app", app, arch, branch, NULL);
}

char **
xdg_app_list_deployed_refs (const char *type,
                            const char *name_prefix,
                            const char *branch,
                            const char *arch,
                            GCancellable *cancellable,
                            GError **error)
{
  gchar **ret = NULL;
  g_autoptr(GPtrArray) names = NULL;
  g_autoptr(GHashTable) hash = NULL;
  g_autoptr(XdgAppDir) user_dir = NULL;
  g_autoptr(XdgAppDir) system_dir = NULL;
  const char *key;
  GHashTableIter iter;

  hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  user_dir = xdg_app_dir_get_user ();
  system_dir = xdg_app_dir_get_system ();

  if (!xdg_app_dir_collect_deployed_refs (user_dir, type, name_prefix,
                                          branch, arch, hash, cancellable,
                                          error))
    goto out;

  if (!xdg_app_dir_collect_deployed_refs (system_dir, type, name_prefix,
                                          branch, arch, hash, cancellable,
                                          error))
    goto out;

  names = g_ptr_array_new ();
  g_hash_table_iter_init (&iter, hash);
  while (g_hash_table_iter_next (&iter, (gpointer *)&key, NULL))
    g_ptr_array_add (names, g_strdup (key));

  g_ptr_array_sort (names, xdg_app_strcmp0_ptr);
  g_ptr_array_add (names, NULL);

  ret = (char **)g_ptr_array_free (names, FALSE);
  names = NULL;

 out:
  return ret;
}

GFile *
xdg_app_find_deploy_dir_for_ref (const char *ref,
                                 GCancellable *cancellable,
                                 GError **error)
{
  g_autoptr(XdgAppDir) user_dir = NULL;
  g_autoptr(XdgAppDir) system_dir = NULL;
  GFile *deploy = NULL;

  user_dir = xdg_app_dir_get_user ();
  system_dir = xdg_app_dir_get_system ();

  deploy = xdg_app_dir_get_if_deployed (user_dir, ref, NULL, cancellable);
  if (deploy == NULL)
    deploy = xdg_app_dir_get_if_deployed (system_dir, ref, NULL, cancellable);
  if (deploy == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s not installed", ref);
      return NULL;
    }

  return deploy;

}

XdgAppDeploy *
xdg_app_find_deploy_for_ref (const char *ref,
                             GCancellable *cancellable,
                             GError **error)
{
  g_autoptr(XdgAppDir) user_dir = NULL;
  g_autoptr(XdgAppDir) system_dir = NULL;
  g_autoptr(XdgAppDeploy) deploy = NULL;
  g_autoptr(GError) my_error = NULL;

  user_dir = xdg_app_dir_get_user ();
  system_dir = xdg_app_dir_get_system ();

  deploy = xdg_app_dir_load_deployed (user_dir, ref, NULL, cancellable, &my_error);
  if (deploy == NULL && g_error_matches (my_error, XDG_APP_DIR_ERROR, XDG_APP_DIR_ERROR_NOT_DEPLOYED))
    {
      g_clear_error (&my_error);
      deploy = xdg_app_dir_load_deployed (system_dir, ref, NULL, cancellable, &my_error);
    }
  if (deploy == NULL)
    g_propagate_error (error, g_steal_pointer (&my_error));

  return g_steal_pointer (&deploy);
}


static gboolean
overlay_symlink_tree_dir (int            source_parent_fd,
                          const char    *source_name,
                          const char    *source_symlink_prefix,
                          int            destination_parent_fd,
                          const char    *destination_name,
                          GCancellable  *cancellable,
                          GError       **error)
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
      gboolean is_dir = FALSE;

      if (!glnx_dirfd_iterator_next_dent (&source_iter, &dent, cancellable, error))
        goto out;

      if (dent == NULL)
        break;

      if (dent->d_type == DT_DIR)
        is_dir = TRUE;
      else if (dent->d_type == DT_UNKNOWN)
        {
          struct stat stbuf;
          if (fstatat (source_iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
            {
              if (errno == ENOENT)
                continue;
              else
                {
                  glnx_set_error_from_errno (error);
                  goto out;
                }
            }
          is_dir = S_ISDIR (stbuf.st_mode);
        }

      if (is_dir)
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
xdg_app_overlay_symlink_tree (GFile    *source,
                              GFile    *destination,
                              const char *symlink_prefix,
                              GCancellable  *cancellable,
                              GError       **error)
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
remove_dangling_symlinks (int            parent_fd,
                          const char    *name,
                          GCancellable  *cancellable,
                          GError       **error)
{
  gboolean ret = FALSE;
  struct dirent *dent;
  GLnxDirFdIterator iter;

  if (!glnx_dirfd_iterator_init_at (parent_fd, name, FALSE, &iter, error))
    goto out;

  while (TRUE)
    {
      gboolean is_dir = FALSE;
      gboolean is_link = FALSE;

      if (!glnx_dirfd_iterator_next_dent (&iter, &dent, cancellable, error))
        goto out;

      if (dent == NULL)
        break;

      if (dent->d_type == DT_DIR)
        is_dir = TRUE;
      else if (dent->d_type == DT_LNK)
        is_link = TRUE;
      else if (dent->d_type == DT_UNKNOWN)
        {
          struct stat stbuf;
          if (fstatat (iter.fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
            {
              if (errno == ENOENT)
                continue;
              else
                {
                  glnx_set_error_from_errno (error);
                  goto out;
                }
            }
          is_dir = S_ISDIR (stbuf.st_mode);
          is_link = S_ISLNK (stbuf.st_mode);
        }

      if (is_dir)
        {
          if (!remove_dangling_symlinks (iter.fd, dent->d_name, cancellable, error))
            goto out;
        }
      else if (is_link)
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
xdg_app_remove_dangling_symlinks (GFile    *dir,
                                  GCancellable  *cancellable,
                                  GError       **error)
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
xdg_app_mkstempat (int dir_fd,
                   gchar *tmpl,
                   int flags,
                   int mode)
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

struct XdgAppTablePrinter {
  GPtrArray *rows;
  GPtrArray *current;
  int n_columns;
};

XdgAppTablePrinter *
xdg_app_table_printer_new (void)
{
  XdgAppTablePrinter *printer = g_new0 (XdgAppTablePrinter, 1);
  printer->rows = g_ptr_array_new_with_free_func ((GDestroyNotify)g_strfreev);
  printer->current = g_ptr_array_new_with_free_func (g_free);

  return printer;
}

void
xdg_app_table_printer_free (XdgAppTablePrinter *printer)
{
  g_ptr_array_free (printer->rows, TRUE);
  g_ptr_array_free (printer->current, TRUE);
  g_free (printer);
}

void
xdg_app_table_printer_add_column (XdgAppTablePrinter *printer,
                                  const char *text)
{
  g_ptr_array_add (printer->current, text ? g_strdup (text) : g_strdup (""));
}

void
xdg_app_table_printer_append_with_comma (XdgAppTablePrinter *printer,
                                         const char *text)
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
xdg_app_table_printer_finish_row (XdgAppTablePrinter *printer)
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
xdg_app_table_printer_print (XdgAppTablePrinter *printer)
{
  g_autofree int *widths = NULL;
  int i, j;

  if (printer->current->len != 0)
    xdg_app_table_printer_finish_row (printer);

  widths = g_new0 (int, printer->n_columns);

  for (i = 0; i < printer->rows->len; i++)
    {
      char **row = g_ptr_array_index (printer->rows,i);

      for (j = 0; row[j] != NULL; j++)
        widths[j] = MAX (widths[j], strlen (row[j]));
    }

  for (i = 0; i < printer->rows->len; i++)
    {
      char **row = g_ptr_array_index (printer->rows,i);

      for (j = 0; row[j] != NULL; j++)
        g_print ("%s%-*s", (j == 0) ? "" : " ", widths[j], row[j]);
      g_print ("\n");
    }
}


static GHashTable *app_ids;

typedef struct {
  char *name;
  char *app_id;
  gboolean exited;
  GList *pending;
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
                                     NULL, (GDestroyNotify)app_id_info_free);
}

static void
got_credentials_cb (GObject *source_object,
                    GAsyncResult *res,
                    gpointer user_data)
{
  AppIdInfo *info = user_data;
  g_autoptr (GDBusMessage) reply = NULL;
  g_autoptr (GError) error = NULL;
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

                  if (g_str_has_prefix (scope, "xdg-app-") &&
                      g_str_has_suffix (scope, ".scope"))
                    {
                      const char *name = scope + strlen("xdg-app-");
                      char *dash = strchr (name, '-');
                      if (dash != NULL)
                        {
                          *dash = 0;
                          info->app_id = g_strdup (name);
                        }
                    }
                  else
                    info->app_id = g_strdup ("");
                }
            }
          g_strfreev (lines);
        }
    }

  for (l = info->pending; l != NULL; l = l->next)
    {
      GTask *task = l->data;

      if (info->app_id == NULL)
        g_task_return_new_error (task, XDG_APP_PORTAL_ERROR, XDG_APP_PORTAL_ERROR_FAILED,
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
xdg_app_invocation_lookup_app_id (GDBusMethodInvocation *invocation,
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
    g_task_return_pointer (task, g_strdup (info->app_id), g_free);
  else
    {
      if (info->pending == NULL)
        {
          g_autoptr (GDBusMessage) msg = g_dbus_message_new_method_call ("org.freedesktop.DBus",
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
xdg_app_invocation_lookup_app_id_finish (GDBusMethodInvocation *invocation,
                                         GAsyncResult    *result,
                                         GError         **error)
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
xdg_app_connection_track_name_owners (GDBusConnection *connection)
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

gboolean
xdg_app_supports_bundles (OstreeRepo *repo)
{
  g_autofree char *tmpfile = g_build_filename (g_get_tmp_dir (), ".xdg-app-test-ostree-XXXXXX", NULL);
  g_autoptr(GFile) file = NULL;
  g_autoptr(GError) error = NULL;
  int fd;
  gboolean res;

  fd = g_mkstemp (tmpfile);
  if (fd == -1)
    return FALSE;

  close (fd);

  res = TRUE;

  file = g_file_new_for_path (tmpfile);
  if (!ostree_repo_static_delta_execute_offline (repo, file, FALSE, NULL, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY))
        res = FALSE;
    }

  unlink (tmpfile);
  return res;
}

typedef struct
{
  GError *error;
  GError *splice_error;
  GMainLoop *loop;
  int refs;
} SpawnData;

static void
spawn_data_exit (SpawnData *data)
{
  data->refs--;
  if (data->refs == 0)
    g_main_loop_quit (data->loop);
}

static void
spawn_output_spliced_cb (GObject    *obj,
                       GAsyncResult  *result,
                       gpointer       user_data)
{
  SpawnData *data = user_data;

  g_output_stream_splice_finish (G_OUTPUT_STREAM (obj), result, &data->splice_error);
  spawn_data_exit (data);
}

static void
spawn_exit_cb (GObject    *obj,
               GAsyncResult  *result,
               gpointer       user_data)
{
  SpawnData *data = user_data;

  g_subprocess_wait_check_finish (G_SUBPROCESS (obj), result, &data->error);
  spawn_data_exit (data);
}

gboolean
xdg_app_spawn (GFile        *dir,
               char        **output,
               GError      **error,
               const gchar  *argv0,
               va_list       ap)
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
      g_output_stream_splice_async  (out,
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
xdg_app_cp_a (GFile         *src,
              GFile         *dest,
              XdgAppCpFlags  flags,
              GCancellable  *cancellable,
              GError       **error)
{
  gboolean ret = FALSE;
  GFileEnumerator *enumerator = NULL;
  GFileInfo *src_info = NULL;
  GFile *dest_child = NULL;
  int dest_dfd = -1;
  gboolean merge = (flags & XDG_APP_CP_FLAGS_MERGE) != 0;
  gboolean no_chown = (flags & XDG_APP_CP_FLAGS_NO_CHOWN) != 0;
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

      if (dest_child) g_object_unref (dest_child);
      dest_child = g_file_get_child (dest, g_file_info_get_name (file_info));

      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!xdg_app_cp_a (src_child, dest_child, flags,
                             cancellable, error))
            goto out;
        }
      else
        {
          (void) unlink (gs_file_get_path_cached (dest_child));
          GFileCopyFlags copyflags = G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS;
          if (!no_chown)
            copyflags |= G_FILE_COPY_ALL_METADATA;
          if (!g_file_copy (src_child, dest_child, copyflags,
                            cancellable, NULL, NULL, error))
            goto out;
        }
    }

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
xdg_app_variant_bsearch_str (GVariant   *array,
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
        imin = imid + 1;
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
xdg_app_repo_set_title (OstreeRepo *repo,
                        const char *title,
                        GError **error)
{
  g_autoptr(GKeyFile) config = NULL;

  config = ostree_repo_copy_config (repo);

  if (title)
    g_key_file_set_string (config, "xdg-app", "title", title);
  else
    g_key_file_remove_key (config, "xdg-app", "title", NULL);

  if (!ostree_repo_write_config (repo, config, error))
    return FALSE;

  return TRUE;
}

gboolean
xdg_app_repo_update (OstreeRepo   *repo,
                     const char  **gpg_key_ids,
                     const char   *gpg_homedir,
                     GCancellable *cancellable,
                     GError      **error)
{
  GVariantBuilder builder;
  GKeyFile *config;
  g_autofree char *title = NULL;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);

  config = ostree_repo_get_config (repo);

  if (config)
    {
      title = g_key_file_get_string (config, "xdg-app", "title", NULL);
    }

  if (title)
    g_variant_builder_add (&builder, "{sv}", "xa.title",
                           g_variant_new_string (title));

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

static OstreeRepoCommitFilterResult
commit_filter (OstreeRepo *repo,
               const char *path,
               GFileInfo *file_info,
               gpointer user_data)
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
validate_component (XdgAppXml *component,
                    const char *ref,
                    const char *id)
{
  XdgAppXml *bundle, *text, *prev, *id_node, *id_text_node;
  g_autofree char *id_text = NULL;

  if (g_strcmp0 (component->element_name, "component") != 0)
    return FALSE;

  id_node = xdg_app_xml_find (component, "id", NULL);
  if (id_node == NULL)
    return FALSE;

  id_text_node = xdg_app_xml_find (id_node, NULL, NULL);
  if (id_text_node == NULL || id_text_node->text == NULL)
    return FALSE;

  id_text = g_strstrip (g_strdup (id_text_node->text));
  if (!g_str_has_prefix (id_text, id) ||
      !g_str_has_suffix (id_text, ".desktop"))
    {
      g_warning ("Invalid id %s", id_text);
      return FALSE;
    }

  while ((bundle = xdg_app_xml_find (component, "bundle", &prev)) != NULL)
    xdg_app_xml_free (xdg_app_xml_unlink (component, bundle));

  bundle = xdg_app_xml_new ("bundle");
  bundle->attribute_names = g_new0 (char *, 2);
  bundle->attribute_values = g_new0 (char *, 2);
  bundle->attribute_names[0] = g_strdup ("type");
  bundle->attribute_values[0] = g_strdup ("xdg-app");

  xdg_app_xml_add (component, xdg_app_xml_new_text ("  "));
  xdg_app_xml_add (component, bundle);
  xdg_app_xml_add (component, xdg_app_xml_new_text ("\n  "));

  text = xdg_app_xml_new (NULL);
  text->text = g_strdup (ref);

  xdg_app_xml_add (bundle, text);

  return TRUE;
}

static gboolean
migrate_xml (XdgAppXml *root,
             XdgAppXml *appstream,
             const char *ref,
             const char *id)
{
  XdgAppXml *components;
  XdgAppXml *component;
  XdgAppXml *prev_component;
  gboolean migrated = FALSE;

  if (root->first_child == NULL ||
      root->first_child->next_sibling != NULL ||
      g_strcmp0 (root->first_child->element_name, "components") != 0)
    return FALSE;

  components = root->first_child;

  component = components->first_child;
  prev_component = NULL;
  while (component != NULL)
    {
      XdgAppXml *next = component->next_sibling;

      if (validate_component (component, ref, id))
        {
          xdg_app_xml_add (appstream, xdg_app_xml_unlink (component, prev_component));
          migrated = TRUE;
        }
      else
        prev_component = component;

      component = next;
    }

  return migrated;
}

static gboolean
copy_icon (const char *id,
           GFile *root,
           GFile *dest,
           const char *size,
           GError **error)
{
  g_autofree char *icon_name = g_strconcat (id, ".png", NULL);
  g_autoptr(GFile) icons_dir =
    g_file_resolve_relative_path (root,
                                  "export/share/app-info/icons/xdg-app");
  g_autoptr(GFile) size_dir =g_file_get_child (icons_dir, size);
  g_autoptr(GFile) icon_file = g_file_get_child (size_dir, icon_name);
  g_autoptr(GFile) dest_dir = g_file_get_child (dest, "icons");
  g_autoptr(GFile) dest_size_dir = g_file_get_child (dest_dir, size);
  g_autoptr(GFile) dest_file = g_file_get_child (dest_size_dir, icon_name);
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(GOutputStream) out = NULL;
  gssize n_bytes_written;

  in = (GInputStream*)g_file_read (icon_file, NULL, error);
  if (!in)
    return FALSE;

  if (!gs_file_ensure_directory (dest_size_dir, TRUE, NULL, error))
    return FALSE;

  out = (GOutputStream*)g_file_replace (dest_file, NULL, FALSE,
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
extract_appstream (OstreeRepo    *repo,
                   XdgAppXml       *appstream_components,
                   const char    *ref,
                   const char    *id,
                   GFile         *dest,
                   GCancellable  *cancellable,
                   GError       **error)
{
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) xmls_dir = NULL;
  g_autoptr(GFile) appstream_file = NULL;
  g_autofree char *appstream_basename = NULL;
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(XdgAppXml) xml_root = NULL;

  if (!ostree_repo_read_commit (repo, ref, &root, NULL, NULL, error))
    return FALSE;

  xmls_dir = g_file_resolve_relative_path (root, "export/share/app-info/xmls");
  appstream_basename = g_strconcat (id, ".xml.gz", NULL);
  appstream_file = g_file_get_child (xmls_dir, appstream_basename);

  in = (GInputStream*)g_file_read (appstream_file, cancellable, error);
  if (!in)
    return FALSE;

  xml_root = xdg_app_xml_parse (in, TRUE, cancellable, error);
  if (xml_root == NULL)
    return FALSE;

  if (migrate_xml (xml_root, appstream_components, ref, id))
    {
      g_autoptr(GError) my_error = NULL;
      if (!copy_icon (id, root, dest, "64x64", &my_error))
        {
          g_print ("Error copying 64x64 icon: %s\n", my_error->message);
          g_clear_error (&my_error);
        }
      if (!copy_icon (id, root, dest, "128x128", &my_error))
        {
          g_print ("Error copying 128x12 icon: %s\n", my_error->message);
          g_clear_error (&my_error);
        }
    }

  return TRUE;
}

gboolean
xdg_app_repo_generate_appstream (OstreeRepo    *repo,
                                 const char   **gpg_key_ids,
                                 const char    *gpg_homedir,
                                 GCancellable  *cancellable,
                                 GError       **error)
{
  g_autoptr(GHashTable) all_refs = NULL;
  g_autoptr(GHashTable) arches = NULL;
  GHashTableIter iter;
  gpointer key;
  gpointer value;

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

      split = xdg_app_decompose_ref (ref, NULL);
      if (!split)
        continue;

      arch = split[2];
      if (!g_hash_table_contains (arches, arch))
        g_hash_table_insert (arches, g_strdup (arch), GINT_TO_POINTER(1));
    }

  g_hash_table_iter_init (&iter, arches);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      GHashTableIter iter2;
      const char *arch = key;
      g_autofree char *tmpdir = g_strdup ("/tmp/xdg-app-appstream-XXXXXX");
      g_autoptr(XdgAppTempDir) tmpdir_file = NULL;
      g_autoptr(GFile) appstream_file = NULL;
      g_autoptr(GFile) root = NULL;
      g_autoptr(OstreeMutableTree) mtree = NULL;
      g_autofree char *commit_checksum = NULL;
      OstreeRepoTransactionStats stats;
      g_autoptr(OstreeRepoCommitModifier) modifier = NULL;
      g_autofree char *parent = NULL;
      g_autofree char *branch = NULL;
      g_autoptr(XdgAppXml) appstream_root = NULL;
      XdgAppXml *appstream_components;
      g_autoptr(GString) xml = NULL;
      g_autoptr(GZlibCompressor) compressor = NULL;
      g_autoptr(GOutputStream) out2 = NULL;
      g_autoptr(GOutputStream) out = NULL;

      if (g_mkdtemp (tmpdir) == NULL)
        return xdg_app_fail (error, "Can't create temporary directory");

      tmpdir_file = g_file_new_for_path (tmpdir);

      appstream_root = xdg_app_xml_new ("root");
      appstream_components = xdg_app_xml_new ("components");
      xdg_app_xml_add (appstream_root, appstream_components);
      xdg_app_xml_add (appstream_components, xdg_app_xml_new_text ("\n  "));

      appstream_components->attribute_names = g_new0 (char *, 3);
      appstream_components->attribute_values = g_new0 (char *, 3);
      appstream_components->attribute_names[0] = g_strdup ("version");
      appstream_components->attribute_values[0] = g_strdup ("0.8");
      appstream_components->attribute_names[1] = g_strdup ("origin");
      appstream_components->attribute_values[1] = g_strdup ("xdg-app");

      g_hash_table_iter_init (&iter2, all_refs);
      while (g_hash_table_iter_next (&iter2, &key, &value))
        {
          const char *ref = key;
          g_auto(GStrv) split = NULL;
          g_autoptr(GError) my_error = NULL;

          split = xdg_app_decompose_ref (ref, NULL);
          if (!split)
            continue;

          if (strcmp (split[2], arch) != 0)
            continue;

          if (!extract_appstream (repo, appstream_components, ref, split[1], tmpdir_file, cancellable, &my_error))
            {
              g_print ("No appstream data for %s\n", ref);
              continue;
            }
        }

      xdg_app_xml_add (appstream_components, xdg_app_xml_new_text ("\n"));

      xml = g_string_new ("");
      xdg_app_xml_to_string (appstream_root, xml);

      compressor = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1);
      out = g_memory_output_stream_new_resizable ();
      out2 = g_converter_output_stream_new (out, G_CONVERTER (compressor));
      if (!g_output_stream_write_all (out2, xml->str, xml->len,
                                      NULL, NULL, error))
        return FALSE;
      if (!g_output_stream_close (out2, NULL, error))
        return FALSE;

      appstream_file = g_file_get_child (tmpdir_file, "appstream.xml.gz");

      if (!g_file_replace_contents (appstream_file,
                                    g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (out)),
                                    g_memory_output_stream_get_data_size (G_MEMORY_OUTPUT_STREAM (out)),
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
                                                  (OstreeRepoCommitFilter)commit_filter, NULL, NULL);

      if (!ostree_repo_write_directory_to_mtree (repo, G_FILE (tmpdir_file), mtree, modifier, cancellable, error))
        goto out;

      if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
        goto out;

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

  return TRUE;

 out:
  ostree_repo_abort_transaction (repo, cancellable, NULL);
  return FALSE;
}

void
xdg_app_extension_free (XdgAppExtension *extension)
{
  g_free (extension->id);
  g_free (extension->installed_id);
  g_free (extension->ref);
  g_free (extension->directory);
  g_free (extension);
}

static XdgAppExtension *
xdg_app_extension_new (const char *id,
                       const char *extension,
                       const char *arch,
                       const char *branch,
                       const char *directory)
{
  XdgAppExtension *ext = g_new0 (XdgAppExtension, 1);

  ext->id = g_strdup (id);
  ext->installed_id = g_strdup (extension);
  ext->ref = g_build_filename ("runtime", extension, arch, branch, NULL);
  ext->directory = g_strdup (directory);
  return ext;
}

GList *
xdg_app_list_extensions (GKeyFile *metakey,
                         const char *arch,
                         const char *default_branch)
{
  g_auto(GStrv) groups = NULL;
  int i;
  GList *res;

  res = NULL;

  if (arch == NULL)
    arch = xdg_app_get_arch ();

  groups = g_key_file_get_groups (metakey, NULL);
  for (i = 0; groups[i] != NULL; i++)
    {
      XdgAppExtension *ext;
      char *extension;

      if (g_str_has_prefix (groups[i], "Extension ") &&
          *(extension = (groups[i] + strlen ("Extension "))) != 0)
        {
          g_autofree char *directory = g_key_file_get_string (metakey, groups[i], "directory", NULL);
          g_autofree char *version = g_key_file_get_string (metakey, groups[i], "version", NULL);
          const char *branch;

          if (directory == NULL)
            continue;

          if (version)
            branch = version;
          else
            branch = default_branch;

          if (g_key_file_get_boolean (metakey, groups[i],
                                      "subdirectories", NULL))
            {
              g_autofree char *prefix = g_strconcat (extension, ".", NULL);
              g_auto(GStrv) refs = NULL;
              int j;

              refs = xdg_app_list_deployed_refs ("runtime", prefix, arch, branch,
                                                 NULL, NULL);
              for (j = 0; refs != NULL && refs[j] != NULL; j++)
                {
                  g_autofree char *extended_dir = g_build_filename (directory, refs[j] + strlen (prefix), NULL);

                  ext = xdg_app_extension_new (extension, refs[j], arch, branch, extended_dir);
                  res = g_list_prepend (res, ext);
                }
            }
          else
            {
              ext = xdg_app_extension_new (extension, extension, arch, branch, directory);
              res = g_list_prepend (res, ext);
            }
        }
    }

  return res;
}


typedef struct {
  XdgAppXml *current;
} XmlData;

XdgAppXml *
xdg_app_xml_new (const gchar *element_name)
{
  XdgAppXml *node = g_new0 (XdgAppXml, 1);
  node->element_name = g_strdup (element_name);
  return node;
}

XdgAppXml *
xdg_app_xml_new_text (const gchar *text)
{
  XdgAppXml *node = g_new0 (XdgAppXml, 1);
  node->text = g_strdup (text);
  return node;
}

void
xdg_app_xml_add (XdgAppXml *parent, XdgAppXml *node)
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
  XdgAppXml *node;

  node = xdg_app_xml_new (element_name);
  node->attribute_names = g_strdupv ((char **)attribute_names);
  node->attribute_values = g_strdupv ((char **)attribute_values);

  xdg_app_xml_add (data->current, node);
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
  XdgAppXml *node;

  node = xdg_app_xml_new (NULL);
  node->text = g_strndup (text, text_len);
  xdg_app_xml_add (data->current, node);
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
xdg_app_xml_free (XdgAppXml *node)
{
  XdgAppXml *child;

  if (node == NULL)
    return;

  child = node->first_child;
  while (child != NULL)
    {
      XdgAppXml *next = child->next_sibling;
      xdg_app_xml_free (child);
      child = next;
    }

  g_free (node->element_name);
  g_free (node->text);
  g_strfreev (node->attribute_names);
  g_strfreev (node->attribute_values);
  g_free (node);
}


void
xdg_app_xml_to_string (XdgAppXml *node, GString *res)
{
  int i;
  XdgAppXml *child;

  if (node->parent == NULL)
    g_string_append (res, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");

  if (node->element_name)
    {
      if (node->parent != NULL)
        {
          if (node->first_child == NULL)
            g_string_append (res, "</");
          else
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
          g_string_append (res, ">");
        }

      child = node->first_child;
      while (child != NULL)
        {
          xdg_app_xml_to_string (child, res);
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

XdgAppXml *
xdg_app_xml_unlink (XdgAppXml *node,
                    XdgAppXml *prev_sibling)
{
  XdgAppXml *parent = node->parent;

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

XdgAppXml *
xdg_app_xml_find (XdgAppXml *node,
                  const char *type,
                  XdgAppXml **prev_child_out)
{
  XdgAppXml *child = NULL;
  XdgAppXml *prev_child = NULL;

  child = node->first_child;
  prev_child = NULL;
  while (child != NULL)
    {
      XdgAppXml *next = child->next_sibling;

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


XdgAppXml *
xdg_app_xml_parse (GInputStream *in,
                   gboolean compressed,
                   GCancellable *cancellable,
                   GError **error)

{
  g_autoptr(GInputStream) real_in = NULL;
  g_autoptr(XdgAppXml) xml_root = NULL;
  XmlData data = { 0 };
  char buffer[32*1024];
  gssize len;
  g_autoptr(GMarkupParseContext) ctx = NULL;
  
  if (compressed)
    {
      g_autoptr(GZlibDecompressor) decompressor = NULL;
      decompressor = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
      real_in = g_converter_input_stream_new (in, G_CONVERTER (decompressor));
    }
  else
    real_in = g_object_ref (in);

  xml_root = xdg_app_xml_new ("root");
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
