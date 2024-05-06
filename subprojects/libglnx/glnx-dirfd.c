/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014,2015 Colin Walters <walters@verbum.org>.
 * SPDX-License-Identifier: LGPL-2.0-or-later
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

#include "libglnx-config.h"

#include <string.h>

#include <glnx-dirfd.h>
#include <glnx-fdio.h>
#include <glnx-errors.h>
#include <glnx-local-alloc.h>
#include <glnx-shutil.h>

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
    return glnx_throw_errno_prefix (error, "opendir(%s)", path);
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
 * @path: Path, may be relative to @dfd
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
  glnx_autofd int fd = -1;
  if (!glnx_opendirat (dfd, path, follow, &fd, error))
    return FALSE;

  if (!glnx_dirfd_iterator_init_take_fd (&fd, out_dfd_iter, error))
    return FALSE;

  return TRUE;
}

/**
 * glnx_dirfd_iterator_init_take_fd:
 * @dfd: File descriptor - ownership is taken, and the value is set to -1
 * @dfd_iter: A directory iterator
 * @error: Error
 *
 * Steal ownership of @dfd, using it to initialize @dfd_iter for
 * iteration.
 */
gboolean
glnx_dirfd_iterator_init_take_fd (int               *dfd,
                                  GLnxDirFdIterator *dfd_iter,
                                  GError           **error)
{
  GLnxRealDirfdIterator *real_dfd_iter = (GLnxRealDirfdIterator*) dfd_iter;
  DIR *d = fdopendir (*dfd);
  if (!d)
    return glnx_throw_errno_prefix (error, "fdopendir");

  real_dfd_iter->fd = g_steal_fd (dfd);
  real_dfd_iter->d = d;
  real_dfd_iter->initialized = TRUE;

  return TRUE;
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
  GLnxRealDirfdIterator *real_dfd_iter = (GLnxRealDirfdIterator*) dfd_iter;

  g_return_val_if_fail (out_dent, FALSE);
  g_return_val_if_fail (dfd_iter->initialized, FALSE);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  do
    {
      errno = 0;
      *out_dent = readdir (real_dfd_iter->d);
      if (*out_dent == NULL && errno != 0)
        return glnx_throw_errno_prefix (error, "readdir");
    } while (*out_dent &&
             (strcmp ((*out_dent)->d_name, ".") == 0 ||
              strcmp ((*out_dent)->d_name, "..") == 0));

  return TRUE;
}

/**
 * glnx_dirfd_iterator_rewind:
 * @dfd_iter: A directory iterator
 *
 * Rewind to the beginning of @dfd_iter. The next call to
 * glnx_dirfd_iterator_next_dent() will provide the first entry in the
 * directory.
 */
void
glnx_dirfd_iterator_rewind (GLnxDirFdIterator  *dfd_iter)
{
  GLnxRealDirfdIterator *real_dfd_iter = (GLnxRealDirfdIterator*) dfd_iter;

  g_return_if_fail (dfd_iter->initialized);

  rewinddir (real_dfd_iter->d);
}

/**
 * glnx_dirfd_iterator_next_dent_ensure_dtype:
 * @dfd_iter: A directory iterator
 * @out_dent: (out) (transfer none): Pointer to dirent; do not free
 * @cancellable: Cancellable
 * @error: Error
 *
 * A variant of @glnx_dirfd_iterator_next_dent, which will ensure the
 * `dent->d_type` member is filled in by calling `fstatat`
 * automatically if the underlying filesystem type sets `DT_UNKNOWN`.
 */
gboolean
glnx_dirfd_iterator_next_dent_ensure_dtype (GLnxDirFdIterator  *dfd_iter,
                                            struct dirent     **out_dent,
                                            GCancellable       *cancellable,
                                            GError            **error)
{
  g_return_val_if_fail (out_dent, FALSE);

  if (!glnx_dirfd_iterator_next_dent (dfd_iter, out_dent, cancellable, error))
    return FALSE;

  struct dirent *ret_dent = *out_dent;
  if (ret_dent)
    {

      if (ret_dent->d_type == DT_UNKNOWN)
        {
          struct stat stbuf;
          if (!glnx_fstatat (dfd_iter->fd, ret_dent->d_name, &stbuf, AT_SYMLINK_NOFOLLOW, error))
            return FALSE;
          ret_dent->d_type = IFTODT (stbuf.st_mode);
        }
    }

  return TRUE;
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
  if (!real_dfd_iter->initialized)
    return;
  (void) closedir (real_dfd_iter->d);
  real_dfd_iter->initialized = FALSE;
}

/**
 * glnx_fdrel_abspath:
 * @dfd: Directory fd
 * @path: Path
 *
 * Turn a fd-relative pair into something that can be used for legacy
 * APIs expecting absolute paths.
 *
 * This is Linux specific, and only valid inside this process (unless
 * you set up the child process to have the exact same fd number, but
 * don't try that).
 */
char *
glnx_fdrel_abspath (int         dfd,
                    const char *path)
{
  dfd = glnx_dirfd_canonicalize (dfd);
  if (dfd == AT_FDCWD)
    return g_strdup (path);
  return g_strdup_printf ("/proc/self/fd/%d/%s", dfd, path);
}

/**
 * glnx_gen_temp_name:
 * @tmpl: (type filename): template directory name, the last 6 characters will be replaced
 *
 * Replace the last 6 characters of @tmpl with random ASCII.  You must
 * use this in combination with a mechanism to ensure race-free file
 * creation such as `O_EXCL`.
 */
