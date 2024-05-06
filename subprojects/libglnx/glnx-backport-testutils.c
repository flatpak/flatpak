/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright 2015 Colin Walters
 * Copyright 2020 Niels De Graef
 * Copyright 2021-2022 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the licence or (at
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

#include "libglnx-config.h"

#include <string.h>
#include <unistd.h>

#include <glib/gprintf.h>

#include "glnx-backport-autocleanups.h"
#include "glnx-backport-autoptr.h"
#include "glnx-backport-testutils.h"
#include "glnx-backports.h"

#include <sys/prctl.h>
#include <sys/resource.h>

#if !GLIB_CHECK_VERSION (2, 68, 0)
/* Backport of g_assertion_message_cmpstrv() */
void
_glnx_assertion_message_cmpstrv (const char         *domain,
                                 const char         *file,
                                 int                 line,
                                 const char         *func,
                                 const char         *expr,
                                 const char * const *arg1,
                                 const char * const *arg2,
                                 gsize               first_wrong_idx)
{
  const char *s1 = arg1[first_wrong_idx], *s2 = arg2[first_wrong_idx];
  char *a1, *a2, *s, *t1 = NULL, *t2 = NULL;

  a1 = g_strconcat ("\"", t1 = g_strescape (s1, NULL), "\"", NULL);
  a2 = g_strconcat ("\"", t2 = g_strescape (s2, NULL), "\"", NULL);
  g_free (t1);
  g_free (t2);
  s = g_strdup_printf ("assertion failed (%s): first differing element at index %" G_GSIZE_FORMAT ": %s does not equal %s",
                       expr, first_wrong_idx, a1, a2);
  g_free (a1);
  g_free (a2);
  g_assertion_message (domain, file, line, func, s);
  g_free (s);
}
#endif

#if !GLIB_CHECK_VERSION(2, 70, 0)
/*
 * Same as g_test_message(), but split messages with newlines into
 * multiple separate messages to avoid corrupting stdout, even in older
 * GLib versions that didn't do this
 */
void
_glnx_test_message_safe (const char *format,
                         ...)
{
  g_autofree char *message = NULL;
  va_list ap;
  char *line;
  char *saveptr = NULL;

  va_start (ap, format);
  g_vasprintf (&message, format, ap);
  va_end (ap);

  for (line = strtok_r (message, "\n", &saveptr);
       line != NULL;
       line = strtok_r (NULL, "\n", &saveptr))
    (g_test_message) ("%s", line);
}

/* Backport of g_test_fail_printf() */
void
_glnx_test_fail_printf (const char *format,
                        ...)
{
  g_autofree char *message = NULL;
  va_list ap;

  va_start (ap, format);
  g_vasprintf (&message, format, ap);
  va_end (ap);

  /* This is the closest we can do in older GLib */
  g_test_message ("Bail out! %s", message);
  g_test_fail ();
}

/* Backport of g_test_skip_printf() */
void
_glnx_test_skip_printf (const char *format,
                        ...)
{
  g_autofree char *message = NULL;
  va_list ap;

  va_start (ap, format);
  g_vasprintf (&message, format, ap);
  va_end (ap);

  g_test_skip (message);
}

/* Backport of g_test_incomplete_printf() */
void
_glnx_test_incomplete_printf (const char *format,
                              ...)
{
  g_autofree char *message = NULL;
  va_list ap;

  va_start (ap, format);
  g_vasprintf (&message, format, ap);
  va_end (ap);

#if GLIB_CHECK_VERSION(2, 58, 0)
  /* Since 2.58, g_test_incomplete() sets the exit status correctly. */
  g_test_incomplete (message);
#elif GLIB_CHECK_VERSION (2, 38, 0)
  /* Before 2.58, g_test_incomplete() was treated like a failure for the
   * purposes of setting the exit status, so prefer to use (our wrapper
   * around) g_test_skip(). */
  g_test_skip_printf ("TODO: %s", message);
#else
  g_test_message ("TODO: %s", message);
#endif
}
#endif

#if !GLIB_CHECK_VERSION (2, 78, 0)
void
_glnx_test_disable_crash_reporting (void)
{
  struct rlimit limit = { 0, 0 };

  (void) setrlimit (RLIMIT_CORE, &limit);

  /* On Linux, RLIMIT_CORE = 0 is ignored if core dumps are
   * configured to be written to a pipe, but PR_SET_DUMPABLE is not. */
  (void) prctl (PR_SET_DUMPABLE, 0, 0, 0, 0);
}
#endif
