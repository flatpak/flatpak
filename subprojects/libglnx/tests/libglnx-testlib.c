/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright 2019 Collabora Ltd.
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
#include "libglnx-testlib.h"

#include <errno.h>

#include <glib/gstdio.h>

#include "libglnx.h"

struct _GLnxTestAutoTempDir
{
  gchar *old_cwd;
  int old_cwd_fd;
  GLnxTmpDir temp_dir;
};

_GLnxTestAutoTempDir *
_glnx_test_auto_temp_dir_enter (void)
{
  GError *error = NULL;
  _GLnxTestAutoTempDir *ret = g_new0 (_GLnxTestAutoTempDir, 1);

  glnx_mkdtemp ("glnx-test-XXXXXX", 0700, &ret->temp_dir, &error);
  g_assert_no_error (error);

  /* just for better diagnostics */
  ret->old_cwd = g_get_current_dir ();

  glnx_opendirat (-1, ".", TRUE, &ret->old_cwd_fd, &error);
  g_assert_no_error (error);

  if (fchdir (ret->temp_dir.fd) != 0)
    g_error ("fchdir(<fd for \"%s\">): %s", ret->temp_dir.path, g_strerror (errno));

  return ret;
}

void
_glnx_test_auto_temp_dir_leave (_GLnxTestAutoTempDir *dir)
{
  GError *error = NULL;

  if (fchdir (dir->old_cwd_fd) != 0)
    g_error ("fchdir(<fd for \"%s\">): %s", dir->old_cwd, g_strerror (errno));

  glnx_tmpdir_delete (&dir->temp_dir, NULL, &error);
  g_assert_no_error (error);

  glnx_close_fd (&dir->old_cwd_fd);

  g_free (dir->old_cwd);
  g_free (dir);
}
