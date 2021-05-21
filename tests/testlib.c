/*
 * Copyright © 2018-2021 Collabora Ltd.
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
#include "testlib.h"

#include <glib.h>
#include <glib/gstdio.h>

#include "libglnx/libglnx.h"

char *
assert_mkdtemp (char *tmpl)
{
  char *ret = g_mkdtemp (tmpl);

  if (ret == NULL)
    g_error ("%s", g_strerror (errno));
  else
    g_assert_true (ret == tmpl);

  return ret;
}

char *isolated_test_dir = NULL;

void
isolated_test_dir_global_setup (void)
{
  g_autofree char *cachedir = NULL;
  g_autofree char *configdir = NULL;
  g_autofree char *datadir = NULL;
  g_autofree char *homedir = NULL;
  g_autofree char *runtimedir = NULL;

  isolated_test_dir = g_strdup ("/tmp/flatpak-test-XXXXXX");
  assert_mkdtemp (isolated_test_dir);
  g_test_message ("isolated_test_dir: %s", isolated_test_dir);

  homedir = g_strconcat (isolated_test_dir, "/home", NULL);
  g_assert_no_errno (g_mkdir_with_parents (homedir, S_IRWXU | S_IRWXG | S_IRWXO));

  g_setenv ("HOME", homedir, TRUE);
  g_test_message ("setting HOME=%s", homedir);

  cachedir = g_strconcat (isolated_test_dir, "/home/cache", NULL);
  g_assert_no_errno (g_mkdir_with_parents (cachedir, S_IRWXU | S_IRWXG | S_IRWXO));
  g_setenv ("XDG_CACHE_HOME", cachedir, TRUE);
  g_test_message ("setting XDG_CACHE_HOME=%s", cachedir);

  configdir = g_strconcat (isolated_test_dir, "/home/config", NULL);
  g_assert_no_errno (g_mkdir_with_parents (configdir, S_IRWXU | S_IRWXG | S_IRWXO));
  g_setenv ("XDG_CONFIG_HOME", configdir, TRUE);
  g_test_message ("setting XDG_CONFIG_HOME=%s", configdir);

  datadir = g_strconcat (isolated_test_dir, "/home/share", NULL);
  g_assert_no_errno (g_mkdir_with_parents (datadir, S_IRWXU | S_IRWXG | S_IRWXO));
  g_setenv ("XDG_DATA_HOME", datadir, TRUE);
  g_test_message ("setting XDG_DATA_HOME=%s", datadir);

  runtimedir = g_strconcat (isolated_test_dir, "/runtime", NULL);
  g_assert_no_errno (g_mkdir_with_parents (runtimedir, S_IRWXU));
  g_setenv ("XDG_RUNTIME_DIR", runtimedir, TRUE);
  g_test_message ("setting XDG_RUNTIME_DIR=%s", runtimedir);

  g_reload_user_special_dirs_cache ();

  g_assert_cmpstr (g_get_user_cache_dir (), ==, cachedir);
  g_assert_cmpstr (g_get_user_config_dir (), ==, configdir);
  g_assert_cmpstr (g_get_user_data_dir (), ==, datadir);
  g_assert_cmpstr (g_get_user_runtime_dir (), ==, runtimedir);
}

void
isolated_test_dir_global_teardown (void)
{
  if (g_getenv ("SKIP_TEARDOWN"))
    return;

  glnx_shutil_rm_rf_at (-1, isolated_test_dir, NULL, NULL);
  g_free (isolated_test_dir);
  isolated_test_dir = NULL;
}

static void
replace_tokens (const char *in_path,
                const char *out_path)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GString) buffer = NULL;
  g_autofree char *contents = NULL;
  const char *iter;

  g_file_get_contents (in_path, &contents, NULL, &error);
  g_assert_no_error (error);

  buffer = g_string_new ("");
  iter = contents;

  while (iter[0] != '\0')
    {
      const char *first_at = strchr (iter, '@');
      const char *second_at;

      if (first_at == NULL)
        {
          /* no more @token@s, append [iter..end] and stop */
          g_string_append (buffer, iter);
          break;
        }

      second_at = strchr (first_at + 1, '@');

      if (second_at == NULL)
        g_error ("Unterminated @token@ in %s: %s", in_path, first_at);

      /* append the literal text [iter..first_at - 1], if non-empty */
      if (first_at != iter)
        g_string_append_len (buffer, iter, first_at - iter);

      /* append the replacement for [first_at..second_at] if known */
      if (g_str_has_prefix (first_at, "@testdir@"))
        {
          g_autofree char *testdir = g_test_build_filename (G_TEST_DIST, ".", NULL);

          g_string_append (buffer, testdir);
        }
      else
        {
          g_error ("Unknown @token@ in %s: %.*s",
                   in_path, (int) (second_at - first_at) + 1, first_at);
        }

      /* continue to process [second_at + 1..end] */
      iter = second_at + 1;
    }

  g_file_set_contents (out_path, buffer->str, -1, &error);
  g_assert_no_error (error);
}

