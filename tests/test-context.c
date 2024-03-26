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
test_usb_list (void)
{
  const char *gtest_srcdir = NULL;
  g_autofree char *test_file_path = NULL;
  g_autofree char *content = NULL;
  g_autofree char *list = NULL;
  gboolean ret = FALSE;
  g_autoptr(GError) error = NULL;
  g_autoptr(GHashTable) allowed = g_hash_table_new_full (g_str_hash, g_str_equal,
							 g_free, (GDestroyNotify) flatpak_usb_query_free);
  g_autoptr(GHashTable) blocked = g_hash_table_new_full (g_str_hash, g_str_equal,
							 g_free, (GDestroyNotify) flatpak_usb_query_free);

  gtest_srcdir = g_getenv("G_TEST_SRCDIR");
  g_assert(gtest_srcdir);
  test_file_path = g_build_filename (gtest_srcdir, "gphoto2-list", NULL);

  ret = g_file_get_contents (test_file_path, &content, NULL, &error);
  g_assert (ret);

  ret = flatpak_context_parse_usb_list (content, allowed, blocked, &error);

  g_assert (ret);
  g_assert_no_error (error);
  g_assert_cmpint (g_hash_table_size (blocked), ==, 4);
  g_assert_cmpint (g_hash_table_size (allowed), ==, 2344);

  list = flatpak_context_devices_to_usb_list (blocked, TRUE);
  g_assert_cmpstr (list, ==, "!vnd:0502+dev:33c3;!vnd:4102+dev:1213;!vnd:0502+dev:365e;!vnd:0502+dev:387a;");

  g_hash_table_remove_all (allowed);
  g_hash_table_remove_all (blocked);
  ret = flatpak_context_parse_usb_list (list, allowed, blocked, &error);
  g_assert_cmpint (g_hash_table_size (blocked), ==, 4);
  g_assert_cmpint (g_hash_table_size (allowed), ==, 0);
}

static void
test_usb_rules_all (void)
{
  g_autoptr(FlatpakUsbRule) usb_rule = NULL;
  g_autoptr(GError) local_error = NULL;
  gboolean ret = FALSE;

  /* Valid USB 'all' rule */
  ret = flatpak_context_parse_usb_rule ("all", &usb_rule, &local_error);
  g_assert_true (ret);
  g_assert_no_error (local_error);
  g_assert_cmpint (usb_rule->rule_type, ==, FLATPAK_USB_RULE_TYPE_ALL);
    {
      g_autoptr(GString) string = g_string_new (NULL);
      flatpak_usb_rule_print (usb_rule, string);
      g_assert_cmpstr (string->str, ==, "all");
    }
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);

  /* Invalid USB 'all' rule */
  ret = flatpak_context_parse_usb_rule ("all:09", &usb_rule, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_rule);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_error (&local_error);
}

