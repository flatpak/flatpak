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

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utime.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "flatpak.h"
#include "flatpak-instance-private.h"

#include "libglnx/libglnx.h"

#include "testlib.h"

static void
test_gc (void)
{
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) instances = NULL;
  g_autofree char *base_dir = flatpak_instance_get_instances_directory ();
  g_autofree char *alive_dir = g_build_filename (base_dir, "1", NULL);
  g_autofree char *alive_lock = g_build_filename (alive_dir, ".ref", NULL);
  g_autofree char *dead_dir = g_build_filename (base_dir, "2", NULL);
  g_autofree char *dead_lock = g_build_filename (dead_dir, ".ref", NULL);
  g_autofree char *hold_lock = g_test_build_filename (G_TEST_BUILT, "hold-lock", NULL);
  struct utimbuf a_while_ago = {};
  const char *hold_lock_argv[] = { "hold-lock", "--lock-file", ".ref", NULL };
  GPid pid = -1;
  int stdout_fd = -1;
  int wstatus = 0;
  FlatpakInstance *instance;
  struct stat stat_buf;

  g_assert_no_errno (g_mkdir_with_parents (alive_dir, 0700));
  g_assert_no_errno (g_mkdir_with_parents (dead_dir, 0700));
  g_file_set_contents (alive_lock, "", 0, &error);
  g_assert_no_error (error);
  g_file_set_contents (dead_lock, "", 0, &error);
  g_assert_no_error (error);

  hold_lock_argv[0] = hold_lock;
  hold_lock_argv[2] = alive_lock;
  g_spawn_async_with_pipes (NULL,
                            (gchar **) hold_lock_argv,
                            NULL,
                            G_SPAWN_DO_NOT_REAP_CHILD,
                            NULL,
                            NULL,
                            &pid,
                            NULL,
                            &stdout_fd,
                            NULL,
                            &error);
  g_assert_no_error (error);
  g_assert_cmpint (pid, >, 1);
  g_assert_cmpint (stdout_fd, >=, 0);

  /* Wait for the child to be ready */
  bytes = glnx_fd_readall_bytes (stdout_fd, NULL, &error);
  g_assert_no_error (error);

  /* Pretend the locks were created in early 1970, to bypass the workaround
   * for a race */
  g_assert_no_errno (g_utime (alive_lock, &a_while_ago));
  g_assert_no_errno (g_utime (dead_lock, &a_while_ago));

  /* This has the side-effect of GC'ing instances */
  instances = flatpak_instance_get_all ();

  g_assert_no_errno (stat (alive_dir, &stat_buf));
  g_assert_cmpint (stat (dead_dir, &stat_buf) == 0 ? 0 : errno, ==, ENOENT);

  g_assert_cmpuint (instances->len, ==, 1);
  instance = g_ptr_array_index (instances, 0);
  g_assert_true (FLATPAK_IS_INSTANCE (instance));
  g_assert_cmpstr (flatpak_instance_get_id (instance), ==, "1");

  kill (pid, SIGTERM);
  g_assert_no_errno (waitpid (pid, &wstatus, 0));
  g_assert_true (WIFSIGNALED (wstatus));
  g_assert_cmpint (WTERMSIG (wstatus), ==, SIGTERM);
  g_spawn_close_pid (pid);
}

int
main (int argc, char *argv[])
{
  int res;

  isolated_test_dir_global_setup ();

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/instance/gc", test_gc);

  res = g_test_run ();

  isolated_test_dir_global_teardown ();

  return res;
}
