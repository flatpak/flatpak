/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014,2015 Colin Walters <walters@verbum.org>.
 *
 * Portions derived from systemd:
 *  Copyright 2010 Lennart Poettering
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
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#include <errno.h>

#include <glnx-fdio.h>
#include <glnx-dirfd.h>
#include <glnx-errors.h>
#include <glnx-xattrs.h>
#include <glnx-backport-autoptr.h>
#include <glnx-local-alloc.h>
#include <glnx-missing.h>

/* The standardized version of BTRFS_IOC_CLONE */
#ifndef FICLONE
#define FICLONE _IOW(0x94, 9, int)
#endif

/* Returns the number of chars needed to format variables of the
 * specified type as a decimal string. Adds in extra space for a
 * negative '-' prefix (hence works correctly on signed
 * types). Includes space for the trailing NUL. */
#define DECIMAL_STR_MAX(type)                                           \
        (2+(sizeof(type) <= 1 ? 3 :                                     \
            sizeof(type) <= 2 ? 5 :                                     \
            sizeof(type) <= 4 ? 10 :                                    \
            sizeof(type) <= 8 ? 20 : sizeof(int[-2*(sizeof(type) > 8)])))

gboolean
glnx_stdio_file_flush (FILE *f, GError **error)
{
  if (fflush (f) != 0)
    return glnx_throw_errno_prefix (error, "fflush");
  if (ferror (f) != 0)
    return glnx_throw_errno_prefix (error, "ferror");
  return TRUE;
}

/* An implementation of renameat2(..., RENAME_NOREPLACE)
 * with fallback to a non-atomic version.
 */
int
glnx_renameat2_noreplace (int olddirfd, const char *oldpath,
                          int newdirfd, const char *newpath)
{
#ifndef ENABLE_WRPSEUDO_COMPAT
  if (renameat2 (olddirfd, oldpath, newdirfd, newpath, RENAME_NOREPLACE) < 0)
    {
      if (G_IN_SET(errno, EINVAL, ENOSYS))
        {
          /* Fall through */
        }
      else
        {
          return -1;
        }
    }
  else
    return TRUE;
#endif

  if (linkat (olddirfd, oldpath, newdirfd, newpath, 0) < 0)
    return -1;

  if (unlinkat (olddirfd, oldpath, 0) < 0)
    return -1;

  return 0;
}

static gboolean
rename_file_noreplace_at (int olddirfd, const char *oldpath,
                          int newdirfd, const char *newpath,
                          gboolean ignore_eexist,
                          GError **error)
{
  if (glnx_renameat2_noreplace (olddirfd, oldpath,
                                newdirfd, newpath) < 0)
    {
      if (errno == EEXIST && ignore_eexist)
        {
          (void) unlinkat (olddirfd, oldpath, 0);
          return TRUE;
        }
      else
        return glnx_throw_errno (error);
    }
  return TRUE;
}

/* An implementation of renameat2(..., RENAME_EXCHANGE)
 * with fallback to a non-atomic version.
 */
int
glnx_renameat2_exchange (int olddirfd, const char *oldpath,
                         int newdirfd, const char *newpath)
{
#ifndef ENABLE_WRPSEUDO_COMPAT
  if (renameat2 (olddirfd, oldpath, newdirfd, newpath, RENAME_EXCHANGE) == 0)
    return 0;
  else
    {
      if (G_IN_SET(errno, ENOSYS, EINVAL))
        {
          /* Fall through */
        }
      else
        {
          return -1;
        }
    }
#endif

  /* Fallback */
  { const char *old_tmp_name = glnx_strjoina (oldpath, ".XXXXXX");

    /* Move old out of the way */
    if (renameat (olddirfd, oldpath, olddirfd, old_tmp_name) < 0)
      return -1;
    /* Now move new into its place */
    if (renameat (newdirfd, newpath, olddirfd, oldpath) < 0)
      return -1;
    /* And finally old(tmp) into new */
    if (renameat (olddirfd, old_tmp_name, newdirfd, newpath) < 0)
      return -1;
  }
  return 0;
}

/* Deallocate a tmpfile, closing the fd and deleting the path, if any. This is
 * normally called by default by the autocleanup attribute, but you can also
 * invoke this directly.
 */