static void
test_usb_rules_cls (void)
{
  g_autoptr(FlatpakUsbRule) usb_rule = NULL;
  g_autoptr(GError) local_error = NULL;
  gboolean ret = FALSE;

  /* Valid USB 'cls' rules */
  ret = flatpak_context_parse_usb_rule ("cls:09:03", &usb_rule, &local_error);
  g_assert_true (ret);
  g_assert_no_error (local_error);
  g_assert_cmpint (usb_rule->rule_type, ==, FLATPAK_USB_RULE_TYPE_CLASS);
  g_assert_cmpint (usb_rule->d.device_class.type, ==, FLATPAK_USB_RULE_CLASS_TYPE_CLASS_SUBCLASS);
  g_assert_cmpuint (usb_rule->d.device_class.class, ==, 0x09);
  g_assert_cmpuint (usb_rule->d.device_class.subclass, ==, 0x03);
    {
      g_autoptr(GString) string = g_string_new (NULL);
      flatpak_usb_rule_print (usb_rule, string);
      g_assert_cmpstr (string->str, ==, "cls:09:03");
    }
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);

  ret = flatpak_context_parse_usb_rule ("cls:09:*", &usb_rule, &local_error);
  g_assert_true (ret);
  g_assert_no_error (local_error);
  g_assert_cmpint (usb_rule->rule_type, ==, FLATPAK_USB_RULE_TYPE_CLASS);
  g_assert_cmpint (usb_rule->d.device_class.type, ==, FLATPAK_USB_RULE_CLASS_TYPE_CLASS_ONLY);
  g_assert_cmpuint (usb_rule->d.device_class.class, ==, 0x09);
    {
      g_autoptr(GString) string = g_string_new (NULL);
      flatpak_usb_rule_print (usb_rule, string);
      g_assert_cmpstr (string->str, ==, "cls:09:*");
    }
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);

  /* Invalid USB 'cls' rules */
  ret = flatpak_context_parse_usb_rule ("cls:00:00", &usb_rule, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_rule);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);
  g_clear_error (&local_error);

  ret = flatpak_context_parse_usb_rule ("cls:0009:0003", &usb_rule, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_rule);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);
  g_clear_error (&local_error);

  ret = flatpak_context_parse_usb_rule ("cls:*:03", &usb_rule, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_rule);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);
  g_clear_error (&local_error);

  ret = flatpak_context_parse_usb_rule ("cls:*:*", &usb_rule, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_rule);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);
  g_clear_error (&local_error);

  ret = flatpak_context_parse_usb_rule ("cls:*", &usb_rule, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_rule);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);
  g_clear_error (&local_error);

  ret = flatpak_context_parse_usb_rule ("cls", &usb_rule, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_rule);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);
  g_clear_error (&local_error);
}

static void
test_usb_rules_dev (void)
{
  g_autoptr(FlatpakUsbRule) usb_rule = NULL;
  g_autoptr(GError) local_error = NULL;
  gboolean ret = FALSE;

  /* Valid USB 'dev' rules */
  ret = flatpak_context_parse_usb_rule ("dev:0060", &usb_rule, &local_error);
  g_assert_true (ret);
  g_assert_no_error (local_error);
  g_assert_cmpint (usb_rule->rule_type, ==, FLATPAK_USB_RULE_TYPE_DEVICE);
  g_assert_cmpuint (usb_rule->d.product.id, ==, 0x0060);
    {
      g_autoptr(GString) string = g_string_new (NULL);
      flatpak_usb_rule_print (usb_rule, string);
      g_assert_cmpstr (string->str, ==, "dev:0060");
    }
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);

  /* Invalid USB 'dev' rules */
  ret = flatpak_context_parse_usb_rule ("dev:0000", &usb_rule, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_rule);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);
  g_clear_error (&local_error);

  ret = flatpak_context_parse_usb_rule ("dev:00", &usb_rule, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_rule);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);
  g_clear_error (&local_error);

  ret = flatpak_context_parse_usb_rule ("dev:*", &usb_rule, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_rule);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);
  g_clear_error (&local_error);

  ret = flatpak_context_parse_usb_rule ("dev", &usb_rule, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_rule);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);
  g_clear_error (&local_error);
}

static void
test_usb_rules_vnd (void)
{
  g_autoptr(FlatpakUsbRule) usb_rule = NULL;
  g_autoptr(GError) local_error = NULL;
  gboolean ret = FALSE;

  /* Valid USB 'vnd' rules */
  ret = flatpak_context_parse_usb_rule ("vnd:0fd9", &usb_rule, &local_error);
  g_assert_true (ret);
  g_assert_no_error (local_error);
  g_assert_cmpint (usb_rule->rule_type, ==, FLATPAK_USB_RULE_TYPE_VENDOR);
  g_assert_cmpuint (usb_rule->d.vendor.id, ==, 0x0fd9);
    {
      g_autoptr(GString) string = g_string_new (NULL);
      flatpak_usb_rule_print (usb_rule, string);
      g_assert_cmpstr (string->str, ==, "vnd:0fd9");
    }
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);

  /* Invalid USB 'vnd' rules */
  ret = flatpak_context_parse_usb_rule ("vnd:0000", &usb_rule, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_rule);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);
  g_clear_error (&local_error);

  ret = flatpak_context_parse_usb_rule ("vnd:00", &usb_rule, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_rule);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);
  g_clear_error (&local_error);

  ret = flatpak_context_parse_usb_rule ("vnd:*", &usb_rule, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_rule);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);
  g_clear_error (&local_error);

  ret = flatpak_context_parse_usb_rule ("vnd", &usb_rule, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_rule);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_rule, flatpak_usb_rule_free);
  g_clear_error (&local_error);
}

