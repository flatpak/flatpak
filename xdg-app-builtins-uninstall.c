#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static char *opt_arch;
static gboolean opt_keep_ref;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to uninstall", "ARCH" },
  { "keep-ref", 0, 0, G_OPTION_ARG_NONE, &opt_keep_ref, "Keep ref in local repository", NULL },
  { NULL }
};

static gboolean
single_child_directory (GFile *dir, const char *name, GCancellable *cancellable)
{
  gboolean ret = FALSE;
  gs_unref_object GFileEnumerator *dir_enum = NULL;
  gs_unref_object GFileInfo *child_info = NULL;

  dir_enum = g_file_enumerate_children (dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancellable, NULL);

  if (!dir_enum)
    goto out;

  while ((child_info = g_file_enumerator_next_file (dir_enum, cancellable, NULL)))
    {
      if (strcmp (name, g_file_info_get_name (child_info)) == 0)
        {
          g_clear_object (&child_info);
          continue;
        }
      goto out;
    }

  ret = TRUE;

out:
  return ret;
}

gboolean
xdg_app_builtin_uninstall_runtime (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object XdgAppDir *dir = NULL;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *arch_dir = NULL;
  gs_unref_object GFile *top_dir = NULL;
  gs_unref_object GFile *origin = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  const char *name;
  const char *arch;
  const char *branch;
  gs_free char *ref = NULL;
  gs_free char *repository = NULL;
  gs_strfreev char **deployed = NULL;
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
      if (!xdg_app_dir_undeploy (dir, ref, deployed[i], cancellable, error))
        goto out;
    }

  g_debug ("removing deploy base");
  if (!gs_shutil_rm_rf (deploy_base, cancellable, error))
    goto out;

  g_debug ("cleaning up empty directories");
  arch_dir = g_file_get_parent (deploy_base);
  if (!g_file_delete (arch_dir, cancellable, &temp_error))
    {
      if (!g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY))
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
      g_clear_error (&temp_error);
    }

  top_dir = g_file_get_parent (arch_dir);
  if (!g_file_delete (top_dir, cancellable, &temp_error))
    {
      if (!g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY))
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
  gs_unref_object XdgAppDir *dir = NULL;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *arch_dir = NULL;
  gs_unref_object GFile *top_dir = NULL;
  gs_unref_object GFile *origin = NULL;
  gs_unref_object OstreeRepo *repo = NULL;
  const char *name;
  const char *arch;
  const char *branch;
  gs_free char *ref = NULL;
  gs_free char *repository = NULL;
  gs_strfreev char **deployed = NULL;
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

  if (!xdg_app_dir_list_deployed (dir, ref, &deployed, cancellable, error))
    goto out;

  for (i = 0; deployed[i] != NULL; i++)
    {
      g_debug ("undeploying %s", deployed[i]);
      if (!xdg_app_dir_undeploy (dir, ref, deployed[i], cancellable, error))
        goto out;
    }

  g_debug ("removing deploy base");
  if (!gs_shutil_rm_rf (deploy_base, cancellable, error))
    goto out;

  g_debug ("cleaning up empty directories");
  arch_dir = g_file_get_parent (deploy_base);
  if (!g_file_delete (arch_dir, cancellable, &temp_error))
    {
      if (!g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY))
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
      g_clear_error (&temp_error);
    }

  top_dir = g_file_get_parent (arch_dir);
  if (single_child_directory (top_dir, "data", cancellable))
    {
       if (!gs_shutil_rm_rf (top_dir, cancellable, error))
         goto out;
    }

  if (!opt_keep_ref)
    {
      repo = xdg_app_dir_get_repo (dir);

      if (!ostree_repo_set_ref_immediate (repo, repository, ref, NULL, cancellable, error))
        goto out;

      if (!xdg_app_dir_prune (dir, cancellable, error))
        goto out;
    }

  ret = TRUE;

 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