void
glnx_gen_temp_name (gchar *tmpl)
{
  g_return_if_fail (tmpl != NULL);
  const size_t len = strlen (tmpl);
  g_return_if_fail (len >= 6);

  static const char letters[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  static const int NLETTERS = sizeof (letters) - 1;

  char *XXXXXX = tmpl + (len - 6);
  for (int i = 0; i < 6; i++)
    XXXXXX[i] = letters[g_random_int_range(0, NLETTERS)];
}

/**
 * glnx_mkdtempat:
 * @dfd: Directory fd
 * @tmpl: (type filename): Initial template directory name, last 6 characters will be replaced
 * @mode: permissions with which to create the temporary directory
 * @out_tmpdir: (out caller-allocates): Initialized tempdir structure
 * @error: Error
 *
 * Somewhat similar to g_mkdtemp_full(), but fd-relative, and returns a
 * structure that uses autocleanups.  Note that the supplied @dfd lifetime
 * must match or exceed that of @out_tmpdir in order to remove the directory.
 */
gboolean
glnx_mkdtempat (int dfd, const char *tmpl, int mode,
                GLnxTmpDir *out_tmpdir, GError **error)
{
  g_return_val_if_fail (tmpl != NULL, FALSE);
  g_return_val_if_fail (out_tmpdir != NULL, FALSE);
  g_return_val_if_fail (!out_tmpdir->initialized, FALSE);

  dfd = glnx_dirfd_canonicalize (dfd);

  g_autofree char *path = g_strdup (tmpl);
  for (int count = 0; count < 100; count++)
    {
      glnx_gen_temp_name (path);

      /* Ideally we could use openat(O_DIRECTORY | O_CREAT | O_EXCL) here
       * to create and open the directory atomically, but that’s not supported by
       * current kernel versions: http://www.openwall.com/lists/oss-security/2014/11/26/14
       * (Tested on kernel 4.10.10-200.fc25.x86_64). For the moment, accept a
       * TOCTTOU race here. */
      if (mkdirat (dfd, path, mode) == -1)
        {
          if (errno == EEXIST)
            continue;

          /* Any other error will apply also to other names we might
           *  try, and there are 2^32 or so of them, so give up now.
           */
          return glnx_throw_errno_prefix (error, "mkdirat");
        }

      /* And open it */
      glnx_autofd int ret_dfd = -1;
      if (!glnx_opendirat (dfd, path, FALSE, &ret_dfd, error))
        {
          /* If we fail to open, let's try to clean up */
          (void)unlinkat (dfd, path, AT_REMOVEDIR);
          return FALSE;
        }

      /* Return the initialized directory struct */
      out_tmpdir->initialized = TRUE;
      out_tmpdir->src_dfd = dfd; /* referenced; see above docs */
      out_tmpdir->fd = g_steal_fd (&ret_dfd);
      out_tmpdir->path = g_steal_pointer (&path);
      return TRUE;
    }

  /* Failure */
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
               "glnx_mkdtempat ran out of combinations to try");
  return FALSE;
}

/**
 * glnx_mkdtemp:
 * @tmpl: (type filename): Source template directory name, last 6 characters will be replaced
 * @mode: permissions to create the temporary directory with
 * @out_tmpdir: (out caller-allocates): Return location for tmpdir data
 * @error: Return location for a #GError, or %NULL
 *
 * Similar to glnx_mkdtempat(), but will use g_get_tmp_dir() as the parent
 * directory to @tmpl.
 *
 * Returns: %TRUE on success, %FALSE otherwise
 * Since: UNRELEASED
 */
gboolean
glnx_mkdtemp (const gchar   *tmpl,
              int      mode,
              GLnxTmpDir *out_tmpdir,
              GError **error)
{
  g_autofree char *path = g_build_filename (g_get_tmp_dir (), tmpl, NULL);
  return glnx_mkdtempat (AT_FDCWD, path, mode,
                         out_tmpdir, error);
}

static gboolean
_glnx_tmpdir_free (GLnxTmpDir *tmpd,
                   gboolean    delete_dir,
                   GCancellable *cancellable,
                   GError    **error)
{
  /* Support being passed NULL so we work nicely in a GPtrArray */
  if (!(tmpd && tmpd->initialized))
    return TRUE;
  g_assert_cmpint (tmpd->fd, !=, -1);
  glnx_close_fd (&tmpd->fd);
  g_assert (tmpd->path);
  g_assert_cmpint (tmpd->src_dfd, !=, -1);
  g_autofree char *path = tmpd->path; /* Take ownership */
  tmpd->initialized = FALSE;
  if (delete_dir)
    {
      if (!glnx_shutil_rm_rf_at (tmpd->src_dfd, path, cancellable, error))
        return FALSE;
    }
  return TRUE;
}

/**
 * glnx_tmpdir_delete:
 * @tmpf: Temporary dir
 * @cancellable: Cancellable
 * @error: Error
 *
 * Deallocate a tmpdir, closing the fd and recursively deleting the path. This
 * is normally called indirectly via glnx_tmpdir_cleanup() by the autocleanup
 * attribute, but you can also invoke this directly.
 *
 * If an error occurs while deleting the filesystem path, @tmpf will still have
 * been deallocated and should not be reused.
 *
 * See also `glnx_tmpdir_unset` to avoid deleting the path.
 */
gboolean
glnx_tmpdir_delete (GLnxTmpDir *tmpf, GCancellable *cancellable, GError **error)
{
  return _glnx_tmpdir_free (tmpf, TRUE, cancellable, error);
}

/**
 * glnx_tmpdir_unset:
 * @tmpf: Temporary dir
 * @cancellable: Cancellable
 * @error: Error
 *
 * Deallocate a tmpdir, but do not delete the filesystem path.  See also
 * `glnx_tmpdir_delete()`.
 */
void
glnx_tmpdir_unset (GLnxTmpDir *tmpf)
{
  (void) _glnx_tmpdir_free (tmpf, FALSE, NULL, NULL);
}
