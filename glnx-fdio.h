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

#pragma once

#include <glnx-backport-autocleanups.h>
#include <gio/gfiledescriptorbased.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <sys/xattr.h>
/* From systemd/src/shared/util.h */
/* When we include libgen.h because we need dirname() we immediately
 * undefine basename() since libgen.h defines it as a macro to the XDG
 * version which is really broken. */
#include <libgen.h>
#undef basename

#include <glnx-macros.h>
#include <glnx-errors.h>

G_BEGIN_DECLS

/* Irritatingly, g_basename() which is what we want
 * is deprecated.
 */
static inline
const char *glnx_basename (const char *path)
{
  return (basename) (path);
}

typedef struct {
  gboolean initialized;
  gboolean anonymous;
  int src_dfd;
  int fd;
  char *path;
} GLnxTmpfile;
void glnx_tmpfile_clear (GLnxTmpfile *tmpf);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GLnxTmpfile, glnx_tmpfile_clear)

gboolean
glnx_open_anonymous_tmpfile (int flags,
                             GLnxTmpfile *out_tmpf,
                             GError **error);

gboolean
glnx_open_tmpfile_linkable_at (int dfd,
                               const char *subpath,
                               int flags,
                               GLnxTmpfile *out_tmpf,
                               GError **error);

typedef enum {
  GLNX_LINK_TMPFILE_REPLACE,
  GLNX_LINK_TMPFILE_NOREPLACE,
  GLNX_LINK_TMPFILE_NOREPLACE_IGNORE_EXIST
} GLnxLinkTmpfileReplaceMode;

gboolean
glnx_link_tmpfile_at (GLnxTmpfile *tmpf,
                      GLnxLinkTmpfileReplaceMode flags,
                      int target_dfd,
                      const char *target,
                      GError **error);

GBytes *
glnx_fd_readall_bytes (int               fd,
                       GCancellable     *cancellable,
                       GError          **error);

char *
glnx_fd_readall_utf8 (int               fd,
                      gsize            *out_len,
                      GCancellable     *cancellable,
                      GError          **error);

char *
glnx_file_get_contents_utf8_at (int                   dfd,
                                const char           *subpath,
                                gsize                *out_len,
                                GCancellable         *cancellable,
                                GError              **error);

/**
 * GLnxFileReplaceFlags:
 * @GLNX_FILE_REPLACE_DATASYNC_NEW: Call fdatasync() even if the file did not exist
 * @GLNX_FILE_REPLACE_NODATASYNC: Never call fdatasync()
 *
 * Flags controlling file replacement.
 */
typedef enum {
  GLNX_FILE_REPLACE_DATASYNC_NEW = (1 << 0),
  GLNX_FILE_REPLACE_NODATASYNC = (1 << 1),
} GLnxFileReplaceFlags;

gboolean
glnx_file_replace_contents_at (int                   dfd,
                               const char           *subpath,
                               const guint8         *buf,
                               gsize                 len,
                               GLnxFileReplaceFlags  flags,
                               GCancellable         *cancellable,
                               GError              **error);

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
                                          GError              **error);

char *
glnx_readlinkat_malloc (int            dfd,
                        const char    *subpath,
                        GCancellable  *cancellable,
                        GError       **error);

int
glnx_loop_write (int fd, const void *buf, size_t nbytes);

int
glnx_regfile_copy_bytes (int fdf, int fdt, off_t max_bytes, gboolean try_reflink);

typedef enum {
  GLNX_FILE_COPY_OVERWRITE = (1 << 0),
  GLNX_FILE_COPY_NOXATTRS = (1 << 1),
  GLNX_FILE_COPY_DATASYNC = (1 << 2)
} GLnxFileCopyFlags;

gboolean
glnx_file_copy_at (int                   src_dfd,
                   const char           *src_subpath,
                   struct stat          *src_stbuf,
                   int                   dest_dfd,
                   const char           *dest_subpath,
                   GLnxFileCopyFlags     copyflags,
                   GCancellable         *cancellable,
                   GError              **error);

