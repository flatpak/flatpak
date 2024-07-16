/*
 * Copyright 2022 Simon McVittie
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

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#if GLIB_CHECK_VERSION (2, 38, 0)
#define GTEST_TAP_OR_VERBOSE "--tap"
#else
#define GTEST_TAP_OR_VERBOSE "--verbose"
#endif

static const char *null = NULL;
static const char *nonnull = "not null";

static void
test_assertions (void)
{
  const char *other_nonnull = "not null";
  g_autoptr(GVariant) va = g_variant_ref_sink (g_variant_new ("i", 42));
  g_autoptr(GVariant) vb = g_variant_ref_sink (g_variant_new ("i", 42));
  const char * const strv1[] = {"one", "two", NULL};
  const char * const strv2[] = {"one", "two", NULL};
  GStatBuf statbuf;

  g_assert_true (null == NULL);
  g_assert_false (null != NULL);
  g_assert_null (null);
  g_assert_nonnull (nonnull);
  g_assert_cmpmem (null, 0, null, 0);
  g_assert_cmpmem (nonnull, strlen (nonnull), other_nonnull, strlen (other_nonnull));
  g_assert_cmpfloat_with_epsilon (1.0, 1.00001, 0.01);
  g_assert_cmpvariant (va, vb);
  g_assert_no_errno (g_stat ("/", &statbuf));
  g_assert_cmpstrv (NULL, NULL);
  g_assert_cmpstrv (&null, &null);
  g_assert_cmpstrv (strv1, strv2);
}

static void
test_assertion_failures (void)
{
  static const char * const assertion_failures[] =
  {
    "true",
    "false",
    "nonnull",
    "null",
    "mem_null_nonnull",
    "mem_nonnull_null",
    "mem_len",
    "mem_cmp",
    "cmpfloat_with_epsilon",
    "cmpvariant",
    "errno",
    "cmpstrv_null_nonnull",
    "cmpstrv_nonnull_null",
    "cmpstrv_len",
    "cmpstrv_cmp",
  };
  g_autoptr(GError) error = NULL;
  g_autofree char *self = NULL;
  g_autofree char *dir = NULL;
  g_autofree char *exe = NULL;
  gsize i;

  self = glnx_readlinkat_malloc (-1, "/proc/self/exe", NULL, &error);
  g_assert_no_error (error);

  dir = g_path_get_dirname (self);
  exe = g_build_filename (dir, "testing-helper", NULL);

  for (i = 0; i < G_N_ELEMENTS (assertion_failures); i++)
    {
      g_autofree char *out = NULL;
      g_autofree char *err = NULL;
      g_autofree char *name = g_strdup_printf ("/assertion-failure/%s", assertion_failures[i]);
      int wait_status = -1;
      const char *argv[] = { NULL, "assertion-failures", "-p", NULL, NULL, NULL };
      char *line;
      char *saveptr = NULL;

      argv[0] = exe;
      argv[3] = name;
      argv[4] = GTEST_TAP_OR_VERBOSE;
      g_test_message ("%s assertion-failures -p %s %s...", exe, name, GTEST_TAP_OR_VERBOSE);

      g_spawn_sync (NULL,   /* cwd */
                    (char **) argv,
                    NULL,   /* envp */
                    G_SPAWN_DEFAULT,
                    NULL,   /* child setup */
                    NULL,   /* user data */
                    &out,
                    &err,
                    &wait_status,
                    &error);
      g_assert_no_error (error);

      g_assert_nonnull (out);
      g_assert_nonnull (err);

      for (line = strtok_r (out, "\n", &saveptr);
           line != NULL;
           line = strtok_r (NULL, "\n", &saveptr))
        g_test_message ("stdout: %s", line);

      saveptr = NULL;

      for (line = strtok_r (err, "\n", &saveptr);
           line != NULL;
           line = strtok_r (NULL, "\n", &saveptr))
        g_test_message ("stderr: %s", line);

      g_test_message ("wait status: 0x%x", wait_status);

      /* It exited with a nonzero status that was not exit status 77 */
      G_STATIC_ASSERT (WIFEXITED (0));
      G_STATIC_ASSERT (WEXITSTATUS (0) == 0);
      g_assert_cmphex (wait_status, !=, 0);
      G_STATIC_ASSERT (WIFEXITED (77 << 8));
      G_STATIC_ASSERT (WEXITSTATUS (77 << 8) == 77);
      g_assert_cmphex (wait_status, !=, (77 << 8));
    }
}

static void
test_failures (void)
{
  static const char * const failures[] =
  {
    "fail",
    "fail-printf",
  };
  g_autoptr(GError) error = NULL;
  g_autofree char *self = NULL;
  g_autofree char *dir = NULL;
  g_autofree char *exe = NULL;
  gsize i;

  self = glnx_readlinkat_malloc (-1, "/proc/self/exe", NULL, &error);
  g_assert_no_error (error);

  dir = g_path_get_dirname (self);
  exe = g_build_filename (dir, "testing-helper", NULL);

  for (i = 0; i < G_N_ELEMENTS (failures); i++)
    {
      g_autofree char *out = NULL;
      g_autofree char *err = NULL;
      int wait_status = -1;
      const char *argv[] = { NULL, NULL, NULL, NULL };
      char *line;
      char *saveptr = NULL;

      argv[0] = exe;
      argv[1] = failures[i];
      argv[2] = GTEST_TAP_OR_VERBOSE;
      g_test_message ("%s %s %s...", exe, failures[i], GTEST_TAP_OR_VERBOSE);

      g_spawn_sync (NULL,   /* cwd */
                    (char **) argv,
                    NULL,   /* envp */
                    G_SPAWN_DEFAULT,
                    NULL,   /* child setup */
                    NULL,   /* user data */
                    &out,
                    &err,
                    &wait_status,
                    &error);
      g_assert_no_error (error);

      for (line = strtok_r (out, "\n", &saveptr);
           line != NULL;
           line = strtok_r (NULL, "\n", &saveptr))
        g_test_message ("stdout: %s", line);

      saveptr = NULL;

      for (line = strtok_r (err, "\n", &saveptr);
           line != NULL;
           line = strtok_r (NULL, "\n", &saveptr))
        g_test_message ("stderr: %s", line);

      g_test_message ("wait status: 0x%x", wait_status);

      G_STATIC_ASSERT (WIFEXITED (0));
      G_STATIC_ASSERT (WEXITSTATUS (0) == 0);
      G_STATIC_ASSERT (WIFEXITED (77 << 8));
      G_STATIC_ASSERT (WEXITSTATUS (77 << 8) == 77);

      g_assert_cmphex (wait_status, !=, 0);
      g_assert_cmphex (wait_status, !=, (77 << 8));
    }
}

