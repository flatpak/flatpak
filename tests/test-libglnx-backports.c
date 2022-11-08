/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 1995-1997  Peter Mattis, Spencer Kimball and Josh MacDonald
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

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/mainloop/steal-fd", test_steal_fd);
  g_test_add_func ("/strfuncs/memdup2", test_memdup2);
  return g_test_run();
}
