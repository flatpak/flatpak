/*
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

#define FUSE_USE_VERSION 31

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
#include <poll.h>
#include <fuse.h>

#include <glib.h>

#include "writer.h"
#include "libglnx.h"

static int basefd = -1;

static GHashTable *outstanding_fds;

static GMutex mutex;

static ssize_t
do_request (int writer_socket,
            RevokefsRequest *request,
            const void *data,
            size_t data_size,
            const void *data2,
            size_t data2_size,
            RevokefsResponse *response,
            void *response_data,
            size_t response_data_size)
{
  size_t request_size;
  size_t response_max_size;
  ssize_t written_size, read_size;
  struct iovec write_vecs[3] = {};
  int n_write_vecs = 0;
  struct iovec read_vecs[2] = {};
  int n_read_vecs = 0;
  g_autoptr(GMutexLocker) locker = NULL;

  request_size = sizeof (RevokefsRequest);
  write_vecs[n_write_vecs].iov_base = (char *)request;
  write_vecs[n_write_vecs++].iov_len = request_size;

  if (data)
    {
      write_vecs[n_write_vecs].iov_base = (char *)data;
      write_vecs[n_write_vecs++].iov_len = data_size;
      request_size += data_size;
    }

  if (data2)
    {
      write_vecs[n_write_vecs].iov_base = (char *)data2;
      write_vecs[n_write_vecs++].iov_len = data2_size;
      request_size += data2_size;
    }

  locker = g_mutex_locker_new (&mutex);
  written_size = TEMP_FAILURE_RETRY (writev (writer_socket, write_vecs, n_write_vecs));
  if (written_size == -1)
    {
      g_printerr ("Write to socket returned error %d\n", errno);
      return -1;
    }
  if (written_size != request_size)
    {
      g_printerr ("Partial Write to socket\n");
      return -1;
    }

  response_max_size = sizeof (RevokefsResponse);
  read_vecs[n_read_vecs].iov_base = (char *)response;
  read_vecs[n_read_vecs++].iov_len = response_max_size;

  if (response_data)
    {
      read_vecs[n_read_vecs].iov_base = response_data;
      read_vecs[n_read_vecs++].iov_len = response_data_size;
      response_max_size += response_data_size;
    }

  read_size = TEMP_FAILURE_RETRY (readv (writer_socket, read_vecs, n_read_vecs));
  if (read_size == -1)
    {
      g_printerr ("Read from socket returned error %d\n", errno);
      return -1;
    }

  if (read_size < sizeof (RevokefsResponse))
    {
      g_printerr ("Invalid read size %zd\n", read_size);
      return -1;
    }

  return read_size - sizeof (RevokefsResponse);
}

static int
request_path_i64_i64 (int writer_socket, RevokefsOps op, const char *path, guint64 arg1, guint64 arg2)
{
  RevokefsRequest request = { op };
  RevokefsResponse response;
  size_t path_len = strlen (path);
  ssize_t response_data_len;

  if (path_len > MAX_DATA_SIZE)
    return -ENAMETOOLONG;

  request.arg1 = arg1;
  request.arg2 = arg2;

  response_data_len = do_request (writer_socket, &request, path, path_len, NULL, 0,
                                  &response, NULL, 0);
  if (response_data_len != 0)
    return -EIO;

  return response.result;
}

static int
request_path_int_int (int writer_socket, RevokefsOps op, const char *path, int arg1, int arg2)
{
  return request_path_i64_i64 (writer_socket, op, path, arg1, arg2);
}

static int
request_path_int (int writer_socket, RevokefsOps op, const char *path, int arg1)
{
  return request_path_int_int (writer_socket, op, path, arg1, 0);
}

static int
request_path (int writer_socket, RevokefsOps op, const char *path)
{
  return request_path_int_int (writer_socket, op, path, 0, 0);
}

static int
request_path_data (int writer_socket, RevokefsOps op, const char *path,
                   const char *data, size_t data_len, guint64 flags)
{
  RevokefsRequest request = { op };
  RevokefsResponse response;
  size_t path_len = strlen (path);
  size_t total_len = path_len + data_len;
  ssize_t response_data_len;

  if (total_len > MAX_DATA_SIZE)
    return -ENAMETOOLONG;

  request.arg1 = path_len;
  request.arg2 = flags;

  response_data_len = do_request (writer_socket, &request, path, path_len, data, data_len,
                                  &response, NULL, 0);
  if (response_data_len != 0)
    return -EIO;

  return response.result;
}

static int
request_path_path (int writer_socket, RevokefsOps op, const char *path1, const char *path2)
{
  return request_path_data (writer_socket, op, path1, path2, strlen(path2), 0);
}

static gboolean
validate_path (char *path)
{
  char *end_segment;

  /* No absolute or empty paths */
  if (*path == '/' || *path == 0)
    return FALSE;

  while (*path != 0)
    {
      end_segment = strchr (path, '/');
      if (end_segment == NULL)
        end_segment = path + strlen (path);

      if (strncmp (path, "..", 2) == 0)
        return FALSE;

      path = end_segment;
      while (*path == '/')
        path++;
    }

  return TRUE;
}

