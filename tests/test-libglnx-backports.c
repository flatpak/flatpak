/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 1995-1997  Peter Mattis, Spencer Kimball and Josh MacDonald
 * Copyright (C) 2011 Red Hat, Inc.
 * Copyright (C) 2018 Endless OS Foundation, LLC
 * Copyright 2019 Emmanuel Fleury
 * Copyright 2021-2024 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later AND LicenseRef-old-glib-tests
 */

#include "libglnx-config.h"
#include "libglnx.h"

#include <glib/gstdio.h>
#include <glib-unix.h>

#include <sys/wait.h>
#include <unistd.h>

static void
async_signal_safe_message (const char *message)
{
  if (write (2, message, strlen (message)) < 0 ||
      write (2, "\n", 1) < 0)
    {
      /* ignore: not much we can do */
    }
}

static void test_closefrom_subprocess_einval (void);

static void
test_closefrom (void)
{
  /* Enough file descriptors to be confident that we're operating on
   * all of them */
  const int N_FDS = 20;
  int *fds;
  int fd;
  int i;
  pid_t child;
  int wait_status;

  /* The loop that populates @fds with pipes assumes this */
  g_assert (N_FDS % 2 == 0);

  for (fd = 0; fd <= 2; fd++)
    {
      int flags;

      g_assert_no_errno ((flags = fcntl (fd, F_GETFD)));
      g_assert_no_errno (fcntl (fd, F_SETFD, flags & ~FD_CLOEXEC));
    }

  fds = g_new0 (int, N_FDS);

  for (i = 0; i < N_FDS; i += 2)
    {
      GError *error = NULL;
      int pipefd[2];
      int res;

      /* Intentionally neither O_CLOEXEC nor FD_CLOEXEC */
      res = g_unix_open_pipe (pipefd, 0, &error);
      g_assert (res);
      g_assert_no_error (error);
      g_clear_error (&error);
      fds[i] = pipefd[0];
      fds[i + 1] = pipefd[1];
    }

  child = fork ();

  /* Child process exits with status = 100 + the first wrong fd,
   * or 0 if all were correct */
  if (child == 0)
    {
      for (i = 0; i < N_FDS; i++)
        {
          int flags = fcntl (fds[i], F_GETFD);

          if (flags == -1)
            {
              async_signal_safe_message ("fd should not have been closed");
              _exit (100 + fds[i]);
            }

          if (flags & FD_CLOEXEC)
            {
              async_signal_safe_message ("fd should not have been close-on-exec yet");
              _exit (100 + fds[i]);
            }
        }

      g_fdwalk_set_cloexec (3);

      for (i = 0; i < N_FDS; i++)
        {
          int flags = fcntl (fds[i], F_GETFD);

          if (flags == -1)
            {
              async_signal_safe_message ("fd should not have been closed");
              _exit (100 + fds[i]);
            }

          if (!(flags & FD_CLOEXEC))
            {
              async_signal_safe_message ("fd should have been close-on-exec");
              _exit (100 + fds[i]);
            }
        }

      g_closefrom (3);

      for (fd = 0; fd <= 2; fd++)
        {
          int flags = fcntl (fd, F_GETFD);

          if (flags == -1)
            {
              async_signal_safe_message ("fd should not have been closed");
              _exit (100 + fd);
            }

          if (flags & FD_CLOEXEC)
            {
              async_signal_safe_message ("fd should not have been close-on-exec");
              _exit (100 + fd);
            }
        }

      for (i = 0; i < N_FDS; i++)
        {
          if (fcntl (fds[i], F_GETFD) != -1 || errno != EBADF)
            {
              async_signal_safe_message ("fd should have been closed");
              _exit (100 + fds[i]);
            }
        }

      _exit (0);
    }

  g_assert_no_errno (waitpid (child, &wait_status, 0));

  if (WIFEXITED (wait_status))
    {
      int exit_status = WEXITSTATUS (wait_status);

      if (exit_status != 0)
        g_test_fail_printf ("File descriptor %d in incorrect state", exit_status - 100);
    }
  else
    {
      g_test_fail_printf ("Unexpected wait status %d", wait_status);
    }

  for (i = 0; i < N_FDS; i++)
    g_assert_no_errno (close (fds[i]));

  g_free (fds);

  if (g_test_undefined ())
    {
#if GLIB_CHECK_VERSION (2, 38, 0)
      g_test_trap_subprocess ("/glib-unix/closefrom/subprocess/einval",
                              0, G_TEST_SUBPROCESS_DEFAULT);
#else
      if (g_test_trap_fork (0, 0))
        {
          test_closefrom_subprocess_einval ();
          exit (0);
        }

#endif
      g_test_trap_assert_passed ();
    }
}

static void
test_closefrom_subprocess_einval (void)
{
  int res;
  int errsv;

  g_log_set_always_fatal (G_LOG_FATAL_MASK);
  g_log_set_fatal_mask ("GLib", G_LOG_FATAL_MASK);

  errno = 0;
  res = g_closefrom (-1);
  errsv = errno;
  g_assert_cmpint (res, ==, -1);
  g_assert_cmpint (errsv, ==, EINVAL);

  errno = 0;
  res = g_fdwalk_set_cloexec (-42);
  errsv = errno;
  g_assert_cmpint (res, ==, -1);
  g_assert_cmpint (errsv, ==, EINVAL);
}

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

  g_test_add_func ("/glib-unix/closefrom", test_closefrom);
#if GLIB_CHECK_VERSION (2, 38, 0)
  g_test_add_func ("/glib-unix/closefrom/subprocess/einval",
                   test_closefrom_subprocess_einval);
#endif
  g_test_add_func ("/mainloop/steal-fd", test_steal_fd);
  g_test_add_func ("/strfuncs/memdup2", test_memdup2);
  g_test_add_func ("/strfuncs/strv-equal", test_strv_equal);
  return g_test_run();
}
