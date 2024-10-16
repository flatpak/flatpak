/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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

#include <stdarg.h>

#include <glib.h>
#include "flatpak.h"
#include "flatpak-context-private.h"
#include "flatpak-metadata-private.h"
#include "flatpak-utils-private.h"

#include "tests/testlib.h"

/* g_str_has_prefix as a GEqualFunc */
static gboolean
str_has_prefix (gconstpointer candidate,
                gconstpointer pattern)
{
  return g_str_has_prefix (candidate, pattern);
}

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

static void context_parse_args (FlatpakContext *context,
                                GError        **error,
                                ...) G_GNUC_NULL_TERMINATED;

static void
context_parse_args (FlatpakContext *context,
                    GError        **error,
                    ...)
{
  g_autoptr(GOptionContext) oc = NULL;
  g_autoptr(GOptionGroup) group = NULL;
  g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func (g_free);
  g_auto(GStrv) argv = NULL;
  const char *arg;
  va_list ap;

  g_ptr_array_add (args, g_strdup ("argv[0]"));

  va_start (ap, error);

  while ((arg = va_arg (ap, const char *)) != NULL)
    g_ptr_array_add (args, g_strdup (arg));

  va_end (ap);

  g_ptr_array_add (args, NULL);
  argv = (GStrv) g_ptr_array_free (g_steal_pointer (&args), FALSE);

  oc = g_option_context_new ("");
  group = flatpak_context_get_options (context);
  g_option_context_add_group (oc, g_steal_pointer (&group));
  g_option_context_parse_strv (oc, &argv, error);
}

