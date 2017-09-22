/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Red Hat, Inc.
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

#include "config.h"
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
  glnx_fd_close int srcfd = -1;
  glnx_fd_close int destfd = -1;

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
  glnx_fd_close int srcfd = -1;
  glnx_fd_close int destfd = -1;
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

  glnx_fd_close int srcfd = -1;
  glnx_fd_close int destfd = -1;
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
}

static void
test_filecopy (void)
{
  _GLNX_TEST_DECLARE_ERROR(local_error, error);
  g_auto(GLnxTmpfile) tmpf = { 0, };
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

int main (int argc, char **argv)
{
  int ret;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/tmpfile", test_tmpfile);
  g_test_add_func ("/stdio-file", test_stdio_file);
  g_test_add_func ("/filecopy", test_filecopy);
  g_test_add_func ("/renameat2-noreplace", test_renameat2_noreplace);
  g_test_add_func ("/renameat2-exchange", test_renameat2_exchange);
  g_test_add_func ("/fstat", test_fstatat);

  ret = g_test_run();

  return ret;
}
