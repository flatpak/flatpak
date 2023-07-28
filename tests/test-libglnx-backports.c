/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 1995-1997  Peter Mattis, Spencer Kimball and Josh MacDonald
 * Copyright (C) 2018 Endless OS Foundation, LLC
 * Copyright 2019 Emmanuel Fleury
 * Copyright 2021 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later AND LicenseRef-old-glib-tests
 */

#include "libglnx-config.h"
#include "libglnx.h"

#include <glib/gstdio.h>

/* Testing g_memdup2() function with various positive and negative cases */
static void
test_memdup2 (void)
{
  gchar *str_dup = NULL;
  const gchar *str = "The quick brown fox jumps over the lazy dog";

  /* Testing negative cases */
  g_assert_null (g_memdup2 (NULL, 1024));
  g_assert_null (g_memdup2 (str, 0));
  g_assert_null (g_memdup2 (NULL, 0));

  /* Testing normal usage cases */
  str_dup = g_memdup2 (str, strlen (str) + 1);
  g_assert_nonnull (str_dup);
  g_assert_cmpstr (str, ==, str_dup);

  g_free (str_dup);
}

static void
test_steal_fd (void)
{
  GError *error = NULL;
  gchar *tmpfile = NULL;
  int fd = -42;
  int borrowed;
  int stolen;

  g_assert_cmpint (g_steal_fd (&fd), ==, -42);
  g_assert_cmpint (fd, ==, -1);
  g_assert_cmpint (g_steal_fd (&fd), ==, -1);
  g_assert_cmpint (fd, ==, -1);

  fd = g_file_open_tmp (NULL, &tmpfile, &error);
  g_assert_cmpint (fd, >=, 0);
  g_assert_no_error (error);
  borrowed = fd;
  stolen = g_steal_fd (&fd);
  g_assert_cmpint (fd, ==, -1);
  g_assert_cmpint (borrowed, ==, stolen);

  g_assert_no_errno (close (g_steal_fd (&stolen)));
  g_assert_cmpint (stolen, ==, -1);

  g_assert_no_errno (remove (tmpfile));
  g_free (tmpfile);

  /* Backwards compatibility with older libglnx: glnx_steal_fd is the same
   * as g_steal_fd */
  fd = -23;
  g_assert_cmpint (glnx_steal_fd (&fd), ==, -23);
  g_assert_cmpint (fd, ==, -1);
}

/* Test g_strv_equal() works for various inputs. */
static void
test_strv_equal (void)
{
  const gchar *strv_empty[] = { NULL };
  const gchar *strv_empty2[] = { NULL };
  const gchar *strv_simple[] = { "hello", "you", NULL };
  const gchar *strv_simple2[] = { "hello", "you", NULL };
  const gchar *strv_simple_reordered[] = { "you", "hello", NULL };
  const gchar *strv_simple_superset[] = { "hello", "you", "again", NULL };
  const gchar *strv_another[] = { "not", "a", "coded", "message", NULL };

  g_assert_true (g_strv_equal (strv_empty, strv_empty));
  g_assert_true (g_strv_equal (strv_empty, strv_empty2));
  g_assert_true (g_strv_equal (strv_empty2, strv_empty));
  g_assert_false (g_strv_equal (strv_empty, strv_simple));
  g_assert_false (g_strv_equal (strv_simple, strv_empty));
  g_assert_true (g_strv_equal (strv_simple, strv_simple));
  g_assert_true (g_strv_equal (strv_simple, strv_simple2));
  g_assert_true (g_strv_equal (strv_simple2, strv_simple));
  g_assert_false (g_strv_equal (strv_simple, strv_simple_reordered));
  g_assert_false (g_strv_equal (strv_simple_reordered, strv_simple));
  g_assert_false (g_strv_equal (strv_simple, strv_simple_superset));
  g_assert_false (g_strv_equal (strv_simple_superset, strv_simple));
  g_assert_false (g_strv_equal (strv_simple, strv_another));
  g_assert_false (g_strv_equal (strv_another, strv_simple));
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/mainloop/steal-fd", test_steal_fd);
  g_test_add_func ("/strfuncs/memdup2", test_memdup2);
  g_test_add_func ("/strfuncs/strv-equal", test_strv_equal);
  return g_test_run();
}
