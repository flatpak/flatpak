/*
 * Based on glib/tests/testing-helper.c from GLib
 *
 * Copyright 2018-2022 Collabora Ltd.
 * Copyright 2019 Руслан Ижбулатов
 * Copyright 2018-2022 Endless OS Foundation LLC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "libglnx-config.h"
#include "libglnx.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

static const char *null = NULL;
static const char *nonnull = "not null";

static void
test_pass (void)
{
}

static void
test_messages (void)
{
  g_test_message ("This message has multiple lines.\n"
                  "In older GLib, it would corrupt TAP output.\n"
                  "That's why libglnx provides a wrapper.\n");
}

static void
test_assertion_failure_true (void)
{
  g_assert_true (null != NULL);
}

static void
test_assertion_failure_false (void)
{
  g_assert_false (null == NULL);
}

static void
test_assertion_failure_nonnull (void)
{
  g_assert_nonnull (null);
}

static void
test_assertion_failure_null (void)
{
  g_assert_null (nonnull);
}

static void
test_assertion_failure_mem_null_nonnull (void)
{
  g_assert_cmpmem (null, 0, nonnull, strlen (nonnull));
}

static void
test_assertion_failure_mem_nonnull_null (void)
{
  g_assert_cmpmem (nonnull, strlen (nonnull), null, 0);
}

static void
test_assertion_failure_mem_len (void)
{
  g_assert_cmpmem (nonnull, strlen (nonnull), nonnull, 0);
}

static void
test_assertion_failure_mem_cmp (void)
{
  g_assert_cmpmem (nonnull, 4, nonnull + 4, 4);
}

static void
test_assertion_failure_cmpfloat_with_epsilon (void)
{
  g_assert_cmpfloat_with_epsilon (1.0, 1.5, 0.001);
}

static void
test_assertion_failure_cmpvariant (void)
{
  g_autoptr(GVariant) a = g_variant_ref_sink (g_variant_new ("i", 42));
  g_autoptr(GVariant) b = g_variant_ref_sink (g_variant_new ("u", 42));

  g_assert_cmpvariant (a, b);
}

static void
test_assertion_failure_errno (void)
{
  g_assert_no_errno (mkdir ("/", 0755));
}

static void
test_assertion_failure_cmpstrv_null_nonnull (void)
{
  const char * const b[] = { NULL };

  g_assert_cmpstrv (NULL, b);
}

static void
test_assertion_failure_cmpstrv_nonnull_null (void)
{
  const char * const a[] = { NULL };

  g_assert_cmpstrv (a, NULL);
}

static void
test_assertion_failure_cmpstrv_len (void)
{
  const char * const a[] = { "one", NULL };
  const char * const b[] = { NULL };

  g_assert_cmpstrv (a, b);
}

static void
test_assertion_failure_cmpstrv_cmp (void)
{
  const char * const a[] = { "one", "two", NULL };
  const char * const b[] = { "one", "three", NULL };

  g_assert_cmpstrv (a, b);
}

static void
test_skip (void)
{
  g_test_skip ("not enough tea");
}

static void
test_skip_printf (void)
{
  const char *beverage = "coffee";

  g_test_skip_printf ("not enough %s", beverage);
}

static void
test_fail (void)
{
  g_test_fail ();
}

static void
test_fail_printf (void)
{
  g_test_fail_printf ("this test intentionally left failing");
}

static void
test_incomplete (void)
{
  g_test_incomplete ("mind reading not implemented yet");
}

static void
test_incomplete_printf (void)
{
  const char *operation = "telekinesis";

  g_test_incomplete_printf ("%s not implemented yet", operation);
}

static void
test_summary (void)
{
  g_test_summary ("Tests that g_test_summary() works with TAP, by outputting a "
                  "known summary message in testing-helper, and checking for "
                  "it in the TAP output later.");
}

int
main (int   argc,
      char *argv[])
{
  char *argv1;

  setlocale (LC_ALL, "");

#ifdef G_OS_WIN32
  /* Windows opens std streams in text mode, with \r\n EOLs.
   * Sometimes it's easier to force a switch to binary mode than
   * to account for extra \r in testcases.
   */
  setmode (fileno (stdout), O_BINARY);
