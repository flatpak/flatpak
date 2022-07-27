/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright 2021 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

__attribute__((constructor)) static void
ctor (void)
{
  pid_t me = getpid ();
  struct stat buf;

  fprintf (stderr, "LD_PRELOAD module got loaded by process %d\n", me);

  if (stat ("/.flatpak-info", &buf) == 0)
    {
      fprintf (stderr, "OK: pid %d is in a Flatpak sandbox\n", me);
    }
  else
    {
      /* If the --env=LD_PRELOAD had come from a call to flatpak-portal,
       * then this would be a sandbox escape (GHSA-4ppf-fxf6-vxg2). */
      fprintf (stderr, "Error: pid %d is not in a Flatpak sandbox\n", me);
      abort ();
    }
}
