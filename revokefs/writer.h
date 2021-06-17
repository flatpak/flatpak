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

#ifndef __REVOKEFS_WRITER_H__
#define __REVOKEFS_WRITER_H__

int request_mkdir(int writer_socket, const char *path, mode_t mode);
int request_rmdir (int writer_socket, const char *path);
int request_unlink (int writer_socket, const char *path);
int request_symlink (int writer_socket, const char *from, const char *to);
int request_link (int writer_socket, const char *from, const char *to);
int request_rename (int writer_socket, const char *from, const char *to, unsigned int flags);
int request_chmod(int writer_socket, const char *path, mode_t mode);
int request_chown(int writer_socket, const char *path, uid_t uid, gid_t gid);
int request_truncate (int writer_socket, const char *path, off_t size);
int request_utimens (int writer_socket, const char *path, const struct timespec tv[2]);
int request_open (int writer_socket, const char *path, mode_t mode, int flags);
int request_read (int writer_socket, int fd, char *buf, size_t size, off_t offset);
int request_write (int writer_socket, int fd, const char *buf, size_t size, off_t offset);
int request_fsync (int writer_socket, int fd);
int request_close (int writer_socket, int fd);
int request_access (int writer_socket, const char *path, int mode);

void  do_writer (int basefd, int socket, int exit_with_fd);


typedef enum {
  REVOKE_FS_MKDIR,
  REVOKE_FS_RMDIR,
  REVOKE_FS_UNLINK,
  REVOKE_FS_SYMLINK,
  REVOKE_FS_LINK,
  REVOKE_FS_RENAME,
  REVOKE_FS_CHMOD,
  REVOKE_FS_CHOWN,
  REVOKE_FS_TRUNCATE,
  REVOKE_FS_UTIMENS,
  REVOKE_FS_OPEN,
  REVOKE_FS_READ,
  REVOKE_FS_WRITE,
  REVOKE_FS_FSYNC,
  REVOKE_FS_CLOSE,
  REVOKE_FS_ACCESS,
} RevokefsOps;

typedef struct {
  guint32 op;
  guint64 arg1;
  guint64 arg2;
  guint64 arg3;
  guchar data[];
} RevokefsRequest;

typedef struct {
  gint32 result;

  guchar data[];
} RevokefsResponse;

#define REQUEST_SIZE(__data_size) (sizeof(RevokefsRequest) + (__data_size))
#define RESPONSE_SIZE(__data_size) (sizeof(RevokefsResponse) + (__data_size))

#define MAX_DATA_SIZE 16384
#define MAX_REQUEST_SIZE REQUEST_SIZE(MAX_DATA_SIZE)
#define MAX_RESPONSE_SIZE RESPONSE_SIZE(MAX_DATA_SIZE)

#endif /* __REVOKEFS_WRITER_H__ */