#endif

  g_return_val_if_fail (argc > 1, 1);
  argv1 = argv[1];

  if (argc > 2)
    memmove (&argv[1], &argv[2], (argc - 2) * sizeof (char *));

  argc -= 1;
  argv[argc] = NULL;

  if (g_strcmp0 (argv1, "init-null-argv0") == 0)
    {
      int test_argc = 0;
      char *test_argva[1] = { NULL };
      char **test_argv = test_argva;

      /* Test that `g_test_init()` can handle being called with an empty argv
       * and argc == 0. While this isn’t recommended, it is possible for another
       * process to use execve() to call a gtest process this way, so we’d
       * better handle it gracefully.
       *
       * This test can’t be run after `g_test_init()` has been called normally,
       * as it isn’t allowed to be called more than once in a process. */
      g_test_init (&test_argc, &test_argv, NULL);

      return 0;
    }

  g_test_init (&argc, &argv, NULL);
  g_test_disable_crash_reporting ();
#if GLIB_CHECK_VERSION(2, 38, 0)
  g_test_set_nonfatal_assertions ();
#endif

  if (g_strcmp0 (argv1, "pass") == 0)
    {
      g_test_add_func ("/pass", test_pass);
    }
  else if (g_strcmp0 (argv1, "messages") == 0)
    {
      g_test_add_func ("/messages", test_messages);
    }
  else if (g_strcmp0 (argv1, "skip") == 0)
    {
      g_test_add_func ("/skip", test_skip);
    }
  else if (g_strcmp0 (argv1, "skip-printf") == 0)
    {
      g_test_add_func ("/skip-printf", test_skip_printf);
    }
  else if (g_strcmp0 (argv1, "incomplete") == 0)
    {
      g_test_add_func ("/incomplete", test_incomplete);
    }
  else if (g_strcmp0 (argv1, "incomplete-printf") == 0)
    {
      g_test_add_func ("/incomplete-printf", test_incomplete_printf);
    }
  else if (g_strcmp0 (argv1, "fail") == 0)
    {
      g_test_add_func ("/fail", test_fail);
    }
  else if (g_strcmp0 (argv1, "fail-printf") == 0)
    {
      g_test_add_func ("/fail-printf", test_fail_printf);
    }
  else if (g_strcmp0 (argv1, "all-non-failures") == 0)
    {
      g_test_add_func ("/pass", test_pass);
      g_test_add_func ("/skip", test_skip);
      g_test_add_func ("/incomplete", test_incomplete);
    }
  else if (g_strcmp0 (argv1, "all") == 0)
    {
      g_test_add_func ("/pass", test_pass);
      g_test_add_func ("/skip", test_skip);
      g_test_add_func ("/incomplete", test_incomplete);
      g_test_add_func ("/fail", test_fail);
    }
  else if (g_strcmp0 (argv1, "skip-options") == 0)
    {
      /* The caller is expected to skip some of these with
       * -p/-r, -s/-x and/or --GTestSkipCount */
      g_test_add_func ("/a", test_pass);
      g_test_add_func ("/b", test_pass);
      g_test_add_func ("/b/a", test_pass);
      g_test_add_func ("/b/b", test_pass);
      g_test_add_func ("/b/b/a", test_pass);
      g_test_add_func ("/prefix/a", test_pass);
      g_test_add_func ("/prefix/b/b", test_pass);
      g_test_add_func ("/prefix-long/a", test_pass);
      g_test_add_func ("/c/a", test_pass);
      g_test_add_func ("/d/a", test_pass);
    }
  else if (g_strcmp0 (argv1, "summary") == 0)
    {
      g_test_add_func ("/summary", test_summary);
    }
  else if (g_strcmp0 (argv1, "assertion-failures") == 0)
    {
      /* Use -p to select a specific one of these */
#define T(x) g_test_add_func ("/assertion-failure/" #x, test_assertion_failure_ ## x)
      T (true);
      T (false);
      T (nonnull);
      T (null);
      T (mem_null_nonnull);
      T (mem_nonnull_null);
      T (mem_len);
      T (mem_cmp);
      T (cmpfloat_with_epsilon);
      T (cmpvariant);
      T (errno);
      T (cmpstrv_null_nonnull);
      T (cmpstrv_nonnull_null);
      T (cmpstrv_len);
      T (cmpstrv_cmp);
#undef T
    }
  else
    {
      g_assert_not_reached ();
    }

  return g_test_run ();
}