static void
test_usb_query_simple (void)
{
  g_autoptr(FlatpakUsbQuery) usb_query = NULL;
  g_autoptr(GError) local_error = NULL;
  gboolean ret = FALSE;

  ret = flatpak_context_parse_usb ("all", &usb_query, &local_error);
  g_assert_true (ret);
  g_assert_nonnull (usb_query);
  g_assert_no_error (local_error);
  g_assert_cmpuint (usb_query->rules->len, ==, 1);
    {
      FlatpakUsbRule *usb_rule = g_ptr_array_index (usb_query->rules, 0);
      g_assert_cmpint (usb_rule->rule_type, ==, FLATPAK_USB_RULE_TYPE_ALL);
    }
    {
      g_autoptr(GString) string = g_string_new (NULL);
      flatpak_usb_query_print (usb_query, string);
      g_assert_cmpstr (string->str, ==, "all");
    }
  g_clear_pointer (&usb_query, flatpak_usb_query_free);

  ret = flatpak_context_parse_usb ("cls:03:*", &usb_query, &local_error);
  g_assert_true (ret);
  g_assert_nonnull (usb_query);
  g_assert_no_error (local_error);
  g_assert_cmpuint (usb_query->rules->len, ==, 1);
    {
      FlatpakUsbRule *usb_rule = g_ptr_array_index (usb_query->rules, 0);
      g_assert_cmpint (usb_rule->rule_type, ==, FLATPAK_USB_RULE_TYPE_CLASS);
      g_assert_cmpint (usb_rule->d.device_class.type, ==, FLATPAK_USB_RULE_CLASS_TYPE_CLASS_ONLY);
      g_assert_cmpuint (usb_rule->d.device_class.class, ==, 0x03);
    }
    {
      g_autoptr(GString) string = g_string_new (NULL);
      flatpak_usb_query_print (usb_query, string);
      g_assert_cmpstr (string->str, ==, "cls:03:*");
    }
  g_clear_pointer (&usb_query, flatpak_usb_query_free);

  ret = flatpak_context_parse_usb ("vnd:0fd9", &usb_query, &local_error);
  g_assert_true (ret);
  g_assert_nonnull (usb_query);
  g_assert_no_error (local_error);
  g_assert_cmpuint (usb_query->rules->len, ==, 1);
    {
      FlatpakUsbRule *usb_rule = g_ptr_array_index (usb_query->rules, 0);
      g_assert_cmpint (usb_rule->rule_type, ==, FLATPAK_USB_RULE_TYPE_VENDOR);
      g_assert_cmpuint (usb_rule->d.vendor.id, ==, 0x0fd9);
    }
    {
      g_autoptr(GString) string = g_string_new (NULL);
      flatpak_usb_query_print (usb_query, string);
      g_assert_cmpstr (string->str, ==, "vnd:0fd9");
    }
  g_clear_pointer (&usb_query, flatpak_usb_query_free);

  /* Invalid USB query */
  ret = flatpak_context_parse_usb ("all:0123", &usb_query, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_query);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_query, flatpak_usb_query_free);
  g_clear_error (&local_error);

  /* Invalid empty USB query */
  ret = flatpak_context_parse_usb ("", &usb_query, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_query);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_query, flatpak_usb_query_free);
  g_clear_error (&local_error);
}

