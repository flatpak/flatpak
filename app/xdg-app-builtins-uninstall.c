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

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to uninstall", "ARCH" },
  { "keep-ref", 0, 0, G_OPTION_ARG_NONE, &opt_keep_ref, "Keep ref in local repository", NULL },
  { "force-remove", 0, 0, G_OPTION_ARG_NONE, &opt_force_remove, "Remove files even if running", NULL },
  { NULL }
};

gboolean
xdg_app_builtin_uninstall_runtime (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(XdgAppDir) dir = NULL;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) arch_dir = NULL;
  g_autoptr(GFile) top_dir = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *name;
  const char *arch;
  const char *branch;
  g_autofree char *ref = NULL;
  g_autofree char *repository = NULL;
  g_auto(GStrv) deployed = NULL;
  int i;
  GError *temp_error = NULL;
  gboolean was_deployed;

  context = g_option_context_new ("RUNTIME [BRANCH] - Uninstall a runtime");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, "RUNTIME must be specified", error);

  name = argv[1];
  if (argc > 2)
    branch = argv[2];
  else
    branch = "master";
  if (opt_arch)
    arch = opt_arch;
  else
    arch = xdg_app_get_arch ();

  if (!xdg_app_is_valid_name (name))
    return xdg_app_fail (error, "'%s' is not a valid runtime name", name);

  if (!xdg_app_is_valid_branch (branch))
    return xdg_app_fail (error, "'%s' is not a valid branch name", branch);

  /* TODO: look for apps, require --force */

  ref = g_build_filename ("runtime", name, arch, branch, NULL);

  repository = xdg_app_dir_get_origin (dir, ref, cancellable, NULL);

  g_debug ("dropping active ref");
  if (!xdg_app_dir_set_active (dir, ref, NULL, cancellable, error))
    return FALSE;

  if (!xdg_app_dir_list_deployed (dir, ref, &deployed, cancellable, error))
    return FALSE;

  for (i = 0; deployed[i] != NULL; i++)
    {
      g_debug ("undeploying %s", deployed[i]);
      if (!xdg_app_dir_undeploy (dir, ref, deployed[i], opt_force_remove, cancellable, error))
        return FALSE;
    }

  deploy_base = xdg_app_dir_get_deploy_dir (dir, ref);
  was_deployed = g_file_query_exists (deploy_base, cancellable);
  if (was_deployed)
    {
      g_debug ("removing deploy base");
      if (!gs_shutil_rm_rf (deploy_base, cancellable, error))
        return FALSE;
    }

  g_debug ("cleaning up empty directories");
  arch_dir = g_file_get_parent (deploy_base);
  if (g_file_query_exists (arch_dir, cancellable) &&
      !g_file_delete (arch_dir, cancellable, &temp_error))
    {
      if (!g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY))
        {
          g_propagate_error (error, temp_error);
          return FALSE;
        }
      g_clear_error (&temp_error);
    }

  top_dir = g_file_get_parent (arch_dir);
  if (g_file_query_exists (top_dir, cancellable) &&
      !g_file_delete (top_dir, cancellable, &temp_error))
    {
      if (!g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY))
        {
          g_propagate_error (error, temp_error);
          return FALSE;
        }
      g_clear_error (&temp_error);
    }

  if (!opt_keep_ref)
    {
      repo = xdg_app_dir_get_repo (dir);

      if (!ostree_repo_set_ref_immediate (repo, repository, ref, NULL, cancellable, error))
        return FALSE;

      if (!xdg_app_dir_prune (dir, cancellable, error))
        return FALSE;
    }

  xdg_app_dir_cleanup_removed (dir, cancellable, NULL);

  if (!was_deployed)
    return xdg_app_fail (error, "Nothing to uninstall");

  return TRUE;
}

gboolean
xdg_app_builtin_uninstall_app (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(XdgAppDir) dir = NULL;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) arch_dir = NULL;
  g_autoptr(GFile) top_dir = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *name;
  const char *arch;
  const char *branch;
  g_autofree char *ref = NULL;
  g_autofree char *repository = NULL;
  g_autofree char *current_ref = NULL;
  g_auto(GStrv) deployed = NULL;
  int i;
  GError *temp_error = NULL;
  gboolean was_deployed;

  context = g_option_context_new ("APP [BRANCH] - Uninstall an application");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, "APP must be specified", error);

  name = argv[1];
  if (argc > 2)
    branch = argv[2];
  else
    branch = "master";
  if (opt_arch)
    arch = opt_arch;
  else
    arch = xdg_app_get_arch ();

  if (!xdg_app_is_valid_name (name))
    return xdg_app_fail (error, "'%s' is not a valid application name", name);

  if (!xdg_app_is_valid_branch (branch))
    return xdg_app_fail (error, "'%s' is not a valid branch name", branch);

  ref = g_build_filename ("app", name, arch, branch, NULL);

  repository = xdg_app_dir_get_origin (dir, ref, cancellable, NULL);

  g_debug ("dropping active ref");
  if (!xdg_app_dir_set_active (dir, ref, NULL, cancellable, error))
    return FALSE;

  current_ref = xdg_app_dir_current_ref (dir, name, cancellable);
  if (current_ref != NULL && strcmp (ref, current_ref) == 0)
    {
      g_debug ("dropping current ref");
      if (!xdg_app_dir_drop_current_ref (dir, name, cancellable, error))
        return FALSE;
    }

  if (!xdg_app_dir_list_deployed (dir, ref, &deployed, cancellable, error))
    return FALSE;

  for (i = 0; deployed[i] != NULL; i++)
    {
      g_debug ("undeploying %s", deployed[i]);
      if (!xdg_app_dir_undeploy (dir, ref, deployed[i], opt_force_remove, cancellable, error))
        return FALSE;
    }

  deploy_base = xdg_app_dir_get_deploy_dir (dir, ref);
  was_deployed = g_file_query_exists (deploy_base, cancellable);
  if (was_deployed)
    {
      g_debug ("removing deploy base");
      if (!gs_shutil_rm_rf (deploy_base, cancellable, error))
        return FALSE;
    }

  if (!xdg_app_dir_update_exports (dir, name, cancellable, error))
    return FALSE;

  g_debug ("cleaning up empty directories");
  arch_dir = g_file_get_parent (deploy_base);
  if (g_file_query_exists (arch_dir, cancellable) &&
      !g_file_delete (arch_dir, cancellable, &temp_error))
    {
      if (!g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY))
        {
          g_propagate_error (error, temp_error);
          return FALSE;
        }
      g_clear_error (&temp_error);
    }

  top_dir = g_file_get_parent (arch_dir);
  if (g_file_query_exists (top_dir, cancellable) &&
      !g_file_delete (top_dir, cancellable, &temp_error))
    {
      if (!g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY))
        {
          g_propagate_error (error, temp_error);
          return FALSE;
        }
      g_clear_error (&temp_error);
    }

  if (!opt_keep_ref)
    {
      repo = xdg_app_dir_get_repo (dir);

      if (!ostree_repo_set_ref_immediate (repo, repository, ref, NULL, cancellable, error))
        return FALSE;

      if (!xdg_app_dir_prune (dir, cancellable, error))
        return FALSE;
    }

  xdg_app_dir_cleanup_removed (dir, cancellable, NULL);

  if (!was_deployed)
    return xdg_app_fail (error, "Nothing to uninstall");

  return TRUE;
}
