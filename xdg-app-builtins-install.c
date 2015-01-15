#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static char *opt_arch;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to install for", "ARCH" },
  { NULL }
};

gboolean
xdg_app_builtin_install_runtime (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object XdgAppDir *dir = NULL;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *origin = NULL;
  const char *repository;
  const char *runtime;
  const char *branch = "master";
  gs_free char *ref = NULL;
  gboolean created_deploy_base = FALSE;

  context = g_option_context_new ("REPOSITORY RUNTIME [BRANCH] - Install a runtime");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    goto out;

  if (argc < 3)
    {
      usage_error (context, "REPOSITORY and RUNTIME must be specified", error);
      goto out;
    }

  repository = argv[1];
  runtime  = argv[2];
  if (argc >= 4)
    branch = argv[3];

  ref = xdg_app_build_runtime_ref (runtime, branch, opt_arch);

  deploy_base = xdg_app_dir_get_deploy_dir (dir, ref);
  if (g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Runtime %s branch %s already installed", runtime, branch);
      goto out;
    }

  if (!xdg_app_dir_pull (dir, repository, ref,
                         cancellable, error))
    goto out;

  if (!g_file_make_directory_with_parents (deploy_base, cancellable, error))
    goto out;
  created_deploy_base = TRUE;

  origin = g_file_get_child (deploy_base, "origin");
  if (!g_file_replace_contents (origin, repository, strlen (repository), NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, cancellable, error))
    goto out;

  if (!xdg_app_dir_deploy (dir, ref, NULL, cancellable, error))
    goto out;

  ret = TRUE;

 out:
  if (created_deploy_base && !ret)
    gs_shutil_rm_rf (deploy_base, cancellable, NULL);

  if (context)
    g_option_context_free (context);
  return ret;
}

gboolean
xdg_app_builtin_install_app (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object XdgAppDir *dir = NULL;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *origin = NULL;
  const char *repository;
  const char *app;
  const char *branch = "master";
  gs_free char *ref = NULL;
  gboolean created_deploy_base = FALSE;

  context = g_option_context_new ("REPOSITORY APP [BRANCH] - Install an application");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    goto out;

  if (argc < 3)
    {
      usage_error (context, "REPOSITORY and APP must be specified", error);
      goto out;
    }

  repository = argv[1];
  app  = argv[2];
  if (argc >= 4)
    branch = argv[3];

  ref = xdg_app_build_app_ref (app, branch, opt_arch);

  deploy_base = xdg_app_dir_get_deploy_dir (dir, ref);
  if (g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "App %s branch %s already installed", app, branch);
      goto out;
    }

  if (!xdg_app_dir_pull (dir, repository, ref,
                         cancellable, error))
    goto out;

  if (!g_file_make_directory_with_parents (deploy_base, cancellable, error))
    goto out;
  created_deploy_base = TRUE;

  origin = g_file_get_child (deploy_base, "origin");
  if (!g_file_replace_contents (origin, repository, strlen (repository), NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, cancellable, error))
    goto out;

  if (!xdg_app_dir_deploy (dir, ref, NULL, cancellable, error))
    goto out;

  ret = TRUE;

 out:
  if (created_deploy_base && !ret)
    gs_shutil_rm_rf (deploy_base, cancellable, NULL);

  if (context)
    g_option_context_free (context);
  return ret;
}
