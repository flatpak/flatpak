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
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to make current for", "ARCH" },
  { NULL }
};

gboolean
xdg_app_builtin_make_current_app (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object XdgAppDir *dir = NULL;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *origin = NULL;
  const char *app;
  const char *branch = "master";
  gs_free char *ref = NULL;

  context = g_option_context_new ("APP BRANCH - Make branch of application current");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    goto out;

  if (argc < 3)
    {
      usage_error (context, "APP and BRANCH must be specified", error);
      goto out;
    }

  app  = argv[1];
  branch = argv[2];

  if (!xdg_app_is_valid_name (app))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "'%s' is not a valid application name", app);
      goto out;
    }

  if (!xdg_app_is_valid_branch (branch))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "'%s' is not a valid branch name", branch);
      goto out;
    }

  ref = xdg_app_build_app_ref (app, branch, opt_arch);

  deploy_base = xdg_app_dir_get_deploy_dir (dir, ref);
  if (!g_file_query_exists (deploy_base, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "App %s branch %s is not installed", app, branch);
      goto out;
    }

  if (!xdg_app_dir_make_current_ref (dir, ref, cancellable, error))
    goto out;

  if (!xdg_app_dir_update_exports (dir, app, cancellable, error))
    goto out;

  ret = TRUE;

 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