void
glnx_tmpfile_clear (GLnxTmpfile *tmpf)
{
  /* Support being passed NULL so we work nicely in a GPtrArray */
  if (!tmpf)
    return;
  if (!tmpf->initialized)
    return;
  if (tmpf->fd == -1)
    return;
  (void) close (tmpf->fd);
  /* If ->path is set, we're likely aborting due to an error. Clean it up */
  if (tmpf->path)
    {
      (void) unlinkat (tmpf->src_dfd, tmpf->path, 0);
      g_free (tmpf->path);
    }
  tmpf->initialized = FALSE;
}

/* Allocate a temporary file, using Linux O_TMPFILE if available. The file mode
 * will be 0600.
 *
 * The result will be stored in @out_tmpf, which is caller allocated
 * so you can store it on the stack in common scenarios.
 *
 * The directory fd @dfd must live at least as long as the output @out_tmpf.
 */
gboolean
glnx_open_tmpfile_linkable_at (int dfd,
                               const char *subpath,
                               int flags,
                               GLnxTmpfile *out_tmpf,
                               GError **error)
{
  const guint mode = 0600;
  glnx_fd_close int fd = -1;
  int count;

  dfd = glnx_dirfd_canonicalize (dfd);

  /* Don't allow O_EXCL, as that has a special meaning for O_TMPFILE */
  g_return_val_if_fail ((flags & O_EXCL) == 0, FALSE);

  /* Creates a temporary file, that shall be renamed to "target"
   * later. If possible, this uses O_TMPFILE – in which case
   * "ret_path" will be returned as NULL. If not possible a the
   * tempoary path name used is returned in "ret_path". Use
   * link_tmpfile() below to rename the result after writing the file
   * in full. */
#if defined(O_TMPFILE) && !defined(DISABLE_OTMPFILE) && !defined(ENABLE_WRPSEUDO_COMPAT)
  fd = openat (dfd, subpath, O_TMPFILE|flags, mode);
  if (fd == -1 && !(G_IN_SET(errno, ENOSYS, EISDIR, EOPNOTSUPP)))
    return glnx_throw_errno_prefix (error, "open(O_TMPFILE)");
  if (fd != -1)
    {
      /* Workaround for https://sourceware.org/bugzilla/show_bug.cgi?id=17523
       * See also https://github.com/ostreedev/ostree/issues/991
       */
      if (fchmod (fd, mode) < 0)
        return glnx_throw_errno_prefix (error, "fchmod");
      out_tmpf->initialized = TRUE;
      out_tmpf->src_dfd = dfd; /* Copied; caller must keep open */
      out_tmpf->fd = glnx_steal_fd (&fd);
      out_tmpf->path = NULL;
      return TRUE;
    }
  /* Fallthrough */
#endif

  { g_autofree char *tmp = g_strconcat (subpath, "/tmp.XXXXXX", NULL);
    const guint count_max = 100;

    for (count = 0; count < count_max; count++)
      {
        glnx_gen_temp_name (tmp);

        fd = openat (dfd, tmp, O_CREAT|O_EXCL|O_NOFOLLOW|O_NOCTTY|flags, mode);
        if (fd < 0)
          {
            if (errno == EEXIST)
              continue;
            else
              return glnx_throw_errno_prefix (error, "Creating temp file");
          }
        else
          {
            out_tmpf->initialized = TRUE;
            out_tmpf->src_dfd = dfd;  /* Copied; caller must keep open */
            out_tmpf->fd = glnx_steal_fd (&fd);
            out_tmpf->path = g_steal_pointer (&tmp);
            return TRUE;
          }
      }
  }
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
               "Exhausted %u attempts to create temporary file", count);
  return FALSE;
}

/* A variant of `glnx_open_tmpfile_linkable_at()` which doesn't support linking.
 * Useful for true temporary storage. The fd will be allocated in /var/tmp to
 * ensure maximum storage space.
 */
