/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 1995-1997  Peter Mattis, Spencer Kimball and Josh MacDonald
 * Copyright 2019 Emmanuel Fleury
 * SPDX-License-Identifier: LGPL-2.1-or-later AND LicenseRef-old-glib-tests
 */

#include "libglnx-config.h"
#include "libglnx.h"

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

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/strfuncs/memdup2", test_memdup2);
  return g_test_run();
}
