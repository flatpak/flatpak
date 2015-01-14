#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static char *opt_arch;
static char *opt_name;
static char *opt_var;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to use", "ARCH" },
  { "name", 'n', 0, G_OPTION_ARG_STRING, &opt_name, "App name", "NAME" },
  { "var", 'v', 0, G_OPTION_ARG_STRING, &opt_var, "Initialize var from named runtime", "RUNTIME" },
  { NULL }
};

gboolean
xdg_app_builtin_build_init (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  gs_unref_object GFile *runtime_deploy_base = NULL;
  gs_unref_object GFile *sdk_deploy_base = NULL;
  gs_unref_object GFile *var_deploy_base = NULL;
  gs_unref_object GFile *var_deploy_files = NULL;
  gs_unref_object GFile *base = NULL;
  gs_unref_object GFile *files_dir = NULL;
  gs_unref_object GFile *var_dir = NULL;
  gs_unref_object GFile *var_tmp_dir = NULL;
  gs_unref_object GFile *var_run_dir = NULL;
  gs_unref_object GFile *metadata_file = NULL;
  gs_unref_object XdgAppDir *user_dir = NULL;
  gs_unref_object XdgAppDir *system_dir = NULL;
  const char *directory;
  const char *sdk;
  const char *runtime;
  const char *branch = "master";
  gs_free char *runtime_ref = NULL;
  gs_free char *var_ref = NULL;
  gs_free char *sdk_ref = NULL;
  gs_free char *metadata_contents = NULL;

  context = g_option_context_new ("DIRECTORY SDK RUNTIME [BRANCH] - Initialize a directory for building");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    goto out;

  if (argc < 4)
    {
      usage_error (context, "RUNTIME must be specified", error);
      goto out;
    }

  directory = argv[1];
  sdk = argv[2];
  runtime = argv[3];
  if (argc >= 5)
    branch = argv[4];

  runtime_ref = xdg_app_build_untyped_ref (runtime, branch, opt_arch);
  sdk_ref = xdg_app_build_untyped_ref (sdk, branch, opt_arch);

  base = g_file_new_for_commandline_arg (directory);

  if (!gs_file_ensure_directory (base, TRUE, cancellable, error))
    goto out;

  files_dir = g_file_get_child (base, "files");
  var_dir = g_file_get_child (base, "var");
  var_tmp_dir = g_file_get_child (var_dir, "tmp");
  var_run_dir = g_file_get_child (var_dir, "run");
  metadata_file = g_file_get_child (base, "metadata");

  if (g_file_query_exists (files_dir, cancellable))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Build directory %s already initialized", directory);
      goto out;
    }

  if (opt_var)
    {
      user_dir = xdg_app_dir_get_user ();
      system_dir = xdg_app_dir_get_system ();

      var_ref = xdg_app_build_runtime_ref (opt_var, branch, opt_arch);

      var_deploy_base = xdg_app_find_deploy_dir_for_ref (var_ref, cancellable, error);
      if (var_deploy_base == NULL)
        goto out;

      var_deploy_files = g_file_get_child (var_deploy_base, "files");
    }

  if (!g_file_make_directory (files_dir, cancellable, error))
    goto out;

  if (var_deploy_files)
    {
      if (!gs_shutil_cp_a (var_deploy_files, var_dir, cancellable, error))
        goto out;
    }
  else
    {
      if (!g_file_make_directory (var_dir, cancellable, error))
        goto out;
    }

  if (!gs_file_ensure_directory (var_tmp_dir, FALSE, cancellable, error))
    goto out;

  if (!g_file_make_symbolic_link (var_run_dir, "/run", cancellable, error))
    goto out;

  metadata_contents = g_strdup_printf("[Application]\n"
                                      "runtime=%s\n"
                                      "sdk=%s\n",
                                      runtime_ref, sdk_ref);
  if (!g_file_replace_contents (metadata_file,
                                metadata_contents, strlen (metadata_contents), NULL, FALSE,
                                G_FILE_CREATE_REPLACE_DESTINATION,
                                NULL, cancellable, error))
    goto out;

  ret = TRUE;
 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
