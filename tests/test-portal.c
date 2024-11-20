/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libglnx.h"

#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "portal/flatpak-portal.h"
#include "portal/flatpak-portal-dbus.h"
#include "testlib.h"

typedef struct
{
  TestsDBusDaemon dbus_daemon;
  GSubprocess *portal;
  gchar *portal_path;
  gchar *mock_flatpak;
  PortalFlatpak *proxy;
  GDBusConnection *conn;
} Fixture;

typedef struct
{
  int dummy;
} Config;

static void
setup (Fixture *f,
       gconstpointer context G_GNUC_UNUSED)
{
  g_autoptr(GError) error = NULL;

  tests_dbus_daemon_setup (&f->dbus_daemon);

  f->portal_path = g_strdup (g_getenv ("FLATPAK_PORTAL"));

  if (f->portal_path == NULL)
    f->portal_path = g_strdup (LIBEXECDIR "/flatpak-portal");

  f->mock_flatpak = g_test_build_filename (G_TEST_BUILT, "mock-flatpak", NULL);

  f->conn = g_dbus_connection_new_for_address_sync (f->dbus_daemon.dbus_address,
                                                    (G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                     G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                                                    NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (f->conn);
}

/* Don't corrupt TAP output if portal outputs on stdout */
static void
launcher_stdout_to_our_stderr (GSubprocessLauncher *launcher)
{
  glnx_autofd int stderr_copy = -1;

  stderr_copy = dup (STDERR_FILENO);
  g_assert_no_errno (stderr_copy);
  g_assert_no_errno (fcntl (stderr_copy, F_SETFD, FD_CLOEXEC));
  g_subprocess_launcher_take_stdout_fd (launcher, g_steal_fd (&stderr_copy));
}

static GSubprocessLauncher *
fixture_make_launcher (Fixture *f)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  launcher_stdout_to_our_stderr (launcher);
  g_subprocess_launcher_setenv (launcher,
                                "DBUS_SESSION_BUS_ADDRESS",
                                f->dbus_daemon.dbus_address,
                                TRUE);
  g_subprocess_launcher_setenv (launcher,
                                "FLATPAK_PORTAL_MOCK_FLATPAK",
                                f->mock_flatpak,
                                TRUE);

  return g_steal_pointer (&launcher);
}

static void
name_appeared_cb (GDBusConnection *conn,
                  const char *name,
                  const char *owner,
                  gpointer user_data)
{
  gchar **name_owner_p = user_data;

  g_clear_pointer (name_owner_p, g_free);
  *name_owner_p = g_strdup (owner);
}

static void
name_vanished_cb (GDBusConnection *conn,
                  const char *name,
                  gpointer user_data)
{
  gchar **name_owner_p = user_data;

  g_clear_pointer (name_owner_p, g_free);
  *name_owner_p = g_strdup ("");
}

static void
name_owner_watch_removed_cb (gpointer user_data)
{
  gchar **name_owner_p = user_data;

  g_clear_pointer (name_owner_p, g_free);
}

static void
fixture_wait_for_name_to_be_owned (Fixture *f,
                                   const char *name)
{
  g_autofree gchar *name_owner = NULL;
  guint watch;

  watch = g_bus_watch_name_on_connection (f->conn,
                                          name,
                                          G_BUS_NAME_WATCHER_FLAGS_NONE,
                                          name_appeared_cb,
                                          name_vanished_cb,
                                          &name_owner,
                                          name_owner_watch_removed_cb);

  /* Wait for name to become owned */
  while (name_owner == NULL || name_owner[0] == '\0')
    g_main_context_iteration (NULL, TRUE);

  g_bus_unwatch_name (watch);

  /* Wait for watch to be cleaned up */
  while (name_owner != NULL)
    g_main_context_iteration (NULL, TRUE);
}

static void
fixture_start_portal (Fixture *f)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocessLauncher) launcher = NULL;

  launcher = fixture_make_launcher (f);
  f->portal = g_subprocess_launcher_spawn (launcher, &error,
                                           f->portal_path,
                                           NULL);
  g_assert_no_error (error);
  g_assert_nonnull (f->portal);

  fixture_wait_for_name_to_be_owned (f, FLATPAK_PORTAL_BUS_NAME);

  f->proxy = portal_flatpak_proxy_new_sync (f->conn,
                                            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                            FLATPAK_PORTAL_BUS_NAME,
                                            FLATPAK_PORTAL_PATH,
                                            NULL,
                                            &error);
  g_assert_no_error (error);
  g_assert_nonnull (f->proxy);
}

