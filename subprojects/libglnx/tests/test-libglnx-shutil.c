/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright © 2017-2019 Endless OS Foundation LLC
 * Copyright © 2024 Collabora Ltd.
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

static void
test_mkdir_p_parent_unsuitable (void)
{
  _GLNX_TEST_SCOPED_TEMP_DIR;
  _GLNX_TEST_DECLARE_ERROR(local_error, error);
  glnx_autofd int dfd = -1;

  if (!glnx_ensure_dir (AT_FDCWD, "test", 0755, error))
    return;
  if (!glnx_opendirat (AT_FDCWD, "test", FALSE, &dfd, error))
    return;

  if (!glnx_file_replace_contents_at (dfd, "file",
                                      (const guint8 *) "", 0,
                                      GLNX_FILE_REPLACE_NODATASYNC,
                                      NULL, error))
    return;

  if (symlinkat ("nosuchtarget", dfd, "link") < 0)
    {
      glnx_throw_errno_prefix (error, "symlinkat");
      return;
    }

  glnx_shutil_mkdir_p_at (dfd, "file/baz", 0755, NULL, error);
  g_test_message ("mkdir %s -> %s", "file/baz",
                  local_error ? local_error->message : "success");
  g_assert_error (local_error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY);
  g_clear_error (&local_error);

  glnx_shutil_mkdir_p_at (dfd, "link/baz", 0755, NULL, error);
  g_test_message ("mkdir %s -> %s", "link/baz",
                  local_error ? local_error->message : "success");
  g_assert_error (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&local_error);
}

static void
test_mkdir_p_enoent (void)
{
  _GLNX_TEST_SCOPED_TEMP_DIR;
  _GLNX_TEST_DECLARE_ERROR(local_error, error);
  glnx_autofd int dfd = -1;

  if (!glnx_ensure_dir (AT_FDCWD, "test", 0755, error))
    return;
  if (!glnx_opendirat (AT_FDCWD, "test", FALSE, &dfd, error))
    return;
  if (rmdir ("test") < 0)
    return (void) glnx_throw_errno_prefix (error, "rmdir(%s)", "test");

  /* This should fail with ENOENT. */
  glnx_shutil_mkdir_p_at (dfd, "blah/baz", 0755, NULL, error);
  g_assert_error (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&local_error);
}

int
main (int    argc,
      char **argv)
{
  int ret;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/mkdir-p/enoent", test_mkdir_p_enoent);
  g_test_add_func ("/mkdir-p/parent-unsuitable", test_mkdir_p_parent_unsuitable);

  ret = g_test_run();

  return ret;
}