gboolean
glnx_open_anonymous_tmpfile (int          flags,
                             GLnxTmpfile *out_tmpf,
                             GError     **error)
{
  if (!glnx_open_tmpfile_linkable_at (AT_FDCWD, "/var/tmp", flags, out_tmpf, error))
    return FALSE;
  if (out_tmpf->path)
    {
      (void) unlinkat (out_tmpf->src_dfd, out_tmpf->path, 0);
      g_clear_pointer (&out_tmpf->path, g_free);
    }
  out_tmpf->anonymous = TRUE;
  out_tmpf->src_dfd = -1;
  return TRUE;
}

/* Use this after calling glnx_open_tmpfile_linkable_at() to give
 * the file its final name (link into place).
 */
gboolean
glnx_link_tmpfile_at (GLnxTmpfile *tmpf,
                      GLnxLinkTmpfileReplaceMode mode,
                      int target_dfd,
                      const char *target,
                      GError **error)
{
  const gboolean replace = (mode == GLNX_LINK_TMPFILE_REPLACE);
  const gboolean ignore_eexist = (mode == GLNX_LINK_TMPFILE_NOREPLACE_IGNORE_EXIST);

  g_return_val_if_fail (!tmpf->anonymous, FALSE);
  g_return_val_if_fail (tmpf->fd >= 0, FALSE);
  g_return_val_if_fail (tmpf->src_dfd == AT_FDCWD || tmpf->src_dfd >= 0, FALSE);

  /* Unlike the original systemd code, this function also supports
   * replacing existing files.
   */

  /* We have `tmpfile_path` for old systems without O_TMPFILE. */
  if (tmpf->path)
    {
      if (replace)
        {
          /* We have a regular tempfile, we're overwriting - this is a
           * simple renameat().
           */
          if (renameat (tmpf->src_dfd, tmpf->path, target_dfd, target) < 0)
            return glnx_throw_errno_prefix (error, "renameat");
        }
      else
        {
          /* We need to use renameat2(..., NOREPLACE) or emulate it */
          if (!rename_file_noreplace_at (tmpf->src_dfd, tmpf->path, target_dfd, target,
                                         ignore_eexist,
                                         error))
            return FALSE;
        }
      /* Now, clear the pointer so we don't try to unlink it */
      g_clear_pointer (&tmpf->path, g_free);
    }
  else
    {
      /* This case we have O_TMPFILE, so our reference to it is via /proc/self/fd */
      char proc_fd_path[strlen("/proc/self/fd/") + DECIMAL_STR_MAX(tmpf->fd) + 1];

      sprintf (proc_fd_path, "/proc/self/fd/%i", tmpf->fd);

      if (replace)
        {
          /* In this case, we had our temp file atomically hidden, but now
           * we need to make it visible in the FS so we can do a rename.
           * Ideally, linkat() would gain AT_REPLACE or so.
           */
          /* TODO - avoid double alloca, we can just alloca a copy of
           * the pathname plus space for tmp.XXXXX */
          char *dnbuf = strdupa (target);
          const char *dn = dirname (dnbuf);
          char *tmpname_buf = glnx_strjoina (dn, "/tmp.XXXXXX");
          guint count;
          const guint count_max = 100;

          for (count = 0; count < count_max; count++)
            {
              glnx_gen_temp_name (tmpname_buf);

              if (linkat (AT_FDCWD, proc_fd_path, target_dfd, tmpname_buf, AT_SYMLINK_FOLLOW) < 0)
                {
                  if (errno == EEXIST)
                    continue;
                  else
                    return glnx_throw_errno_prefix (error, "linkat");
                }
              else
                break;
            }
          if (count == count_max)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
               "Exhausted %u attempts to create temporary file", count);
              return FALSE;
            }
          if (!glnx_renameat (target_dfd, tmpname_buf, target_dfd, target, error))
            {
              /* This is currently the only case where we need to have
               * a cleanup unlinkat() still with O_TMPFILE.
               */
              (void) unlinkat (target_dfd, tmpname_buf, 0);
              return FALSE;
            }
        }
      else
        {
          if (linkat (AT_FDCWD, proc_fd_path, target_dfd, target, AT_SYMLINK_FOLLOW) < 0)
            {
              if (errno == EEXIST && mode == GLNX_LINK_TMPFILE_NOREPLACE_IGNORE_EXIST)
                ;
              else
                return glnx_throw_errno_prefix (error, "linkat");
            }
        }

    }
  return TRUE;
}