static char *
get_valid_path (guchar *data, size_t len)
{
  char *path = g_strndup ((const char *) data, len);

  if (!validate_path (path))
    {
      g_printerr ("Invalid path argument %s\n", path);
      exit (1);
    }

  return path;
}

static int
mask_mode (int mode)
{
  /* mask setuid, setgid and world-writable permissions bits */
  return mode & ~S_ISUID & ~S_ISGID & ~(S_IWGRP | S_IWOTH);
}

static void
get_any_path_and_valid_path (RevokefsRequest *request,
                             gsize data_size,
                             char **any_path1,
                             char **valid_path2)
{
  if (request->arg1 >= data_size)
    {
      g_printerr ("Invalid path1 size\n");
      exit (1);
    }

  *any_path1 = g_strndup ((const char *) request->data, request->arg1);
  *valid_path2 = get_valid_path (request->data + request->arg1, data_size - request->arg1);
}

static void
get_valid_2path (RevokefsRequest *request,
                 gsize data_size,
                 char **path1,
                 char **path2)
{
  if (request->arg1 >= data_size)
    {
      g_printerr ("Invalid path1 size\n");
      exit (1);
    }

  *path1 = get_valid_path (request->data, request->arg1);
  *path2 = get_valid_path (request->data + request->arg1, data_size - request->arg1);
}

static ssize_t
handle_mkdir (RevokefsRequest *request,
              gsize data_size,
              RevokefsResponse *response)
{
  g_autofree char *path = get_valid_path (request->data, data_size);
  int mode = request->arg1;

  if (mkdirat (basefd, path, mask_mode (mode)) == -1)
    response->result = -errno;
  else
    response->result = 0;

  return 0;
}

int
request_mkdir (int writer_socket, const char *path, mode_t mode)
{
  return request_path_int (writer_socket, REVOKE_FS_MKDIR, path, mode);
}

static ssize_t
handle_rmdir (RevokefsRequest *request,
              gsize data_size,
              RevokefsResponse *response)
{
  g_autofree char *path = get_valid_path (request->data, data_size);

  if (unlinkat (basefd, path, AT_REMOVEDIR) == -1)
    response->result = -errno;
  else
    response->result = 0;

  return 0;
}

int
request_rmdir (int writer_socket, const char *path)
{
  return request_path (writer_socket, REVOKE_FS_RMDIR, path);
}

static ssize_t
handle_unlink (RevokefsRequest *request,
              gsize data_size,
              RevokefsResponse *response)
{
  g_autofree char *path = get_valid_path (request->data, data_size);

  if (unlinkat (basefd, path, 0) == -1)
    response->result = -errno;
  else
    response->result = 0;

  return 0;
}

int
request_unlink (int writer_socket, const char *path)
{
  return request_path (writer_socket, REVOKE_FS_UNLINK, path);
}

static ssize_t
handle_symlink (RevokefsRequest *request,
                gsize data_size,
                RevokefsResponse *response)
{
  g_autofree char *from = NULL;
  g_autofree char *to = NULL;

  /* from doesn't have to be a valid path, it can be absolute or whatever */
  get_any_path_and_valid_path (request, data_size,  &from, &to);

  if (symlinkat (from, basefd, to) == -1)
    response->result = -errno;
  else
    response->result = 0;

  return 0;
}

int
request_symlink (int writer_socket, const char *from, const char *to)
{
  return request_path_path (writer_socket, REVOKE_FS_SYMLINK, from, to);
}

static ssize_t
handle_link (RevokefsRequest *request,
             gsize data_size,
             RevokefsResponse *response)
{
  g_autofree char *from = NULL;
  g_autofree char *to = NULL;

  get_valid_2path (request, data_size,  &from, &to);

  if (linkat (basefd, from, basefd, to, 0) == -1)
    response->result = -errno;
  else
    response->result = 0;

  return 0;
}

int
request_link (int writer_socket, const char *from, const char *to)
{
  return request_path_path (writer_socket, REVOKE_FS_LINK, from, to);
}

