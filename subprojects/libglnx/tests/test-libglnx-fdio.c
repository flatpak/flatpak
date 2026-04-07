/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Red Hat, Inc.
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "libglnx-config.h"
#include "libglnx.h"
#include <glib.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <err.h>
#include <string.h>

#include "libglnx-testlib.h"

static gboolean
renameat_test_setup (int *out_srcfd, int *out_destfd,
                     GError **error)
{
  glnx_autofd int srcfd = -1;
  glnx_autofd int destfd = -1;

  (void) glnx_shutil_rm_rf_at (AT_FDCWD, "srcdir", NULL, NULL);
  if (mkdir ("srcdir", 0755) < 0)
    err (1, "mkdir");
  if (!glnx_opendirat (AT_FDCWD, "srcdir", TRUE, &srcfd, error))
    return FALSE;
  (void) glnx_shutil_rm_rf_at (AT_FDCWD, "destdir", NULL, NULL);
  if (mkdir ("destdir", 0755) < 0)
    err (1, "mkdir");
  if (!glnx_opendirat (AT_FDCWD, "destdir", TRUE, &destfd, error))
    return FALSE;

  if (!glnx_file_replace_contents_at (srcfd, "foo", (guint8*)"foo contents", strlen ("foo contents"),
                                      GLNX_FILE_REPLACE_NODATASYNC, NULL, error))
    return FALSE;
  if (!glnx_file_replace_contents_at (destfd, "bar", (guint8*)"bar contents", strlen ("bar contents"),
                                      GLNX_FILE_REPLACE_NODATASYNC, NULL, error))
    return FALSE;

  *out_srcfd = srcfd; srcfd = -1;
  *out_destfd = destfd; destfd = -1;
  return TRUE;
}

