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
/* See linux.git/fs/btrfs/ioctl.h */
#define BTRFS_IOCTL_MAGIC 0x94
#define BTRFS_IOC_CLONE _IOW(BTRFS_IOCTL_MAGIC, 9, int)

#include <glnx-fdio.h>
#include <glnx-dirfd.h>
#include <glnx-errors.h>
#include <glnx-xattrs.h>
#include <glnx-backport-autoptr.h>
#include <glnx-local-alloc.h>

static guint8*
glnx_fd_readall_malloc (int               fd,
                        gsize            *out_len,
                        gboolean          nul_terminate,
                        GCancellable     *cancellable,
                        GError          **error)
{
  gboolean success = FALSE;
  const guint maxreadlen = 4096;
  int res;
  struct stat stbuf;
  guint8* buf = NULL;
  gsize buf_allocated;
  gsize buf_size = 0;
  gssize bytes_read;

  do
    res = fstat (fd, &stbuf);
  while (G_UNLIKELY (res == -1 && errno == EINTR));
  if (res == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (S_ISREG (stbuf.st_mode) && stbuf.st_size > 0)
    buf_allocated = stbuf.st_size;
  else
    buf_allocated = 16;
        
  buf = g_malloc (buf_allocated);

  while (TRUE)
    {
      gsize readlen = MIN (buf_allocated - buf_size, maxreadlen);
      
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        goto out;

      do
        bytes_read = read (fd, buf + buf_size, readlen);
      while (G_UNLIKELY (bytes_read == -1 && errno == EINTR));
      if (G_UNLIKELY (bytes_read == -1))
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
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

  success = TRUE;
 out:
  if (success)
    {
      *out_len = buf_size;
      return buf;
    }
  g_free (buf);
  return NULL;
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
  guint8 *buf;
  gsize len;
  
  buf = glnx_fd_readall_malloc (fd, &len, FALSE, cancellable, error);
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
  gboolean success = FALSE;
  guint8 *buf;
  gsize len;
  
  buf = glnx_fd_readall_malloc (fd, &len, TRUE, cancellable, error);
  if (!buf)
    goto out;

  if (!g_utf8_validate ((char*)buf, len, NULL))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_DATA,
                   "Invalid UTF-8");
      goto out;
    }

  success = TRUE;
 out:
  if (success)
    {
      if (out_len)
        *out_len = len;
      return (char*)buf;
    }
  g_free (buf);
  return NULL;
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
  gboolean success = FALSE;
  glnx_fd_close int fd = -1;
  char *buf = NULL;
  gsize len;

  dfd = glnx_dirfd_canonicalize (dfd);

  do
    fd = openat (dfd, subpath, O_RDONLY | O_NOCTTY | O_CLOEXEC);
  while (G_UNLIKELY (fd == -1 && errno == EINTR));
  if (G_UNLIKELY (fd == -1))
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  buf = glnx_fd_readall_utf8 (fd, &len, cancellable, error);
  if (G_UNLIKELY(!buf))
    goto out;
  
  success = TRUE;
 out:
  if (success)
    {
      if (out_len)
        *out_len = len;
      return buf;
    }
  g_free (buf);
  return NULL;
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
      char *c;
      ssize_t n;

      c = g_malloc (l);
      n = TEMP_FAILURE_RETRY (readlinkat (dfd, subpath, c, l-1));
      if (n < 0)
        {
          glnx_set_error_from_errno (error);
          g_free (c);
          return FALSE;
        }

      if ((size_t) n < l-1)
        {
          c[n] = 0;
          return c;
        }

      g_free (c);
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
  gboolean ret = FALSE;
  g_autofree char *buf = NULL;

  buf = glnx_readlinkat_malloc (src_dfd, src_subpath, cancellable, error);
  if (!buf)
    goto out;

  if (TEMP_FAILURE_RETRY (symlinkat (buf, dest_dfd, dest_subpath)) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }
  
  if (!(copyflags & GLNX_FILE_COPY_NOXATTRS))
    {
      g_autoptr(GVariant) xattrs = NULL;

      if (!glnx_dfd_name_get_all_xattrs (src_dfd, src_subpath, &xattrs,
                                         cancellable, error))
        goto out;

      if (!glnx_dfd_name_set_all_xattrs (dest_dfd, dest_subpath, xattrs,
                                         cancellable, error))
        goto out;
    }
  
  if (TEMP_FAILURE_RETRY (fchownat (dest_dfd, dest_subpath,
                                    src_stbuf->st_uid, src_stbuf->st_gid,
                                    AT_SYMLINK_NOFOLLOW)) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

#define COPY_BUFFER_SIZE (16*1024)

/* From systemd */

static int btrfs_reflink(int infd, int outfd) {
        int r;

        g_return_val_if_fail(infd >= 0, -1);
        g_return_val_if_fail(outfd >= 0, -1);

        r = ioctl(outfd, BTRFS_IOC_CLONE, infd);
        if (r < 0)
                return -errno;

        return 0;
}

int glnx_loop_write(int fd, const void *buf, size_t nbytes) {
        const uint8_t *p = buf;

        g_return_val_if_fail(fd >= 0, -1);
        g_return_val_if_fail(buf, -1);

        errno = 0;

        while (nbytes > 0) {
                ssize_t k;

                k = write(fd, p, nbytes);
                if (k < 0) {
                        if (errno == EINTR)
                                continue;

                        return -errno;
                }

                if (k == 0) /* Can't really happen */
                        return -EIO;

                p += k;
                nbytes -= k;
        }

        return 0;
}

static int copy_bytes(int fdf, int fdt, off_t max_bytes, bool try_reflink) {
        bool try_sendfile = true;
        int r;

        g_return_val_if_fail (fdf >= 0, -1);
        g_return_val_if_fail (fdt >= 0, -1);

        /* Try btrfs reflinks first. */
        if (try_reflink && max_bytes == (off_t) -1) {
                r = btrfs_reflink(fdf, fdt);
                if (r >= 0)
                        return r;
        }

        for (;;) {
                size_t m = COPY_BUFFER_SIZE;
                ssize_t n;

                if (max_bytes != (off_t) -1) {

                        if (max_bytes <= 0)
                                return -EFBIG;

                        if ((off_t) m > max_bytes)
                                m = (size_t) max_bytes;
                }

                /* First try sendfile(), unless we already tried */
                if (try_sendfile) {

                        n = sendfile(fdt, fdf, NULL, m);
                        if (n < 0) {
                                if (errno != EINVAL && errno != ENOSYS)
                                        return -errno;

                                try_sendfile = false;
                                /* use fallback below */
                        } else if (n == 0) /* EOF */
                                break;
                        else if (n > 0)
                                /* Succcess! */
                                goto next;
                }

                /* As a fallback just copy bits by hand */
                {
                        char buf[m];

                        n = read(fdf, buf, m);
                        if (n < 0)
                                return -errno;
                        if (n == 0) /* EOF */
                                break;

                        r = glnx_loop_write(fdt, buf, (size_t) n);
                        if (r < 0)
                                return r;
                }

        next:
                if (max_bytes != (off_t) -1) {
                        g_assert(max_bytes >= n);
                        max_bytes -= n;
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

  src_fd = TEMP_FAILURE_RETRY (openat (src_dfd, src_subpath, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NOFOLLOW));
  if (src_fd == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

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

  r = copy_bytes (src_fd, dest_fd, (off_t) -1, TRUE);
  if (r < 0)
    {
      errno = -r;
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
  gboolean ret = FALSE;
  int r;
  /* We use the /proc/self trick as there's no mkostemp_at() yet */
  g_autofree char *tmppath = g_strdup_printf ("/proc/self/fd/%d/.tmpXXXXXX", dfd);
  glnx_fd_close int fd = -1;

  dfd = glnx_dirfd_canonicalize (dfd);

  if ((fd = g_mkstemp_full (tmppath, O_WRONLY | O_CLOEXEC,
                            mode == (mode_t) -1 ? 0666 : mode)) == -1)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (len == -1)
    len = strlen ((char*)buf);

  /* Note that posix_fallocate does *not* set errno but returns it. */
  r = posix_fallocate (fd, 0, len);
  if (r != 0)
    {
      errno = r;
      glnx_set_error_from_errno (error);
      goto out;
    }

  if ((r = glnx_loop_write (fd, buf, len)) != 0)
    {
      errno = -r;
      glnx_set_error_from_errno (error);
      goto out;
    }
    
  if (!(flags & GLNX_FILE_REPLACE_NODATASYNC))
    {
      struct stat stbuf;
      gboolean do_sync;
      
      if (fstatat (dfd, subpath, &stbuf, AT_SYMLINK_NOFOLLOW) != 0)
        {
          if (errno != ENOENT)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
          do_sync = (flags & GLNX_FILE_REPLACE_DATASYNC_NEW) > 0;
        }
      else
        do_sync = TRUE;

      if (do_sync)
        {
          if (fdatasync (fd) != 0)
            {
              glnx_set_error_from_errno (error);
              goto out;
            }
        }
    }

  if (uid != (uid_t) -1)
    {
      if (fchown (fd, uid, gid) != 0)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  /* If a mode was forced, override umask */
  if (mode != (mode_t) -1)
    {
      if (fchmod (fd, mode) != 0)
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  if (renameat (dfd, tmppath, dfd, subpath) != 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}