static void
test_usb_query_device_and_vendor (void)
{
  g_autoptr(FlatpakUsbQuery) usb_query = NULL;
  g_autoptr(GError) local_error = NULL;
  gboolean ret = FALSE;

  ret = flatpak_context_parse_usb ("vnd:0fd9+dev:0063", &usb_query, &local_error);
  g_assert_true (ret);
  g_assert_no_error (local_error);
  g_assert_cmpuint (usb_query->rules->len, ==, 2);
    {
      FlatpakUsbRule *usb_rule;

      usb_rule = g_ptr_array_index (usb_query->rules, 0);
      g_assert_cmpint (usb_rule->rule_type, ==, FLATPAK_USB_RULE_TYPE_VENDOR);
      g_assert_cmpuint (usb_rule->d.vendor.id, ==, 0x0fd9);

      usb_rule = g_ptr_array_index (usb_query->rules, 1);
      g_assert_cmpint (usb_rule->rule_type, ==, FLATPAK_USB_RULE_TYPE_DEVICE);
      g_assert_cmpuint (usb_rule->d.product.id, ==, 0x063);
    }
    {
      g_autoptr(GString) string = g_string_new (NULL);
      flatpak_usb_query_print (usb_query, string);
      g_assert_cmpstr (string->str, ==, "vnd:0fd9+dev:0063");
    }
  g_clear_pointer (&usb_query, flatpak_usb_query_free);

  ret = flatpak_context_parse_usb ("vnd:0fd9+dev:0063+cls:09:*", &usb_query, &local_error);
  g_assert_true (ret);
  g_assert_no_error (local_error);
  g_assert_cmpuint (usb_query->rules->len, ==, 3);
    {
      FlatpakUsbRule *usb_rule;

      usb_rule = g_ptr_array_index (usb_query->rules, 0);
      g_assert_cmpint (usb_rule->rule_type, ==, FLATPAK_USB_RULE_TYPE_VENDOR);
      g_assert_cmpuint (usb_rule->d.vendor.id, ==, 0x0fd9);

      usb_rule = g_ptr_array_index (usb_query->rules, 1);
      g_assert_cmpint (usb_rule->rule_type, ==, FLATPAK_USB_RULE_TYPE_DEVICE);
      g_assert_cmpuint (usb_rule->d.product.id, ==, 0x063);

      usb_rule = g_ptr_array_index (usb_query->rules, 2);
      g_assert_cmpint (usb_rule->rule_type, ==, FLATPAK_USB_RULE_TYPE_CLASS);
      g_assert_cmpint (usb_rule->d.device_class.type, ==, FLATPAK_USB_RULE_CLASS_TYPE_CLASS_ONLY);
      g_assert_cmpuint (usb_rule->d.device_class.class, ==, 0x09);
    }
    {
      g_autoptr(GString) string = g_string_new (NULL);
      flatpak_usb_query_print (usb_query, string);
      g_assert_cmpstr (string->str, ==, "vnd:0fd9+dev:0063+cls:09:*");
    }
  g_clear_pointer (&usb_query, flatpak_usb_query_free);

  /* Device without vendor is invalid */
  ret = flatpak_context_parse_usb ("dev:0063", &usb_query, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_query);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_query, flatpak_usb_query_free);
  g_clear_error (&local_error);

  /* 'all' in the query invalidates further rules */
  ret = flatpak_context_parse_usb ("all+dev:0063", &usb_query, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_query);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_query, flatpak_usb_query_free);
  g_clear_error (&local_error);

  ret = flatpak_context_parse_usb ("all+vnd:0fd+dev:0063", &usb_query, &local_error);
  g_assert_false (ret);
  g_assert_null (usb_query);
  g_assert_error (local_error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE);
  g_clear_pointer (&usb_query, flatpak_usb_query_free);
  g_clear_error (&local_error);
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

  g_test_add_func ("/context/usb-list", test_usb_list);
  g_test_add_func ("/context/usb-rules/all", test_usb_rules_all);
  g_test_add_func ("/context/usb-rules/cls", test_usb_rules_cls);
  g_test_add_func ("/context/usb-rules/dev", test_usb_rules_dev);
  g_test_add_func ("/context/usb-rules/vnd", test_usb_rules_vnd);

  g_test_add_func ("/context/usb-query/simple", test_usb_query_simple);
  g_test_add_func ("/context/usb-query/device-and-vendor", test_usb_query_device_and_vendor);

  return g_test_run ();
}
