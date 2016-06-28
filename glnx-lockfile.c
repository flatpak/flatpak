/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.
  Now copied into libglnx:
    - Use GError

  Copyright 2010 Lennart Poettering
  Copyright 2015 Colin Walters <walters@verbum.org>

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include "config.h"

#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "glnx-lockfile.h"
#include "glnx-errors.h"
#include "glnx-fdio.h"
#include "glnx-backport-autocleanups.h"
#include "glnx-local-alloc.h"

#define newa(t, n) ((t*) alloca(sizeof(t)*(n)))

/**
 * glnx_make_lock_file:
 * @dfd: Directory file descriptor (if not `AT_FDCWD`, must have lifetime `>=` @out_lock)
 * @p: Path
 * @operation: one of `LOCK_SH`, `LOCK_EX`, `LOCK_UN`, as passed to flock()
 * @out_lock: (out) (caller allocates): Return location for lock
 * @error: Error
 *
 * Block until a lock file named @p (relative to @dfd) can be created,
 * using the flags in @operation, returning the lock data in the
 * caller-allocated location @out_lock.
 *
 * This API wraps new-style process locking if available, otherwise
 * falls back to BSD locks.
 */
gboolean
glnx_make_lock_file(int dfd, const char *p, int operation, GLnxLockFile *out_lock, GError **error) {
        gboolean ret = FALSE;
        glnx_fd_close int fd = -1;
        g_autofree char *t = NULL;
        int r;

        /*
         * We use UNPOSIX locks if they are available. They have nice
         * semantics, and are mostly compatible with NFS. However,
         * they are only available on new kernels. When we detect we
         * are running on an older kernel, then we fall back to good
         * old BSD locks. They also have nice semantics, but are
         * slightly problematic on NFS, where they are upgraded to
         * POSIX locks, even though locally they are orthogonal to
         * POSIX locks.
         */

        t = g_strdup(p);

        for (;;) {
#ifdef F_OFD_SETLK
                struct flock fl = {
                        .l_type = (operation & ~LOCK_NB) == LOCK_EX ? F_WRLCK : F_RDLCK,
                        .l_whence = SEEK_SET,
                };
#endif
                struct stat st;

                fd = openat(dfd, p, O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NOCTTY, 0600);
                if (fd < 0) {
                        glnx_set_error_from_errno(error);
                        goto out;
                }

                /* Unfortunately, new locks are not in RHEL 7.1 glibc */
#ifdef F_OFD_SETLK
                r = fcntl(fd, (operation & LOCK_NB) ? F_OFD_SETLK : F_OFD_SETLKW, &fl);
#else
                r = -1;
                errno = EINVAL;
#endif
                if (r < 0) {

                        /* If the kernel is too old, use good old BSD locks */
                        if (errno == EINVAL)
                                r = flock(fd, operation);

                        if (r < 0) {
                                glnx_set_error_from_errno(error);
                                goto out;
                        }
                }

                /* If we acquired the lock, let's check if the file
                 * still exists in the file system. If not, then the
                 * previous exclusive owner removed it and then closed
                 * it. In such a case our acquired lock is worthless,
                 * hence try again. */

                r = fstat(fd, &st);
                if (r < 0) {
                        glnx_set_error_from_errno(error);
                        goto out;
                }
                if (st.st_nlink > 0)
                        break;

                (void) close(fd);
                fd = -1;
        }

        /* Note that if this is not AT_FDCWD, the caller takes responsibility
         * for the fd's lifetime being >= that of the lock.
         */
        out_lock->dfd = dfd;
        out_lock->path = t;
        out_lock->fd = fd;
        out_lock->operation = operation;

        fd = -1;
        t = NULL;

        ret = TRUE;
 out:
        return ret;
}

void glnx_release_lock_file(GLnxLockFile *f) {
        int r;

        if (!f)
                return;

        if (f->path) {

                /* If we are the exclusive owner we can safely delete
                 * the lock file itself. If we are not the exclusive
                 * owner, we can try becoming it. */

                if (f->fd >= 0 &&
                    (f->operation & ~LOCK_NB) == LOCK_SH) {
#ifdef F_OFD_SETLK
                        static const struct flock fl = {
                                .l_type = F_WRLCK,
                                .l_whence = SEEK_SET,
                        };

                        r = fcntl(f->fd, F_OFD_SETLK, &fl);
#else
                        r = -1;
                        errno = EINVAL;
#endif
                        if (r < 0 && errno == EINVAL)
                                r = flock(f->fd, LOCK_EX|LOCK_NB);

                        if (r >= 0)
                                f->operation = LOCK_EX|LOCK_NB;
                }

                if ((f->operation & ~LOCK_NB) == LOCK_EX) {
                        (void) unlinkat(f->dfd, f->path, 0);
                }

                g_free(f->path);
                f->path = NULL;
        }

        if (f->fd != -1)
                (void) close (f->fd);
        f->fd = -1;
        f->operation = 0;
}
