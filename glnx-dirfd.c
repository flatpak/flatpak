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

#include <glnx-dirfd.h>
#include <glnx-errors.h>
#include <glnx-local-alloc.h>

/**
 * glnx_opendirat_with_errno:
 * @dfd: File descriptor for origin directory
 * @name: Pathname, relative to @dfd
 * @follow: Whether or not to follow symbolic links
 *
 * Use openat() to open a directory, using a standard set of flags.
 * This function sets errno.
 */
int
glnx_opendirat_with_errno (int           dfd,
                           const char   *path,
                           gboolean      follow)
{
  int flags = O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY;
  if (!follow)
    flags |= O_NOFOLLOW;

  dfd = glnx_dirfd_canonicalize (dfd);

  return openat (dfd, path, flags);
}

/**
 * glnx_opendirat:
 * @dfd: File descriptor for origin directory
 * @path: Pathname, relative to @dfd
 * @follow: Whether or not to follow symbolic links
 * @error: Error
 *
 * Use openat() to open a directory, using a standard set of flags.
 */
gboolean
glnx_opendirat (int             dfd,
                const char     *path,
                gboolean        follow,
                int            *out_fd,
                GError        **error)
{
  int ret = glnx_opendirat_with_errno (dfd, path, follow);
  if (ret == -1)
    {
      glnx_set_prefix_error_from_errno (error, "%s", "openat");
      return FALSE;
    }
  *out_fd = ret;
  return TRUE;
}

struct GLnxRealDirfdIterator
{
  gboolean initialized;
  int fd;
  DIR *d;
};
typedef struct GLnxRealDirfdIterator GLnxRealDirfdIterator;

/**
 * glnx_dirfd_iterator_init_at:
 * @dfd: File descriptor, may be AT_FDCWD or -1
 * @path: Path, may be relative to @df
 * @follow: If %TRUE and the last component of @path is a symlink, follow it
 * @out_dfd_iter: (out caller-allocates): A directory iterator, will be initialized
 * @error: Error
 *
 * Initialize @out_dfd_iter from @dfd and @path.
 */
gboolean
glnx_dirfd_iterator_init_at (int                     dfd,
                             const char             *path,
                             gboolean                follow,
                             GLnxDirFdIterator      *out_dfd_iter,
                             GError                **error)
{
  gboolean ret = FALSE;
  glnx_fd_close int fd = -1;
  
  if (!glnx_opendirat (dfd, path, follow, &fd, error))
    goto out;

  if (!glnx_dirfd_iterator_init_take_fd (fd, out_dfd_iter, error))
    goto out;
  fd = -1; /* Transfer ownership */

  ret = TRUE;
 out:
  return ret;
}

/**
 * glnx_dirfd_iterator_init_take_fd:
 * @dfd: File descriptor - ownership is taken
 * @dfd_iter: A directory iterator
 * @error: Error
 *
 * Steal ownership of @dfd, using it to initialize @dfd_iter for
 * iteration.
 */
gboolean
glnx_dirfd_iterator_init_take_fd (int                dfd,
                                  GLnxDirFdIterator *dfd_iter,
                                  GError           **error)
{
  gboolean ret = FALSE;
  GLnxRealDirfdIterator *real_dfd_iter = (GLnxRealDirfdIterator*) dfd_iter;
  DIR *d = NULL;

  d = fdopendir (dfd);
  if (!d)
    {
      glnx_set_prefix_error_from_errno (error, "%s", "fdopendir");
      goto out;
    }

  real_dfd_iter->fd = dfd;
  real_dfd_iter->d = d;

  ret = TRUE;
 out:
  return ret;
}

/**
 * glnx_dirfd_iterator_next_dent:
 * @dfd_iter: A directory iterator
 * @out_dent: (out) (transfer none): Pointer to dirent; do not free
 * @cancellable: Cancellable
 * @error: Error
 *
 * Read the next value from @dfd_iter, causing @out_dent to be
 * updated.  If end of stream is reached, @out_dent will be set
 * to %NULL, and %TRUE will be returned.
 */
gboolean
glnx_dirfd_iterator_next_dent (GLnxDirFdIterator  *dfd_iter,
                               struct dirent     **out_dent,
                               GCancellable       *cancellable,
                               GError             **error)
{
  gboolean ret = FALSE;
  GLnxRealDirfdIterator *real_dfd_iter = (GLnxRealDirfdIterator*) dfd_iter;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    goto out;

  do
    {
      errno = 0;
      *out_dent = readdir (real_dfd_iter->d);
      if (*out_dent == NULL && errno != 0)
        {
          glnx_set_prefix_error_from_errno (error, "%s", "fdopendir");
          goto out;
        }
    } while (*out_dent &&
             (strcmp ((*out_dent)->d_name, ".") == 0 ||
              strcmp ((*out_dent)->d_name, "..") == 0));

  ret = TRUE;
 out:
  return ret;
}

/**
 * glnx_dirfd_iterator_clear:
 * @dfd_iter: Iterator, will be de-initialized
 *
 * Unset @dfd_iter, freeing any resources.  If @dfd_iter is not
 * initialized, do nothing.
 */
void
glnx_dirfd_iterator_clear (GLnxDirFdIterator *dfd_iter)
{
  GLnxRealDirfdIterator *real_dfd_iter = (GLnxRealDirfdIterator*) dfd_iter;
  /* fd is owned by dfd_iter */
  (void) closedir (real_dfd_iter->d);
  real_dfd_iter->initialized = FALSE;
}
