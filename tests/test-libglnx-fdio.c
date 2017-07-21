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
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  glnx_fd_close int srcfd = -1;
  glnx_fd_close int destfd = -1;
  struct stat stbuf;

  if (!renameat_test_setup (&srcfd, &destfd, error))
    goto out;

  if (glnx_renameat2_noreplace (srcfd, "foo", destfd, "bar") == 0)
    g_assert_not_reached ();
  else
    {
      g_assert_cmpint (errno, ==, EEXIST);
    }

  if (glnx_renameat2_noreplace (srcfd, "foo", destfd, "baz") < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }
  if (!glnx_fstatat (destfd, "bar", &stbuf, AT_SYMLINK_NOFOLLOW, error))
    goto out;

  if (fstatat (srcfd, "foo", &stbuf, AT_SYMLINK_NOFOLLOW) == 0)
    g_assert_not_reached ();
  else
    g_assert_cmpint (errno, ==, ENOENT);

 out:
  g_assert_no_error (local_error);
}

static void
test_renameat2_exchange (void)
{
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  glnx_fd_close int srcfd = -1;
  glnx_fd_close int destfd = -1;
  struct stat stbuf;

  if (!renameat_test_setup (&srcfd, &destfd, error))
    goto out;

  if (glnx_renameat2_exchange (AT_FDCWD, "srcdir", AT_FDCWD, "destdir") < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  /* Ensure the dir fds are the same */
  if (fstatat (srcfd, "foo", &stbuf, AT_SYMLINK_NOFOLLOW) < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }
  if (fstatat (destfd, "bar", &stbuf, AT_SYMLINK_NOFOLLOW) < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }
  /* But the dirs should be swapped */
  if (fstatat (AT_FDCWD, "destdir/foo", &stbuf, AT_SYMLINK_NOFOLLOW) < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }
  if (fstatat (AT_FDCWD, "srcdir/bar", &stbuf, AT_SYMLINK_NOFOLLOW) < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

 out:
  g_assert_no_error (local_error);
}

static void
test_tmpfile (void)
{
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  g_auto(GLnxTmpfile) tmpf = { 0, };

  if (!glnx_open_tmpfile_linkable_at (AT_FDCWD, ".", O_WRONLY|O_CLOEXEC, &tmpf, error))
    goto out;

  if (glnx_loop_write (tmpf.fd, "foo", strlen ("foo")) < 0)
    {
      (void)glnx_throw_errno_prefix (error, "write");
      goto out;
    }

  if (glnx_link_tmpfile_at (&tmpf, GLNX_LINK_TMPFILE_NOREPLACE, AT_FDCWD, "foo", error))
    goto out;

 out:
  g_assert_no_error (local_error);
}

static void
test_stdio_file (void)
{
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  g_auto(GLnxTmpfile) tmpf = { 0, };
  g_autoptr(FILE) f = NULL;

  if (!glnx_open_anonymous_tmpfile (O_RDWR|O_CLOEXEC, &tmpf, error))
    goto out;
  f = fdopen (tmpf.fd, "w");
  if (!f)
    {
      (void)glnx_throw_errno_prefix (error, "fdopen");
      goto out;
    }

  if (fwrite ("hello", 1, strlen ("hello"), f) != strlen ("hello"))
    {
      (void)glnx_throw_errno_prefix (error, "fwrite");
      goto out;
    }
  if (!glnx_stdio_file_flush (f, error))
    goto out;

 out:
  g_assert_no_error (local_error);
}

int main (int argc, char **argv)
{
  int ret;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/tmpfile", test_tmpfile);
  g_test_add_func ("/stdio-file", test_stdio_file);
  g_test_add_func ("/renameat2-noreplace", test_renameat2_noreplace);
  g_test_add_func ("/renameat2-exchange", test_renameat2_exchange);

  ret = g_test_run();

  return ret;
}
