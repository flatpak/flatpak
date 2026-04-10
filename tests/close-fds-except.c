/*
 * Copyright 2026 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <glib.h>

#include "libglnx.h"

#include "flatpak-utils-private.h"

/*
 * close-fds-except [FD...] -- COMMAND [ARG...]
 *
 * Run COMMAND [ARG...] with all fds closed, except for stdin, stdout,
 * stderr and each FD given.
 */
int
main (int    argc,
      char **argv)
{
  int i;

  g_debug ("Running command with all fds >= 3 closed, except as specified");
  g_fdwalk_set_cloexec (3);

  for (i = 1; i < argc; i++)
    {
      g_autoptr(GError) error = NULL;
      const char *arg = argv[i];
      int fd;

      if (strcmp (arg, "--") == 0)
        {
          i++;

          if (i >= argc)
            {
              g_printerr ("close-fds-except: COMMAND is required\n");
              return 125;
            }

          g_debug ("execvp %s", argv[i]);

          execvp (argv[i], &argv[i]);
          /* If still here, execvp failed */
          return 125;
        }

      fd = flatpak_parse_fd (arg, &error);

      if (fd < 0)
        {
          g_printerr ("close-fds-except: %s\n", error->message);
          return 125;
        }

      g_debug ("Leaving %d inheritable", fd);

      if (!flatpak_unset_cloexec (fd))
        {
          g_printerr ("close-fds-except: fd %d: %s\n", fd, g_strerror (errno));
          return 125;
        }
   }

  g_printerr ("close-fds-except: -- separator not found\n");
  return 125;
}
