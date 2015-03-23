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
  gboolean ret = FALSE;
  GOptionContext *context;
  g_autoptr(XdgAppDir) dir = NULL;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) arch_dir = NULL;
  g_autoptr(GFile) top_dir = NULL;
  g_autoptr(GFile) origin = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *name;
  const char *arch;
  const char *branch;
  g_autofree char *ref = NULL;
  g_autofree char *repository = NULL;
  glnx_strfreev char **deployed = NULL;
  int i;
  GError *temp_error = NULL;

  context = g_option_context_new ("RUNTIME [BRANCH] - Uninstall a runtime");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "RUNTIME must be specified", error);
      goto out;
    }

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
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "'%s' is not a valid runtime name", name);
      goto out;
    }

  if (!xdg_app_is_valid_branch (branch))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "'%s' is not a valid branch name", branch);
      goto out;
    }

  /* TODO: look for apps, require --force */

  ref = g_build_filename ("runtime", name, arch, branch, NULL);

  deploy_base = xdg_app_dir_get_deploy_dir (dir, ref);
  if (!g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Nothing to uninstall");
      goto out;
    }

  origin = g_file_get_child (deploy_base, "origin");
  if (!g_file_load_contents (origin, cancellable, &repository, NULL, NULL, error))
    goto out;

  g_debug ("dropping active ref");
  if (!xdg_app_dir_set_active (dir, ref, NULL, cancellable, error))
    goto out;

  if (!xdg_app_dir_list_deployed (dir, ref, &deployed, cancellable, error))
    goto out;

  for (i = 0; deployed[i] != NULL; i++)
    {
      g_debug ("undeploying %s", deployed[i]);
      if (!xdg_app_dir_undeploy (dir, ref, deployed[i], opt_force_remove, cancellable, error))
        goto out;
    }

  g_debug ("removing deploy base");
  if (!gs_shutil_rm_rf (deploy_base, cancellable, error))
    goto out;

  g_debug ("cleaning up empty directories");
  arch_dir = g_file_get_parent (deploy_base);
  if (!g_file_delete (arch_dir, cancellable, &temp_error))
    {
      if (!g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY))
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
      g_clear_error (&temp_error);
    }

  top_dir = g_file_get_parent (arch_dir);
  if (!g_file_delete (top_dir, cancellable, &temp_error))
    {
      if (!g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY))
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
      g_clear_error (&temp_error);
    }

  if (!opt_keep_ref)
    {
      repo = xdg_app_dir_get_repo (dir);

      if (!ostree_repo_set_ref_immediate (repo, repository, ref, NULL, cancellable, error))
        goto out;

      if (!xdg_app_dir_prune (dir, cancellable, error))
        goto out;
    }

  xdg_app_dir_cleanup_removed (dir, cancellable, NULL);

  ret = TRUE;

 out:
  if (context)
    g_option_context_free (context);
  return ret;
}

gboolean
xdg_app_builtin_uninstall_app (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  g_autoptr(XdgAppDir) dir = NULL;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) arch_dir = NULL;
  g_autoptr(GFile) top_dir = NULL;
  g_autoptr(GFile) origin = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *name;
  const char *arch;
  const char *branch;
  g_autofree char *ref = NULL;
  g_autofree char *repository = NULL;
  g_autofree char *current_ref = NULL;
  glnx_strfreev char **deployed = NULL;
  int i;
  GError *temp_error = NULL;

  context = g_option_context_new ("APP [BRANCH] - Uninstall an application");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "APP must be specified", error);
      goto out;
    }

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
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "'%s' is not a valid application name", name);
      goto out;
    }

  if (!xdg_app_is_valid_branch (branch))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "'%s' is not a valid branch name", branch);
      goto out;
    }

  ref = g_build_filename ("app", name, arch, branch, NULL);

  deploy_base = xdg_app_dir_get_deploy_dir (dir, ref);
  if (!g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Nothing to uninstall");
      goto out;
    }

  origin = g_file_get_child (deploy_base, "origin");
  if (!g_file_load_contents (origin, cancellable, &repository, NULL, NULL, error))
    goto out;

  g_debug ("dropping active ref");
  if (!xdg_app_dir_set_active (dir, ref, NULL, cancellable, error))
    goto out;

  current_ref = xdg_app_dir_current_ref (dir, name, cancellable);
  if (current_ref != NULL && strcmp (ref, current_ref) == 0)
    {
      g_debug ("dropping current ref");
      if (!xdg_app_dir_drop_current_ref (dir, name, cancellable, error))
        goto out;
    }

  if (!xdg_app_dir_list_deployed (dir, ref, &deployed, cancellable, error))
    goto out;

  for (i = 0; deployed[i] != NULL; i++)
    {
      g_debug ("undeploying %s", deployed[i]);
      if (!xdg_app_dir_undeploy (dir, ref, deployed[i], opt_force_remove, cancellable, error))
        goto out;
    }

  g_debug ("removing deploy base");
  if (!gs_shutil_rm_rf (deploy_base, cancellable, error))
    goto out;

  if (!xdg_app_dir_update_exports (dir, name, cancellable, error))
    goto out;

  g_debug ("cleaning up empty directories");
  arch_dir = g_file_get_parent (deploy_base);
  if (!g_file_delete (arch_dir, cancellable, &temp_error))
    {
      if (!g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY))
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
      g_clear_error (&temp_error);
    }

  top_dir = g_file_get_parent (arch_dir);
  if (!g_file_delete (top_dir, cancellable, &temp_error))
    {
      if (!g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY))
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
      g_clear_error (&temp_error);
    }

  if (!opt_keep_ref)
    {
      repo = xdg_app_dir_get_repo (dir);

      if (!ostree_repo_set_ref_immediate (repo, repository, ref, NULL, cancellable, error))
        goto out;

      if (!xdg_app_dir_prune (dir, cancellable, error))
        goto out;
    }

  xdg_app_dir_cleanup_removed (dir, cancellable, NULL);

  ret = TRUE;

 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
