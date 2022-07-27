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
#include "flatpak-run-private.h"

#include "libglnx.h"
#include "tests/libglnx-testlib.h"

#include "testlib.h"

static void
populate_with_files (const char *dir)
{
  static const char * const names[] = { "one", "two", "three" };
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (names); i++)
    {
      g_autoptr(GError) error = NULL;
      g_autofree char *path = g_build_filename (dir, names[i], NULL);

      g_file_set_contents (path, "hello", -1, &error);
      g_assert_no_error (error);
    }
}

static void
test_gc (void)
{
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) instances = NULL;
  g_autofree char *instances_dir = flatpak_instance_get_instances_directory ();
  g_autofree char *apps_dir = flatpak_instance_get_instances_directory ();
  g_autofree char *hold_lock = g_test_build_filename (G_TEST_BUILT, "hold-lock", NULL);
  g_autofree char *alive_app_dir = NULL;
  g_autofree char *alive_app_lock = NULL;
  g_autofree char *alive_app_tmp = NULL;
  g_autofree char *alive_instance_dir = NULL;
  g_autofree char *alive_instance_info = NULL;
  g_autofree char *alive_instance_lock = NULL;
  g_autofree char *alive_dead_instance_dir = NULL;
  g_autofree char *alive_dead_instance_info = NULL;
  g_autofree char *alive_dead_instance_lock = NULL;
  g_autofree char *dead_app_dir = NULL;
  g_autofree char *dead_app_lock = NULL;
  g_autofree char *dead_app_tmp = NULL;
  g_autofree char *dead_instance_dir = NULL;
  g_autofree char *dead_instance_info = NULL;
  g_autofree char *dead_instance_lock = NULL;
  struct utimbuf a_while_ago = {};
  const char *hold_lock_argv[] =
  {
    "<BUILT>/hold-lock",
    "--lock-file",
    "<instance>/.ref",
    "--lock-file",
    "<appID>/.ref",
    NULL
  };
  GPid pid = -1;
  int stdout_fd = -1;
  int wstatus = 0;
  FlatpakInstance *instance;
  struct stat stat_buf;

  /* com.example.Alive has one instance, #1, running.
   * A second instance, #2, was running until recently but has exited. */
  alive_app_dir = g_build_filename (apps_dir, "com.example.Alive", NULL);
  g_assert_no_errno (g_mkdir_with_parents (alive_app_dir, 0700));
  alive_app_tmp = g_build_filename (alive_app_dir, "tmp", NULL);
  g_assert_no_errno (g_mkdir_with_parents (alive_app_tmp, 0700));
  populate_with_files (alive_app_tmp);
  alive_app_lock = g_build_filename (alive_app_dir, ".ref", NULL);
  g_file_set_contents (alive_app_lock, "", 0, &error);
  g_assert_no_error (error);

  alive_instance_dir = g_build_filename (instances_dir, "1", NULL);
  g_assert_no_errno (g_mkdir_with_parents (alive_instance_dir, 0700));
  alive_instance_info = g_build_filename (alive_instance_dir, "info", NULL);
  g_file_set_contents (alive_instance_info,
                       "[" FLATPAK_METADATA_GROUP_APPLICATION "]\n"
                       FLATPAK_METADATA_KEY_NAME "=com.example.Alive\n",
                       -1, &error);
  g_assert_no_error (error);
  alive_instance_lock = g_build_filename (alive_instance_dir, ".ref", NULL);
  g_file_set_contents (alive_instance_lock, "", 0, &error);
  g_assert_no_error (error);

  alive_dead_instance_dir = g_build_filename (instances_dir, "2", NULL);
  g_assert_no_errno (g_mkdir_with_parents (alive_dead_instance_dir, 0700));
  alive_dead_instance_info = g_build_filename (alive_dead_instance_dir, "info", NULL);
  g_file_set_contents (alive_dead_instance_info,
                       "[" FLATPAK_METADATA_GROUP_APPLICATION "]\n"
                       FLATPAK_METADATA_KEY_NAME "=com.example.Alive\n",
                       -1, &error);
  g_assert_no_error (error);
  alive_dead_instance_lock = g_build_filename (alive_dead_instance_dir, ".ref", NULL);
  g_file_set_contents (alive_dead_instance_lock, "", 0, &error);
  g_assert_no_error (error);

  /* This represents the running instance #1. We have to do this
   * out-of-process because the locks we use are process-oriented,
   * so the locks we take during GC would not conflict with locks held
   * by our own process. */
  hold_lock_argv[0] = hold_lock;
  hold_lock_argv[2] = alive_instance_lock;
  hold_lock_argv[4] = alive_app_lock;
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

  /* com.example.Dead has no instances running.
   * Instance #4 was running until recently but has exited. */
  dead_app_dir = g_build_filename (apps_dir, "com.example.Dead", NULL);
  g_assert_no_errno (g_mkdir_with_parents (dead_app_dir, 0700));
  dead_app_tmp = g_build_filename (dead_app_dir, "tmp", NULL);
  g_assert_no_errno (g_mkdir_with_parents (dead_app_tmp, 0700));
  populate_with_files (dead_app_tmp);
  dead_app_lock = g_build_filename (dead_app_dir, ".ref", NULL);
  g_file_set_contents (dead_app_lock, "", 0, &error);
  g_assert_no_error (error);

  dead_instance_dir = g_build_filename (instances_dir, "4", NULL);
  g_assert_no_errno (g_mkdir_with_parents (dead_instance_dir, 0700));
  dead_instance_info = g_build_filename (dead_instance_dir, "info", NULL);
  g_file_set_contents (dead_instance_info,
                       "[" FLATPAK_METADATA_GROUP_APPLICATION "]\n"
                       FLATPAK_METADATA_KEY_NAME "=com.example.Dead\n",
                       -1, &error);
  g_assert_no_error (error);
  dead_instance_lock = g_build_filename (dead_instance_dir, ".ref", NULL);
  g_file_set_contents (dead_instance_lock, "", 0, &error);
  g_assert_no_error (error);

  /* Wait for the child to be ready */
  bytes = glnx_fd_readall_bytes (stdout_fd, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (bytes);
  g_assert_cmpuint (g_bytes_get_size (bytes), ==, 0);

  /* Pretend the locks were created in early 1970, to bypass the workaround
   * for a race */
  g_assert_no_errno (g_utime (alive_app_lock, &a_while_ago));
  g_assert_no_errno (g_utime (alive_instance_lock, &a_while_ago));
  g_assert_no_errno (g_utime (alive_dead_instance_lock, &a_while_ago));
  g_assert_no_errno (g_utime (dead_app_lock, &a_while_ago));
  g_assert_no_errno (g_utime (dead_instance_lock, &a_while_ago));

  /* This has the side-effect of GC'ing instances */
  instances = flatpak_instance_get_all ();

  /* We GC exactly those instances that are no longer running */
  g_assert_no_errno (stat (alive_instance_dir, &stat_buf));
  g_assert_cmpint (stat (alive_dead_instance_dir, &stat_buf) == 0 ? 0 : errno, ==, ENOENT);
  g_assert_cmpint (stat (dead_instance_dir, &stat_buf) == 0 ? 0 : errno, ==, ENOENT);

  /* We don't GC the per-app directories themselves, or their lock files */
  g_assert_no_errno (stat (alive_app_dir, &stat_buf));
  g_assert_no_errno (stat (alive_app_lock, &stat_buf));
  g_assert_no_errno (stat (dead_app_dir, &stat_buf));
  g_assert_no_errno (stat (dead_app_lock, &stat_buf));

  /* We GC the tmp subdirectory if there is no instance alive.
   * We do not GC it if there is still an instance holding the lock. */
  g_assert_no_errno (stat (alive_app_tmp, &stat_buf));
  g_assert_cmpint (stat (dead_app_tmp, &stat_buf) == 0 ? 0 : errno, ==, ENOENT);

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

static void
test_claim_per_app_temp_directory (void)
{
  /* Run in a temporary directory so we can create a bunch of symlinks */
  _GLNX_TEST_SCOPED_TEMP_DIR;

  gboolean ok;
  glnx_autofd int lock_fd = -1;
  glnx_autofd int fd = -1;
  g_autofree char *result = NULL;
  g_autofree char *flag_path = NULL;
  g_autofree char *symlink_path = NULL;
  g_autofree char *non_directory_path = NULL;
  g_autofree char *dir_in_tmp = NULL;
  g_autoptr(GError) error = NULL;
  struct stat stat_buf;

  /* In real life this would be the per-app-ID lock, but in fact
   * we just need some sort of file descriptor - as currently
   * implemented, we don't even need to lock it. */
  lock_fd = open ("mock-per-app-id-lock",
                  O_CLOEXEC | O_CREAT | O_NOCTTY | O_NOFOLLOW,
                  0600);
  g_assert_no_errno (lock_fd >= 0 ? 0 : -1);

  /* This emulates the sort of directory that we want to reuse. */
  dir_in_tmp = g_strdup ("/tmp/flatpak-com.example.App-XXXXXX");
  g_assert_nonnull (g_mkdtemp (dir_in_tmp));

  ok = flatpak_instance_claim_per_app_temp_directory ("com.example.App",
                                                      lock_fd,
                                                      AT_FDCWD,
                                                      "doesnt-exist",
                                                      "/tmp",
                                                      &result,
                                                      &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_assert_null (result);
  g_assert_false (ok);
  g_clear_error (&error);

  /* If link_path is a symlink to a directory not in /tmp, we refuse
   * to reuse it */
  g_assert_no_errno (symlink ("/nope", "bad-prefix"));
  ok = flatpak_instance_claim_per_app_temp_directory ("com.example.App",
                                                      lock_fd,
                                                      AT_FDCWD,
                                                      "bad-prefix",
                                                      "/tmp",
                                                      &result,
                                                      &error);
  g_assert_nonnull (error);
  g_assert_cmpstr (error->message, ==, "/nope does not start with /tmp");
  g_assert_null (result);
  g_assert_false (ok);
  g_clear_error (&error);

  /* Similar */
  g_assert_no_errno (symlink ("/tmptation", "bad-prefix2"));
  ok = flatpak_instance_claim_per_app_temp_directory ("com.example.App",
                                                      lock_fd,
                                                      AT_FDCWD,
                                                      "bad-prefix2",
                                                      "/tmp",
                                                      &result,
                                                      &error);
  g_assert_nonnull (error);
  g_assert_cmpstr (error->message, ==, "/tmptation does not start with /tmp/");
  g_assert_null (result);
  g_assert_false (ok);
  g_clear_error (&error);

  /* If link_path points to a subdirectory of /tmp that doesn't match the
   * expected pattern, we refuse to reuse it */
  g_assert_no_errno (symlink ("/tmp/nope", "bad-prefix3"));
  ok = flatpak_instance_claim_per_app_temp_directory ("com.example.App",
                                                      lock_fd,
                                                      AT_FDCWD,
                                                      "bad-prefix3",
                                                      "/tmp",
                                                      &result,
                                                      &error);
  g_assert_nonnull (error);
  g_assert_cmpstr (error->message, ==, "/tmp/nope does not start with /tmp/flatpak-");
  g_assert_null (result);
  g_assert_false (ok);
  g_clear_error (&error);

  /* Similar */
  g_assert_no_errno (symlink ("/tmp/flatpak-/nope", "too-many-levels"));
  ok = flatpak_instance_claim_per_app_temp_directory ("com.example.App",
                                                      lock_fd,
                                                      AT_FDCWD,
                                                      "too-many-levels",
                                                      "/tmp",
                                                      &result,
                                                      &error);
  g_assert_nonnull (error);
  g_assert_cmpstr (error->message, ==,
                   "/tmp/flatpak-/nope has too many directory separators");
  g_assert_null (result);
  g_assert_false (ok);
  g_clear_error (&error);

  /* Similar */
  g_assert_no_errno (symlink ("/tmp/flatpak-abc/", "too-many-levels2"));
  ok = flatpak_instance_claim_per_app_temp_directory ("com.example.App",
                                                      lock_fd,
                                                      AT_FDCWD,
                                                      "too-many-levels2",
                                                      "/tmp",
                                                      &result,
                                                      &error);
  g_assert_nonnull (error);
  g_assert_cmpstr (error->message, ==,
                   "/tmp/flatpak-abc/ has too many directory separators");
  g_assert_null (result);
  g_assert_false (ok);
  g_clear_error (&error);

  g_assert_no_errno (symlink ("/tmp/flatpak-org.example.Other-XXXXXX", "wrong-app"));
  ok = flatpak_instance_claim_per_app_temp_directory ("com.example.App",
                                                      lock_fd,
                                                      AT_FDCWD,
                                                      "wrong-app",
                                                      "/tmp",
                                                      &result,
                                                      &error);
  g_assert_nonnull (error);
  g_assert_cmpstr (error->message, ==,
                   "/tmp/flatpak-org.example.Other-XXXXXX does not "
                   "start with /tmp/flatpak-com.example.App");
  g_assert_null (result);
  g_assert_false (ok);
  g_clear_error (&error);

  g_assert_no_errno (symlink ("/tmp/flatpak-com.example.ApparentlyNot", "wrong-app2"));
  ok = flatpak_instance_claim_per_app_temp_directory ("com.example.App",
                                                      lock_fd,
                                                      AT_FDCWD,
                                                      "wrong-app2",
                                                      "/tmp",
                                                      &result,
                                                      &error);
  g_assert_nonnull (error);
  g_assert_cmpstr (error->message, ==,
                   "/tmp/flatpak-com.example.ApparentlyNot does not "
                   "start with /tmp/flatpak-com.example.App-");
  g_assert_null (result);
  g_assert_false (ok);
  g_clear_error (&error);

  /* If it points to a filesystem object matching the right pattern, but
   * that is not a directory, we refuse to reuse it */
  non_directory_path = g_strdup ("/tmp/flatpak-com.example.App-XXXXXX");
  g_assert_no_errno ((fd = g_mkstemp (non_directory_path)));
  g_assert_no_errno (symlink (non_directory_path, "not-a-directory"));
  ok = flatpak_instance_claim_per_app_temp_directory ("com.example.App",
                                                      lock_fd,
                                                      AT_FDCWD,
                                                      "not-a-directory",
                                                      "/tmp",
                                                      &result,
                                                      &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY);
  g_assert_null (result);
  g_assert_false (ok);
  g_clear_error (&error);

  /* Reuse @non_directory_path as the name of a symlink to a directory:
   * we consider that to be equally invalid. Create it inside our
   * directory in /tmp so that we can rename() it into place,
   * because symlink() does not overwrite, but rename() does. */
  symlink_path = g_build_filename (dir_in_tmp, "symlink", NULL);
  g_assert_no_errno (symlink (dir_in_tmp, symlink_path));
  /* Overwrite the file with the symlink */
  g_assert_no_errno (rename (symlink_path, non_directory_path));

  /* We'll refuse to follow the symlink: for all we know it could be
   * attacker-controlled. */
  ok = flatpak_instance_claim_per_app_temp_directory ("com.example.App",
                                                      lock_fd,
                                                      AT_FDCWD,
                                                      "not-a-directory",
                                                      "/tmp",
                                                      &result,
                                                      &error);

  /* Either of these would be reasonable */
  if (error->code == G_IO_ERROR_TOO_MANY_LINKS)
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_TOO_MANY_LINKS);
  else
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY);

  g_assert_null (result);
  g_assert_false (ok);
  g_clear_error (&error);

  /* If link_path points to a directory owned by someone else, we refuse
   * to use it. This part of the test will be skipped unless you pre-create
   * this directory as root. */
  if (stat ("/tmp/flatpak-com.example.App-OwnedByRoot", &stat_buf) == 0
      && stat_buf.st_uid == 0
      && geteuid () != 0)
    {
      g_assert_no_errno (symlink ("/tmp/flatpak-com.example.App-OwnedByRoot",
                                  "not-our-directory"));
      ok = flatpak_instance_claim_per_app_temp_directory ("com.example.App",
                                                          lock_fd,
                                                          AT_FDCWD,
                                                          "not-our-directory",
                                                          "/tmp",
                                                          &result,
                                                          &error);
      g_assert_nonnull (error);
      g_assert_cmpstr (error->message, ==,
                       "/tmp/flatpak-com.example.App-OwnedByRoot does not "
                       "belong to this user");
      g_assert_null (result);
      g_assert_false (ok);
      g_clear_error (&error);
    }

  glnx_close_fd (&fd);
  g_assert_no_errno (unlink (non_directory_path));
  g_clear_pointer (&non_directory_path, g_free);

  /* Even when we have a symlink to a directory matching the right pattern
   * that we own, if it doesn't contain the flag file that indicates that
   * it's one of our temp directories, we'll still refuse to use it. */
  g_assert_no_errno (symlink (dir_in_tmp, "good-symlink"));
  ok = flatpak_instance_claim_per_app_temp_directory ("com.example.App",
                                                      lock_fd,
                                                      AT_FDCWD,
                                                      "good-symlink",
                                                      "/tmp",
                                                      &result,
                                                      &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_assert_true (g_str_has_prefix (error->message,
                                   "opening flag file /tmp/flatpak-com.example.App-"));
  g_assert_nonnull (strstr (error->message, "/.flatpak-tmpdir:"));
  g_assert_null (result);
  g_assert_false (ok);
  g_clear_error (&error);

  /* Create the flag file (of course in real life this would have happened
   * much sooner) */
  flag_path = g_build_filename (dir_in_tmp, ".flatpak-tmpdir", NULL);
  g_file_set_contents (flag_path, "", 0, &error);
  g_assert_no_error (error);

  /* Now we are finally willing to reuse the directory! A happy ending
   * at last. */
  ok = flatpak_instance_claim_per_app_temp_directory ("com.example.App",
                                                      lock_fd,
                                                      AT_FDCWD,
                                                      "good-symlink",
                                                      "/tmp",
                                                      &result,
                                                      &error);
  g_assert_no_error (error);
  g_assert_cmpstr (result, ==, dir_in_tmp);
  g_assert_true (ok);

  g_assert_no_errno (unlink (flag_path));
}

int
main (int argc, char *argv[])
{
  int res;

  isolated_test_dir_global_setup ();

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/instance/gc", test_gc);
  g_test_add_func ("/instance/claim-per-app-temp-directory",
                   test_claim_per_app_temp_directory);

  res = g_test_run ();

  isolated_test_dir_global_teardown ();

  return res;
}
