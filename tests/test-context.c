/*
 * Copyright © 2021 Collabora Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <glib.h>
#include "flatpak.h"
#include "flatpak-context-private.h"

#include "tests/testlib.h"

static void
test_context_env (void)
{
  g_autoptr(FlatpakContext) context = flatpak_context_new ();
  g_autoptr(GError) error = NULL;
  gboolean ok;
  const char env[] = "ONE=one\0TWO=two\0THREE=three\0EMPTY=\0X=x";

  ok = flatpak_context_parse_env_block (context, env, sizeof (env), &error);
  g_assert_no_error (error);
  g_assert_true (ok);
  g_assert_cmpstr (g_hash_table_lookup (context->env_vars, "ONE"), ==, "one");
  g_assert_cmpstr (g_hash_table_lookup (context->env_vars, "EMPTY"), ==, "");
  g_assert_cmpstr (g_hash_table_lookup (context->env_vars, "nope"), ==, NULL);

  ok = flatpak_context_parse_env_block (context,
                                        "FOO=barnstorming past the end",
                                        strlen ("FOO=bar"),
                                        &error);
  g_assert_no_error (error);
  g_assert_true (ok);
  g_assert_cmpstr (g_hash_table_lookup (context->env_vars, "FOO"), ==, "bar");

  ok = flatpak_context_parse_env_block (context,
                                        "BAD=barnstorming past the end",
                                        strlen ("BA"),
                                        &error);
  g_assert_nonnull (error);
  g_test_message ("Got error as expected: %s #%d: %s",
                  g_quark_to_string (error->domain), error->code,
                  error->message);
  g_assert_false (ok);
  g_assert_cmpstr (g_hash_table_lookup (context->env_vars, "BA"), ==, NULL);
  g_assert_cmpstr (g_hash_table_lookup (context->env_vars, "BAD"), ==, NULL);
  g_clear_error (&error);

  ok = flatpak_context_parse_env_block (context, "=x", strlen ("=x"), &error);
  g_assert_nonnull (error);
  g_test_message ("Got error as expected: %s #%d: %s",
                  g_quark_to_string (error->domain), error->code,
                  error->message);
  g_assert_false (ok);
  g_assert_cmpstr (g_hash_table_lookup (context->env_vars, ""), ==, NULL);
  g_clear_error (&error);

  ok = flatpak_context_parse_env_block (context, "\0", 1, &error);
  g_assert_nonnull (error);
  g_test_message ("Got error as expected: %s #%d: %s",
                  g_quark_to_string (error->domain), error->code,
                  error->message);
  g_assert_false (ok);
  g_assert_cmpstr (g_hash_table_lookup (context->env_vars, ""), ==, NULL);
  g_clear_error (&error);

  ok = flatpak_context_parse_env_block (context, "", 0, &error);
  g_assert_no_error (error);
  g_assert_true (ok);
}

static void
test_context_env_fd (void)
{
  g_autoptr(FlatpakContext) context = flatpak_context_new ();
  g_autoptr(GError) error = NULL;
  gboolean ok;
  const char env[] = "ONE=one\0TWO=two\0THREE=three\0EMPTY=\0X=x";
  glnx_autofd int fd = -1;
  g_autofree char *path = NULL;
  int bad_fd;

  path = g_strdup_printf ("/tmp/flatpak-test.XXXXXX");
  fd = g_mkstemp (path);
  g_assert_no_errno (glnx_loop_write (fd, env, sizeof (env)));
  g_assert_no_errno (lseek (fd, 0, SEEK_SET));

  ok = flatpak_context_parse_env_fd (context, fd, &error);
  g_assert_no_error (error);
  g_assert_true (ok);
  g_assert_cmpstr (g_hash_table_lookup (context->env_vars, "ONE"), ==, "one");
  g_assert_cmpstr (g_hash_table_lookup (context->env_vars, "EMPTY"), ==, "");
  g_assert_cmpstr (g_hash_table_lookup (context->env_vars, "nope"), ==, NULL);

  bad_fd = fd;
  glnx_close_fd (&fd);
  ok = flatpak_context_parse_env_fd (context, bad_fd, &error);
  g_assert_nonnull (error);
  g_test_message ("Got error as expected: %s #%d: %s",
                  g_quark_to_string (error->domain), error->code,
                  error->message);
  g_assert_false (ok);
  g_clear_error (&error);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/context/env", test_context_env);
  g_test_add_func ("/context/env-fd", test_context_env_fd);

  return g_test_run ();
}
