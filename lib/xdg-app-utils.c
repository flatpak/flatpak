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

#include "xdg-app-utils.h"
#include "xdg-app-dir.h"

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
  if (arch == NULL)
    arch = xdg_app_get_arch ();

  return g_build_filename ("runtime", runtime, arch, branch, NULL);
}

char *
xdg_app_build_app_ref (const char *app,
                       const char *branch,
                       const char *arch)
{
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

  g_ptr_array_sort (names, (GCompareFunc)g_strcmp0);
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

static gboolean
load_contents (const char *uri, GBytes **contents, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autofree char *scheme = NULL;

  scheme = g_uri_parse_scheme (uri);
  if (strcmp (scheme, "file") == 0)
    {
      char *buffer;
      gsize length;
      g_autoptr(GFile) file = NULL;

      g_debug ("Loading summary %s using GIO", uri);
      file = g_file_new_for_uri (uri);
      if (!g_file_load_contents (file, cancellable, &buffer, &length, NULL, NULL))
        goto out;

      *contents = g_bytes_new_take (buffer, length);
    }
  else
    {
      g_autoptr(SoupSession) session = NULL;
      g_autoptr(SoupMessage) msg = NULL;

      g_debug ("Loading summary %s using libsoup", uri);
      session = soup_session_new ();
      msg = soup_message_new ("GET", uri);
      soup_session_send_message (session, msg);

      if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
        goto out;

      *contents = g_bytes_new (msg->response_body->data, msg->response_body->length);
    }

  ret = TRUE;

  g_debug ("Received %" G_GSIZE_FORMAT " bytes", g_bytes_get_size (*contents));

out:
  return ret;
}

gboolean
ostree_repo_load_summary (const char *repository_url,
                          GHashTable **refs,
                          gchar **title,
                          GCancellable *cancellable,
                          GError **error)
{
  gboolean ret = FALSE;
  g_autofree char *summary_url = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GHashTable) local_refs = NULL;
  g_autofree char *local_title = NULL;

  local_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  summary_url = g_build_filename (repository_url, "summary", NULL);
  if (load_contents (summary_url, &bytes, cancellable, NULL))
    {
      g_autoptr(GVariant) summary;
      g_autoptr(GVariant) ref_list;
      g_autoptr(GVariant) extensions;
      GVariantDict dict;
      int i, n;

      summary = g_variant_new_from_bytes (OSTREE_SUMMARY_GVARIANT_FORMAT, bytes, FALSE);
      ref_list = g_variant_get_child_value (summary, 0);
      extensions = g_variant_get_child_value (summary, 1);

      n = g_variant_n_children (ref_list);
      g_debug ("Summary contains %d refs", n);
      for (i = 0; i < n; i++)
        {
          g_autoptr(GVariant) ref = NULL;
          g_autoptr(GVariant) csum_v = NULL;
          char *refname;
          char *checksum;

          ref = g_variant_get_child_value (ref_list, i);
          g_variant_get (ref, "(&s(t@aya{sv}))", &refname, NULL, &csum_v, NULL);

          if (!ostree_validate_rev (refname, error))
            goto out;

          checksum = ostree_checksum_from_bytes_v (csum_v);
          g_debug ("\t%s -> %s", refname, checksum);
          g_hash_table_insert (local_refs, g_strdup (refname), checksum);
        }

       g_variant_dict_init (&dict, extensions);
       g_variant_dict_lookup (&dict, "xa.title", "s", &local_title);
       g_debug ("Summary title: %s", local_title);
       g_variant_dict_end (&dict);
    }

  *refs = g_hash_table_ref (local_refs);
  *title = g_strdup (local_title);

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
