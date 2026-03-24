/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2026 Red Hat, Inc.
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "libglnx-config.h"
#include "libglnx.h"
#include <glib.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <err.h>
#include <string.h>

#include "libglnx-testlib.h"

#define GLNX_CHASE_DEBUG_NO_OPENAT2 (1U << 31)
#define GLNX_CHASE_DEBUG_NO_OPEN_TREE (1U << 30)

const char *test_paths[] = {
  "file/baz",
  "file/baz/",
  "file/baz/.",
  "file/baz/../baz",
  "file////baz/..//baz",
  "file////baz/..//../file/baz",
};

static ino_t
get_ino (int fd)
{
  int r;
  struct stat st;

  r = fstatat (fd, "", &st, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
  g_assert_cmpint (r, >=, 0);

  return st.st_ino;
}

static ino_t
path_get_ino (const char *path)
{
  int r;
  struct stat st;

  r = fstatat (AT_FDCWD, path, &st, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW);
  g_assert_cmpint (r, >=, 0);

  return st.st_ino;
}

static char *
get_abspath (int         dfd,
             const char *path)
{
  g_autofree char *proc_fd_path = NULL;
  g_autofree char *abs = NULL;
  g_autoptr(GError) error = NULL;

  proc_fd_path = g_strdup_printf ("/proc/self/fd/%d", dfd);
  abs = glnx_readlinkat_malloc (AT_FDCWD, proc_fd_path, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (abs);

  return g_strdup_printf ("%s/%s", abs, path);
}

static void
check_chase (int             dfd,
             const char     *path,
             GlnxChaseFlags  flags,
             int             expected_ino)
{
  g_autoptr(GError) error = NULL;
  glnx_autofd int chase_fd = -1;

  /* let's try to test the openat2 impl */
  chase_fd = glnx_chaseat (dfd, path, flags, &error);
  g_assert_no_error (error);
  g_assert_cmpint (chase_fd, >=, 0);
  g_assert_cmpint (get_ino (chase_fd), ==, expected_ino);
  g_clear_fd (&chase_fd, NULL);

  /* let's try to test the open_tree impl */
  chase_fd = glnx_chaseat (dfd, path,
                           flags | GLNX_CHASE_DEBUG_NO_OPENAT2,
                           &error);
  g_assert_no_error (error);
  g_assert_cmpint (chase_fd, >=, 0);
  g_assert_cmpint (get_ino (chase_fd), ==, expected_ino);
  g_clear_fd (&chase_fd, NULL);

  /* let's try to test the openat impl */
  chase_fd = glnx_chaseat (dfd, path,
                           flags |
                           GLNX_CHASE_DEBUG_NO_OPENAT2 |
                           GLNX_CHASE_DEBUG_NO_OPEN_TREE,
                           &error);
  g_assert_no_error (error);
  g_assert_cmpint (chase_fd, >=, 0);
  g_assert_cmpint (get_ino (chase_fd), ==, expected_ino);
  g_clear_fd (&chase_fd, NULL);
}

static void
check_chase_error (int             dfd,
                   const char     *path,
                   GlnxChaseFlags  flags,
                   GQuark          err_domain,
                   gint            err_code)
{
  g_autoptr(GError) error = NULL;
  glnx_autofd int chase_fd = -1;

  /* let's try to test the openat2 impl */
  chase_fd = glnx_chaseat (dfd, path, flags, &error);
  g_assert_cmpint (chase_fd, <, 0);
  g_assert_error (error, err_domain, err_code);
  g_clear_error (&error);

  /* let's try to test the open_tree impl */
  chase_fd = glnx_chaseat (dfd, path,
                           flags | GLNX_CHASE_DEBUG_NO_OPENAT2,
                           &error);
  g_assert_cmpint (chase_fd, <, 0);
  g_assert_error (error, err_domain, err_code);
  g_clear_error (&error);

  /* let's try to test the openat impl */
  chase_fd = glnx_chaseat (dfd, path,
                           flags |
                           GLNX_CHASE_DEBUG_NO_OPENAT2 |
                           GLNX_CHASE_DEBUG_NO_OPEN_TREE,
                           &error);
  g_assert_cmpint (chase_fd, <, 0);
  g_assert_error (error, err_domain, err_code);
  g_clear_error (&error);
}

static void
test_chase_relative (void)
{
  g_autoptr(GError) error = NULL;
  glnx_autofd int dfd = -1;
  int expected_ino;

  g_assert_true (glnx_shutil_mkdir_p_at_open (AT_FDCWD, "file/baz", 0755,
                                              &dfd,
                                              NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (dfd, >=, 0);

  expected_ino = get_ino (dfd);

  for (size_t i = 0; i < G_N_ELEMENTS (test_paths); i++)
    check_chase (AT_FDCWD, test_paths[i], 0, expected_ino);

  check_chase_error (AT_FDCWD, "nope", 0, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

static void
test_chase_relative_fd (void)
{
  g_autoptr(GError) error = NULL;
  glnx_autofd int dfd = -1;
  int expected_ino;
  glnx_autofd int cwdfd = -1;

  g_assert_true (glnx_shutil_mkdir_p_at_open (AT_FDCWD, "file/baz", 0755,
                                              &dfd,
                                              NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (dfd, >=, 0);

  expected_ino = get_ino (dfd);

  cwdfd = openat (AT_FDCWD, ".", O_PATH | O_CLOEXEC | O_NOFOLLOW);
  g_assert_cmpint (cwdfd, >=, 0);

  for (size_t i = 0; i < G_N_ELEMENTS (test_paths); i++)
    check_chase (cwdfd, test_paths[i], 0, expected_ino);

  check_chase_error (cwdfd, "nope", 0, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

static void
test_chase_absolute (void)
{
  g_autoptr(GError) error = NULL;
  glnx_autofd int dfd = -1;
  int expected_ino;
  glnx_autofd int cwdfd = -1;
  g_autofree char *proc_fd_path = NULL;
  g_autofree char *cwd_path = NULL;

  g_assert_true (glnx_shutil_mkdir_p_at_open (AT_FDCWD, "file/baz", 0755,
                                              &dfd,
                                              NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (dfd, >=, 0);

  expected_ino = get_ino (dfd);

  cwdfd = openat (AT_FDCWD, ".", O_PATH | O_CLOEXEC | O_NOFOLLOW);
  g_assert_cmpint (cwdfd, >=, 0);

  cwd_path = get_abspath (cwdfd, "");

  for (size_t i = 0; i < G_N_ELEMENTS (test_paths); i++)
    {
      g_autofree char *abspath = NULL;

      abspath = g_strdup_printf ("%s/%s", cwd_path, test_paths[i]);
      check_chase (AT_FDCWD, abspath, 0, expected_ino);
    }

  check_chase_error (AT_FDCWD, "/nope/nope/nope/345298308497623012313243543", 0,
                     G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

static void
test_chase_link (void)
{
  g_autoptr(GError) error = NULL;
  glnx_autofd int dfd = -1;
  int link_ino;
  int target_ino;

  g_assert_true (glnx_shutil_mkdir_p_at_open (AT_FDCWD, "file/baz", 0755,
                                              &dfd,
                                              NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (dfd, >=, 0);

  g_assert_cmpint (symlinkat ("file/baz", AT_FDCWD, "link"), ==, 0);

  target_ino = get_ino (dfd);
  link_ino = path_get_ino ("link");

  check_chase (AT_FDCWD, "link", 0, target_ino);
  check_chase (AT_FDCWD, "link/", 0, target_ino);
  check_chase (AT_FDCWD, "link///", 0, target_ino);
  check_chase (AT_FDCWD, "link/.//.", 0, target_ino);
  check_chase (AT_FDCWD, "link", 0, target_ino);

  check_chase (AT_FDCWD, "link", GLNX_CHASE_NOFOLLOW, link_ino);
  check_chase (AT_FDCWD, "./file/../link", GLNX_CHASE_NOFOLLOW, link_ino);
  check_chase (AT_FDCWD, "link/", GLNX_CHASE_NOFOLLOW, target_ino);
  check_chase (AT_FDCWD, "././link/.", GLNX_CHASE_NOFOLLOW, target_ino);
  check_chase (AT_FDCWD, "link/.//", GLNX_CHASE_NOFOLLOW, target_ino);

  check_chase (AT_FDCWD, "link",
               GLNX_CHASE_NOFOLLOW | GLNX_CHASE_RESOLVE_NO_SYMLINKS,
               link_ino);
  check_chase_error (AT_FDCWD, "link",
                     GLNX_CHASE_RESOLVE_NO_SYMLINKS,
                     G_IO_ERROR, G_IO_ERROR_TOO_MANY_LINKS);
}

static void
test_chase_resolve (void)
{
  g_autoptr(GError) error = NULL;
  glnx_autofd int foo_dfd = -1;
  glnx_autofd int bar_dfd = -1;
  g_autofree char *foo_abspath = NULL;
  int ino;

  g_assert_true (glnx_shutil_mkdir_p_at_open (AT_FDCWD, "foo", 0755,
                                              &foo_dfd,
                                              NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (foo_dfd, >=, 0);

  g_assert_true (glnx_shutil_mkdir_p_at_open (AT_FDCWD, "foo/bar", 0755,
                                              &bar_dfd,
                                              NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (bar_dfd, >=, 0);

  foo_abspath = get_abspath (foo_dfd, "");

  g_assert_cmpint (symlinkat ("..", foo_dfd, "link1"), ==, 0);
  g_assert_cmpint (symlinkat ("bar/../..", foo_dfd, "link2"), ==, 0);
  g_assert_cmpint (symlinkat (foo_abspath, foo_dfd, "link3"), ==, 0);
  g_assert_cmpint (symlinkat ("/bar", foo_dfd, "link4"), ==, 0);
  g_assert_cmpint (symlinkat ("link1/foo", foo_dfd, "link5"), ==, 0);
  g_assert_cmpint (symlinkat ("link7", foo_dfd, "link6"), ==, 0);
  g_assert_cmpint (symlinkat ("link6", foo_dfd, "link7"), ==, 0);

  ino = get_ino (bar_dfd);

  /* A bunch of different ways to get from CWD and foo to bar */
  check_chase (foo_dfd, "./bar", 0, ino);
  check_chase (foo_dfd, "../foo/bar", 0, ino);
  check_chase (foo_dfd, "link1/foo/bar", 0, ino);
  check_chase (AT_FDCWD, "foo/link1/foo/bar", 0, ino);
  check_chase (foo_dfd, "link2/foo/bar", 0, ino);
  check_chase (AT_FDCWD, ".///foo/./link2/foo/bar", 0, ino);
  check_chase (foo_dfd, "link3/bar", 0, ino);
  check_chase (AT_FDCWD, ".///foo/./link3/bar", 0, ino);
  check_chase (foo_dfd, "link5/bar", 0, ino);

  /* check that NO_SYMLINKS works with a component in the middle */
  check_chase_error (AT_FDCWD, "foo/link3/bar",
                     GLNX_CHASE_RESOLVE_NO_SYMLINKS,
                     G_IO_ERROR, G_IO_ERROR_TOO_MANY_LINKS);

  /* link6 points to link 7, points to link6, ... This should error out! */
  check_chase_error (foo_dfd, "link6/bar", 0,
                     G_IO_ERROR, G_IO_ERROR_TOO_MANY_LINKS);

  /* Test with links which never go below the dfd */
  check_chase (AT_FDCWD, "foo/link1/foo/bar",
               GLNX_CHASE_RESOLVE_BENEATH,
               ino);
  check_chase (AT_FDCWD, "foo/link2/foo/bar",
               GLNX_CHASE_RESOLVE_BENEATH,
               ino);
  /* An absolute link is always below the dfd */
  check_chase_error (AT_FDCWD, "foo/link3/foo/bar",
                     GLNX_CHASE_RESOLVE_BENEATH,
                     G_IO_ERROR, G_IO_ERROR_FAILED);

  /* Same, but from foo instead of cwd */
  check_chase_error (foo_dfd, "link1/foo/bar",
                     GLNX_CHASE_RESOLVE_BENEATH,
                     G_IO_ERROR, G_IO_ERROR_FAILED);
  check_chase_error (foo_dfd, "link2/foo/bar",
                     GLNX_CHASE_RESOLVE_BENEATH,
                     G_IO_ERROR, G_IO_ERROR_FAILED);
  check_chase_error (foo_dfd, "link3/foo/bar",
                     GLNX_CHASE_RESOLVE_BENEATH,
                     G_IO_ERROR, G_IO_ERROR_FAILED);

  /* Check that trying to be below the dfd with RESOLVE_IN_ROOT resolves to the
   * dfd itself */
  check_chase (foo_dfd, "link1/bar",
               GLNX_CHASE_RESOLVE_IN_ROOT,
               ino);
  check_chase (foo_dfd, "link2/bar",
               GLNX_CHASE_RESOLVE_IN_ROOT,
               ino);
  /* The absolute link is relative to dfd with RESOLVE_IN_ROOT, so this
   * fails... */
  check_chase_error (foo_dfd, "link3",
                     GLNX_CHASE_RESOLVE_IN_ROOT,
                     G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  /* ... but the link /bar resolves correctly from foo as dfd. */
  check_chase (foo_dfd, "link4",
               GLNX_CHASE_RESOLVE_IN_ROOT,
               ino);
}

static void
test_chase_resolve_in_root_absolute (void)
{
  g_autoptr(GError) error = NULL;
  glnx_autofd int foo_dfd = -1;
  glnx_autofd int bar_dfd = -1;
  glnx_autofd int baz_dfd = -1;

  g_assert_true (glnx_shutil_mkdir_p_at_open (AT_FDCWD, "foo", 0755,
                                              &foo_dfd,
                                              NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (foo_dfd, >=, 0);

  g_assert_true (glnx_shutil_mkdir_p_at_open (AT_FDCWD, "foo/bar", 0755,
                                              &bar_dfd,
                                              NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (bar_dfd, >=, 0);

  g_assert_true (glnx_shutil_mkdir_p_at_open (AT_FDCWD, "foo/bar/baz", 0755,
                                              &baz_dfd,
                                              NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (baz_dfd, >=, 0);

  /* Test the absolute symlink doesn't break tracking of the root level */
  g_assert_cmpint (symlinkat ("/..", baz_dfd, "link1"), ==, 0);

  /* We should not be able to break out of the root! */
  check_chase (bar_dfd, "./baz/link1", GLNX_CHASE_RESOLVE_IN_ROOT, get_ino (bar_dfd));
}

static void
check_chase_and_statxat (int             dfd,
                         const char     *path,
                         GlnxChaseFlags  flags,
                         ino_t           expected_ino,
                         mode_t          expected_type)
{
  g_autoptr(GError) error = NULL;
  glnx_autofd int chase_fd = -1;
  struct glnx_statx stx;

  /* let's try to test the openat2 impl */
  chase_fd = glnx_chase_and_statxat (dfd, path, flags,
                                     GLNX_STATX_TYPE | GLNX_STATX_INO,
                                     &stx, &error);
  g_assert_cmpint (chase_fd, >=, 0);
  g_assert_no_error (error);
  g_assert_cmpint (stx.stx_ino, ==, expected_ino);
  g_assert_cmpint (stx.stx_mode & S_IFMT, ==, expected_type);
  g_clear_fd (&chase_fd, NULL);

  /* let's try to test the open_tree impl */
  chase_fd = glnx_chase_and_statxat (dfd, path,
                                     flags | GLNX_CHASE_DEBUG_NO_OPENAT2,
                                     GLNX_STATX_TYPE | GLNX_STATX_INO,
                                     &stx, &error);
  g_assert_cmpint (chase_fd, >=, 0);
  g_assert_no_error (error);
  g_assert_cmpint (stx.stx_ino, ==, expected_ino);
  g_assert_cmpint (stx.stx_mode & S_IFMT, ==, expected_type);
  g_clear_fd (&chase_fd, NULL);

  /* let's try to test the openat impl */
  chase_fd = glnx_chase_and_statxat (dfd, path,
                                     flags |
                                     GLNX_CHASE_DEBUG_NO_OPENAT2 |
                                     GLNX_CHASE_DEBUG_NO_OPEN_TREE,
                                     GLNX_STATX_TYPE | GLNX_STATX_INO,
                                     &stx, &error);
  g_assert_cmpint (chase_fd, >=, 0);
  g_assert_no_error (error);
  g_assert_cmpint (stx.stx_ino, ==, expected_ino);
  g_assert_cmpint (stx.stx_mode & S_IFMT, ==, expected_type);
  g_clear_fd (&chase_fd, NULL);
}

static void
check_chase_and_statxat_error (int             dfd,
                               const char     *path,
                               GlnxChaseFlags  flags,
                               GQuark          err_domain,
                               gint            err_code)
{
  g_autoptr(GError) error = NULL;
  glnx_autofd int chase_fd = -1;
  struct glnx_statx stx;

  /* let's try to test the openat2 impl */
  chase_fd = glnx_chase_and_statxat (dfd, path, flags,
                                     GLNX_STATX_TYPE | GLNX_STATX_INO,
                                     &stx, &error);
  g_assert_cmpint (chase_fd, <, 0);
  g_assert_error (error, err_domain, err_code);
  g_clear_error (&error);

  /* let's try to test the open_tree impl */
  chase_fd = glnx_chase_and_statxat (dfd, path,
                                     flags | GLNX_CHASE_DEBUG_NO_OPENAT2,
                                     GLNX_STATX_TYPE | GLNX_STATX_INO,
                                     &stx, &error);
  g_assert_cmpint (chase_fd, <, 0);
  g_assert_error (error, err_domain, err_code);
  g_clear_error (&error);

  /* let's try to test the openat impl */
  chase_fd = glnx_chase_and_statxat (dfd, path,
                                     flags |
                                     GLNX_CHASE_DEBUG_NO_OPENAT2 |
                                     GLNX_CHASE_DEBUG_NO_OPEN_TREE,
                                     GLNX_STATX_TYPE | GLNX_STATX_INO,
                                     &stx, &error);
  g_assert_cmpint (chase_fd, <, 0);
  g_assert_error (error, err_domain, err_code);
  g_clear_error (&error);
}

static void
test_chase_and_statxat_basic (void)
{
  g_autoptr(GError) error = NULL;
  glnx_autofd int dfd = -1;
  glnx_autofd int file_fd = -1;
  ino_t expected_ino;

  g_assert_true (glnx_shutil_mkdir_p_at_open (AT_FDCWD, "file/baz", 0755,
                                              &dfd,
                                              NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (dfd, >=, 0);

  expected_ino = get_ino (dfd);

  /* Test with various path forms */
  for (size_t i = 0; i < G_N_ELEMENTS (test_paths); i++)
    check_chase_and_statxat (AT_FDCWD, test_paths[i], 0, expected_ino, S_IFDIR);

  /* Create a regular file and test it */
  file_fd = openat (dfd, "testfile", O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
  g_assert_cmpint (file_fd, >=, 0);
  g_clear_fd (&file_fd, NULL);

  expected_ino = path_get_ino ("file/baz/testfile");
  check_chase_and_statxat (AT_FDCWD, "file/baz/testfile", 0, expected_ino, S_IFREG);

  /* Test error cases */
  check_chase_and_statxat_error (AT_FDCWD, "nope", 0, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

static void
test_chase_and_statxat_symlink (void)
{
  g_autoptr(GError) error = NULL;
  glnx_autofd int dfd = -1;
  glnx_autofd int chase_fd = -1;
  ino_t link_ino;
  ino_t target_ino;
  struct glnx_statx stx;

  g_assert_true (glnx_shutil_mkdir_p_at_open (AT_FDCWD, "file/baz", 0755,
                                              &dfd,
                                              NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (dfd, >=, 0);

  g_assert_cmpint (symlinkat ("file/baz", AT_FDCWD, "fstatlink"), ==, 0);

  target_ino = get_ino (dfd);
  link_ino = path_get_ino ("fstatlink");

  /* Following symlinks should give us the directory */
  check_chase_and_statxat (AT_FDCWD, "fstatlink", 0, target_ino, S_IFDIR);
  check_chase_and_statxat (AT_FDCWD, "fstatlink/", 0, target_ino, S_IFDIR);

  /* With NOFOLLOW, we should get the symlink itself */
  check_chase_and_statxat (AT_FDCWD, "fstatlink", GLNX_CHASE_NOFOLLOW, link_ino, S_IFLNK);

  /* Verify we can distinguish between regular files, directories, and symlinks */
  chase_fd = glnx_chase_and_statxat (AT_FDCWD, "fstatlink", GLNX_CHASE_NOFOLLOW,
                                     GLNX_STATX_TYPE | GLNX_STATX_INO,
                                     &stx, &error);
  g_assert_cmpint (chase_fd, >=, 0);
  g_assert_no_error (error);
  g_assert_true (S_ISLNK (stx.stx_mode));
  g_clear_fd (&chase_fd, NULL);

  chase_fd = glnx_chase_and_statxat (AT_FDCWD, "fstatlink", 0,
                                     GLNX_STATX_TYPE | GLNX_STATX_INO,
                                     &stx, &error);
  g_assert_cmpint (chase_fd, >=, 0);
  g_assert_no_error (error);
  g_assert_true (S_ISDIR (stx.stx_mode));
  g_clear_fd (&chase_fd, NULL);

  /* Test with RESOLVE_NO_SYMLINKS */
  check_chase_and_statxat_error (AT_FDCWD, "fstatlink",
                                 GLNX_CHASE_RESOLVE_NO_SYMLINKS,
                                 G_IO_ERROR, G_IO_ERROR_TOO_MANY_LINKS);
}

static void
test_chase_and_statxat_permissions (void)
{
  g_autoptr(GError) error = NULL;
  glnx_autofd int dfd = -1;
  glnx_autofd int file_fd = -1;
  glnx_autofd int chase_fd = -1;
  struct glnx_statx stx;
  mode_t expected_mode = 0640;

  g_assert_true (glnx_shutil_mkdir_p_at_open (AT_FDCWD, "permtest", 0755,
                                              &dfd,
                                              NULL, &error));
  g_assert_no_error (error);

  /* Create a file with specific permissions */
  file_fd = openat (dfd, "testfile", O_WRONLY | O_CREAT | O_CLOEXEC, expected_mode);
  g_assert_cmpint (file_fd, >=, 0);
  g_clear_fd (&file_fd, NULL);

  /* Verify that glnx_chase_and_statxat returns the correct permissions */
  chase_fd = glnx_chase_and_statxat (dfd, "testfile", 0,
                                     GLNX_STATX_TYPE | GLNX_STATX_MODE,
                                     &stx, &error);
  g_assert_cmpint (chase_fd, >=, 0);
  g_assert_no_error (error);
  g_assert_cmpint (stx.stx_mode & 0777, ==, expected_mode);
  g_assert_true (S_ISREG (stx.stx_mode));
  g_clear_fd (&chase_fd, NULL);
}

int main (int argc, char **argv)
{
  _GLNX_TEST_SCOPED_TEMP_DIR;
  int ret;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/chase-relative", test_chase_relative);
  g_test_add_func ("/chase-relative-fd", test_chase_relative_fd);
  g_test_add_func ("/chase-absolute", test_chase_absolute);
  g_test_add_func ("/chase-link", test_chase_link);
  g_test_add_func ("/chase-resolve", test_chase_resolve);
  g_test_add_func ("/chase-resolve-in-root-absolute", test_chase_resolve_in_root_absolute);
  g_test_add_func ("/chase-and-statxat-basic", test_chase_and_statxat_basic);
  g_test_add_func ("/chase-and-statxat-symlink", test_chase_and_statxat_symlink);
  g_test_add_func ("/chase-and-statxat-permissions", test_chase_and_statxat_permissions);

  ret = g_test_run();

  return ret;
}
