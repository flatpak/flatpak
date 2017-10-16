/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright © 2017 Endless Mobile, Inc.
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

static void
test_mkdir_p_enoent (void)
{
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

  ret = g_test_run();

  return ret;
}