void
tests_dbus_daemon_setup (TestsDBusDaemon *self)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *config_arg = NULL;
  g_autofree char *session_conf = NULL;
  g_autofree char *session_conf_in = NULL;
  GInputStream *address_pipe;
  gchar address_buffer[4096] = { 0 };
  char *newline;

  g_return_if_fail (self != NULL);
  g_return_if_fail (self->dbus_daemon == NULL);
  g_return_if_fail (self->dbus_address == NULL);
  g_return_if_fail (self->temp_dir == NULL);

  self->temp_dir = g_dir_make_tmp ("flatpak-test.XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (self->temp_dir);

  session_conf_in = g_test_build_filename (G_TEST_DIST, "session.conf.in", NULL);
  session_conf = g_build_filename (self->temp_dir, "test-bus.conf", NULL);
  replace_tokens (session_conf_in, session_conf);
  config_arg = g_strdup_printf ("--config-file=%s", session_conf);
  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  self->dbus_daemon = g_subprocess_launcher_spawn (launcher, &error,
                                                   "dbus-daemon",
                                                   config_arg,
                                                   "--print-address=1",
                                                   "--nofork",
                                                   NULL);
  g_assert_no_error (error);
  g_assert_nonnull (self->dbus_daemon);

  address_pipe = g_subprocess_get_stdout_pipe (self->dbus_daemon);
  g_assert_nonnull (address_pipe);

  /* Crash if it takes too long to get the address */
  alarm (30);

  while (strchr (address_buffer, '\n') == NULL)
    {
      if (strlen (address_buffer) >= sizeof (address_buffer) - 1)
        g_error ("Read %" G_GSIZE_FORMAT " bytes from dbus-daemon with "
                 "no newline",
                 sizeof (address_buffer) - 1);

      g_input_stream_read (address_pipe,
                           address_buffer + strlen (address_buffer),
                           sizeof (address_buffer) - strlen (address_buffer),
                           NULL, &error);
      g_assert_no_error (error);
    }

  /* Disable alarm */
  alarm (0);

  newline = strchr (address_buffer, '\n');
  g_assert_nonnull (newline);
  *newline = '\0';
  self->dbus_address = g_strdup (address_buffer);
}

void
tests_dbus_daemon_teardown (TestsDBusDaemon *self)
{
  g_autoptr(GError) error = NULL;

  if (self->dbus_daemon != NULL)
    {
      g_subprocess_send_signal (self->dbus_daemon, SIGTERM);
      g_subprocess_wait (self->dbus_daemon, NULL, &error);
      g_assert_no_error (error);
    }

  if (self->temp_dir != NULL)
    {
      glnx_shutil_rm_rf_at (AT_FDCWD, self->temp_dir, NULL, &error);
      g_assert_no_error (error);
    }

  g_clear_object (&self->dbus_daemon);
  g_clear_pointer (&self->dbus_address, g_free);
  g_clear_pointer (&self->temp_dir, g_free);
}
