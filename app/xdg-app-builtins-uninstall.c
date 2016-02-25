/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static char *opt_arch;
static gboolean opt_keep_ref;
static gboolean opt_force_remove;
static gboolean opt_runtime;
static gboolean opt_app;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to uninstall", "ARCH" },
  { "keep-ref", 0, 0, G_OPTION_ARG_NONE, &opt_keep_ref, "Keep ref in local repository", NULL },
  { "force-remove", 0, 0, G_OPTION_ARG_NONE, &opt_force_remove, "Remove files even if running", NULL },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, "Look for runtime with the specified name", },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, "Look for app with the specified name", },
  { NULL }
};

gboolean
xdg_app_builtin_uninstall (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(XdgAppDir) dir = NULL;
  const char *name = NULL;
  const char *branch = NULL;
  g_autofree char *ref = NULL;
  g_autofree char *repository = NULL;
  g_autofree char *current_ref = NULL;
  gboolean was_deployed;
  gboolean is_app;
  g_auto(GLnxLockFile) lock = GLNX_LOCK_FILE_INIT;

  context = g_option_context_new ("APP [BRANCH] - Uninstall an application");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, "APP must be specified", error);

  name = argv[1];
  if (argc > 2)
    branch = argv[2];

  if (!opt_app && !opt_runtime)
    opt_app = opt_runtime = TRUE;

  ref = xdg_app_dir_find_installed_ref (dir,
                                        name,
                                        branch,
                                        opt_arch,
                                        opt_app, opt_runtime, &is_app,
                                        error);
  if (ref == NULL)
    return FALSE;

  /* TODO: when removing runtimes, look for apps that use it, require --force */

  if (!xdg_app_dir_lock (dir, &lock,
                         cancellable, error))
    return FALSE;

  repository = xdg_app_dir_get_origin (dir, ref, cancellable, NULL);

  g_debug ("dropping active ref");
  if (!xdg_app_dir_set_active (dir, ref, NULL, cancellable, error))
    return FALSE;

  if (is_app)
    {
      current_ref = xdg_app_dir_current_ref (dir, name, cancellable);
      if (current_ref != NULL && strcmp (ref, current_ref) == 0)
        {
          g_debug ("dropping current ref");
          if (!xdg_app_dir_drop_current_ref (dir, name, cancellable, error))
            return FALSE;
        }
    }

  if (!xdg_app_dir_undeploy_all (dir, ref, opt_force_remove, &was_deployed, cancellable, error))
    return FALSE;

  if (!opt_keep_ref)
    {
      if (!xdg_app_dir_remove_ref (dir, repository, ref, cancellable, error))
        return FALSE;
    }

  glnx_release_lock_file (&lock);

  if (!opt_keep_ref)
    {
      if (!xdg_app_dir_prune (dir, cancellable, error))
        return FALSE;
    }

  xdg_app_dir_cleanup_removed (dir, cancellable, NULL);

  if (is_app)
    {
      if (!xdg_app_dir_update_exports (dir, name, cancellable, error))
        return FALSE;
    }

  if (repository != NULL &&
      g_str_has_suffix (repository, "-origin") &&
      xdg_app_dir_get_remote_noenumerate (dir, repository))
    {
      ostree_repo_remote_delete (xdg_app_dir_get_repo (dir), repository, NULL, NULL);
    }

  if (!xdg_app_dir_mark_changed (dir, error))
    return FALSE;

  if (!was_deployed)
    return xdg_app_fail (error, "Nothing to uninstall");

  return TRUE;
}

gboolean
xdg_app_builtin_uninstall_runtime (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  opt_runtime = TRUE;
  opt_app = FALSE;

  return xdg_app_builtin_uninstall (argc, argv, cancellable, error);
}

gboolean
xdg_app_builtin_uninstall_app (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  opt_runtime = FALSE;
  opt_app = TRUE;

  return xdg_app_builtin_uninstall (argc, argv, cancellable, error);
}