/**
 * glnx_openat_rdonly:
 * @dfd: File descriptor for origin directory
 * @path: Pathname, relative to @dfd
 * @follow: Whether or not to follow symbolic links in the final component
 * @out_fd: (out): File descriptor
 * @error: Error
 *
 * Use openat() to open a file, with flags `O_RDONLY | O_CLOEXEC | O_NOCTTY`.
 * Like the other libglnx wrappers, will use `TEMP_FAILURE_RETRY` and
 * also includes @path in @error in case of failure.
 */
gboolean
glnx_openat_rdonly (int             dfd,
                    const char     *path,
                    gboolean        follow,
                    int            *out_fd,
                    GError        **error)
{
  int flags = O_RDONLY | O_CLOEXEC | O_NOCTTY;
  if (!follow)
    flags |= O_NOFOLLOW;
  int fd = TEMP_FAILURE_RETRY (openat (dfd, path, flags));
  if (fd == -1)
    return glnx_throw_errno_prefix (error, "openat(%s)", path);
  *out_fd = fd;
  return TRUE;
}

static guint8*
glnx_fd_readall_malloc (int               fd,
                        gsize            *out_len,
                        gboolean          nul_terminate,
                        GCancellable     *cancellable,
                        GError          **error)
{
  const guint maxreadlen = 4096;

  struct stat stbuf;
  if (TEMP_FAILURE_RETRY (fstat (fd, &stbuf)) < 0)
    return glnx_null_throw_errno (error);

  gsize buf_allocated;
  if (S_ISREG (stbuf.st_mode) && stbuf.st_size > 0)
    buf_allocated = stbuf.st_size;
  else
    buf_allocated = 16;

  g_autofree guint8* buf = g_malloc (buf_allocated);

  gsize buf_size = 0;
  while (TRUE)
    {
      gsize readlen = MIN (buf_allocated - buf_size, maxreadlen);

      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return FALSE;

      gssize bytes_read;
      do
        bytes_read = read (fd, buf + buf_size, readlen);
      while (G_UNLIKELY (bytes_read == -1 && errno == EINTR));
      if (G_UNLIKELY (bytes_read == -1))
        return glnx_null_throw_errno (error);
      if (bytes_read == 0)
        break;

      buf_size += bytes_read;
      if (buf_allocated - buf_size < maxreadlen)
        buf = g_realloc (buf, buf_allocated *= 2);
    }

  if (nul_terminate)
    {
      if (buf_allocated - buf_size == 0)
        buf = g_realloc (buf, buf_allocated + 1);
      buf[buf_size] = '\0';
    }

  *out_len = buf_size;
  return g_steal_pointer (&buf);
}

/**
 * glnx_fd_readall_bytes:
 * @fd: A file descriptor
 * @cancellable: Cancellable:
 * @error: Error
 *
 * Read all data from file descriptor @fd into a #GBytes.  It's
 * recommended to only use this for small files.
 *
 * Returns: (transfer full): A newly allocated #GBytes
 */
GBytes *
glnx_fd_readall_bytes (int               fd,
                       GCancellable     *cancellable,
                       GError          **error)
{
  gsize len;
  guint8 *buf = glnx_fd_readall_malloc (fd, &len, FALSE, cancellable, error);
  if (!buf)
    return NULL;
  return g_bytes_new_take (buf, len);
}

/**
 * glnx_fd_readall_utf8:
 * @fd: A file descriptor
 * @out_len: (out): Returned length
 * @cancellable: Cancellable:
 * @error: Error
 *
 * Read all data from file descriptor @fd, validating
 * the result as UTF-8.
 *
 * Returns: (transfer full): A string validated as UTF-8, or %NULL on error.
 */
char *
glnx_fd_readall_utf8 (int               fd,
                      gsize            *out_len,
                      GCancellable     *cancellable,
                      GError          **error)
{
  gsize len;
  g_autofree guint8 *buf = glnx_fd_readall_malloc (fd, &len, TRUE, cancellable, error);
  if (!buf)
    return FALSE;

  if (!g_utf8_validate ((char*)buf, len, NULL))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_DATA,
                   "Invalid UTF-8");
      return FALSE;
    }

  if (out_len)
    *out_len = len;
  return (char*)g_steal_pointer (&buf);
}

