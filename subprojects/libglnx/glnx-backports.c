/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright 2000-2022 Red Hat, Inc.
 * Copyright 2006-2007 Matthias Clasen
 * Copyright 2006 Padraig O'Briain
 * Copyright 2007 Lennart Poettering
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 * Copyright 2018-2022 Endless OS Foundation, LLC
 * Copyright 2018 Peter Wu
 * Copyright 2019 Ting-Wei Lan
 * Copyright 2019 Sebastian Schwarz
 * Copyright 2020 Matt Rose
 * Copyright 2021 Casper Dik
 * Copyright 2022 Alexander Richardson
 * Copyright 2022 Ray Strode
 * Copyright 2022 Thomas Haller
 * Copyright 2023-2024 Collabora Ltd.
 * Copyright 2023 Sebastian Wilhelmi
 * Copyright 2023 CaiJingLong
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "libglnx-config.h"

#include "glnx-backports.h"
#include "glnx-missing.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#if !GLIB_CHECK_VERSION(2, 44, 0)
gboolean
glnx_strv_contains (const gchar * const *strv,
                    const gchar         *str)
{
  g_return_val_if_fail (strv != NULL, FALSE);
  g_return_val_if_fail (str != NULL, FALSE);

  for (; *strv != NULL; strv++)
    {
      if (g_str_equal (str, *strv))
        return TRUE;
    }

  return FALSE;
}

gboolean
glnx_set_object (GObject **object_ptr,
                 GObject  *new_object)
{
  GObject *old_object = *object_ptr;

  if (old_object == new_object)
    return FALSE;

  if (new_object != NULL)
    g_object_ref (new_object);

  *object_ptr = new_object;

  if (old_object != NULL)
    g_object_unref (old_object);

  return TRUE;
}
#endif

#if !GLIB_CHECK_VERSION(2, 60, 0)
gboolean
_glnx_strv_equal (const gchar * const *strv1,
                  const gchar * const *strv2)
{
  g_return_val_if_fail (strv1 != NULL, FALSE);
  g_return_val_if_fail (strv2 != NULL, FALSE);

  if (strv1 == strv2)
    return TRUE;

  for (; *strv1 != NULL && *strv2 != NULL; strv1++, strv2++)
    {
      if (!g_str_equal (*strv1, *strv2))
        return FALSE;
    }

  return (*strv1 == NULL && *strv2 == NULL);
}
#endif

#if !GLIB_CHECK_VERSION(2, 80, 0)
/* This function is called between fork() and exec() and hence must be
 * async-signal-safe (see signal-safety(7)). */
static int
set_cloexec (void *data, gint fd)
{
  if (fd >= GPOINTER_TO_INT (data))
    fcntl (fd, F_SETFD, FD_CLOEXEC);

  return 0;
}

/* fdwalk()-compatible callback to close a fd for non-compliant
 * implementations of fdwalk() that potentially pass already
 * closed fds.
 *
 * It is not an error to pass an invalid fd to this function.
 *
 * This function is called between fork() and exec() and hence must be
 * async-signal-safe (see signal-safety(7)).
 */
G_GNUC_UNUSED static int
close_func_with_invalid_fds (void *data, int fd)
{
  /* We use close and not g_close here because on some platforms, we
   * don't know how to close only valid, open file descriptors, so we
   * have to pass bad fds to close too. g_close warns if given a bad
   * fd.
   *
   * This function returns no error, because there is nothing that the caller
   * could do with that information. That is even the case for EINTR. See
   * g_close() about the specialty of EINTR and why that is correct.
   * If g_close() ever gets extended to handle EINTR specially, then this place
   * should get updated to do the same handling.
   */
  if (fd >= GPOINTER_TO_INT (data))
    close (fd);

  return 0;
}

#ifdef __linux__
struct linux_dirent64
{
  guint64        d_ino;    /* 64-bit inode number */
  guint64        d_off;    /* 64-bit offset to next structure */
  unsigned short d_reclen; /* Size of this dirent */
  unsigned char  d_type;   /* File type */
  char           d_name[]; /* Filename (null-terminated) */
};

