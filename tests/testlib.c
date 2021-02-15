/*
 * Copyright © 2020-2021 Collabora Ltd.
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
#include "testlib.h"

#include <glib.h>
#include <glib/gstdio.h>

#include "libglnx/libglnx.h"

char *
assert_mkdtemp (char *tmpl)
{
  char *ret = g_mkdtemp (tmpl);

  if (ret == NULL)
    g_error ("%s", g_strerror (errno));
  else
    g_assert_true (ret == tmpl);

  return ret;
}

char *isolated_test_dir = NULL;

void
isolated_test_dir_global_setup (void)
{
  g_autofree char *cachedir = NULL;
  g_autofree char *configdir = NULL;
  g_autofree char *datadir = NULL;
  g_autofree char *homedir = NULL;
  g_autofree char *runtimedir = NULL;

  isolated_test_dir = g_strdup ("/tmp/flatpak-test-XXXXXX");
  assert_mkdtemp (isolated_test_dir);
  g_test_message ("isolated_test_dir: %s", isolated_test_dir);

  homedir = g_strconcat (isolated_test_dir, "/home", NULL);
  g_assert_no_errno (g_mkdir_with_parents (homedir, S_IRWXU | S_IRWXG | S_IRWXO));

  g_setenv ("HOME", homedir, TRUE);
  g_test_message ("setting HOME=%s", homedir);

  cachedir = g_strconcat (isolated_test_dir, "/home/cache", NULL);
  g_assert_no_errno (g_mkdir_with_parents (cachedir, S_IRWXU | S_IRWXG | S_IRWXO));
  g_setenv ("XDG_CACHE_HOME", cachedir, TRUE);
  g_test_message ("setting XDG_CACHE_HOME=%s", cachedir);

  configdir = g_strconcat (isolated_test_dir, "/home/config", NULL);
  g_assert_no_errno (g_mkdir_with_parents (configdir, S_IRWXU | S_IRWXG | S_IRWXO));
  g_setenv ("XDG_CONFIG_HOME", configdir, TRUE);
  g_test_message ("setting XDG_CONFIG_HOME=%s", configdir);

  datadir = g_strconcat (isolated_test_dir, "/home/share", NULL);
  g_assert_no_errno (g_mkdir_with_parents (datadir, S_IRWXU | S_IRWXG | S_IRWXO));
  g_setenv ("XDG_DATA_HOME", datadir, TRUE);
  g_test_message ("setting XDG_DATA_HOME=%s", datadir);

  runtimedir = g_strconcat (isolated_test_dir, "/runtime", NULL);
  g_assert_no_errno (g_mkdir_with_parents (runtimedir, S_IRWXU));
  g_setenv ("XDG_RUNTIME_DIR", runtimedir, TRUE);
  g_test_message ("setting XDG_RUNTIME_DIR=%s", runtimedir);

  g_reload_user_special_dirs_cache ();

  g_assert_cmpstr (g_get_user_cache_dir (), ==, cachedir);
  g_assert_cmpstr (g_get_user_config_dir (), ==, configdir);
  g_assert_cmpstr (g_get_user_data_dir (), ==, datadir);
  g_assert_cmpstr (g_get_user_runtime_dir (), ==, runtimedir);
}

void
isolated_test_dir_global_teardown (void)
{
  if (g_getenv ("SKIP_TEARDOWN"))
    return;

  glnx_shutil_rm_rf_at (-1, isolated_test_dir, NULL, NULL);
  g_free (isolated_test_dir);
  isolated_test_dir = NULL;
}