static void
test_help (Fixture *f,
           gconstpointer context G_GNUC_UNUSED)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autofree gchar *stdout_buf;
  g_autofree gchar *stderr_buf;
  g_autoptr(GError) error = NULL;

  /* Don't use fixture_make_launcher() here because we want to
   * capture stdout */
  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                        G_SUBPROCESS_FLAGS_STDERR_PIPE);
  g_subprocess_launcher_setenv (launcher,
                                "DBUS_SESSION_BUS_ADDRESS",
                                f->dbus_daemon.dbus_address,
                                TRUE);

  f->portal = g_subprocess_launcher_spawn (launcher, &error,
                                           f->portal_path,
                                           "--help",
                                           NULL);
  g_assert_no_error (error);
  g_assert_nonnull (f->portal);

  g_subprocess_communicate_utf8 (f->portal, NULL, NULL, &stdout_buf,
                                 &stderr_buf, &error);
  g_assert_nonnull (stdout_buf);
  g_assert_nonnull (stderr_buf);
  g_test_message ("flatpak-portal --help: %s", stdout_buf);
  g_assert_true (strstr (stdout_buf, "--replace") != NULL);

  g_subprocess_wait_check (f->portal, NULL, &error);
  g_assert_no_error (error);
}

static void
count_successful_exit_cb (PortalFlatpak *proxy,
                          guint pid,
                          guint wait_status,
                          gpointer user_data)
{
  gsize *times_exited_p = user_data;

  g_info ("Process %u exited with wait status %u", pid, wait_status);
  g_assert_true (WIFEXITED (wait_status));
  g_assert_cmpuint (WEXITSTATUS (wait_status), ==, 0);
  (*times_exited_p) += 1;
}

static void
test_basic (Fixture *f,
            gconstpointer context G_GNUC_UNUSED)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GUnixFDList) fds_out = NULL;
  guint pid;
  gboolean ok;
  const char * const argv[] = { "hello", NULL };
  gsize times_exited = 0;
  gulong handler_id;

  fixture_start_portal (f);

  /* We can't easily tell whether EXPOSE_PIDS ought to be set or not */
  g_assert_cmpuint ((portal_flatpak_get_supports (f->proxy) &
                     (~FLATPAK_SPAWN_SUPPORT_FLAGS_EXPOSE_PIDS)), ==, 0);
  g_assert_cmpuint (portal_flatpak_get_version (f->proxy), ==, 7);

  handler_id = g_signal_connect (f->proxy, "spawn-exited",
                                 G_CALLBACK (count_successful_exit_cb),
                                 &times_exited);

  ok = portal_flatpak_call_spawn_sync (f->proxy,
                                       "/",           /* cwd */
                                       argv,          /* argv */
                                       g_variant_new ("a{uh}", NULL),
                                       g_variant_new ("a{ss}", NULL),
                                       FLATPAK_SPAWN_FLAGS_NONE,
                                       g_variant_new ("a{sv}", NULL),
                                       NULL,          /* fd list */
                                       &pid,
                                       &fds_out,
                                       NULL,
                                       &error);
  g_assert_no_error (error);
  g_assert_true (ok);
  g_assert_cmpuint (pid, >, 1);

  while (times_exited == 0)
    g_main_context_iteration (NULL, TRUE);

  g_signal_handler_disconnect (f->proxy, handler_id);

  if (fds_out != NULL)
    g_assert_cmpint (g_unix_fd_list_get_length (fds_out), ==, 0);

  g_subprocess_send_signal (f->portal, SIGTERM);
  g_subprocess_wait (f->portal, NULL, &error);
  g_assert_no_error (error);
}