static ssize_t
handle_rename (RevokefsRequest *request,
             gsize data_size,
             RevokefsResponse *response)
{
  g_autofree char *from = NULL;
  g_autofree char *to = NULL;
  unsigned int flags;

  get_valid_2path (request, data_size,  &from, &to);
  flags = (unsigned int)request->arg2;

  if (renameat2 (basefd, from, basefd, to, flags) == -1)
    response->result = -errno;
  else
    response->result = 0;

  return 0;
}

int
request_rename (int writer_socket,
                const char *from,
                const char *to,
                unsigned int flags)
{
  return request_path_data (writer_socket, REVOKE_FS_RENAME, from, to, strlen (to), flags);
}

static ssize_t
handle_chmod (RevokefsRequest *request,
              gsize data_size,
              RevokefsResponse *response)
{
  g_autofree char *path = get_valid_path (request->data, data_size);
  int mode = request->arg1;

  /* Note we can't use AT_SYMLINK_NOFOLLOW yet;
   * https://marc.info/?l=linux-kernel&m=148830147803162&w=2
   * https://marc.info/?l=linux-fsdevel&m=149193779929561&w=2
   */
  if (fchmodat (basefd, path, mask_mode (mode), 0) != 0)
    response->result = -errno;
  else
    response->result = 0;

  return 0;
}

int
request_chmod(int writer_socket, const char *path, mode_t mode)
{
  return request_path_int (writer_socket, REVOKE_FS_CHMOD, path, mode);
}

static ssize_t
handle_chown (RevokefsRequest *request,
              gsize data_size,
              RevokefsResponse *response)
{
  g_autofree char *path = get_valid_path (request->data, data_size);
  uid_t uid = request->arg1;
  gid_t gid = request->arg2;

  if (fchownat (basefd, path, uid, gid, AT_SYMLINK_NOFOLLOW) != 0)
    response->result = -errno;
  else
    response->result = 0;

  return 0;
}

int
request_chown(int writer_socket, const char *path, uid_t uid, gid_t gid)
{
  return request_path_int_int (writer_socket, REVOKE_FS_CHOWN, path, uid, gid);
}

static ssize_t
handle_truncate (RevokefsRequest *request,
                 gsize data_size,
                 RevokefsResponse *response)
{
  g_autofree char *path = get_valid_path (request->data, data_size);
  off_t size = request->arg1;

  glnx_autofd int fd = openat (basefd, path, O_NOFOLLOW|O_WRONLY);
  if (fd == -1)
    response->result = -errno;
  else
    {
      if (ftruncate (fd, size) == -1)
        response->result = -errno;
      else
        response->result = 0;
    }

  return 0;
}

int
request_truncate (int writer_socket, const char *path, off_t size)
{
  return request_path_i64_i64 (writer_socket, REVOKE_FS_TRUNCATE, path, size, 0);
}

static ssize_t
handle_utimens (RevokefsRequest *request,
                gsize data_size,
                RevokefsResponse *response)
{
  g_autofree char *path = NULL;
  struct timespec *tv;

  if (request->arg1 + sizeof (struct timespec) * 2 != data_size)
    {
      g_printerr ("Invalid data size\n");
      exit (1);
    }

  path = get_valid_path (request->data, request->arg1);
  tv = (struct timespec *)(request->data + request->arg1);

  if (utimensat (basefd, path, tv, AT_SYMLINK_NOFOLLOW) == -1)
    response->result = -errno;
  else
    response->result = 0;

  return 0;
}

int
request_utimens (int writer_socket, const char *path, const struct timespec tv[2])
{
  return request_path_data (writer_socket, REVOKE_FS_UTIMENS, path,
                            (const char *)tv, sizeof (struct timespec) * 2, 0);
}

static ssize_t
handle_open (RevokefsRequest *request,
             gsize data_size,
             RevokefsResponse *response)
{
  g_autofree char *path = get_valid_path (request->data, data_size);
  int mode = request->arg1;
  int flags = request->arg2;
  int fd;


  /* We need to specially handle O_TRUNC. Also, Fuse should have already
   * resolved symlinks, but use O_NOFOLLOW to be safe to avoid following
   * symlinks to some other filesystem. */
  fd = openat (basefd, path, (flags & ~O_TRUNC) | O_NOFOLLOW, mask_mode (mode));
  if (fd == -1)
    response->result = -errno;
  else
    {
      response->result = 0;
      if (flags & O_TRUNC)
        {
          if (ftruncate (fd, 0) == -1)
            response->result = -errno;
        }

      if (response->result == 0)
        {
          g_hash_table_insert (outstanding_fds, GUINT_TO_POINTER(fd), GUINT_TO_POINTER(1));
          response->result = fd;
        }
      else
        (void) close (fd);
    }

  return 0;
}