/* This function is called between fork() and exec() and hence must be
 * async-signal-safe (see signal-safety(7)). */
static gint
filename_to_fd (const char *p)
{
  char c;
  int fd = 0;
  const int cutoff = G_MAXINT / 10;
  const int cutlim = G_MAXINT % 10;

  if (*p == '\0')
    return -1;

  while ((c = *p++) != '\0')
    {
      if (c < '0' || c > '9')
        return -1;
      c -= '0';

      /* Check for overflow. */
      if (fd > cutoff || (fd == cutoff && c > cutlim))
        return -1;

      fd = fd * 10 + c;
    }

  return fd;
}
#endif

static int safe_fdwalk_with_invalid_fds (int (*cb)(void *data, int fd), void *data);

/* This function is called between fork() and exec() and hence must be
 * async-signal-safe (see signal-safety(7)). */
static int
safe_fdwalk (int (*cb)(void *data, int fd), void *data)
{
#if 0
  /* Use fdwalk function provided by the system if it is known to be
   * async-signal safe.
   *
   * Currently there are no operating systems known to provide a safe
   * implementation, so this section is not used for now.
   */
  return fdwalk (cb, data);
#else
  /* Fallback implementation of fdwalk. It should be async-signal safe, but it
   * may fail on non-Linux operating systems. See safe_fdwalk_with_invalid_fds
   * for a slower alternative.
   */

#ifdef __linux__
  gint fd;
  gint res = 0;

  /* Avoid use of opendir/closedir since these are not async-signal-safe. */
  int dir_fd = open ("/proc/self/fd", O_RDONLY | O_DIRECTORY);
  if (dir_fd >= 0)
    {
      /* buf needs to be aligned correctly to receive linux_dirent64.
       * C11 has _Alignof for this purpose, but for now a
       * union serves the same purpose. */
      union
      {
        char buf[4096];
        struct linux_dirent64 alignment;
      } u;
      int pos, nread;
      struct linux_dirent64 *de;

      while ((nread = syscall (SYS_getdents64, dir_fd, u.buf, sizeof (u.buf))) > 0)
        {
          for (pos = 0; pos < nread; pos += de->d_reclen)
            {
              de = (struct linux_dirent64 *) (u.buf + pos);

              fd = filename_to_fd (de->d_name);
              if (fd < 0 || fd == dir_fd)
                  continue;

              if ((res = cb (data, fd)) != 0)
                  break;
            }
        }

      close (dir_fd);
      return res;
    }

  /* If /proc is not mounted or not accessible we fail here and rely on
   * safe_fdwalk_with_invalid_fds to fall back to the old
   * rlimit trick. */

#endif

#if defined(__sun__) && defined(F_PREVFD) && defined(F_NEXTFD)
/*
 * Solaris 11.4 has a signal-safe way which allows
 * us to find all file descriptors in a process.
 *
 * fcntl(fd, F_NEXTFD, maxfd)
 * - returns the first allocated file descriptor <= maxfd  > fd.
 *
 * fcntl(fd, F_PREVFD)
 * - return highest allocated file descriptor < fd.
 */
  gint fd;
  gint res = 0;

  open_max = fcntl (INT_MAX, F_PREVFD); /* find the maximum fd */
  if (open_max < 0) /* No open files */
    return 0;

  for (fd = -1; (fd = fcntl (fd, F_NEXTFD, open_max)) != -1; )
    if ((res = cb (data, fd)) != 0 || fd == open_max)
      break;

  return res;
#endif

  return safe_fdwalk_with_invalid_fds (cb, data);
#endif
}

/* This function is called between fork() and exec() and hence must be
 * async-signal-safe (see signal-safety(7)). */