static void
test_renameat2_noreplace (void)
{
  _GLNX_TEST_DECLARE_ERROR(local_error, error);
  glnx_autofd int srcfd = -1;
  glnx_autofd int destfd = -1;
  struct stat stbuf;

  if (!renameat_test_setup (&srcfd, &destfd, error))
    return;

  if (glnx_renameat2_noreplace (srcfd, "foo", destfd, "bar") == 0)
    g_assert_not_reached ();
  else
    {
      g_assert_cmpint (errno, ==, EEXIST);
    }

  if (glnx_renameat2_noreplace (srcfd, "foo", destfd, "baz") < 0)
    return (void)glnx_throw_errno_prefix (error, "renameat");
  if (!glnx_fstatat (destfd, "bar", &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return;

  if (fstatat (srcfd, "foo", &stbuf, AT_SYMLINK_NOFOLLOW) == 0)
    g_assert_not_reached ();
  else
    g_assert_cmpint (errno, ==, ENOENT);
}

static void
test_renameat2_exchange (void)
{
  _GLNX_TEST_DECLARE_ERROR(local_error, error);

  glnx_autofd int srcfd = -1;
  glnx_autofd int destfd = -1;
  if (!renameat_test_setup (&srcfd, &destfd, error))
    return;

  if (glnx_renameat2_exchange (AT_FDCWD, "srcdir", AT_FDCWD, "destdir") < 0)
    return (void)glnx_throw_errno_prefix (error, "renameat");

  /* Ensure the dir fds are the same */
  struct stat stbuf;
  if (!glnx_fstatat (srcfd, "foo", &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return;
  if (!glnx_fstatat (destfd, "bar", &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return;
  /* But the dirs should be swapped */
  if (!glnx_fstatat (AT_FDCWD, "destdir/foo", &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return;
  if (!glnx_fstatat (AT_FDCWD, "srcdir/bar", &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return;
}

static void
test_tmpfile (void)
{
  _GLNX_TEST_DECLARE_ERROR(local_error, error);

  g_auto(GLnxTmpfile) tmpf = { 0, };
  if (!glnx_open_tmpfile_linkable_at (AT_FDCWD, ".", O_WRONLY|O_CLOEXEC, &tmpf, error))
    return;
  if (glnx_loop_write (tmpf.fd, "foo", strlen ("foo")) < 0)
    return (void)glnx_throw_errno_prefix (error, "write");
  if (glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_NOREPLACE, AT_FDCWD, "foo", error))
    return;
}

static void
test_stdio_file (void)
{
  _GLNX_TEST_DECLARE_ERROR(local_error, error);
  g_auto(GLnxTmpfile) tmpf = { 0, };
  g_autoptr(FILE) f = NULL;

  if (!glnx_open_anonymous_tmpfile (O_RDWR|O_CLOEXEC, &tmpf, error))
    return;
  f = fdopen (tmpf.fd, "w");
  tmpf.fd = -1; /* Ownership was transferred via fdopen() */
  if (!f)
    return (void)glnx_throw_errno_prefix (error, "fdopen");
  if (fwrite ("hello", 1, strlen ("hello"), f) != strlen ("hello"))
    return (void)glnx_throw_errno_prefix (error, "fwrite");
  if (!glnx_stdio_file_flush (f, error))
    return;
}

static void
test_fstatat (void)
{
  _GLNX_TEST_DECLARE_ERROR(local_error, error);
  struct stat stbuf = { 0, };

  if (!glnx_fstatat_allow_noent (AT_FDCWD, ".", &stbuf, 0, error))
    return;
  g_assert_cmpint (errno, ==, 0);
  g_assert_no_error (local_error);
  g_assert (S_ISDIR (stbuf.st_mode));
  if (!glnx_fstatat_allow_noent (AT_FDCWD, "nosuchfile", &stbuf, 0, error))
    return;
  g_assert_cmpint (errno, ==, ENOENT);
  g_assert_no_error (local_error);

  /* test NULL parameter for stat */
  if (!glnx_fstatat_allow_noent (AT_FDCWD, ".", NULL, 0, error))
    return;
  g_assert_cmpint (errno, ==, 0);
  g_assert_no_error (local_error);
  if (!glnx_fstatat_allow_noent (AT_FDCWD, "nosuchfile", NULL, 0, error))
    return;
  g_assert_cmpint (errno, ==, ENOENT);
  g_assert_no_error (local_error);
}

static void
test_filecopy (void)
{
  _GLNX_TEST_DECLARE_ERROR(local_error, error);
  const char foo[] = "foo";
  struct stat stbuf;

  if (!glnx_ensure_dir (AT_FDCWD, "subdir", 0755, error))
    return;

  if (!glnx_file_replace_contents_at (AT_FDCWD, foo, (guint8*)foo, sizeof (foo),
                                      GLNX_FILE_REPLACE_NODATASYNC, NULL, error))
    return;

  /* Copy it into both the same dir and a subdir */
  if (!glnx_file_copy_at (AT_FDCWD, foo, NULL, AT_FDCWD, "bar",
                          GLNX_FILE_COPY_NOXATTRS, NULL, error))
    return;
  if (!glnx_file_copy_at (AT_FDCWD, foo, NULL, AT_FDCWD, "subdir/bar",
                          GLNX_FILE_COPY_NOXATTRS, NULL, error))
    return;
  if (!glnx_fstatat (AT_FDCWD, "subdir/bar", &stbuf, 0, error))
    return;

  if (glnx_file_copy_at (AT_FDCWD, foo, NULL, AT_FDCWD, "bar",
                         GLNX_FILE_COPY_NOXATTRS, NULL, error))
    g_assert_not_reached ();
  g_assert_error (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error (&local_error);

  if (!glnx_file_copy_at (AT_FDCWD, foo, NULL, AT_FDCWD, "bar",
                          GLNX_FILE_COPY_NOXATTRS | GLNX_FILE_COPY_OVERWRITE,
                          NULL, error))
    return;

  if (symlinkat ("nosuchtarget", AT_FDCWD, "link") < 0)
    return (void) glnx_throw_errno_prefix (error, "symlinkat");

  /* Shouldn't be able to overwrite a symlink without GLNX_FILE_COPY_OVERWRITE */
  if (glnx_file_copy_at (AT_FDCWD, foo, NULL, AT_FDCWD, "link",
                         GLNX_FILE_COPY_NOXATTRS,
                         NULL, error))
    g_assert_not_reached ();
  g_assert_error (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error (&local_error);

  /* Test overwriting symlink */
  if (!glnx_file_copy_at (AT_FDCWD, foo, NULL, AT_FDCWD, "link",
                          GLNX_FILE_COPY_NOXATTRS | GLNX_FILE_COPY_OVERWRITE,
                          NULL, error))
    return;

  if (!glnx_fstatat_allow_noent (AT_FDCWD, "nosuchtarget", &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return;
  g_assert_cmpint (errno, ==, ENOENT);
  g_assert_no_error (local_error);

  if (!glnx_fstatat (AT_FDCWD, "link", &stbuf, AT_SYMLINK_NOFOLLOW, error))
    return;
  g_assert (S_ISREG (stbuf.st_mode));
}

static void
test_filecopy_procfs (void)
{
  const char * const pseudo_files[] =
  {
    /* A file in /proc that stat()s as empty (at least on Linux 5.15) */
    "/proc/version",
    /* A file in /sys that stat()s as empty (at least on Linux 5.15) */
    "/sys/fs/cgroup/cgroup.controllers",
    /* A file in /sys that stat()s as non-empty (at least on Linux 5.15) */
    "/sys/fs/ext4/features/meta_bg_resize",
  };
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (pseudo_files); i++)
    {
      _GLNX_TEST_DECLARE_ERROR(local_error, error);
      g_autofree char *contents = NULL;
      g_autofree char *contents_of_copy = NULL;
      gsize len;
      gsize len_copy;

      if (!g_file_get_contents (pseudo_files[i], &contents, &len, error))
        {
          g_test_message ("Not testing %s: %s",
                          pseudo_files[i], local_error->message);
          g_clear_error (&local_error);
          continue;
        }

      if (!glnx_file_copy_at (AT_FDCWD, pseudo_files[i], NULL,
                              AT_FDCWD, "copy",
                              (GLNX_FILE_COPY_OVERWRITE |
                               GLNX_FILE_COPY_NOCHOWN |
                               GLNX_FILE_COPY_NOXATTRS),
                              NULL, error))
        return;

      g_assert_no_error (local_error);

      if (!g_file_get_contents ("copy", &contents_of_copy, &len_copy, error))
        return;

      g_assert_no_error (local_error);

      g_assert_cmpstr (contents, ==, contents_of_copy);
      g_assert_cmpuint (len, ==, len_copy);
    }
}

static void
test_fd_reopen (void)
{
  g_autoptr(GError) error = NULL;
  glnx_autofd int dfd = -1;
  glnx_autofd int opath_fd = -1;
  glnx_autofd int regular_fd = -1;
  glnx_autofd int testfile_fd = -1;
  glnx_autofd int link_opath_fd = -1;
  glnx_autofd int reopened_fd = -1;
  struct stat st1, st2;
  const char *test_data = "test content";
  char buf[100];
  ssize_t n;
  gboolean ok;
  int flags;

  /* Create a test directory and file */
  ok = glnx_shutil_mkdir_p_at_open (AT_FDCWD, "reopen_test", 0755, &dfd, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (ok);
  g_assert_no_errno (dfd);

  glnx_file_replace_contents_at (dfd, "testfile",
                                 (const void *) test_data, strlen (test_data),
                                 GLNX_FILE_REPLACE_NODATASYNC, NULL, &error);
  g_assert_no_error (error);

  /* Test 1: Reopen O_PATH fd as regular fd for reading and writing */
  opath_fd = openat (dfd, "testfile", O_PATH | O_CLOEXEC);
  g_assert_no_errno (opath_fd);

  regular_fd = glnx_fd_reopen (opath_fd, O_RDWR, &error);
  g_assert_no_errno (regular_fd);
  g_assert_no_error (error);

  flags = fcntl (regular_fd, F_GETFL);
  g_assert_no_errno (flags);
  g_assert_cmpint (flags & (O_RDONLY | O_WRONLY | O_RDWR), ==, O_RDWR);
  g_assert_cmpint (flags & (O_PATH | O_DIRECTORY | O_NOFOLLOW), ==, 0);
  flags = fcntl (regular_fd, F_GETFD);
  g_assert_no_errno (flags);
  g_assert_cmpint (flags & FD_CLOEXEC, ==, FD_CLOEXEC);

  /* Verify we can read from the reopened fd */
  n = read (regular_fd, buf, sizeof (buf));
  g_assert_cmpmem (buf, n, test_data, strlen (test_data));

  g_clear_fd (&regular_fd, NULL);
  g_clear_fd (&opath_fd, NULL);

  /* Test 2: Reopen directory fd with O_DIRECTORY */
  opath_fd = openat (AT_FDCWD, "reopen_test", O_PATH | O_CLOEXEC);
  g_assert_no_errno (opath_fd);

  reopened_fd = glnx_fd_reopen (opath_fd, O_RDONLY | O_DIRECTORY, &error);
  g_assert_no_error (error);
  g_assert_no_errno (reopened_fd);

  flags = fcntl (reopened_fd, F_GETFL);
  g_assert_no_errno (flags);
  g_assert_cmpint (flags & (O_RDONLY | O_WRONLY | O_RDWR), ==, O_RDONLY);
  g_assert_cmpint (flags & (O_PATH | O_DIRECTORY | O_NOFOLLOW), ==, O_DIRECTORY);
  flags = fcntl (reopened_fd, F_GETFD);
  g_assert_no_errno (flags);
  g_assert_cmpint (flags & FD_CLOEXEC, ==, FD_CLOEXEC);

  /* Verify both fds point to the same inode */
  g_assert_no_errno (fstat (opath_fd, &st1));
  g_assert_no_errno (fstat (reopened_fd, &st2));
  g_assert_cmpint (st1.st_ino, ==, st2.st_ino);

  g_clear_fd (&reopened_fd, NULL);
  g_clear_fd (&opath_fd, NULL);

  /* Test 3: Reopen AT_FDCWD */
  reopened_fd = glnx_fd_reopen (AT_FDCWD, O_RDONLY | O_DIRECTORY, &error);
  g_assert_no_error (error);
  g_assert_no_errno (reopened_fd);

  g_clear_fd (&reopened_fd, NULL);

  /* Test 4: Test that O_NOFOLLOW is rejected */
  opath_fd = openat (dfd, "testfile", O_PATH | O_CLOEXEC);
  g_assert_no_errno (opath_fd);

  regular_fd = glnx_fd_reopen (opath_fd, O_RDONLY | O_NOFOLLOW, &error);
  g_assert_cmpint (regular_fd, <, 0);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_TOO_MANY_LINKS);
  g_clear_error (&error);

  g_clear_fd (&opath_fd, NULL);

  /* Test 5: Reopen O_PATH fd to symlink with O_PATH (should work) */
  g_assert_no_errno (symlinkat ("testfile", dfd, "testlink"));

  link_opath_fd = openat (dfd, "testlink", O_PATH | O_NOFOLLOW);
  g_assert_no_errno (link_opath_fd);

  /* Verify it's a symlink */
  g_assert_no_errno (fstatat (link_opath_fd, "", &st1, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW));
  g_assert_true (S_ISLNK (st1.st_mode));

  /* Reopen with O_PATH should work */
  reopened_fd = glnx_fd_reopen (link_opath_fd, O_PATH, &error);
  g_assert_no_error (error);
  g_assert_no_errno (reopened_fd);

  flags = fcntl (reopened_fd, F_GETFL);
  g_assert_no_errno (flags);
  g_assert_cmpint (flags & (O_RDONLY | O_WRONLY | O_RDWR), ==, O_RDONLY);
  g_assert_cmpint (flags & (O_PATH | O_DIRECTORY | O_NOFOLLOW), ==, O_PATH);
  flags = fcntl (reopened_fd, F_GETFD);
  g_assert_no_errno (flags);
  g_assert_cmpint (flags & FD_CLOEXEC, ==, FD_CLOEXEC);

  /* Verify both point to the same symlink */
  g_assert_no_errno (fstatat (reopened_fd, "", &st2, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW));
  g_assert_cmpint (st1.st_ino, ==, st2.st_ino);
  g_assert_true (S_ISLNK (st2.st_mode));

  g_clear_fd (&reopened_fd, NULL);

  /* Test 6: Reopening O_PATH fd to symlink without O_PATH should fail with ELOOP */
  reopened_fd = glnx_fd_reopen (link_opath_fd, O_RDONLY, &error);
  g_assert_cmpint (reopened_fd, <, 0);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_TOO_MANY_LINKS);
  g_clear_error (&error);

  g_clear_fd (&link_opath_fd, NULL);

  /* Test 7: Verify read index is reset */
  testfile_fd = openat (dfd, "testfile", O_RDONLY | O_CLOEXEC);
  g_assert_no_errno (testfile_fd);

  /* Read some data to advance the read index */
  n = read (testfile_fd, buf, 4);
  g_assert_cmpint (n, ==, 4);

  /* Reopen should reset the read index */
  reopened_fd = glnx_fd_reopen (testfile_fd, O_RDONLY, &error);
  g_assert_no_error (error);
  g_assert_no_errno (reopened_fd);

  /* Should read from the beginning again */
  n = read (reopened_fd, buf, sizeof (buf));
  g_assert_cmpmem (buf, n, test_data, strlen (test_data));

  g_clear_fd (&reopened_fd, NULL);
  g_clear_fd (&testfile_fd, NULL);
}

int main (int argc, char **argv)
{
  _GLNX_TEST_SCOPED_TEMP_DIR;
  int ret;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/tmpfile", test_tmpfile);
  g_test_add_func ("/stdio-file", test_stdio_file);
  g_test_add_func ("/filecopy", test_filecopy);
  g_test_add_func ("/filecopy-procfs", test_filecopy_procfs);
  g_test_add_func ("/renameat2-noreplace", test_renameat2_noreplace);
  g_test_add_func ("/renameat2-exchange", test_renameat2_exchange);
  g_test_add_func ("/fstat", test_fstatat);
  g_test_add_func ("/fd-reopen", test_fd_reopen);

  ret = g_test_run();

  return ret;
}
