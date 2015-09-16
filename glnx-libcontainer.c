/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Portions derived from src/nspawn/nspawn.c:
 *  Copyright 2010 Lennart Poettering
 *
 * Copyright (C) 2014,2015 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <string.h>
#include <glib-unix.h>
#include <sys/mount.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <gio/gunixoutputstream.h>

#include "glnx-libcontainer.h"

#include "glnx-backport-autocleanups.h"
#include "glnx-local-alloc.h"

static void _perror_fatal (const char *message) __attribute__ ((noreturn));

static void
_perror_fatal (const char *message)
{
  perror (message);
  exit (1);
}

typedef enum {
  CONTAINER_UNINIT = 0,
  CONTAINER_YES = 1,
  CONTAINER_NO = 2
} ContainerDetectionState;

static gboolean
currently_in_container (void)
{
  static gsize container_detected = CONTAINER_UNINIT;

  if (g_once_init_enter (&container_detected))
    {
      ContainerDetectionState tmp_state = CONTAINER_NO;
      struct stat stbuf;
      
      /* http://www.freedesktop.org/wiki/Software/systemd/ContainerInterface/ */
      if (getenv ("container") != NULL
          || stat ("/.dockerinit", &stbuf) == 0)
        tmp_state = CONTAINER_YES;
      /* But since Docker isn't on board, yet, so...
         http://stackoverflow.com/questions/23513045/how-to-check-if-a-process-is-running-inside-docker-container */
      g_once_init_leave (&container_detected, tmp_state);
    }
  return container_detected == CONTAINER_YES;
}

#if 0
static gboolean
glnx_libcontainer_bind_mount_readonly (const char *path, GError **error)
{
  gboolean ret = FALSE;

  if (mount (path, path, NULL, MS_BIND | MS_PRIVATE, NULL) != 0)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "mount(%s, MS_BIND): %s",
                   path,
                   g_strerror (errsv));
      goto out;
    }
  if (mount (path, path, NULL, MS_BIND | MS_PRIVATE | MS_REMOUNT | MS_RDONLY, NULL) != 0)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "mount(%s, MS_BIND | MS_RDONLY): %s",
                   path,
                   g_strerror (errsv));
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}
#endif

/* Based on code from nspawn.c */
static int
glnx_libcontainer_make_api_mounts (const char *dest)
{
  typedef struct MountPoint {
    const char *what;
    const char *where;
    const char *type;
    const char *options;
    unsigned long flags;
    gboolean fatal;
  } MountPoint;

  static const MountPoint mount_table[] = {
    { "proc",      "/proc",     "proc",  NULL,        MS_NOSUID|MS_NOEXEC|MS_NODEV,           TRUE  },
    { "/proc/sys", "/proc/sys", NULL,    NULL,        MS_BIND,                                TRUE  },   /* Bind mount first */
    { NULL,        "/proc/sys", NULL,    NULL,        MS_BIND|MS_RDONLY|MS_REMOUNT,           TRUE  },   /* Then, make it r/o */
    { "sysfs",     "/sys",      "sysfs", NULL,        MS_RDONLY|MS_NOSUID|MS_NOEXEC|MS_NODEV, TRUE  },
    { "tmpfs",     "/dev",      "tmpfs", "mode=755",  MS_NOSUID|MS_STRICTATIME,               TRUE  },
    { "devpts",    "/dev/pts",  "devpts","newinstance,ptmxmode=0666,mode=620,gid=5", MS_NOSUID|MS_NOEXEC, TRUE },
    { "tmpfs",     "/dev/shm",  "tmpfs", "mode=1777", MS_NOSUID|MS_NODEV|MS_STRICTATIME,      TRUE  },
    { "tmpfs",     "/run",      "tmpfs", "mode=755",  MS_NOSUID|MS_NODEV|MS_STRICTATIME,      TRUE  },
    { "/sys/fs/selinux", "/sys/fs/selinux", NULL, NULL, MS_BIND,                              FALSE },  /* Bind mount first */
    { NULL,              "/sys/fs/selinux", NULL, NULL, MS_BIND|MS_RDONLY|MS_REMOUNT,         FALSE },  /* Then, make it r/o */
  };

  unsigned k;

  for (k = 0; k < G_N_ELEMENTS(mount_table); k++)
    {
      g_autofree char *where = NULL;
      int t;

      where = g_build_filename (dest, mount_table[k].where, NULL);

      t = mkdir (where, 0755);
      if (t < 0 && errno != EEXIST)
        {
          if (!mount_table[k].fatal)
            continue;
          return -1;
        }

      if (mount (mount_table[k].what,
                 where,
                 mount_table[k].type,
                 mount_table[k].flags,
                 mount_table[k].options) < 0)
        {
          if (errno == ENOENT && !mount_table[k].fatal)
            continue;
          return -1;
        }
    }

  return 0;
}