static int
safe_fdwalk_with_invalid_fds (int (*cb)(void *data, int fd), void *data)
{
  /* Fallback implementation of fdwalk. It should be async-signal safe, but it
   * may be slow, especially on systems allowing very high number of open file
   * descriptors.
   */
  gint open_max = -1;
  gint fd;
  gint res = 0;

#if 0 && defined(HAVE_SYS_RESOURCE_H)
  struct rlimit rl;

  /* Use getrlimit() function provided by the system if it is known to be
   * async-signal safe.
   *
   * Currently there are no operating systems known to provide a safe
   * implementation, so this section is not used for now.
   */
  if (getrlimit (RLIMIT_NOFILE, &rl) == 0 && rl.rlim_max != RLIM_INFINITY)
    open_max = rl.rlim_max;
#endif
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
  /* Use sysconf() function provided by the system if it is known to be
   * async-signal safe.
   *
   * FreeBSD: sysconf() is included in the list of async-signal safe functions
   * found in https://man.freebsd.org/sigaction(2).
   *
   * OpenBSD: sysconf() is included in the list of async-signal safe functions
   * found in https://man.openbsd.org/sigaction.2.
   *
   * Apple: sysconf() is included in the list of async-signal safe functions
   * found in https://opensource.apple.com/source/xnu/xnu-517.12.7/bsd/man/man2/sigaction.2
   */
  if (open_max < 0)
    open_max = sysconf (_SC_OPEN_MAX);
#endif
  /* Hardcoded fallback: the default process hard limit in Linux as of 2020 */
  if (open_max < 0)
    open_max = 4096;

#if defined(__APPLE__) && defined(HAVE_LIBPROC_H)
  /* proc_pidinfo isn't documented as async-signal-safe but looking at the implementation
   * in the darwin tree here:
   *
   * https://opensource.apple.com/source/Libc/Libc-498/darwin/libproc.c.auto.html
   *
   * It's just a thin wrapper around a syscall, so it's probably okay.
   */
  {
    char buffer[4096 * PROC_PIDLISTFD_SIZE];
    ssize_t buffer_size;

    buffer_size = proc_pidinfo (getpid (), PROC_PIDLISTFDS, 0, buffer, sizeof (buffer));

    if (buffer_size > 0 &&
        sizeof (buffer) >= (size_t) buffer_size &&
        (buffer_size % PROC_PIDLISTFD_SIZE) == 0)
      {
        const struct proc_fdinfo *fd_info = (const struct proc_fdinfo *) buffer;
        size_t number_of_fds = (size_t) buffer_size / PROC_PIDLISTFD_SIZE;

        for (size_t i = 0; i < number_of_fds; i++)
          if ((res = cb (data, fd_info[i].proc_fd)) != 0)
            break;

        return res;
      }
  }
#endif

  for (fd = 0; fd < open_max; fd++)
      if ((res = cb (data, fd)) != 0)
          break;

  return res;
}

/**
 * g_fdwalk_set_cloexec:
 * @lowfd: Minimum fd to act on, which must be non-negative
 *
 * Mark every file descriptor equal to or greater than @lowfd to be closed
 * at the next `execve()` or similar, as if via the `FD_CLOEXEC` flag.
 *
 * Typically @lowfd will be 3, to leave standard input, standard output
 * and standard error open after exec.
 *
 * This is the same as Linux `close_range (lowfd, ~0U, CLOSE_RANGE_CLOEXEC)`,
 * but portable to other OSs and to older versions of Linux.
 *
 * This function is async-signal safe, making it safe to call from a
 * signal handler or a [callback@GLib.SpawnChildSetupFunc], as long as @lowfd is
 * non-negative.
 * See [`signal(7)`](man:signal(7)) and
 * [`signal-safety(7)`](man:signal-safety(7)) for more details.
 *
 * Returns: 0 on success, -1 with errno set on error
 * Since: 2.80
 */
