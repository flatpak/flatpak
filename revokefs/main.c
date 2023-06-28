/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright (C) 2015,2016 Colin Walters <walters@verbum.org>
 * Copyright (C) 2018 Alexander Larsson <alexl@redhat.com>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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

#ifndef FUSE_USE_VERSION
#error config.h needs to define FUSE_USE_VERSION
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <stdio.h>
#include <err.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/xattr.h>
#include <dirent.h>
#include <unistd.h>
#include <fuse.h>

#include <glib.h>

#include "writer.h"
#include "libglnx.h"

/* fh >= REMOTE_FD_OFFSET means the fd is in the writer side, otherwise it is local */
#define REMOTE_FD_OFFSET ((guint64)G_MAXUINT32)

// Global to store our read-write path
static char *base_path = NULL;
static int basefd = -1;
static int writer_socket = -1;

static inline const char *
ENSURE_RELPATH (const char *path)
{
  path = path + strspn (path, "/");
  if (*path == 0)
    return ".";
  return path;
}

static int
#if FUSE_USE_VERSION >= 31
callback_getattr (const char *path, struct stat *st_data, struct fuse_file_info *finfo)
#else
callback_getattr (const char *path, struct stat *st_data)
#endif
{
  path = ENSURE_RELPATH (path);
  if (!*path)
    {
      if (fstat (basefd, st_data) == -1)
        return -errno;
    }
  else
    {
      if (fstatat (basefd, path, st_data, AT_SYMLINK_NOFOLLOW) == -1)
        return -errno;
    }
  return 0;
}

static int
callback_readlink (const char *path, char *buf, size_t size)
{
  int r;

  path = ENSURE_RELPATH (path);

  /* Note FUSE wants the string to be always nul-terminated, even if
   * truncated.
   */
  r = readlinkat (basefd, path, buf, size - 1);
  if (r == -1)
    return -errno;
  buf[r] = '\0';
  return 0;
}