/**
 * glnx_file_get_contents_utf8_at:
 * @dfd: Directory file descriptor
 * @subpath: Path relative to @dfd
 * @out_len: (out) (allow-none): Optional length
 * @cancellable: Cancellable
 * @error: Error
 *
 * Read the entire contents of the file referred
 * to by @dfd and @subpath, validate the result as UTF-8.
 * The length is optionally stored in @out_len.
 *
 * Returns: (transfer full): UTF-8 validated text, or %NULL on error
 */
char *
glnx_file_get_contents_utf8_at (int                   dfd,
                                const char           *subpath,
                                gsize                *out_len,
                                GCancellable         *cancellable,
                                GError              **error)
{
  dfd = glnx_dirfd_canonicalize (dfd);

  glnx_fd_close int fd = -1;
  if (!glnx_openat_rdonly (dfd, subpath, TRUE, &fd, error))
    return NULL;

  gsize len;
  g_autofree char *buf = glnx_fd_readall_utf8 (fd, &len, cancellable, error);
  if (G_UNLIKELY(!buf))
    return FALSE;

  if (out_len)
    *out_len = len;
  return g_steal_pointer (&buf);
}

/**
 * glnx_readlinkat_malloc:
 * @dfd: Directory file descriptor
 * @subpath: Subpath
 * @cancellable: Cancellable
 * @error: Error
 *
 * Read the value of a symlink into a dynamically
 * allocated buffer.
 */
char *
glnx_readlinkat_malloc (int            dfd,
                        const char    *subpath,
                        GCancellable  *cancellable,
                        GError       **error)
{
  size_t l = 100;

  dfd = glnx_dirfd_canonicalize (dfd);

  for (;;)
    {
      g_autofree char *c = NULL;
      ssize_t n;

      c = g_malloc (l);
      n = TEMP_FAILURE_RETRY (readlinkat (dfd, subpath, c, l-1));
      if (n < 0)
        return glnx_null_throw_errno (error);

      if ((size_t) n < l-1)
        {
          c[n] = 0;
          return g_steal_pointer (&c);
        }

      l *= 2;
    }

  g_assert_not_reached ();
}

static gboolean
copy_symlink_at (int                   src_dfd,
                 const char           *src_subpath,
                 const struct stat    *src_stbuf,
                 int                   dest_dfd,
                 const char           *dest_subpath,
                 GLnxFileCopyFlags     copyflags,
                 GCancellable         *cancellable,
                 GError              **error)
{
  g_autofree char *buf = glnx_readlinkat_malloc (src_dfd, src_subpath, cancellable, error);
  if (!buf)
    return FALSE;

  if (TEMP_FAILURE_RETRY (symlinkat (buf, dest_dfd, dest_subpath)) != 0)
    return glnx_throw_errno_prefix (error, "symlinkat");

  if (!(copyflags & GLNX_FILE_COPY_NOXATTRS))
    {
      g_autoptr(GVariant) xattrs = NULL;

      if (!glnx_dfd_name_get_all_xattrs (src_dfd, src_subpath, &xattrs,
                                         cancellable, error))
        return FALSE;

      if (!glnx_dfd_name_set_all_xattrs (dest_dfd, dest_subpath, xattrs,
                                         cancellable, error))
        return FALSE;
    }

  if (TEMP_FAILURE_RETRY (fchownat (dest_dfd, dest_subpath,
                                    src_stbuf->st_uid, src_stbuf->st_gid,
                                    AT_SYMLINK_NOFOLLOW)) != 0)
    return glnx_throw_errno (error);

  return TRUE;
}

#define COPY_BUFFER_SIZE (16*1024)

/* Most of the code below is from systemd, but has been reindented to GNU style,
 * and changed to use POSIX error conventions (return -1, set errno) to more
 * conveniently fit in with the rest of libglnx.
 */

/* Like write(), but loop until @nbytes are written, or an error
 * occurs.
 *
 * On error, -1 is returned an @errno is set.  NOTE: This is an
 * API change from previous versions of this function.
 */
