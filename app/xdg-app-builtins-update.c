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
static char **opt_subpaths;
static gboolean opt_force_remove;
static gboolean opt_no_pull;
static gboolean opt_no_deploy;
static gboolean opt_runtime;
static gboolean opt_app;
static gboolean opt_appstream;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to update for", "ARCH" },
  { "commit", 0, 0, G_OPTION_ARG_STRING, &opt_commit, "Commit to deploy", "COMMIT" },
  { "force-remove", 0, 0, G_OPTION_ARG_NONE, &opt_force_remove, "Remove old files even if running", NULL },
  { "no-pull", 0, 0, G_OPTION_ARG_NONE, &opt_no_pull, "Don't pull, only update from local cache", },
  { "no-deploy", 0, 0, G_OPTION_ARG_NONE, &opt_no_deploy, "Don't deploy, only download to local cache", },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, "Look for runtime with the specified name", },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, "Look for app with the specified name", },
  { "appstream", 0, 0, G_OPTION_ARG_NONE, &opt_appstream, "Update appstream for remote", },
  { "subpath", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_subpaths, "Only update this subpath", "path" },
  { NULL }
};

static gboolean
update_appstream (XdgAppDir *dir, const char *remote, GCancellable *cancellable, GError **error)
{
  gboolean changed;
  if (!xdg_app_dir_update_appstream (dir, remote, opt_arch, &changed,
                                   NULL, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
do_update (XdgAppDir* dir,
           const char *name,
           const char *branch,
           const char *arch,
           gboolean check_app,
           gboolean check_runtime,
           GCancellable *cancellable,
           GError **error)
{
  g_autofree char *ref = NULL;
  g_autofree char *repository = NULL;
  g_auto(GStrv) subpaths = NULL;
  gboolean was_updated = FALSE;
  gboolean is_app;
  g_auto(GLnxLockFile) lock = GLNX_LOCK_FILE_INIT;

  ref = xdg_app_dir_find_installed_ref (dir,
                                        name,
                                        branch,
                                        arch,
                                        check_app, check_runtime, &is_app,
                                        error);
  if (ref == NULL)
    return FALSE;

  repository = xdg_app_dir_get_origin (dir, ref, cancellable, error);
  if (repository == NULL)
    return FALSE;

  subpaths = xdg_app_dir_get_subpaths (dir, ref, cancellable, error);
  if (subpaths == NULL)
    return FALSE;

  if (!opt_no_pull)
    {
      if (!xdg_app_dir_pull (dir, repository, ref, opt_subpaths ? opt_subpaths : subpaths,
                             NULL, cancellable, error))
        return FALSE;
    }

  if (!opt_no_deploy)
    {
      if (!xdg_app_dir_lock (dir, &lock,
                             cancellable, error))
        return FALSE;

      if (!xdg_app_dir_deploy_update (dir, ref, opt_commit, &was_updated, cancellable, error))
        return FALSE;

      if (was_updated && is_app)
        {
          if (!xdg_app_dir_update_exports (dir, name, cancellable, error))
            return FALSE;
        }

      glnx_release_lock_file (&lock);
    }

  if (was_updated)
    {
      if (!xdg_app_dir_prune (dir, cancellable, error))
        return FALSE;

      if (!xdg_app_dir_mark_changed (dir, error))
        return FALSE;
    }

  return  TRUE;
}

gboolean
xdg_app_builtin_update (int argc,
                        char **argv,
                        GCancellable *cancellable,
                        GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(XdgAppDir) dir = NULL;
  const char *name = NULL;
  const char *branch = NULL;
  const char *arch = opt_arch;
  int i;

  context = g_option_context_new ("[NAME [BRANCH]] - Update an application or runtime");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 1)
    return usage_error (context, "NAME must be specified", error);

  if (argc >= 2)
    name = argv[1];
  if (argc >= 3)
    branch = argv[2];

  if (arch == NULL)
    arch = xdg_app_get_arch ();

  if (!opt_app && !opt_runtime)
    opt_app = opt_runtime = TRUE;

  if (opt_appstream)
    return update_appstream (dir, name, cancellable, error);

  if (branch == NULL || name == NULL)
    {
      if (opt_app)
        {
          g_auto(GStrv) refs = NULL;

          if (!xdg_app_dir_list_refs (dir, "app", &refs,
                                      cancellable,
                                      error))
            return FALSE;

          for (i = 0; refs != NULL && refs[i] != NULL; i++)
            {
              g_auto(GStrv) parts = xdg_app_decompose_ref (refs[i], error);
              if (parts == NULL)
                return FALSE;

              if (name != NULL && strcmp (parts[1], name) != 0)
                continue;

              if (strcmp (parts[2], arch) != 0)
                continue;

              g_print ("Updating application %s %s\n", parts[1], parts[3]);

              if (!do_update (dir,
                              parts[1],
                              parts[3],
                              arch,
                              TRUE, FALSE,
                              cancellable,
                              error))
                return FALSE;
            }
        }

      if (opt_runtime)
        {
          g_auto(GStrv) refs = NULL;

          if (!xdg_app_dir_list_refs (dir, "runtime", &refs,
                                      cancellable,
                                      error))
            return FALSE;

          for (i = 0; refs != NULL && refs[i] != NULL; i++)
            {
              g_auto(GStrv) parts = xdg_app_decompose_ref (refs[i], error);
              if (parts == NULL)
                return FALSE;

              if (name != NULL && strcmp (parts[1], name) != 0)
                continue;

              if (strcmp (parts[2], arch) != 0)
                continue;

              g_print ("Updating runtime %s %s\n", parts[1], parts[3]);
              if (!do_update (dir,
                              parts[1],
                              parts[3],
                              arch,
                              FALSE, TRUE,
                              cancellable,
                              error))
                return FALSE;
            }
        }

    }
  else
    {
      if (!do_update (dir,
                      name,
                      branch,
                      arch,
                      opt_app, opt_runtime,
                      cancellable,
                      error))
        return FALSE;
    }

  xdg_app_dir_cleanup_removed (dir, cancellable, NULL);

  return  TRUE;
}

gboolean
xdg_app_builtin_update_runtime (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  opt_runtime = TRUE;
  opt_app = FALSE;

  return xdg_app_builtin_update (argc, argv, cancellable, error);
}

gboolean
xdg_app_builtin_update_app (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  opt_runtime = FALSE;
  opt_app = TRUE;

  return xdg_app_builtin_update (argc, argv, cancellable, error);
}