static int
glnx_libcontainer_prep_dev (const char  *dest_devdir)
{
  glnx_fd_close int src_fd = -1;
  glnx_fd_close int dest_fd = -1;
  struct stat stbuf;
  guint i;
  static const char *const devnodes[] = { "null", "zero", "full", "random", "urandom", "tty" };

  src_fd = openat (AT_FDCWD, "/dev", O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (src_fd == -1)
    return -1;

  dest_fd = openat (AT_FDCWD, dest_devdir, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (dest_fd == -1)
    return -1;

  for (i = 0; i < G_N_ELEMENTS (devnodes); i++)
    {
      const char *nodename = devnodes[i];
      
      if (fstatat (src_fd, nodename, &stbuf, 0) == -1)
        {
          if (errno == ENOENT)
            continue;
          else
            return -1;
        }

      if (mknodat (dest_fd, nodename, stbuf.st_mode, stbuf.st_rdev) != 0)
        return -1;
      if (fchmodat (dest_fd, nodename, stbuf.st_mode, 0) != 0)
        return -1;
    }

  return 0;
}

pid_t
glnx_libcontainer_run_chroot_private (const char  *dest,
                                      const char  *binary,
                                      char **argv)
{
  /* Make most new namespaces; note our use of CLONE_NEWNET means we
   * have no networking in the container root.
   */
  const int cloneflags = 
    SIGCHLD | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET | CLONE_SYSVSEM | CLONE_NEWUTS;
  pid_t child;
  gboolean in_container = currently_in_container ();

  if (!in_container)
    {
      if ((child = syscall (__NR_clone, cloneflags, NULL)) < 0)
        return -1;
    }
  else
    {
      if ((child = fork ()) < 0)
        return -1;
    }

  if (child != 0)
    return child;

  if (!in_container)
    {
      if (mount (NULL, "/", "none", MS_PRIVATE | MS_REC, NULL) != 0)
        {
          if (errno == EINVAL)
            {
              /* Ok, we may be inside a mock chroot or the like.  In
               * that case, let's just fall back to not
               * containerizing.
               */
              in_container = TRUE;
            }
          else
            _perror_fatal ("mount: ");
        }
      
      if (!in_container)
        {
          if (mount (NULL, "/", "none", MS_PRIVATE | MS_REMOUNT | MS_NOSUID, NULL) != 0)
            _perror_fatal ("mount (MS_NOSUID): ");
        }
    }

  if (chdir (dest) != 0)
    _perror_fatal ("chdir: ");

  if (!in_container)
    {
      if (glnx_libcontainer_make_api_mounts (dest) != 0)
        _perror_fatal ("preparing api mounts: ");

      if (glnx_libcontainer_prep_dev ("dev") != 0)
        _perror_fatal ("preparing /dev: ");
      
      if (mount (".", ".", NULL, MS_BIND | MS_PRIVATE, NULL) != 0)
        _perror_fatal ("mount (MS_BIND)");
      
      if (mount (dest, "/", NULL, MS_MOVE, NULL) != 0)
        _perror_fatal ("mount (MS_MOVE)");
    }

  if (chroot (".") != 0)
    _perror_fatal ("chroot: ");

  if (chdir ("/") != 0)
    _perror_fatal ("chdir: ");

  if (binary[0] == '/')
    {
      if (execv (binary, argv) != 0)
        _perror_fatal ("execv: ");
    }
  else
    {
      /* Set PATH to something sane. */
      setenv ("PATH", "/usr/sbin:/usr/bin", 1);

      if (execvp (binary, argv) != 0)
        _perror_fatal ("execvp: ");
    }

  g_assert_not_reached ();
}
