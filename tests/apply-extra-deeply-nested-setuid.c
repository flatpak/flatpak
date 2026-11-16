/*
 * Copyright 2026 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* We want this to be more than the default RLIMIT_NOFILE, 1024. */
size_t depth = 1234;

int main(void)
{
  int dfd = -1;
  int exe_fd = -1;

  if (mkdir ("/app/extra", 0755) < 0 && errno != EEXIST)
    err (1, "mkdir /app/extra");

  dfd = open ("/app/extra", O_CLOEXEC | O_DIRECTORY | O_PATH | O_NOFOLLOW);

  if (dfd < 0)
    err (1, "open /app/extra");

  exe_fd = openat (dfd, "exe", O_CLOEXEC | O_CREAT | O_EXCL | O_RDWR, 02744);

  if (exe_fd < 0)
    err (1, "openat /app/extra/exe");

  if (fchmod (exe_fd, 06744) < 0)
    err (1, "fchmod /app/extra/exe");

  if (close (exe_fd) < 0)
    err (1, "close /app/extra/exe");

  exe_fd = -1;

  for (size_t i = 1; i <= depth; i++)
    {
      int child_dfd = -1;

      if (mkdirat (dfd, "d", 0755) < 0)
        err (1, "mkdirat depth=%zu", i);

      child_dfd = openat (dfd, "d", O_CLOEXEC | O_DIRECTORY | O_PATH | O_NOFOLLOW);

      if (child_dfd < 0)
        err (1, "openat depth=%zu", i);

      close (dfd);
      dfd = child_dfd;
      child_dfd = -1;
    }

  exe_fd = openat (dfd, "exe", O_CLOEXEC | O_CREAT | O_EXCL | O_RDWR, 02744);

  if (exe_fd < 0)
    err (1, "openat /app/extra/d/.../d/exe");

  if (fchmod (exe_fd, 06744) < 0)
    err (1, "fchmod /app/extra/d/.../d/exe");

  if (close (exe_fd) < 0)
    err (1, "close /app/extra/d/.../d/exe");

  exe_fd = -1;

  close (dfd);
  dfd = -1;

  return 0;
}