static void
test_skips (void)
{
  static const char * const skips[] =
  {
    "skip",
    "skip-printf",
    "incomplete",
    "incomplete-printf",
  };
  g_autoptr(GError) error = NULL;
  g_autofree char *self = NULL;
  g_autofree char *dir = NULL;
  g_autofree char *exe = NULL;
  gsize i;

  self = glnx_readlinkat_malloc (-1, "/proc/self/exe", NULL, &error);
  g_assert_no_error (error);

  dir = g_path_get_dirname (self);
  exe = g_build_filename (dir, "testing-helper", NULL);

  for (i = 0; i < G_N_ELEMENTS (skips); i++)
    {
      g_autofree char *out = NULL;
      g_autofree char *err = NULL;
      int wait_status = -1;
      const char *argv[] = { NULL, NULL, NULL, NULL };
      char *line;
      char *saveptr = NULL;

      argv[0] = exe;
      argv[1] = skips[i];
      argv[2] = GTEST_TAP_OR_VERBOSE;
      g_test_message ("%s %s %s...", exe, skips[i], GTEST_TAP_OR_VERBOSE);

      g_spawn_sync (NULL,   /* cwd */
                    (char **) argv,
                    NULL,   /* envp */
                    G_SPAWN_DEFAULT,
                    NULL,   /* child setup */
                    NULL,   /* user data */
                    &out,
                    &err,
                    &wait_status,
                    &error);
      g_assert_no_error (error);

      for (line = strtok_r (out, "\n", &saveptr);
           line != NULL;
           line = strtok_r (NULL, "\n", &saveptr))
        g_test_message ("stdout: %s", line);

      saveptr = NULL;

      for (line = strtok_r (err, "\n", &saveptr);
           line != NULL;
           line = strtok_r (NULL, "\n", &saveptr))
        g_test_message ("stderr: %s", line);

      g_test_message ("wait status: 0x%x", wait_status);

      G_STATIC_ASSERT (WIFEXITED (0));
      G_STATIC_ASSERT (WEXITSTATUS (0) == 0);
      G_STATIC_ASSERT (WIFEXITED (77 << 8));
      G_STATIC_ASSERT (WEXITSTATUS (77 << 8) == 77);

      /* Ideally the exit status is 77, but it might be 0 with older GLib */
      if (wait_status != 0)
        g_assert_cmphex (wait_status, ==, (77 << 8));
    }
}

static void
test_successes (void)
{
  static const char * const successes[] =
  {
    "messages",
    "pass",
    "summary",
  };
  g_autoptr(GError) error = NULL;
  g_autofree char *self = NULL;
  g_autofree char *dir = NULL;
  g_autofree char *exe = NULL;
  gsize i;

  self = glnx_readlinkat_malloc (-1, "/proc/self/exe", NULL, &error);
  g_assert_no_error (error);

  dir = g_path_get_dirname (self);
  exe = g_build_filename (dir, "testing-helper", NULL);

  for (i = 0; i < G_N_ELEMENTS (successes); i++)
    {
      g_autofree char *out = NULL;
      g_autofree char *err = NULL;
      int wait_status = -1;
      const char *argv[] = { NULL, NULL, NULL, NULL };
      char *line;
      char *saveptr = NULL;

      argv[0] = exe;
      argv[1] = successes[i];
      argv[2] = GTEST_TAP_OR_VERBOSE;
      g_test_message ("%s %s %s...", exe, successes[i], GTEST_TAP_OR_VERBOSE);

      g_spawn_sync (NULL,   /* cwd */
                    (char **) argv,
                    NULL,   /* envp */
                    G_SPAWN_DEFAULT,
                    NULL,   /* child setup */
                    NULL,   /* user data */
                    &out,
                    &err,
                    &wait_status,
                    &error);
      g_assert_no_error (error);

      for (line = strtok_r (out, "\n", &saveptr);
           line != NULL;
           line = strtok_r (NULL, "\n", &saveptr))
        g_test_message ("stdout: %s", line);

      saveptr = NULL;

      for (line = strtok_r (err, "\n", &saveptr);
           line != NULL;
           line = strtok_r (NULL, "\n", &saveptr))
        g_test_message ("stderr: %s", line);

      g_test_message ("wait status: 0x%x", wait_status);

      G_STATIC_ASSERT (WIFEXITED (0));
      G_STATIC_ASSERT (WEXITSTATUS (0) == 0);
      g_assert_cmphex (wait_status, ==, 0);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/assertions", test_assertions);
  g_test_add_func ("/assertion_failures", test_assertion_failures);
  g_test_add_func ("/failures", test_failures);
  g_test_add_func ("/skips", test_skips);
  g_test_add_func ("/successes", test_successes);
  return g_test_run();
}
