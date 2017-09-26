/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014,2015 Colin Walters <walters@verbum.org>.
 *
 * This library is free software; you can redistribute it and/or
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <string.h>

#include <glnx-shutil.h>
#include <glnx-errors.h>
#include <glnx-local-alloc.h>

static gboolean
glnx_shutil_rm_rf_children (GLnxDirFdIterator    *dfd_iter,
                            GCancellable       *cancellable,
                            GError            **error)
{
  struct dirent *dent;

  while (TRUE)
    {
      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (dfd_iter, &dent, cancellable, error))
        return FALSE;
      if (dent == NULL)
        break;

      if (dent->d_type == DT_DIR)
        {
          g_auto(GLnxDirFdIterator) child_dfd_iter = { 0, };

          if (!glnx_dirfd_iterator_init_at (dfd_iter->fd, dent->d_name, FALSE,
                                            &child_dfd_iter, error))
            return FALSE;

          if (!glnx_shutil_rm_rf_children (&child_dfd_iter, cancellable, error))
            return FALSE;

          if (unlinkat (dfd_iter->fd, dent->d_name, AT_REMOVEDIR) == -1)
            return glnx_throw_errno_prefix (error, "unlinkat");
        }
      else
        {
          if (unlinkat (dfd_iter->fd, dent->d_name, 0) == -1)
            {
              if (errno != ENOENT)
                return glnx_throw_errno_prefix (error, "unlinkat");
            }
        }
    }

  return TRUE;
}

/**
 * glnx_shutil_rm_rf_at:
 * @dfd: A directory file descriptor, or `AT_FDCWD` or `-1` for current
 * @path: Path
 * @cancellable: Cancellable
 * @error: Error
 *
 * Recursively delete the filename referenced by the combination of
 * the directory fd @dfd and @path; it may be a file or directory.  No
 * error is thrown if @path does not exist.
 */
gboolean
glnx_shutil_rm_rf_at (int                   dfd,
                      const char           *path,
                      GCancellable         *cancellable,
                      GError              **error)
{
  glnx_fd_close int target_dfd = -1;
  g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

  dfd = glnx_dirfd_canonicalize (dfd);

  /* With O_NOFOLLOW first */
  target_dfd = openat (dfd, path,
                       O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);

  if (target_dfd == -1)
    {
      int errsv = errno;
      if (errsv == ENOENT)
        {
          ;
        }
      else if (errsv == ENOTDIR || errsv == ELOOP)
        {
          if (unlinkat (dfd, path, 0) != 0)
            return glnx_throw_errno_prefix (error, "unlinkat");
        }
      else
        return glnx_throw_errno_prefix (error, "open(%s)", path);
    }
  else
    {
      if (!glnx_dirfd_iterator_init_take_fd (&target_dfd, &dfd_iter, error))
        return FALSE;

      if (!glnx_shutil_rm_rf_children (&dfd_iter, cancellable, error))
        return FALSE;

      if (unlinkat (dfd, path, AT_REMOVEDIR) == -1)
        {
          if (errno != ENOENT)
            return glnx_throw_errno_prefix (error, "unlinkat");
        }
    }

  return TRUE;
}

static gboolean
mkdir_p_at_internal (int              dfd,
                     char            *path,
                     int              mode,
                     GCancellable    *cancellable,
                     GError         **error)
{
  gboolean did_recurse = FALSE;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

 again:
  if (mkdirat (dfd, path, mode) == -1)
    {
      if (errno == ENOENT)
        {
          char *lastslash;

          g_assert (!did_recurse);

          lastslash = strrchr (path, '/');
          if (lastslash == NULL)
            {
              /* This can happen if @dfd was deleted between being opened and
               * passed to mkdir_p_at_internal(). */
              return glnx_throw_errno_prefix (error, "mkdir(%s)", path);
            }

          /* Note we can mutate the buffer as we dup'd it */
          *lastslash = '\0';

          if (!glnx_shutil_mkdir_p_at (dfd, path, mode,
                                       cancellable, error))
            return FALSE;

          /* Now restore it for another mkdir attempt */
          *lastslash = '/';

          did_recurse = TRUE;
          goto again;
        }
      else if (errno == EEXIST)
        {
          /* Fall through; it may not have been a directory,
           * but we'll find that out on the next call up.
           */
        }
      else
        return glnx_throw_errno_prefix (error, "mkdir(%s)", path);
    }

  return TRUE;
}

/**
 * glnx_shutil_mkdir_p_at:
 * @dfd: Directory fd
 * @path: Directory path to be created
 * @mode: Mode for newly created directories
 * @cancellable: Cancellable
 * @error: Error
 *
 * Similar to g_mkdir_with_parents(), except operates relative to the
 * directory fd @dfd.
 *
 * See also glnx_ensure_dir() for a non-recursive version.
 *
 * This will return %G_IO_ERROR_NOT_FOUND if @dfd has been deleted since being
 * opened. It may return other errors from mkdirat() in other situations.
 */
gboolean
glnx_shutil_mkdir_p_at (int                   dfd,
                        const char           *path,
                        int                   mode,
                        GCancellable         *cancellable,
                        GError              **error)
{
  struct stat stbuf;
  char *buf;

  /* Fast path stat to see whether it already exists */
  if (fstatat (dfd, path, &stbuf, AT_SYMLINK_NOFOLLOW) == 0)
    {
      /* Note early return */
      if (S_ISDIR (stbuf.st_mode))
        return TRUE;
    }

  buf = strdupa (path);

  if (!mkdir_p_at_internal (dfd, buf, mode, cancellable, error))
    return FALSE;

  return TRUE;
}

/**
 * glnx_shutil_mkdir_p_at_open:
 * @dfd: Directory fd
 * @path: Directory path to be created
 * @mode: Mode for newly created directories
 * @out_dfd: (out caller-allocates): Return location for an FD to @dfd/@path,
 *    or `-1` on error
 * @cancellable: (nullable): Cancellable, or %NULL
 * @error: Return location for a #GError, or %NULL
 *
 * Similar to glnx_shutil_mkdir_p_at(), except it opens the resulting directory
 * and returns a directory FD to it. Currently, this is not guaranteed to be
 * race-free.
 *
 * Returns: %TRUE on success, %FALSE otherwise
 * Since: UNRELEASED
 */
gboolean
glnx_shutil_mkdir_p_at_open (int            dfd,
                             const char    *path,
                             int            mode,
                             int           *out_dfd,
                             GCancellable  *cancellable,
                             GError       **error)
{
  /* FIXME: It’s not possible to eliminate the race here until
   * openat(O_DIRECTORY | O_CREAT) works (and returns a directory rather than a
   * file). It appears to be not supported in current kernels. (Tested with
   * 4.10.10-200.fc25.x86_64.) */
  *out_dfd = -1;

  if (!glnx_shutil_mkdir_p_at (dfd, path, mode, cancellable, error))
    return FALSE;

  return glnx_opendirat (dfd, path, TRUE, out_dfd, error);
}