int
request_open (int writer_socket, const char *path, mode_t mode, int flags)
{
  return request_path_int_int (writer_socket, REVOKE_FS_OPEN, path, mode, flags);
}

static ssize_t
handle_read (RevokefsRequest *request,
             gsize data_size,
             RevokefsResponse *response)
{
  int r;
  int fd = request->arg1;
  size_t size = request->arg2;
  off_t offset = request->arg3;

  if (size > MAX_DATA_SIZE)
    size = MAX_DATA_SIZE;

  if (g_hash_table_lookup (outstanding_fds, GUINT_TO_POINTER(fd)) == NULL)
    {
      response->result = -EBADFD;
      return 0;
    }

  r = pread (fd, response->data, size, offset);
  if (r == -1)
    {
      response->result = -errno;
      return 0;
    }
  else
    {
      response->result = r;
      return r;
    }
}

int
request_read (int writer_socket, int fd, char *buf, size_t size, off_t offset)
{
  RevokefsRequest request = { REVOKE_FS_READ };
  RevokefsResponse response;
  ssize_t response_data_len;

  request.arg1 = fd;
  request.arg2 = size;
  request.arg3 = offset;

  response_data_len = do_request (writer_socket, &request, NULL, 0, NULL, 0,
                                  &response, buf, size);
  if (response_data_len < 0)
    return -EIO;

  return response.result;
}

static ssize_t
handle_write (RevokefsRequest *request,
              gsize data_size,
              RevokefsResponse *response)
{
  int r;
  int fd = request->arg1;
  off_t offset = request->arg2;

  if (g_hash_table_lookup (outstanding_fds, GUINT_TO_POINTER(fd)) == NULL)
    {
      response->result = -EBADFD;
      return 0;
    }

  r = pwrite (fd, request->data, data_size, offset);
  if (r == -1)
    response->result = -errno;
  else
    response->result = r;

  return 0;
}

int
request_write (int writer_socket, int fd, const char *buf, size_t size, off_t offset)
{
  RevokefsRequest request = { REVOKE_FS_WRITE };
  RevokefsResponse response;
  ssize_t response_data_len;

  if (size > MAX_DATA_SIZE)
    size = MAX_DATA_SIZE;

  request.arg1 = fd;
  request.arg2 = offset;

  response_data_len = do_request (writer_socket, &request, buf, size, NULL, 0,
                                  &response, NULL, 0);
  if (response_data_len < 0)
    return -EIO;

  return response.result;
}

static ssize_t
handle_fsync (RevokefsRequest *request,
              gsize data_size,
              RevokefsResponse *response)
{
  int r;
  int fd = request->arg1;

  if (g_hash_table_lookup (outstanding_fds, GUINT_TO_POINTER(fd)) == NULL)
    {
      response->result = -EBADFD;
      return 0;
    }

  r = fsync (fd);
  if (r == -1)
    response->result = -errno;
  else
    response->result = r;

  return 0;
}

int
request_fsync (int writer_socket, int fd)
{
  RevokefsRequest request = { REVOKE_FS_FSYNC };
  RevokefsResponse response;
  ssize_t response_data_len;

  request.arg1 = fd;

  response_data_len = do_request (writer_socket, &request, NULL, 0, NULL, 0,
                                  &response, NULL, 0);
  if (response_data_len < 0)
    return -EIO;

  return response.result;
}

static ssize_t
handle_close (RevokefsRequest *request,
              gsize data_size,
              RevokefsResponse *response)
{
  int fd = request->arg1;

  if (!g_hash_table_remove (outstanding_fds, GUINT_TO_POINTER(fd)))
    {
      response->result = -EBADFD;
      return 0;
    }

  close (fd);
  response->result = 0;
  return 0;
}

int
request_close (int writer_socket, int fd)
{
  RevokefsRequest request = { REVOKE_FS_CLOSE };
  RevokefsResponse response;
  ssize_t response_data_len;

  request.arg1 = fd;
  response_data_len = do_request (writer_socket, &request, NULL, 0, NULL, 0,
                                  &response, NULL, 0);
  if (response_data_len < 0)
    return -EIO;

  return response.result;
}

static ssize_t
handle_access (RevokefsRequest *request,
               gsize data_size,
               RevokefsResponse *response)
{
  g_autofree char *path = get_valid_path (request->data, data_size);
  int mode = request->arg1;

  /* Apparently at least GNU coreutils rm calls `faccessat(W_OK)`
   * before trying to do an unlink.  So...we'll just lie about
   * writable access here.
   */
  if (faccessat (basefd, path, mode, AT_SYMLINK_NOFOLLOW) == -1)
    response->result = -errno;
  else
    response->result = 0;

  return 0;
}