static int
#if FUSE_USE_VERSION >= 31
callback_readdir (const char *path, void *buf, fuse_fill_dir_t filler,
                  off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
#else
callback_readdir (const char *path, void *buf, fuse_fill_dir_t filler,
                  off_t offset, struct fuse_file_info *fi)
#endif
{
  DIR *dp;
  struct dirent *de;
  int dfd;

  path = ENSURE_RELPATH (path);

  if (!*path)
    {
      dfd = fcntl (basefd, F_DUPFD_CLOEXEC, 3);
      if (dfd < 0)
        return -errno;
      lseek (dfd, 0, SEEK_SET);
    }
  else
    {
      dfd = openat (basefd, path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
      if (dfd == -1)
        return -errno;
    }

  /* Transfers ownership of fd */
  dp = fdopendir (dfd);
  if (dp == NULL)
    return -errno;

  while ((de = readdir (dp)) != NULL)
    {
      struct stat st;
      memset (&st, 0, sizeof (st));
      st.st_ino = de->d_ino;
      st.st_mode = de->d_type << 12;

#if FUSE_USE_VERSION >= 31
      if (filler (buf, de->d_name, &st, 0, 0))
        break;
#else
      if (filler (buf, de->d_name, &st, 0))
        break;
#endif
    }

  (void) closedir (dp);
  return 0;
}

static int
callback_mknod (const char *path, mode_t mode, dev_t rdev)
{
  return -EROFS;
}

static int
callback_mkdir (const char *path, mode_t mode)
{
  path = ENSURE_RELPATH (path);
  return request_mkdir (writer_socket, path, mode);
}

static int
callback_unlink (const char *path)
{
  path = ENSURE_RELPATH (path);
  return request_unlink (writer_socket, path);
}

static int
callback_rmdir (const char *path)
{
  path = ENSURE_RELPATH (path);
  return request_rmdir (writer_socket, path);
}

static int
callback_symlink (const char *from, const char *to)
{
  struct stat stbuf;
  int res;

  to = ENSURE_RELPATH (to);

  res = request_symlink (writer_socket, from, to);
  if (res < 0)
    return res;

  if (fstatat (basefd, to, &stbuf, AT_SYMLINK_NOFOLLOW) == -1)
    {
      fprintf (stderr, "Failed to find newly created symlink '%s': %s\n",
               to, g_strerror (errno));
      exit (EXIT_FAILURE);
    }
  return 0;
}

static int
#if FUSE_USE_VERSION >= 31
callback_rename (const char *from, const char *to, unsigned int flags)
#else
callback_rename (const char *from, const char *to)
#endif
{
#if FUSE_USE_VERSION < 31
  unsigned int flags = 0;
#endif

  from = ENSURE_RELPATH (from);
  to = ENSURE_RELPATH (to);

  return request_rename (writer_socket, from, to, flags);
}

static int
callback_link (const char *from, const char *to)
{
  from = ENSURE_RELPATH (from);
  to = ENSURE_RELPATH (to);

  return request_link (writer_socket, from, to);
}

static int
#if FUSE_USE_VERSION >= 31
callback_chmod (const char *path, mode_t mode, struct fuse_file_info *finfo)
#else
callback_chmod (const char *path, mode_t mode)
#endif
{
  path = ENSURE_RELPATH (path);
  return request_chmod (writer_socket, path, mode);
}

static int
#if FUSE_USE_VERSION >= 31
callback_chown (const char *path, uid_t uid, gid_t gid, struct fuse_file_info *finfo)
#else
callback_chown (const char *path, uid_t uid, gid_t gid)
#endif
{
  path = ENSURE_RELPATH (path);
  return request_chown (writer_socket, path, uid, gid);
}

static int
#if FUSE_USE_VERSION >= 31
callback_truncate (const char *path, off_t size, struct fuse_file_info *finfo)
#else
callback_truncate (const char *path, off_t size)
#endif
{
  path = ENSURE_RELPATH (path);
  return request_truncate (writer_socket, path, size);
}

static int
#if FUSE_USE_VERSION >= 31
callback_utimens (const char *path, const struct timespec tv[2], struct fuse_file_info *finfo)
#else
callback_utimens (const char *path, const struct timespec tv[2])
#endif
{
  path = ENSURE_RELPATH (path);

  return request_utimens (writer_socket, path, tv);
}

static int
do_open (const char *path, mode_t mode, struct fuse_file_info *finfo)
{
  int fd;

  path = ENSURE_RELPATH (path);

  if ((finfo->flags & O_ACCMODE) == O_RDONLY)
    {
      /* Read */
      fd = openat (basefd, path, finfo->flags, mode);
      if (fd == -1)
        return -errno;

      finfo->fh = fd;
    }
  else
    {
      /* Write */

      fd = request_open (writer_socket, path, mode, finfo->flags);
      if (fd < 0)
        return fd;

      finfo->fh = fd + REMOTE_FD_OFFSET;

      /* Ensure all I/O requests bypass the page cache and are sent to
       * the backend. */
      finfo->direct_io = 1;
    }

  return 0;
}

static int
callback_open (const char *path, struct fuse_file_info *finfo)
{
  return do_open (path, 0, finfo);
}

static int
callback_create(const char *path, mode_t mode, struct fuse_file_info *finfo)
{
  return do_open (path, mode, finfo);
}

static int
callback_read (const char *path, char *buf, size_t size, off_t offset,
               struct fuse_file_info *finfo)
{
  int r;
  if (finfo->fh >= REMOTE_FD_OFFSET)
    {
      return request_read (writer_socket, finfo->fh - REMOTE_FD_OFFSET, buf, size, offset);
    }
  else
    {
      r = pread (finfo->fh, buf, size, offset);
      if (r == -1)
        return -errno;
      return r;
    }
}

static int
callback_write (const char *path, const char *buf, size_t size, off_t offset,
                struct fuse_file_info *finfo)
{
  int r;

  if (finfo->fh >= REMOTE_FD_OFFSET)
    {
      return request_write (writer_socket, finfo->fh - REMOTE_FD_OFFSET, buf, size, offset);
    }
  else
    {
      r = pwrite (finfo->fh, buf, size, offset);
      if (r == -1)
        return -errno;
      return r;
    }
}

static int
callback_statfs (const char *path, struct statvfs *st_buf)
{
  if (fstatvfs (basefd, st_buf) == -1)
    return -errno;
  return 0;
}

static int
callback_release (const char *path, struct fuse_file_info *finfo)
{
  if (finfo->fh >= REMOTE_FD_OFFSET)
    {
      return request_close (writer_socket, finfo->fh - REMOTE_FD_OFFSET);
    }
  else
    {
      (void) close (finfo->fh);
      return 0;
    }
}

static int
callback_fsync (const char *path, int crap, struct fuse_file_info *finfo)
{
  if (finfo->fh >= REMOTE_FD_OFFSET)
    {
      return request_fsync (writer_socket, finfo->fh - REMOTE_FD_OFFSET);
    }
  else
    {
      if (fsync (finfo->fh) == -1)
        return -errno;
      return 0;
    }
}

static int
callback_access (const char *path, int mode)
{
  path = ENSURE_RELPATH (path);

  /* Apparently at least GNU coreutils rm calls `faccessat(W_OK)`
   * before trying to do an unlink.  So...we'll just lie about
   * writable access here.
   */
  if (faccessat (basefd, path, mode, AT_SYMLINK_NOFOLLOW) == -1)
    return -errno;
  return 0;
}

static int
callback_setxattr (const char *path, const char *name, const char *value,
                   size_t size, int flags)
{
  return -ENOTSUP;
}

static int
callback_getxattr (const char *path, const char *name, char *value,
                   size_t size)
{
  return -ENOTSUP;
}

/*
 * List the supported extended attributes.
 */
static int
callback_listxattr (const char *path, char *list, size_t size)
{
  return -ENOTSUP;

}

/*
 * Remove an extended attribute.
 */
static int
callback_removexattr (const char *path, const char *name)
{
  return -ENOTSUP;

}

struct fuse_operations callback_oper = {
  .getattr = callback_getattr,
  .readlink = callback_readlink,
  .readdir = callback_readdir,
  .mknod = callback_mknod,
  .mkdir = callback_mkdir,
  .symlink = callback_symlink,
  .unlink = callback_unlink,
  .rmdir = callback_rmdir,
  .rename = callback_rename,
  .link = callback_link,
  .chmod = callback_chmod,
  .chown = callback_chown,
  .truncate = callback_truncate,
  .utimens = callback_utimens,
  .create = callback_create,
  .open = callback_open,
  .read = callback_read,
  .write = callback_write,
  .statfs = callback_statfs,
  .release = callback_release,
  .fsync = callback_fsync,
  .access = callback_access,

  /* Extended attributes support for userland interaction */
  .setxattr = callback_setxattr,
  .getxattr = callback_getxattr,
  .listxattr = callback_listxattr,
  .removexattr = callback_removexattr
};

enum {
  KEY_HELP,
};

static void
usage (const char *progname)
{
  fprintf (stdout,
           "usage: %s basepath mountpoint [options]\n"
           "\n"
           "   Makes basepath visible at mountpoint such that files are writeable only through\n"
           "   fd passed in the --socket argument.\n"
           "\n"
           "general options:\n"
           "   -o opt,[opt...]     mount options\n"
           "   -h  --help          print help\n"
           "   --socket=fd         Pass in the socket fd\n"
           "   --backend           Run the backend instead of fuse\n"
           "   --exit-with-fd=fd   With --backend, exit when the given file descriptor is closed\n"
           "\n", progname);
}

static int
revokefs_opt_proc (void *data,
                   const char *arg,
                   int key,
                   struct fuse_args *outargs)
{
  (void) data;

  switch (key)
    {
    case FUSE_OPT_KEY_NONOPT:
      if (base_path == NULL)
        {
          base_path = g_strdup (arg);
          return 0;
        }
      return 1;
    case FUSE_OPT_KEY_OPT:
      return 1;
    case KEY_HELP:
      usage (outargs->argv[0]);
      exit (EXIT_SUCCESS);
    default:
      fprintf (stderr, "see `%s -h' for usage\n", outargs->argv[0]);
      exit (EXIT_FAILURE);
    }
  return 1;
}

struct revokefs_config {
  int socket_fd;
  int exit_with_fd;
  int backend;
};

#define REVOKEFS_OPT(t, p, v) { t, offsetof(struct revokefs_config, p), v }

static struct fuse_opt revokefs_opts[] = {
  REVOKEFS_OPT ("--socket=%i", socket_fd, -1),
  REVOKEFS_OPT ("--exit-with-fd=%i", exit_with_fd, -1),
  REVOKEFS_OPT ("--backend", backend, 1),

  FUSE_OPT_KEY ("-h", KEY_HELP),
  FUSE_OPT_KEY ("--help", KEY_HELP),
  FUSE_OPT_END
};

int
main (int argc, char *argv[])
{
  struct fuse_args args = FUSE_ARGS_INIT (argc, argv);
  int res;
  struct revokefs_config conf = { -1, -1 };

  res = fuse_opt_parse (&args, &conf, revokefs_opts, revokefs_opt_proc);
  if (res != 0)
    {
      fprintf (stderr, "Invalid arguments\n");
      fprintf (stderr, "see `%s -h' for usage\n", argv[0]);
      exit (EXIT_FAILURE);
    }

  if (base_path == NULL)
    {
      fprintf (stderr, "Missing basepath\n");
      fprintf (stderr, "see `%s -h' for usage\n", argv[0]);
      exit (EXIT_FAILURE);
    }

  basefd = openat (AT_FDCWD, base_path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (basefd == -1)
    {
      perror ("opening basepath");
      exit (EXIT_FAILURE);
    }

  if (conf.backend)
    {
      if (conf.socket_fd == -1)
        {
          fprintf (stderr, "No --socket passed, required for --backend\n");
          exit (EXIT_FAILURE);
        }

      do_writer (basefd, conf.socket_fd, conf.exit_with_fd);
      exit (0);
    }

  if (conf.socket_fd != -1)
    {
      writer_socket = conf.socket_fd;
    }
  else
    {
      int sockets[2];
      pid_t pid;

      if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets))
        {
          perror ("Failed to create socket pair");
          exit (EXIT_FAILURE);
        }

      pid = fork ();
      if (pid == -1)
        {
          perror ("Failed to fork writer");
          exit (EXIT_FAILURE);
        }

      if (pid == 0)
        {
          /* writer process */
          close (sockets[0]);
          do_writer (basefd, sockets[1], -1);
          exit (0);
        }

      /* Main process */
      close (sockets[1]);
      writer_socket = sockets[0];
    }

  fuse_main (args.argc, args.argv, &callback_oper, NULL);

  return 0;
}