static void
test_context_merge_fs (void)
{
  /*
   * We want to arrive at the same result regardless of whether we:
   * - start from lowest precedence, and successively merge higher
   *   precedences into it, discarding them when done;
   * - successively merge highest precedence into second-highest, and
   *   then discard highest
   */
  enum { LOWEST_FIRST, HIGHEST_FIRST, INVALID } merge_order;

  for (merge_order = LOWEST_FIRST; merge_order < INVALID; merge_order++)
    {
      g_autoptr(FlatpakContext) lowest = flatpak_context_new ();
      g_autoptr(FlatpakContext) middle = flatpak_context_new ();
      g_autoptr(FlatpakContext) highest = flatpak_context_new ();
      g_autoptr(GError) local_error = NULL;
      gpointer value;

      context_parse_args (lowest,
                          &local_error,
                          "--filesystem=/one",
                          NULL);
      g_assert_no_error (local_error);
      context_parse_args (middle,
                          &local_error,
                          "--nofilesystem=host:reset",
                          "--filesystem=/two",
                          NULL);
      g_assert_no_error (local_error);
      context_parse_args (highest,
                          &local_error,
                          "--nofilesystem=host",
                          "--filesystem=/three",
                          NULL);
      g_assert_no_error (local_error);

      g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "host", NULL, NULL));
      g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "host-reset", NULL, NULL));
      g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "/one", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
      g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "/two", NULL, NULL));
      g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "/three", NULL, NULL));

      g_assert_true (g_hash_table_lookup_extended (middle->filesystems, "host", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
      g_assert_true (g_hash_table_lookup_extended (middle->filesystems, "host-reset", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
      g_assert_false (g_hash_table_lookup_extended (middle->filesystems, "/one", NULL, NULL));
      g_assert_true (g_hash_table_lookup_extended (middle->filesystems, "/two", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
      g_assert_false (g_hash_table_lookup_extended (middle->filesystems, "/three", NULL, NULL));

      g_assert_true (g_hash_table_lookup_extended (highest->filesystems, "host", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
      g_assert_false (g_hash_table_lookup_extended (highest->filesystems, "host-reset", NULL, NULL));
      g_assert_false (g_hash_table_lookup_extended (highest->filesystems, "/one", NULL, NULL));
      g_assert_false (g_hash_table_lookup_extended (highest->filesystems, "/two", NULL, NULL));
      g_assert_true (g_hash_table_lookup_extended (highest->filesystems, "/three", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);

      if (merge_order == LOWEST_FIRST)
        {
          flatpak_context_merge (lowest, middle);

          g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "host", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
          g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "host-reset", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
          g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "/one", NULL, NULL));
          g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "/two", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
          g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "/three", NULL, NULL));

          flatpak_context_merge (lowest, highest);
        }
      else
        {
          flatpak_context_merge (middle, highest);

          g_assert_true (g_hash_table_lookup_extended (middle->filesystems, "host", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
          g_assert_true (g_hash_table_lookup_extended (middle->filesystems, "host-reset", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
          g_assert_false (g_hash_table_lookup_extended (middle->filesystems, "/one", NULL, NULL));
          g_assert_true (g_hash_table_lookup_extended (middle->filesystems, "/two", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
          g_assert_true (g_hash_table_lookup_extended (middle->filesystems, "/three", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);

          flatpak_context_merge (lowest, middle);
        }

      g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "host", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
      g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "host-reset", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
      g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "/one", NULL, NULL));
      g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "/two", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
      g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "/three", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
    }

  for (merge_order = LOWEST_FIRST; merge_order < INVALID; merge_order++)
    {
      g_autoptr(FlatpakContext) lowest = flatpak_context_new ();
      g_autoptr(FlatpakContext) mid_low = flatpak_context_new ();
      g_autoptr(FlatpakContext) mid_high = flatpak_context_new ();
      g_autoptr(FlatpakContext) highest = flatpak_context_new ();
      g_autoptr(GError) local_error = NULL;
      g_autoptr(GKeyFile) metakey = g_key_file_new ();
      g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func (g_free);
      g_autofree char *filesystems = NULL;
      gpointer value;

      context_parse_args (lowest,
                          &local_error,
                          "--filesystem=/one",
                          NULL);
      g_assert_no_error (local_error);
      context_parse_args (mid_low,
                          &local_error,
                          "--nofilesystem=host:reset",
                          "--filesystem=/two",
                          NULL);
      g_assert_no_error (local_error);
      context_parse_args (mid_high,
                          &local_error,
                          "--filesystem=host",
                          "--filesystem=/three",
                          NULL);
      g_assert_no_error (local_error);
      context_parse_args (highest,
                          &local_error,
                          "--nofilesystem=host",
                          "--filesystem=/four",
                          NULL);
      g_assert_no_error (local_error);

      g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "host", NULL, NULL));
      g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "host-reset", NULL, NULL));
      g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "/one", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
      g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "/two", NULL, NULL));
      g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "/three", NULL, NULL));
      g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "/four", NULL, NULL));

      g_assert_true (g_hash_table_lookup_extended (mid_low->filesystems, "host", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
      g_assert_true (g_hash_table_lookup_extended (mid_low->filesystems, "host-reset", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
      g_assert_false (g_hash_table_lookup_extended (mid_low->filesystems, "/one", NULL, NULL));
      g_assert_true (g_hash_table_lookup_extended (mid_low->filesystems, "/two", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
      g_assert_false (g_hash_table_lookup_extended (mid_low->filesystems, "/three", NULL, NULL));
      g_assert_false (g_hash_table_lookup_extended (mid_low->filesystems, "/four", NULL, NULL));

      g_assert_true (g_hash_table_lookup_extended (mid_high->filesystems, "host", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
      g_assert_false (g_hash_table_lookup_extended (mid_high->filesystems, "host-reset", NULL, NULL));
      g_assert_false (g_hash_table_lookup_extended (mid_high->filesystems, "/one", NULL, NULL));
      g_assert_false (g_hash_table_lookup_extended (mid_high->filesystems, "/two", NULL, NULL));
      g_assert_true (g_hash_table_lookup_extended (mid_high->filesystems, "/three", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
      g_assert_false (g_hash_table_lookup_extended (mid_high->filesystems, "/four", NULL, NULL));

      g_assert_true (g_hash_table_lookup_extended (highest->filesystems, "host", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
      g_assert_false (g_hash_table_lookup_extended (mid_high->filesystems, "host-reset", NULL, NULL));
      g_assert_false (g_hash_table_lookup_extended (highest->filesystems, "/one", NULL, NULL));
      g_assert_false (g_hash_table_lookup_extended (highest->filesystems, "/two", NULL, NULL));
      g_assert_false (g_hash_table_lookup_extended (highest->filesystems, "/three", NULL, NULL));
      g_assert_true (g_hash_table_lookup_extended (highest->filesystems, "/four", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);

      if (merge_order == LOWEST_FIRST)
        {
          flatpak_context_merge (lowest, mid_low);

          g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "host", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
          g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "host-reset", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
          g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "/one", NULL, NULL));
          g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "/two", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
          g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "/three", NULL, NULL));
          g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "/four", NULL, NULL));

          flatpak_context_merge (lowest, mid_high);

          g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "host", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
          g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "host-reset", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
          g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "/one", NULL, NULL));
          g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "/two", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
          g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "/three", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
          g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "/four", NULL, NULL));

          flatpak_context_merge (lowest, highest);
        }
      else
        {
          flatpak_context_merge (mid_high, highest);

          g_assert_true (g_hash_table_lookup_extended (mid_high->filesystems, "host", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
          g_assert_false (g_hash_table_lookup_extended (mid_high->filesystems, "host-reset", NULL, NULL));
          g_assert_false (g_hash_table_lookup_extended (mid_high->filesystems, "/one", NULL, NULL));
          g_assert_false (g_hash_table_lookup_extended (mid_high->filesystems, "/two", NULL, NULL));
          g_assert_true (g_hash_table_lookup_extended (mid_high->filesystems, "/three", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
          g_assert_true (g_hash_table_lookup_extended (mid_high->filesystems, "/four", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);

          flatpak_context_merge (mid_low, mid_high);

          g_assert_true (g_hash_table_lookup_extended (mid_low->filesystems, "host", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
          g_assert_true (g_hash_table_lookup_extended (mid_low->filesystems, "host-reset", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
          g_assert_false (g_hash_table_lookup_extended (mid_low->filesystems, "/one", NULL, NULL));
          g_assert_true (g_hash_table_lookup_extended (mid_low->filesystems, "/two", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
          g_assert_true (g_hash_table_lookup_extended (mid_low->filesystems, "/three", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
          g_assert_true (g_hash_table_lookup_extended (mid_low->filesystems, "/four", NULL, &value));
          g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);

          flatpak_context_merge (lowest, mid_low);
        }

      g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "host", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
      g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "host-reset", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_NONE);
      g_assert_false (g_hash_table_lookup_extended (lowest->filesystems, "/one", NULL, NULL));
      g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "/two", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
      g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "/three", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);
      g_assert_true (g_hash_table_lookup_extended (lowest->filesystems, "/four", NULL, &value));
      g_assert_cmpint (GPOINTER_TO_INT (value), ==, FLATPAK_FILESYSTEM_MODE_READ_WRITE);

      flatpak_context_save_metadata (lowest, FALSE, metakey);
      filesystems = g_key_file_get_value (metakey,
                                          FLATPAK_METADATA_GROUP_CONTEXT,
                                          FLATPAK_METADATA_KEY_FILESYSTEMS,
                                          &local_error);
      g_assert_no_error (local_error);
      g_test_message ("%s=%s", FLATPAK_METADATA_KEY_FILESYSTEMS, filesystems);
      /* !host:reset is serialized first */
      g_assert_true (g_str_has_prefix (filesystems, "!host:reset;"));
      /* The rest are serialized in arbitrary order */
      g_assert_nonnull (strstr (filesystems, ";!host;"));
      g_assert_null (strstr (filesystems, "/one"));
      g_assert_nonnull (strstr (filesystems, ";/two;"));
      g_assert_nonnull (strstr (filesystems, ";/three;"));
      g_assert_nonnull (strstr (filesystems, ";/four;"));

      flatpak_context_to_args (lowest, args);
      /* !host:reset is serialized first */
      g_assert_cmpuint (args->len, >, 0);
      g_assert_cmpstr (g_ptr_array_index (args, 0), ==,
                       "--nofilesystem=host:reset");
      /* The rest are serialized in arbitrary order */
      g_assert_true (g_ptr_array_find_with_equal_func (args, "--nofilesystem=host", g_str_equal, NULL));
      g_assert_false (g_ptr_array_find_with_equal_func (args, "--filesystem=/one", str_has_prefix, NULL));
      g_assert_false (g_ptr_array_find_with_equal_func (args, "--nofilesystem=/one", str_has_prefix, NULL));
      g_assert_true (g_ptr_array_find_with_equal_func (args, "--filesystem=/two", g_str_equal, NULL));
      g_assert_true (g_ptr_array_find_with_equal_func (args, "--filesystem=/three", g_str_equal, NULL));
      g_assert_true (g_ptr_array_find_with_equal_func (args, "--filesystem=/four", g_str_equal, NULL));
    }
}

const char *invalid_path_args[] = {
  "--filesystem=/\033[J:ro",
  "--filesystem=/\033[J",
  "--persist=\033[J",
};

/* CVE-2023-28101 */
static void
test_validate_path_args (void)
{
  gsize idx;

  for (idx = 0; idx < G_N_ELEMENTS (invalid_path_args); idx++)
    {
      g_autoptr(FlatpakContext) context = flatpak_context_new ();
      g_autoptr(GError) local_error = NULL;
      const char *path = invalid_path_args[idx];

      context_parse_args (context, &local_error, path, NULL);
      g_assert_error (local_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
      g_assert (strstr (local_error->message, "Non-graphical character"));
    }
}

typedef struct {
  const char *key;
  const char *value;
} PathValidityData;

PathValidityData invalid_path_meta[] = {
  {FLATPAK_METADATA_KEY_FILESYSTEMS, "\033[J"},
  {FLATPAK_METADATA_KEY_PERSISTENT, "\033[J"},
};

/* CVE-2023-28101 */
static void
test_validate_path_meta (void)
{
  gsize idx;

  for (idx = 0; idx < G_N_ELEMENTS (invalid_path_meta); idx++)
    {
      g_autoptr(FlatpakContext) context = flatpak_context_new ();
      g_autoptr(GKeyFile) metakey = g_key_file_new ();
      g_autoptr(GError) local_error = NULL;
      PathValidityData *data = &invalid_path_meta[idx];
      gboolean ret = FALSE;

      g_key_file_set_string_list (metakey, FLATPAK_METADATA_GROUP_CONTEXT,
                                  data->key, &data->value, 1);

      ret = flatpak_context_load_metadata (context, metakey, &local_error);
      g_assert_false (ret);
      g_assert_error (local_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
      g_assert (strstr (local_error->message, "Non-graphical character"));
    }

}

static void
test_devices (void)
{
  const char *devices_str[] = {
    "all",
    "!dri",
    "input",
    "!if:all:has-input-device",
  };
  int i;
  GPtrArray *conditionals;
  FlatpakContextDevices devices;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autofree char *saved_devices = NULL;
  g_autoptr(FlatpakContext) context = flatpak_context_new ();

  for (i = 0; i < G_N_ELEMENTS (devices_str); i++)
    flatpak_context_load_device (context, devices_str[i]);

  g_assert_cmpint (context->devices_valid, ==,
                   FLATPAK_CONTEXT_DEVICE_ALL |
                   FLATPAK_CONTEXT_DEVICE_DRI |
                   FLATPAK_CONTEXT_DEVICE_INPUT);
  g_assert_cmpint (context->devices, ==,
                   FLATPAK_CONTEXT_DEVICE_ALL |
                   FLATPAK_CONTEXT_DEVICE_INPUT);

  conditionals = g_hash_table_lookup (context->conditional_devices,
                                      GINT_TO_POINTER (FLATPAK_CONTEXT_DEVICE_ALL));
  g_assert_nonnull (conditionals);
  g_assert_cmpuint (conditionals->len, ==, 1);
  g_assert_cmpstr ((const char *)conditionals->pdata[0], ==, "has-input-device");

  devices = flatpak_context_compute_allowed_devices (context, NULL);
  g_assert_cmpint (devices, ==, FLATPAK_CONTEXT_DEVICE_INPUT);

  flatpak_context_load_device (context, "!if:all:false");

  conditionals = g_hash_table_lookup (context->conditional_devices,
                                      GINT_TO_POINTER (FLATPAK_CONTEXT_DEVICE_ALL));
  g_assert_nonnull (conditionals);
  g_assert_cmpuint (conditionals->len, ==, 2);
  g_assert_cmpstr ((const char *)conditionals->pdata[0], ==, "false");
  g_assert_cmpstr ((const char *)conditionals->pdata[1], ==, "has-input-device");

  devices = flatpak_context_compute_allowed_devices (context, NULL);
  g_assert_cmpint (devices, ==,
                   FLATPAK_CONTEXT_DEVICE_ALL |
                   FLATPAK_CONTEXT_DEVICE_INPUT);

  metakey = g_key_file_new ();
  flatpak_context_save_metadata (context, FALSE, metakey);
  saved_devices = g_key_file_get_value (metakey,
					FLATPAK_METADATA_GROUP_CONTEXT,
					FLATPAK_METADATA_KEY_DEVICES,
					&local_error);
  g_assert_cmpstr (saved_devices, ==,
                   "!dri;all;input;!if:all:false:has-input-device;");
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/context/env", test_context_env);
  g_test_add_func ("/context/env-fd", test_context_env_fd);
  g_test_add_func ("/context/merge-fs", test_context_merge_fs);
  g_test_add_func ("/context/validate-path-args", test_validate_path_args);
  g_test_add_func ("/context/validate-path-meta", test_validate_path_meta);
  g_test_add_func ("/context/devices", test_devices);

  return g_test_run ();
}