int
glnx_loop_write(int fd, const void *buf, size_t nbytes)
{
  const uint8_t *p = buf;

  g_return_val_if_fail(fd >= 0, -1);
  g_return_val_if_fail(buf, -1);

  errno = 0;

  while (nbytes > 0)
    {
      ssize_t k;

      k = write(fd, p, nbytes);
      if (k < 0)
        {
          if (errno == EINTR)
            continue;

          return -1;
        }

      if (k == 0) /* Can't really happen */
        {
          errno = EIO;
          return -1;
        }

      p += k;
      nbytes -= k;
    }

  return 0;
}

/* Read from @fdf until EOF, writing to @fdt. If max_bytes is -1, a full-file
 * clone will be attempted. Otherwise Linux copy_file_range(), sendfile()
 * syscall will be attempted.  If none of those work, this function will do a
 * plain read()/write() loop.
 *
 * The file descriptor @fdf must refer to a regular file.
 *
 * If provided, @max_bytes specifies the maximum number of bytes to read from @fdf.
 * On error, this function returns `-1` and @errno will be set.
 */
int
glnx_regfile_copy_bytes (int fdf, int fdt, off_t max_bytes)
{
  /* Last updates from systemd as of commit 6bda23dd6aaba50cf8e3e6024248cf736cc443ca */
  static int have_cfr = -1; /* -1 means unknown */
  bool try_cfr = have_cfr != 0;
  static int have_sendfile = -1; /* -1 means unknown */
  bool try_sendfile = have_sendfile != 0;

  g_return_val_if_fail (fdf >= 0, -1);
  g_return_val_if_fail (fdt >= 0, -1);
  g_return_val_if_fail (max_bytes >= -1, -1);

  /* If we've requested to copy the whole range, try a full-file clone first.
   */
  if (max_bytes == (off_t) -1)
    {
      if (ioctl (fdt, FICLONE, fdf) == 0)
        return 0;
      /* Fall through */
      struct stat stbuf;

      /* Gather the size so we can provide the whole thing at once to
       * copy_file_range() or sendfile().
       */
      if (fstat (fdf, &stbuf) < 0)
        return -1;
      max_bytes = stbuf.st_size;
    }

  while (TRUE)
    {
      ssize_t n;

      /* First, try copy_file_range(). Note this is an inlined version of
       * try_copy_file_range() from systemd upstream, which works better since
       * we use POSIX errno style.
       */
      if (try_cfr)
        {
          n = copy_file_range (fdf, NULL, fdt, NULL, max_bytes, 0u);
          if (n < 0)
            {
              if (errno == ENOSYS)
                {
                  /* No cfr in kernel, mark as permanently unavailable
                   * and fall through to sendfile().
                   */
                  have_cfr = 0;
                  try_cfr = false;
                }
              else if (errno == EXDEV)
                /* We won't try cfr again for this run, but let's be
                 * conservative and not mark it as available/unavailable until
                 * we know for sure.
                 */
                try_cfr = false;
              else
                return -1;
            }
          else
            {
              /* cfr worked, mark it as available */
              if (have_cfr == -1)
                have_cfr = 1;

              if (n == 0) /* EOF */
                break;
              else
                /* Success! */
                goto next;
            }
        }

      /* Next try sendfile(); this version is also changed from systemd upstream
       * to match the same logic we have for copy_file_range().
       */
      if (try_sendfile)
        {
          n = sendfile (fdt, fdf, NULL, max_bytes);
          if (n < 0)
            {
              if (G_IN_SET (errno, EINVAL, ENOSYS))
                {
                  /* No sendfile(), or it doesn't work on regular files.
                   * Mark it as permanently unavailable, and fall through
                   * to plain read()/write().
                   */
                  have_sendfile = 0;
                  try_sendfile = false;
                }
              else
                return -1;
            }
          else
            {
              /* sendfile() worked, mark it as available */
              if (have_sendfile == -1)
                have_sendfile = 1;

              if (n == 0) /* EOF */
                break;
              else if (n > 0)
                /* Succcess! */
                goto next;
            }
        }

      /* As a fallback just copy bits by hand */
      { size_t m = COPY_BUFFER_SIZE;
        if (max_bytes != (off_t) -1)
          {
            if ((off_t) m > max_bytes)
              m = (size_t) max_bytes;
          }
        char buf[m];

        n = TEMP_FAILURE_RETRY (read (fdf, buf, m));
        if (n < 0)
          return -1;
        if (n == 0) /* EOF */
          break;

        if (glnx_loop_write (fdt, buf, (size_t) n) < 0)
          return -1;
      }

    next:
      if (max_bytes != (off_t) -1)
        {
          g_assert_cmpint (max_bytes, >=, n);
          max_bytes -= n;
          if (max_bytes == 0)
            break;
        }
    }

  return 0;
}

