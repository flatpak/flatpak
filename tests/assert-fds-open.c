/*
 * Copyright 2026 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <dirent.h>
#include <err.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * assert-fds-open FD...
 *
 * Assert that exactly the given file descriptors are open, and that no
 * other file descriptor is open.
 */
int
main (int    argc,
      char **argv)
{
  DIR *dir = NULL;
  struct dirent *entry;
  char *dirfd_str = NULL;
  bool failed = false;
  int i;

  dir = opendir ("/proc/self/fd");

  if (dir == NULL)
    err (1, "opendir");

  if (asprintf (&dirfd_str, "%d", dirfd (dir)) < 0)
    err (1, "asprintf");

  fprintf (stderr, "Asserting that exactly the desired fds are open...\n");

  while ((entry = readdir (dir)) != NULL)
    {
      bool found = false;

      if (strcmp (entry->d_name, ".") == 0)
        continue;

      if (strcmp (entry->d_name, "..") == 0)
        continue;

      if (strcmp (entry->d_name, dirfd_str) == 0)
        continue;

      for (i = 1; i < argc; i++)
        {
          if (argv[i] != NULL && strcmp (entry->d_name, argv[i]) == 0)
            {
              fprintf (stderr, "fd %s is open as expected\n", entry->d_name);
              argv[i] = NULL;
              found = true;
              break;
            }
        }

      if (!found)
        {
          fprintf (stderr, "fd %s should not have been open\n", entry->d_name);
          failed = true;
        }
    }

  for (i = 1; i < argc; i++)
    {
      if (argv[i] != NULL)
        {
          fprintf (stderr, "fd %s should have been open\n", argv[i]);
          failed = true;
        }
    }

  closedir (dir);
  free (dirfd_str);
  return failed ? 1 : 0;
}
