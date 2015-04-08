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

static unsigned char
struct_stat_to_dt (struct stat *stbuf)
{
  if (S_ISDIR (stbuf->st_mode))
    return DT_DIR;
  if (S_ISREG (stbuf->st_mode))
    return DT_REG;
  if (S_ISCHR (stbuf->st_mode))
    return DT_CHR;
  if (S_ISBLK (stbuf->st_mode))
    return DT_BLK;
  if (S_ISFIFO (stbuf->st_mode))
    return DT_FIFO;
  if (S_ISLNK (stbuf->st_mode))
    return DT_LNK;
  if (S_ISSOCK (stbuf->st_mode))
    return DT_SOCK;
  return DT_UNKNOWN;
}

static gboolean
glnx_shutil_rm_rf_children (GLnxDirFdIterator    *dfd_iter,
                            GCancellable       *cancellable,
                            GError            **error)
{
  gboolean ret = FALSE;
  struct dirent *dent;

  while (TRUE)
    {
      if (!glnx_dirfd_iterator_next_dent (dfd_iter, &dent, cancellable, error))
        goto out;

      if (dent == NULL)
        break;

      if (dent->d_type == DT_UNKNOWN)
        {
          struct stat stbuf;
          if (fstatat (dfd_iter->fd, dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
            {
              if (errno == ENOENT)
                continue;
              else
                {
                  glnx_set_error_from_errno (error);
                  goto out;
                }
            }
          dent->d_type = struct_stat_to_dt (&stbuf);
          /* Assume unknown types are just treated like regular files */
          if (dent->d_type == DT_UNKNOWN)
            dent->d_type = DT_REG;
        }

      if (dent->d_type == DT_DIR)
        {
          g_auto(GLnxDirFdIterator) child_dfd_iter = { 0, };

          if (!glnx_dirfd_iterator_init_at (dfd_iter->fd, dent->d_name, FALSE,
                                            &child_dfd_iter, error))
            goto out;

          if (!glnx_shutil_rm_rf_children (&child_dfd_iter, cancellable, error))
            goto out;

          if (unlinkat (dfd_iter->fd, dent->d_name, AT_REMOVEDIR) == -1)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }
      else
        {
          if (unlinkat (dfd_iter->fd, dent->d_name, 0) == -1)
            {
              if (errno != ENOENT)
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
  gboolean ret = FALSE;
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
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }
      else
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }
  else
    {
      if (!glnx_dirfd_iterator_init_take_fd (target_dfd, &dfd_iter, error))
        goto out;
      target_dfd = -1;

      if (!glnx_shutil_rm_rf_children (&dfd_iter, cancellable, error))
        goto out;

      if (unlinkat (dfd, path, AT_REMOVEDIR) == -1)
        {
          int errsv = errno;
          if (errsv != ENOENT)
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

static gboolean
mkdir_p_at_internal (int              dfd,
                     char            *path,
                     int              mode,
                     GCancellable    *cancellable,
                     GError         **error)
{
  gboolean ret = FALSE;
  gboolean did_recurse = FALSE;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    goto out;

 again:
  if (mkdirat (dfd, path, mode) == -1)
    {
      if (errno == ENOENT)
        {
          char *lastslash;

          g_assert (!did_recurse);

          lastslash = strrchr (path, '/');
          g_assert (lastslash != NULL);
          /* Note we can mutate the buffer as we dup'd it */
          *lastslash = '\0';

          if (!glnx_shutil_mkdir_p_at (dfd, path, mode,
                                       cancellable, error))
            goto out;

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
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
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
 */
gboolean
glnx_shutil_mkdir_p_at (int                   dfd,
                        const char           *path,
                        int                   mode,
                        GCancellable         *cancellable,
                        GError              **error)
{
  gboolean ret = FALSE;
  struct stat stbuf;
  char *buf;

  /* Fast path stat to see whether it already exists */
  if (fstatat (dfd, path, &stbuf, AT_SYMLINK_NOFOLLOW) == 0)
    {
      if (S_ISDIR (stbuf.st_mode))
        {
          ret = TRUE;
          goto out;
        }
    }

  buf = strdupa (path);

  if (!mkdir_p_at_internal (dfd, buf, mode, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}