/**
 * glnx_file_copy_at:
 * @src_dfd: Source directory fd
 * @src_subpath: Subpath relative to @src_dfd
 * @dest_dfd: Target directory fd
 * @dest_subpath: Destination name
 * @copyflags: Flags
 * @cancellable: cancellable
 * @error: Error
 *
 * Perform a full copy of the regular file or
 * symbolic link from @src_subpath to @dest_subpath.
 *
 * If @src_subpath is anything other than a regular
 * file or symbolic link, an error will be returned.
 */
gboolean
glnx_file_copy_at (int                   src_dfd,
                   const char           *src_subpath,
                   struct stat          *src_stbuf,
                   int                   dest_dfd,
                   const char           *dest_subpath,
                   GLnxFileCopyFlags     copyflags,
                   GCancellable         *cancellable,
                   GError              **error)
{
  gboolean ret = FALSE;
  int r;
  int dest_open_flags;
  struct timespec ts[2];
  glnx_fd_close int src_fd = -1;
  glnx_fd_close int dest_fd = -1;
  struct stat local_stbuf;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    goto out;

  src_dfd = glnx_dirfd_canonicalize (src_dfd);
  dest_dfd = glnx_dirfd_canonicalize (dest_dfd);

  /* Automatically do stat() if no stat buffer was supplied */
  if (!src_stbuf)
    {
      if (fstatat (src_dfd, src_subpath, &local_stbuf, AT_SYMLINK_NOFOLLOW) != 0)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
      src_stbuf = &local_stbuf;
    }

  if (S_ISLNK (src_stbuf->st_mode))
    {
      return copy_symlink_at (src_dfd, src_subpath, src_stbuf,
                              dest_dfd, dest_subpath,
                              copyflags,
                              cancellable, error);
    }
  else if (!S_ISREG (src_stbuf->st_mode))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Cannot copy non-regular/non-symlink file: %s", src_subpath);
      goto out;
    }

  if (!glnx_openat_rdonly (src_dfd, src_subpath, FALSE, &src_fd, error))
    goto out;

  dest_open_flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NOCTTY;
  if (!(copyflags & GLNX_FILE_COPY_OVERWRITE))
    dest_open_flags |= O_EXCL;
  else
    dest_open_flags |= O_TRUNC;

  dest_fd = TEMP_FAILURE_RETRY (openat (dest_dfd, dest_subpath, dest_open_flags, src_stbuf->st_mode));
  if (dest_fd == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  r = glnx_regfile_copy_bytes (src_fd, dest_fd, (off_t) -1);
  if (r < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (fchown (dest_fd, src_stbuf->st_uid, src_stbuf->st_gid) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (fchmod (dest_fd, src_stbuf->st_mode & 07777) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  ts[0] = src_stbuf->st_atim;
  ts[1] = src_stbuf->st_mtim;
  (void) futimens (dest_fd, ts);

  if (!(copyflags & GLNX_FILE_COPY_NOXATTRS))
    {
      g_autoptr(GVariant) xattrs = NULL;

      if (!glnx_fd_get_all_xattrs (src_fd, &xattrs,
                                   cancellable, error))
        goto out;

      if (!glnx_fd_set_all_xattrs (dest_fd, xattrs,
                                   cancellable, error))
        goto out;
    }

  if (copyflags & GLNX_FILE_COPY_DATASYNC)
    {
      if (fdatasync (dest_fd) < 0)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }
  
  r = close (dest_fd);
  dest_fd = -1;
  if (r < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  ret = TRUE;
 out:
  if (!ret)
    (void) unlinkat (dest_dfd, dest_subpath, 0);
  return ret;
}

/**
 * glnx_file_replace_contents_at:
 * @dfd: Directory fd
 * @subpath: Subpath
 * @buf: (array len=len) (element-type guint8): File contents
 * @len: Length (if `-1`, assume @buf is `NUL` terminated)
 * @flags: Flags
 * @cancellable: Cancellable
 * @error: Error
 *
 * Create a new file, atomically replacing the contents of @subpath
 * (relative to @dfd) with @buf.  By default, if the file already
 * existed, fdatasync() will be used before rename() to ensure stable
 * contents.  This and other behavior can be controlled via @flags.
 *
 * Note that no metadata from the existing file is preserved, such as
 * uid/gid or extended attributes.  The default mode will be `0666`,
 * modified by umask.
 */ 
gboolean
glnx_file_replace_contents_at (int                   dfd,
                               const char           *subpath,
                               const guint8         *buf,
                               gsize                 len,
                               GLnxFileReplaceFlags  flags,
                               GCancellable         *cancellable,
                               GError              **error)
{
  return glnx_file_replace_contents_with_perms_at (dfd, subpath, buf, len,
                                                   (mode_t) -1, (uid_t) -1, (gid_t) -1,
                                                   flags, cancellable, error);
}

/**
 * glnx_file_replace_contents_with_perms_at:
 * @dfd: Directory fd
 * @subpath: Subpath
 * @buf: (array len=len) (element-type guint8): File contents
 * @len: Length (if `-1`, assume @buf is `NUL` terminated)
 * @mode: File mode; if `-1`, use `0666 - umask`
 * @flags: Flags
 * @cancellable: Cancellable
 * @error: Error
 *
 * Like glnx_file_replace_contents_at(), but also supports
 * setting mode, and uid/gid.
 */ 
gboolean
glnx_file_replace_contents_with_perms_at (int                   dfd,
                                          const char           *subpath,
                                          const guint8         *buf,
                                          gsize                 len,
                                          mode_t                mode,
                                          uid_t                 uid,
                                          gid_t                 gid,
                                          GLnxFileReplaceFlags  flags,
                                          GCancellable         *cancellable,
                                          GError              **error)
{
  char *dnbuf = strdupa (subpath);
  const char *dn = dirname (dnbuf);

  dfd = glnx_dirfd_canonicalize (dfd);

  /* With O_TMPFILE we can't use umask, and we can't sanely query the
   * umask...let's assume something relatively standard.
   */
  if (mode == (mode_t) -1)
    mode = 0644;

  g_auto(GLnxTmpfile) tmpf = { 0, };
  if (!glnx_open_tmpfile_linkable_at (dfd, dn, O_WRONLY | O_CLOEXEC,
                                      &tmpf, error))
    return FALSE;

  if (len == -1)
    len = strlen ((char*)buf);

  if (!glnx_try_fallocate (tmpf.fd, 0, len, error))
    return FALSE;

  if (glnx_loop_write (tmpf.fd, buf, len) < 0)
    return glnx_throw_errno (error);

  if (!(flags & GLNX_FILE_REPLACE_NODATASYNC))
    {
      struct stat stbuf;
      gboolean do_sync;

      if (fstatat (dfd, subpath, &stbuf, AT_SYMLINK_NOFOLLOW) != 0)
        {
          if (errno != ENOENT)
            return glnx_throw_errno (error);
          do_sync = (flags & GLNX_FILE_REPLACE_DATASYNC_NEW) > 0;
        }
      else
        do_sync = TRUE;

      if (do_sync)
        {
          if (fdatasync (tmpf.fd) != 0)
            return glnx_throw_errno_prefix (error, "fdatasync");
        }
    }

  if (uid != (uid_t) -1)
    {
      if (fchown (tmpf.fd, uid, gid) != 0)
        return glnx_throw_errno (error);
    }

  if (fchmod (tmpf.fd, mode) != 0)
    return glnx_throw_errno (error);

  if (!glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_REPLACE,
                             dfd, subpath, error))
    return FALSE;

  return TRUE;
}