gboolean
glnx_stream_fstat (GFileDescriptorBased *stream,
                   struct stat          *stbuf,
                   GError              **error);

int glnx_renameat2_noreplace (int olddirfd, const char *oldpath,
                              int newdirfd, const char *newpath);
int glnx_renameat2_exchange (int olddirfd, const char *oldpath,
                             int newdirfd, const char *newpath);

/**
 * glnx_try_fallocate:
 * @fd: File descriptor
 * @size: Size
 * @error: Error
 *
 * Wrapper for Linux fallocate().  Explicitly ignores a @size of zero.
 * Also, will silently do nothing if the underlying filesystem doesn't
 * support it.  Use this instead of posix_fallocate(), since the glibc fallback
 * is bad: https://sourceware.org/bugzilla/show_bug.cgi?id=18515
 */
static inline gboolean
glnx_try_fallocate (int      fd,
                    off_t    offset,
                    off_t    size,
                    GError **error)
{
  /* This is just nicer than throwing an error */
  if (size == 0)
    return TRUE;

  if (fallocate (fd, 0, offset, size) < 0)
    {
      if (G_IN_SET(errno, ENOSYS, EOPNOTSUPP))
        ; /* Ignore */
      else
        return glnx_throw_errno_prefix (error, "fallocate");
    }

  return TRUE;
}

/**
 * glnx_fstat:
 * @fd: FD to stat
 * @buf: (out caller-allocates): Return location for stat details
 * @error: Return location for a #GError, or %NULL
 *
 * Wrapper around fstat() which adds #GError support and ensures that it retries
 * on %EINTR.
 *
 * Returns: %TRUE on success, %FALSE otherwise
 * Since: UNRELEASED
 */
static inline gboolean
glnx_fstat (int           fd,
            struct stat  *buf,
            GError      **error)
{
  if (TEMP_FAILURE_RETRY (fstat (fd, buf)) != 0)
    return glnx_throw_errno (error);

  return TRUE;
}

/**
 * glnx_fstatat:
 * @dfd: Directory FD to stat beneath
 * @path: Path to stat beneath @dfd
 * @buf: (out caller-allocates): Return location for stat details
 * @flags: Flags to pass to fstatat()
 * @error: Return location for a #GError, or %NULL
 *
 * Wrapper around fstatat() which adds #GError support and ensures that it
 * retries on %EINTR.
 *
 * Returns: %TRUE on success, %FALSE otherwise
 * Since: UNRELEASED
 */
static inline gboolean
glnx_fstatat (int           dfd,
              const gchar  *path,
              struct stat  *buf,
              int           flags,
              GError      **error)
{
  if (TEMP_FAILURE_RETRY (fstatat (dfd, path, buf, flags)) != 0)
    return glnx_throw_errno_prefix (error, "fstatat(%s)", path);
  return TRUE;
}

/**
 * glnx_renameat:
 *
 * Wrapper around renameat() which adds #GError support and ensures that it
 * retries on %EINTR.
 */
static inline gboolean
glnx_renameat (int           src_dfd,
               const gchar  *src_path,
               int           dest_dfd,
               const gchar  *dest_path,
               GError      **error)
{
  if (TEMP_FAILURE_RETRY (renameat (src_dfd, src_path, dest_dfd, dest_path)) != 0)
    return glnx_throw_errno_prefix (error, "renameat(%s, %s)", src_path, dest_path);
  return TRUE;
}

/**
 * glnx_unlinkat:
 *
 * Wrapper around unlinkat() which adds #GError support and ensures that it
 * retries on %EINTR.
 */
static inline gboolean
glnx_unlinkat (int           dfd,
               const gchar  *path,
               int           flags,
               GError      **error)
{
  if (TEMP_FAILURE_RETRY (unlinkat (dfd, path, flags)) != 0)
    return glnx_throw_errno_prefix (error, "unlinkat(%s)", path);
  return TRUE;
}

G_END_DECLS