int
request_access (int writer_socket, const char *path, int mode)
{
  return request_path_int (writer_socket, REVOKE_FS_ACCESS, path, mode);
}

void
do_writer (int basefd_arg,
           int fuse_socket,
           int exit_with_fd)
{
  guchar request_buffer[MAX_REQUEST_SIZE];
  RevokefsRequest *request = (RevokefsRequest *)&request_buffer;
  guchar response_buffer[MAX_RESPONSE_SIZE];
  RevokefsResponse *response = (RevokefsResponse *)&response_buffer;

  basefd = basefd_arg;
  outstanding_fds = g_hash_table_new (g_direct_hash, g_direct_equal);

  while (1)
    {
      ssize_t data_size, size;
      ssize_t response_data_size, response_size, written_size;
      int res;
      struct pollfd pollfds[2] =  {
         {fuse_socket, POLLIN, 0 },
         {exit_with_fd, POLLIN, 0 },
      };

      res = poll(pollfds, exit_with_fd >= 0 ? 2 : 1, -1);
      if (res < 0)
        {
          perror ("Got error polling sockets: ");
          exit (1);
        }

      if (exit_with_fd >= 0 && (pollfds[1].revents & (POLLERR|POLLHUP)) != 0)
        {
          g_printerr ("Received EOF on exit-with-fd argument");
          exit (1);
        }

      if ((pollfds[0].revents & POLLIN) == 0)
        continue;

      size = TEMP_FAILURE_RETRY (read (fuse_socket, request_buffer, sizeof (request_buffer)));
      if (size == -1)
        {
          perror ("Got error reading from fuse socket: ");
          exit (1);
        }

      if (size == 0)
        {
          /* Fuse filesystem finished */
          exit (1);
        }

      if (size < sizeof (RevokefsRequest))
        {
          g_printerr ("Invalid request size %zd", size);
          exit (1);
        }

      data_size = size - sizeof (RevokefsRequest);
      memset (response_buffer, 0, sizeof(RevokefsResponse));

      switch (request->op)
        {
        case REVOKE_FS_MKDIR:
          response_data_size = handle_mkdir (request, data_size, response);
          break;
        case REVOKE_FS_RMDIR:
          response_data_size = handle_rmdir (request, data_size, response);
          break;
        case REVOKE_FS_UNLINK:
          response_data_size = handle_unlink (request, data_size, response);
          break;
        case REVOKE_FS_SYMLINK:
          response_data_size = handle_symlink (request, data_size, response);
          break;
        case REVOKE_FS_LINK:
          response_data_size = handle_link (request, data_size, response);
          break;
        case REVOKE_FS_RENAME:
          response_data_size = handle_rename (request, data_size, response);
          break;
        case REVOKE_FS_CHMOD:
          response_data_size = handle_chmod (request, data_size, response);
          break;
        case REVOKE_FS_CHOWN:
          response_data_size = handle_chown (request, data_size, response);
          break;
        case REVOKE_FS_TRUNCATE:
          response_data_size = handle_truncate (request, data_size, response);
          break;
        case REVOKE_FS_UTIMENS:
          response_data_size = handle_utimens (request, data_size, response);
          break;
        case REVOKE_FS_OPEN:
          response_data_size = handle_open (request, data_size, response);
          break;
        case REVOKE_FS_READ:
          response_data_size = handle_read (request, data_size, response);
          break;
        case REVOKE_FS_WRITE:
          response_data_size = handle_write (request, data_size, response);
          break;
        case REVOKE_FS_FSYNC:
          response_data_size = handle_fsync (request, data_size, response);
          break;
        case REVOKE_FS_CLOSE:
          response_data_size = handle_close (request, data_size, response);
          break;
        case REVOKE_FS_ACCESS:
          response_data_size = handle_access (request, data_size, response);
          break;
        default:
          g_printerr ("Invalid request op %d", (guint) request->op);
          exit (1);
        }

      if (response_data_size < 0 || response_data_size > MAX_DATA_SIZE)
        {
          g_printerr ("Invalid response size %ld", response_size);
          exit (1);
        }

      response_size = RESPONSE_SIZE(response_data_size);

      written_size = TEMP_FAILURE_RETRY (write (fuse_socket, response_buffer, response_size));
      if (written_size == -1)
        {
          perror ("Got error writing to fuse socket: ");
          exit (1);
        }

      if (written_size != response_size)
        {
          g_printerr ("Got partial write to fuse socket");
          exit (1);
        }
    }
}
