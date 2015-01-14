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
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to update for", "ARCH" },
  { NULL }
};

gboolean
xdg_app_builtin_update_runtime (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object XdgAppDir *dir = NULL;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *origin = NULL;
  const char *runtime;
  const char *branch = "master";
  gs_free char *previous_deployment = NULL;
  gs_free char *ref = NULL;
  gs_free char *repository = NULL;
  GError *my_error;

  context = g_option_context_new ("RUNTIME [BRANCH] - Update a runtime");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "RUNTIME must be specified", error);
      goto out;
    }

  runtime = argv[1];
  if (argc >= 3)
    branch = argv[2];

  ref = xdg_app_build_runtime_ref (runtime, branch, opt_arch);

  deploy_base = xdg_app_dir_get_deploy_dir (dir, ref);
  if (!g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Runtime %s branch %s not installed", runtime, branch);
      goto out;
    }

  origin = g_file_get_child (deploy_base, "origin");
  if (!g_file_load_contents (origin, cancellable, &repository, NULL, NULL, error))
    goto out;

  if (!xdg_app_dir_pull (dir, repository, ref,
                         cancellable, error))
    goto out;

  previous_deployment = xdg_app_dir_read_active (dir, ref, cancellable);

  my_error = NULL;
  if (!xdg_app_dir_deploy (dir, ref, NULL, cancellable, &my_error))
    {
      if (g_error_matches (my_error, XDG_APP_DIR_ERROR, XDG_APP_DIR_ERROR_ALREADY_DEPLOYED))
        g_error_free (my_error);
      else
        {
          g_propagate_error (error, my_error);
          goto out;
        }
    }

  if (previous_deployment != NULL)
    {
      if (!xdg_app_dir_undeploy (dir, ref, previous_deployment,
                                 cancellable, error))
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
xdg_app_builtin_update_app (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object XdgAppDir *dir = NULL;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *origin = NULL;
  const char *app;
  const char *branch = "master";
  gs_free char *ref = NULL;
  gs_free char *repository = NULL;
  gs_free char *previous_deployment = NULL;
  GError *my_error;

  context = g_option_context_new ("APP [BRANCH] - Update an application");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "APP must be specified", error);
      goto out;
    }

  app = argv[1];
  if (argc >= 3)
    branch = argv[2];

  ref = xdg_app_build_app_ref (app, branch, opt_arch);

  deploy_base = xdg_app_dir_get_deploy_dir (dir, ref);
  if (!g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "App %s branch %s not installed", app, branch);
      goto out;
    }

  origin = g_file_get_child (deploy_base, "origin");
  if (!g_file_load_contents (origin, cancellable, &repository, NULL, NULL, error))
    goto out;

  if (!xdg_app_dir_pull (dir, repository, ref,
                         cancellable, error))
    goto out;

  previous_deployment = xdg_app_dir_read_active (dir, ref, cancellable);

  my_error = NULL;
  if (!xdg_app_dir_deploy (dir, ref, NULL, cancellable, &my_error))
    {
      if (g_error_matches (my_error, XDG_APP_DIR_ERROR, XDG_APP_DIR_ERROR_ALREADY_DEPLOYED))
        g_error_free (my_error);
      else
        {
          g_propagate_error (error, my_error);
          goto out;
        }
    }

  if (previous_deployment != NULL)
    {
      if (!xdg_app_dir_undeploy (dir, ref, previous_deployment,
                                 cancellable, error))
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
