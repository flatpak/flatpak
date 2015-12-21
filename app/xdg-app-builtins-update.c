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
static char *opt_commit;
static gboolean opt_force_remove;
static gboolean opt_no_pull;
static gboolean opt_no_deploy;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to update for", "ARCH" },
  { "commit", 0, 0, G_OPTION_ARG_STRING, &opt_commit, "Commit to deploy", "COMMIT" },
  { "force-remove", 0, 0, G_OPTION_ARG_NONE, &opt_force_remove, "Remove old files even if running", NULL },
  { "no-pull", 0, 0, G_OPTION_ARG_NONE, &opt_no_pull, "Don't pull, only update from local cache", },
  { "no-deploy", 0, 0, G_OPTION_ARG_NONE, &opt_no_deploy, "Don't deploy, only download to local cache", },
  { NULL }
};

gboolean
xdg_app_builtin_update_runtime (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(XdgAppDir) dir = NULL;
  const char *runtime;
  const char *branch = NULL;
  g_autofree char *ref = NULL;
  g_autofree char *repository = NULL;
  gboolean was_updated;
  g_auto(GLnxLockFile) lock = GLNX_LOCK_FILE_INIT;

  context = g_option_context_new ("RUNTIME [BRANCH] - Update a runtime");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, "RUNTIME must be specified", error);

  runtime = argv[1];
  if (argc >= 3)
    branch = argv[2];

  ref = xdg_app_compose_ref (FALSE, runtime, branch, opt_arch, error);
  if (ref == NULL)
    return FALSE;

  repository = xdg_app_dir_get_origin (dir, ref, cancellable, error);
  if (repository == NULL)
    return FALSE;

  if (!opt_no_pull)
    {
      if (!xdg_app_dir_pull (dir, repository, ref, NULL,
                             cancellable, error))
        return FALSE;
    }

  if (!opt_no_deploy)
    {
      if (!xdg_app_dir_lock (dir, &lock,
                             cancellable, error))
        return FALSE;

      if (!xdg_app_dir_deploy_update (dir, ref, opt_commit, &was_updated, cancellable, error))
        return FALSE;

      glnx_release_lock_file (&lock);
    }

  if (was_updated)
    {
      if (!xdg_app_dir_prune (dir, cancellable, error))
        return FALSE;
    }

  xdg_app_dir_cleanup_removed (dir, cancellable, NULL);

  if (!xdg_app_dir_mark_changed (dir, error))
    return FALSE;

  return TRUE;
}

gboolean
xdg_app_builtin_update_app (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(XdgAppDir) dir = NULL;
  const char *app;
  const char *branch = NULL;
  g_autofree char *ref = NULL;
  g_autofree char *repository = NULL;
  gboolean was_updated;
  g_auto(GLnxLockFile) lock = GLNX_LOCK_FILE_INIT;

  context = g_option_context_new ("APP [BRANCH] - Update an application");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, "APP must be specified", error);

  app = argv[1];
  if (argc >= 3)
    branch = argv[2];

  ref = xdg_app_compose_ref (TRUE, app, branch, opt_arch, error);
  if (ref == NULL)
    return FALSE;

  repository = xdg_app_dir_get_origin (dir, ref, cancellable, error);
  if (repository == NULL)
    return FALSE;

  if (!opt_no_pull)
    {
      if (!xdg_app_dir_pull (dir, repository, ref, NULL,
                             cancellable, error))
        return FALSE;
    }

  if (!opt_no_deploy)
    {
      if (!xdg_app_dir_lock (dir, &lock,
                             cancellable, error))
        return FALSE;

      if (!xdg_app_dir_deploy_update (dir, ref, opt_commit, &was_updated, cancellable, error))
        return FALSE;
    }

  if (was_updated)
    {
      if (!xdg_app_dir_update_exports (dir, app, cancellable, error))
        return FALSE;
    }

  glnx_release_lock_file (&lock);

  if (was_updated)
    {
      if (!xdg_app_dir_prune (dir, cancellable, error))
        return FALSE;
    }

  xdg_app_dir_cleanup_removed (dir, cancellable, NULL);

  if (!xdg_app_dir_mark_changed (dir, error))
    return FALSE;

  return  TRUE;
}
