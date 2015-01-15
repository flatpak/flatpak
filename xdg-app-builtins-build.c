#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static gboolean opt_runtime;
static gboolean opt_network;
static gboolean opt_x11;

static GOptionEntry options[] = {
  { "runtime", 'r', 0, G_OPTION_ARG_NONE, &opt_runtime, "Use non-devel runtime", NULL },
  { "network", 'n', 0, G_OPTION_ARG_NONE, &opt_network, "Allow network access", NULL },
  { "x11", 'x', 0, G_OPTION_ARG_NONE, &opt_x11, "Allow x11 access", NULL },
  { NULL }
};

gboolean
xdg_app_builtin_build (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  gs_unref_object XdgAppDir *user_dir = NULL;
  gs_unref_variant_builder GVariantBuilder *optbuilder = NULL;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *var = NULL;
  gs_unref_object GFile *var_tmp = NULL;
  gs_unref_object GFile *var_run = NULL;
  gs_unref_object GFile *app_deploy = NULL;
  gs_unref_object GFile *app_files = NULL;
  gs_unref_object GFile *runtime_deploy = NULL;
  gs_unref_object GFile *runtime_files = NULL;
  gs_unref_object GFile *metadata = NULL;
  gs_free char *metadata_contents = NULL;
  gs_free char *runtime = NULL;
  gs_free char *default_command = NULL;
  gs_free char *runtime_ref = NULL;
  gs_free char *app_ref = NULL;
  gs_unref_keyfile GKeyFile *metakey = NULL;
  gs_free_error GError *my_error = NULL;
  gs_free_error GError *my_error2 = NULL;
  gs_unref_ptrarray GPtrArray *argv_array = NULL;
  gsize metadata_size;
  const char *directory = NULL;
  const char *command = "/bin/sh";
  int i;
  int rest_argv_start, rest_argc;

  context = g_option_context_new ("DIRECTORY [COMMAND [args...]] - Build in directory");

  rest_argc = 0;
  for (i = 1; i < argc; i++)
    {
      /* The non-option is the directory, take it out of the arguments */
      if (argv[i][0] != '-')
        {
          rest_argv_start = i;
          rest_argc = argc - i;
          argc = i;
          break;
        }
    }

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    goto out;

  if (rest_argc == 0)
    {
      usage_error (context, "DIRECTORY must be specified", error);
      goto out;
    }

  directory = argv[rest_argv_start];
  if (rest_argc >= 2)
    command = argv[rest_argv_start+1];

  app_deploy = g_file_new_for_commandline_arg (directory);

  metadata = g_file_get_child (app_deploy, "metadata");
  if (!g_file_load_contents (metadata, cancellable, &metadata_contents, &metadata_size, NULL, error))
    goto out;

  metakey = g_key_file_new ();
  if (!g_key_file_load_from_data (metakey, metadata_contents, metadata_size, 0, error))
    goto out;

  runtime = g_key_file_get_string (metakey, "Application", opt_runtime ? "runtime" : "sdk", error);
  if (runtime == NULL)
    goto out;

  runtime_ref = g_build_filename ("runtime", runtime, NULL);

  runtime_deploy = xdg_app_find_deploy_dir_for_ref (runtime_ref, cancellable, error);
  if (runtime_deploy == NULL)
    goto out;

  var = g_file_get_child (app_deploy, "var");
  if (!gs_file_ensure_directory (var, TRUE, cancellable, error))
    goto out;

  app_files = g_file_get_child (app_deploy, "files");
  runtime_files = g_file_get_child (runtime_deploy, "files");

  argv_array = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv_array, g_strdup (HELPER));

  g_ptr_array_add (argv_array, g_strdup ("-i"));
  g_ptr_array_add (argv_array, g_strdup ("-f"));
  g_ptr_array_add (argv_array, g_strdup ("-H"));

  if (opt_network)
    g_ptr_array_add (argv_array, g_strdup ("-n"));

  if (opt_x11)
    xdg_app_run_add_x11_args (argv_array);
  else
    xdg_app_run_add_no_x11_args (argv_array);

  g_ptr_array_add (argv_array, g_strdup ("-w"));
  g_ptr_array_add (argv_array, g_strdup ("-a"));
  g_ptr_array_add (argv_array, g_file_get_path (app_files));
  g_ptr_array_add (argv_array, g_strdup ("-v"));
  g_ptr_array_add (argv_array, g_file_get_path (var));
  g_ptr_array_add (argv_array, g_file_get_path (runtime_files));

  g_ptr_array_add (argv_array, g_strdup (command));
  for (i = 2; i < rest_argc; i++)
    g_ptr_array_add (argv_array, g_strdup (argv[rest_argv_start + i]));

  g_ptr_array_add (argv_array, NULL);

  g_setenv ("ACLOCAL_PATH", "/self/share/aclocal", TRUE);
  g_setenv ("C_INCLUDE_PATH", "/self/include", TRUE);
  g_setenv ("CPLUS_INCLUDE_PATH", "/self/include", TRUE);
  g_setenv ("GI_TYPELIB_PATH", "/self/lib/girepository-1.0", TRUE);
  g_setenv ("LDFLAGS", "-L/self/lib ", TRUE);
  g_setenv ("PKG_CONFIG_PATH", "/self/lib/pkgconfig:/self/share/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig", TRUE);

  g_setenv ("XDG_DATA_DIRS", "/self/share:/usr/share", TRUE);
  g_unsetenv ("LD_LIBRARY_PATH");
  g_setenv ("PATH", "/self/bin:/usr/bin", TRUE);

  if (!execv (HELPER, (char **)argv_array->pdata))
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Unable to start app");
      goto out;
    }

  /* Not actually reached... */
  ret = TRUE;

 out:
  if (context)
    g_option_context_free (context);
  return ret;
}
