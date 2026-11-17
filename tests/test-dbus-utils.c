/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 *
 * Copyright © 2020-2026 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <locale.h>

#include <glib.h>

#include "flatpak-dbus-utils-private.h"

#if GLIB_CHECK_VERSION (2, 78, 0)
# define OPEN_PIPE_CLOEXEC O_CLOEXEC
#else
# define OPEN_PIPE_CLOEXEC FD_CLOEXEC
#endif

static void
test_unique_fd_list (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(FlatpakUniqueFDList) unique = NULL;
  int fds[2];
  int fd;

  g_unix_open_pipe (fds, OPEN_PIPE_CLOEXEC, &error);
  g_assert_no_error (error);

  fd_list = g_unix_fd_list_new_from_array (fds, G_N_ELEMENTS (fds));
  g_assert_nonnull (fd_list);

  unique = flatpak_unique_fd_list_new (fd_list, &error);
  g_assert_no_error (error);
  g_assert_nonnull (unique);

  fd = flatpak_unique_fd_list_get (unique, -1, &error);
  g_assert_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
  g_assert_cmpint (fd, <, 0);
  g_clear_error (&error);

  fd = flatpak_unique_fd_list_get (unique, 47, &error);
  g_assert_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
  g_assert_cmpint (fd, <, 0);
  g_clear_error (&error);

  fd = flatpak_unique_fd_list_get (unique, 2, &error);
  g_assert_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
  g_assert_cmpint (fd, <, 0);
  g_clear_error (&error);

  fd = flatpak_unique_fd_list_get (unique, 0, &error);
  g_assert_no_error (error);
  g_assert_cmpint (fd, ==, fds[0]);
  g_clear_error (&error);

  fd = flatpak_unique_fd_list_get (unique, 0, &error);
  g_assert_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
  g_assert_cmpint (fd, <, 0);
  g_clear_error (&error);

  fd = flatpak_unique_fd_list_get (unique, 1, &error);
  g_assert_no_error (error);
  g_assert_cmpint (fd, ==, fds[1]);
  g_clear_error (&error);
}

static void
test_unique_fd_list_dup (void)
{
  GLogLevelFlags old_always_fatal;
  GLogLevelFlags old_fatal_mask;

  if (!g_test_undefined ())
    {
      g_test_skip ("Not exercising programming errors");
      return;
    }

  old_always_fatal = g_log_set_always_fatal (G_LOG_FATAL_MASK);
  old_fatal_mask = g_log_set_fatal_mask ("GLib", G_LOG_FATAL_MASK);

    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GUnixFDList) fd_list = NULL;
      g_autoptr(FlatpakUniqueFDList) unique = NULL;
      int fds[3];

      g_unix_open_pipe (&fds[0], OPEN_PIPE_CLOEXEC, &error);
      g_assert_no_error (error);

      /* This is a programming error. GUnixFDList will close all fds passed
       * to it, therefore it's always incorrect for the list to have duplicates. */
      fds[2] = fds[0];

      fd_list = g_unix_fd_list_new_from_array (fds, G_N_ELEMENTS (fds));
      g_assert_nonnull (fd_list);

      unique = flatpak_unique_fd_list_new (fd_list, &error);
      g_assert_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
      g_test_message ("Non-unique GUnixFDList -> %s", error->message);
      g_assert_null (unique);

      /* Autofreeing fd_list here will cause a critical, which we ignore */
    }

  g_log_set_always_fatal (old_always_fatal);
  g_log_set_fatal_mask ("GLib", old_fatal_mask);
}

int
main (int argc, char *argv[])
{
  setlocale (LC_ALL, "");

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/dbus-utils/unique-fd-list", test_unique_fd_list);
  g_test_add_func ("/dbus-utils/unique-fd-list-dup", test_unique_fd_list_dup);

  return g_test_run ();
}