int
_glnx_fdwalk_set_cloexec (int lowfd)
{
  int ret;

  g_return_val_if_fail (lowfd >= 0, (errno = EINVAL, -1));

#if defined(HAVE_CLOSE_RANGE) && defined(CLOSE_RANGE_CLOEXEC)
  /* close_range() is available in Linux since kernel 5.9, and on FreeBSD at
   * around the same time. It was designed for use in async-signal-safe
   * situations: https://bugs.python.org/issue38061
   *
   * The `CLOSE_RANGE_CLOEXEC` flag was added in Linux 5.11, and is not yet
   * present in FreeBSD.
   *
   * Handle ENOSYS in case it’s supported in libc but not the kernel; if so,
   * fall back to safe_fdwalk(). Handle EINVAL in case `CLOSE_RANGE_CLOEXEC`
   * is not supported. */
  ret = close_range (lowfd, G_MAXUINT, CLOSE_RANGE_CLOEXEC);
  if (ret == 0 || !(errno == ENOSYS || errno == EINVAL))
    return ret;
#endif  /* HAVE_CLOSE_RANGE */

  ret = safe_fdwalk (set_cloexec, GINT_TO_POINTER (lowfd));

  return ret;
}

/**
 * g_closefrom:
 * @lowfd: Minimum fd to close, which must be non-negative
 *
 * Close every file descriptor equal to or greater than @lowfd.
 *
 * Typically @lowfd will be 3, to leave standard input, standard output
 * and standard error open.
 *
 * This is the same as Linux `close_range (lowfd, ~0U, 0)`,
 * but portable to other OSs and to older versions of Linux.
 * Equivalently, it is the same as BSD `closefrom (lowfd)`, but portable,
 * and async-signal-safe on all OSs.
 *
 * This function is async-signal safe, making it safe to call from a
 * signal handler or a [callback@GLib.SpawnChildSetupFunc], as long as @lowfd is
 * non-negative.
 * See [`signal(7)`](man:signal(7)) and
 * [`signal-safety(7)`](man:signal-safety(7)) for more details.
 *
 * Returns: 0 on success, -1 with errno set on error
 * Since: 2.80
 */
int
_glnx_closefrom (int lowfd)
{
  int ret;

  g_return_val_if_fail (lowfd >= 0, (errno = EINVAL, -1));

#if defined(HAVE_CLOSE_RANGE)
  /* close_range() is available in Linux since kernel 5.9, and on FreeBSD at
   * around the same time. It was designed for use in async-signal-safe
   * situations: https://bugs.python.org/issue38061
   *
   * Handle ENOSYS in case it’s supported in libc but not the kernel; if so,
   * fall back to safe_fdwalk(). */
  ret = close_range (lowfd, G_MAXUINT, 0);
  if (ret == 0 || errno != ENOSYS)
    return ret;
#endif  /* HAVE_CLOSE_RANGE */

#if defined(__FreeBSD__) || defined(__OpenBSD__) || \
  (defined(__sun__) && defined(F_CLOSEFROM))
  /* Use closefrom function provided by the system if it is known to be
   * async-signal safe.
   *
   * FreeBSD: closefrom is included in the list of async-signal safe functions
   * found in https://man.freebsd.org/sigaction(2).
   *
   * OpenBSD: closefrom is not included in the list, but a direct system call
   * should be safe to use.
   *
   * In Solaris as of 11.3 SRU 31, closefrom() is also a direct system call.
   * On such systems, F_CLOSEFROM is defined.
   */
  (void) closefrom (lowfd);
  return 0;
#elif defined(__DragonFly__)
  /* It is unclear whether closefrom function included in DragonFlyBSD libc_r
   * is safe to use because it calls a lot of library functions. It is also
   * unclear whether libc_r itself is still being used. Therefore, we do a
   * direct system call here ourselves to avoid possible issues.
   */
  (void) syscall (SYS_closefrom, lowfd);
  return 0;
#elif defined(F_CLOSEM)
  /* NetBSD and AIX have a special fcntl command which does the same thing as
   * closefrom. NetBSD also includes closefrom function, which seems to be a
   * simple wrapper of the fcntl command.
   */
  return fcntl (lowfd, F_CLOSEM);
#else
  ret = safe_fdwalk (close_func_with_invalid_fds, GINT_TO_POINTER (lowfd));

  return ret;
#endif
}
#endif /* !2.80.0 */