static void
test_fd_passing (Fixture *f,
                 gconstpointer context G_GNUC_UNUSED)
{
#define SOME_FDS 16
  g_autoptr(GError) error = NULL;
  char *tempfile_paths[SOME_FDS];
  int tempfile_fds[SOME_FDS];
  guint gap_size;
  gsize times_exited = 0;
  gulong handler_id;
  gsize i;

  fixture_start_portal (f);

  handler_id = g_signal_connect (f->proxy, "spawn-exited",
                                 G_CALLBACK (count_successful_exit_cb),
                                 &times_exited);

  for (i = 0; i < SOME_FDS; i++)
    {
      tempfile_paths[i] = g_strdup ("/tmp/flatpak-portal-test.XXXXXX");
      tempfile_fds[i] = g_mkstemp (tempfile_paths[i]);
      g_assert_no_errno (tempfile_fds[i]);
    }

  /* Using a non-contiguous block of fds can help to tickle bugs in the
   * portal. */
  for (gap_size = 0; gap_size < 128; gap_size += 16)
    {
      g_autoptr(GUnixFDList) fds_in = g_unix_fd_list_new ();
      g_autoptr(GUnixFDList) fds_out = NULL;
      g_auto(GVariantBuilder) fd_map_builder = {};
      g_auto(GVariantBuilder) env_builder = {};
      guint pid;
      gboolean ok;
      const char * const argv[] = { "hello", NULL };
      g_autofree char *output = NULL;

      g_variant_builder_init (&fd_map_builder, G_VARIANT_TYPE ("a{uh}"));
      g_variant_builder_init (&env_builder, G_VARIANT_TYPE ("a{ss}"));
      times_exited = 0;

      g_variant_builder_add (&env_builder, "{ss}", "FOO", "bar");

      for (i = 0; i < SOME_FDS; i++)
        {
          int handle = g_unix_fd_list_append (fds_in, tempfile_fds[i], &error);
          guint32 desired_fd;

          g_assert_no_error (error);
          g_assert_cmpint (handle, >=, 0);

          if (i <= STDERR_FILENO)
            desired_fd = i;
          else
            desired_fd = i + gap_size;

          g_variant_builder_add (&fd_map_builder, "{uh}",
                                 desired_fd, (gint32) handle);
        }

      ok = portal_flatpak_call_spawn_sync (f->proxy,
                                           "/",           /* cwd */
                                           argv,          /* argv */
                                           g_variant_builder_end (&fd_map_builder),
                                           g_variant_builder_end (&env_builder),
                                           FLATPAK_SPAWN_FLAGS_NONE,
                                           g_variant_new ("a{sv}", NULL),
                                           fds_in,
                                           &pid,
                                           &fds_out,
                                           NULL,
                                           &error);
      g_assert_no_error (error);
      g_assert_true (ok);
      g_assert_cmpuint (pid, >, 1);

      /* Wait for this one to exit */
      while (times_exited == 0)
        g_main_context_iteration (NULL, TRUE);

      if (fds_out != NULL)
        g_assert_cmpint (g_unix_fd_list_get_length (fds_out), ==, 0);

      /* stdout from the portal should have ended up in temp file [1] */
      g_assert_no_errno (lseek (tempfile_fds[1], 0, SEEK_SET));
      output = glnx_fd_readall_utf8 (tempfile_fds[1], NULL, NULL, &error);
      g_assert_no_error (error);
      g_assert_nonnull (output);
      g_assert_no_errno (lseek (tempfile_fds[1], 0, SEEK_SET));
      g_assert_no_errno (ftruncate (tempfile_fds[1], 0));
      g_test_message ("Output from mock Flatpak: %s", output);

      if (strstr (output, "env[FOO] = bar") != NULL)
        g_test_message ("Found env[FOO] = bar in output");
      else
        g_error ("env[FOO] = bar not found in \"%s\"", output);

      for (i = 0; i < SOME_FDS; i++)
        {
          struct stat stat_buf;
          g_autofree char *expected = NULL;
          int desired_fd;

          g_assert_no_errno (fstat (tempfile_fds[i], &stat_buf));

          if (i <= STDERR_FILENO)
            desired_fd = i;
          else
            desired_fd = i + gap_size;

          expected = g_strdup_printf ("fd[%d] = (dev=%" G_GUINT64_FORMAT " ino=%" G_GUINT64_FORMAT ")",
                                      desired_fd,
                                      (guint64) stat_buf.st_dev,
                                      (guint64) stat_buf.st_ino);

          if (strstr (output, expected) != NULL)
            g_test_message ("fd %d OK: \"%s\"", desired_fd, expected);
          else
            g_error ("\"%s\" not found in \"%s\"", expected, output);
        }
    }

  g_signal_handler_disconnect (f->proxy, handler_id);

  g_subprocess_send_signal (f->portal, SIGTERM);
  g_subprocess_wait (f->portal, NULL, &error);
  g_assert_no_error (error);

  for (i = 0; i < SOME_FDS; i++)
    {
      g_assert_no_errno (unlink (tempfile_paths[i]));
      glnx_close_fd (&tempfile_fds[i]);
      g_free (tempfile_paths[i]);
    }
}

static void
test_replace (Fixture *f,
              gconstpointer context G_GNUC_UNUSED)
{
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocess) gets_replaced = NULL;

  /* Not using fixture_start_portal() here because we want to --replace */
  launcher = fixture_make_launcher (f);
  gets_replaced = g_subprocess_launcher_spawn (launcher, &error,
                                               f->portal_path,
                                               "--replace",
                                               NULL);
  g_assert_no_error (error);
  g_assert_nonnull (gets_replaced);

  fixture_wait_for_name_to_be_owned (f, FLATPAK_PORTAL_BUS_NAME);

  g_clear_object (&launcher);
  launcher = fixture_make_launcher (f);
  f->portal = g_subprocess_launcher_spawn (launcher, &error,
                                           f->portal_path,
                                           "--replace",
                                           NULL);
  g_assert_no_error (error);
  g_assert_nonnull (f->portal);

  /* f->portal replaces gets_replaced, which exits 0 */
  g_subprocess_wait_check (gets_replaced, NULL, &error);
  g_assert_no_error (error);

  g_subprocess_send_signal (f->portal, SIGTERM);
  g_subprocess_wait (f->portal, NULL, &error);
  g_assert_no_error (error);
}

static void
teardown (Fixture *f,
          gconstpointer context G_GNUC_UNUSED)
{
  tests_dbus_daemon_teardown (&f->dbus_daemon);
  g_clear_object (&f->portal);
  g_free (f->portal_path);
  g_free (f->mock_flatpak);
}

int
main (int argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add ("/help", Fixture, NULL, setup, test_help, teardown);
  g_test_add ("/basic", Fixture, NULL, setup, test_basic, teardown);
  g_test_add ("/fd-passing", Fixture, NULL, setup, test_fd_passing, teardown);
  g_test_add ("/replace", Fixture, NULL, setup, test_replace, teardown);

  return g_test_run ();
}
