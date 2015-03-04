#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"
#include "xdg-app-dbus.h"
#include "xdg-app-run.h"

static char *opt_arch;
static char *opt_branch;
static char *opt_command;
static gboolean opt_devel;
static char *opt_runtime;
static char **opt_allow;
static char **opt_forbid;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to use", "ARCH" },
  { "command", 0, 0, G_OPTION_ARG_STRING, &opt_command, "Command to run", "COMMAND" },
  { "branch", 0, 0, G_OPTION_ARG_STRING, &opt_branch, "Branch to use", "BRANCH" },
  { "devel", 'd', 0, G_OPTION_ARG_NONE, &opt_devel, "Use development runtime", NULL },
  { "runtime", 0, 0, G_OPTION_ARG_STRING, &opt_runtime, "Runtime to use", "RUNTIME" },
  { "allow", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_allow, "Environment options to set to true", "KEY" },
  { "forbid", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_forbid, "Environment options to set to false", "KEY" },
  { NULL }
};

static void
add_extension_arg (const char *directory,
		   const char *type, const char *extension, const char *arch, const char *branch,
		   GPtrArray *argv_array, GCancellable *cancellable)
{
  gs_free char *extension_ref;
  gs_unref_object GFile *deploy = NULL;
  gs_free char *full_directory = NULL;
  gboolean is_app;

  is_app = strcmp (type, "app") == 0;

  full_directory = g_build_filename (is_app ? "/self" : "/usr", directory, NULL);

  extension_ref = g_build_filename (type, extension, arch, branch, NULL);
  deploy = xdg_app_find_deploy_dir_for_ref (extension_ref, cancellable, NULL);
  if (deploy != NULL)
    {
      gs_unref_object GFile *files = g_file_get_child (deploy, "files");
      g_ptr_array_add (argv_array, g_strdup ("-b"));
      g_ptr_array_add (argv_array, g_strdup_printf ("%s=%s", full_directory, gs_file_get_path_cached (files)));
    }
}

static gboolean
add_extension_args (GKeyFile *metakey, const char *full_ref,
		    GPtrArray *argv_array, GCancellable *cancellable, GError **error)
{
  gs_strfreev gchar **groups = NULL;
  gs_strfreev gchar **parts = NULL;
  gboolean ret = FALSE;
  int i;

  ret = TRUE;

  parts = g_strsplit (full_ref, "/", 0);
  if (g_strv_length (parts) != 4)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to determine parts from ref: %s", full_ref);
      goto out;
    }

  groups = g_key_file_get_groups (metakey, NULL);
  for (i = 0; groups[i] != NULL; i++)
    {
      char *extension;

      if (g_str_has_prefix (groups[i], "Extension ") &&
	  *(extension = (groups[i] + strlen ("Extension "))) != 0)
	{
	  gs_free char *directory = g_key_file_get_string (metakey, groups[i], "directory", NULL);

	  if (directory == NULL)
	    continue;

	  if (g_key_file_get_boolean (metakey, groups[i],
				      "subdirectories", NULL))
	    {
	      gs_free char *prefix = g_strconcat (extension, ".", NULL);
	      gs_strfreev char **refs = NULL;
	      int i;

	      refs = xdg_app_list_deployed_refs (parts[0], prefix, parts[2], parts[3],
						 cancellable, error);
	      if (refs == NULL)
		goto out;

	      for (i = 0; refs[i] != NULL; i++)
		{
		  gs_free char *extended_dir = g_build_filename (directory, refs[i] + strlen (prefix), NULL);
		  add_extension_arg (extended_dir, parts[0], refs[i], parts[2], parts[3],
				     argv_array, cancellable);
		}
	    }
	  else
	    add_extension_arg (directory, parts[0], extension, parts[2], parts[3],
			       argv_array, cancellable);
	}
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
xdg_app_builtin_run (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  gs_unref_variant_builder GVariantBuilder *optbuilder = NULL;
  gs_unref_object GFile *deploy_base = NULL;
  gs_unref_object GFile *app_deploy = NULL;
  gs_unref_object GFile *app_files = NULL;
  gs_unref_object GFile *runtime_deploy = NULL;
  gs_unref_object GFile *runtime_files = NULL;
  gs_unref_object GFile *metadata = NULL;
  gs_unref_object GFile *app_id_dir = NULL;
  gs_unref_object GFile *app_id_dir_data = NULL;
  gs_unref_object GFile *app_id_dir_config = NULL;
  gs_unref_object GFile *app_id_dir_cache = NULL;
  gs_unref_object GFile *runtime_metadata = NULL;
  gs_unref_object XdgAppSessionHelper *session_helper = NULL;
  gs_free char *metadata_contents = NULL;
  gs_free char *runtime_metadata_contents = NULL;
  gs_free char *runtime = NULL;
  gs_free char *default_command = NULL;
  gs_free char *runtime_ref = NULL;
  gs_free char *app_ref = NULL;
  gs_free char *path = NULL;
  gs_unref_keyfile GKeyFile *metakey = NULL;
  gs_unref_keyfile GKeyFile *runtime_metakey = NULL;
  gs_free_error GError *my_error = NULL;
  gs_free_error GError *my_error2 = NULL;
  gs_unref_ptrarray GPtrArray *argv_array = NULL;
  gs_free char *monitor_path = NULL;
  gsize metadata_size, runtime_metadata_size;
  const char *app;
  const char *branch = "master";
  const char *command = "/bin/sh";
  int i;
  int rest_argv_start, rest_argc;

  context = g_option_context_new ("APP [args...] - Run an app");

  rest_argc = 0;
  for (i = 1; i < argc; i++)
    {
      /* The non-option is the command, take it out of the arguments */
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
      usage_error (context, "APP must be specified", error);
      goto out;
    }

  app = argv[rest_argv_start];

  if (opt_branch)
    branch = opt_branch;

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

  app_ref = xdg_app_build_app_ref (app, branch, opt_arch);

  app_deploy = xdg_app_find_deploy_dir_for_ref (app_ref, cancellable, error);
  if (app_deploy == NULL)
    goto out;

  path = g_file_get_path (app_deploy);
  g_debug ("Running application in %s", path);

  metadata = g_file_get_child (app_deploy, "metadata");
  if (!g_file_load_contents (metadata, cancellable, &metadata_contents, &metadata_size, NULL, error))
    goto out;

  metakey = g_key_file_new ();
  if (!g_key_file_load_from_data (metakey, metadata_contents, metadata_size, 0, error))
    goto out;

  argv_array = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv_array, g_strdup (HELPER));
  g_ptr_array_add (argv_array, g_strdup ("-l"));

  if (!add_extension_args (metakey, app_ref, argv_array, cancellable, error))
    goto out;

  if (opt_runtime)
    runtime = opt_runtime;
  else
    {
      runtime = g_key_file_get_string (metakey, "Application", opt_devel ? "sdk" : "runtime", error);
      if (*error)
        goto out;
    }

  runtime_ref = g_build_filename ("runtime", runtime, NULL);

  runtime_deploy = xdg_app_find_deploy_dir_for_ref (runtime_ref, cancellable, error);
  if (runtime_deploy == NULL)
    goto out;

  g_free (path);
  path = g_file_get_path (runtime_deploy);
  g_debug ("Using runtime in %s", path);

  runtime_metadata = g_file_get_child (runtime_deploy, "metadata");
  if (g_file_load_contents (runtime_metadata, cancellable, &runtime_metadata_contents, &runtime_metadata_size, NULL, NULL))
    {

      runtime_metakey = g_key_file_new ();
      if (!g_key_file_load_from_data (runtime_metakey, runtime_metadata_contents, runtime_metadata_size, 0, error))
	goto out;

      if (!add_extension_args (runtime_metakey, runtime_ref, argv_array, cancellable, error))
	goto out;
    }

  if ((app_id_dir = xdg_app_ensure_data_dir (app, cancellable, error)) == NULL)
      goto out;

  app_files = g_file_get_child (app_deploy, "files");
  runtime_files = g_file_get_child (runtime_deploy, "files");

  default_command = g_key_file_get_string (metakey, "Application", "command", error);
  if (*error)
    goto out;
  if (opt_command)
    command = opt_command;
  else
    command = default_command;

  session_helper = xdg_app_session_helper_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
								  G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
								  "org.freedesktop.XdgApp.SessionHelper",
								  "/org/freedesktop/XdgApp/SessionHelper",
								  NULL, NULL);
  if (session_helper)
    {
      if (xdg_app_session_helper_call_request_monitor_sync (session_helper,
							    &monitor_path,
							    NULL, NULL))
	{
	  g_ptr_array_add (argv_array, g_strdup ("-m"));
	  g_ptr_array_add (argv_array, monitor_path);
	}
    }

  if (!xdg_app_run_verify_environment_keys ((const char **)opt_forbid, error))
    goto out;

  if (!xdg_app_run_verify_environment_keys ((const char **)opt_allow, error))
    goto out;

  xdg_app_run_add_environment_args (argv_array, metakey,
				    (const char **)opt_allow,
				    (const char **)opt_forbid);

  g_ptr_array_add (argv_array, g_strdup ("-a"));
  g_ptr_array_add (argv_array, g_file_get_path (app_files));
  g_ptr_array_add (argv_array, g_strdup ("-I"));
  g_ptr_array_add (argv_array, g_strdup (app));
  g_ptr_array_add (argv_array, g_file_get_path (runtime_files));

  g_ptr_array_add (argv_array, g_strdup (command));
  for (i = 1; i < rest_argc; i++)
    g_ptr_array_add (argv_array, g_strdup (argv[rest_argv_start + i]));

  g_ptr_array_add (argv_array, NULL);

  g_setenv ("XDG_DATA_DIRS", "/self/share:/usr/share", TRUE);
  g_unsetenv ("LD_LIBRARY_PATH");
  g_setenv ("PATH", "/self/bin:/usr/bin", TRUE);

  app_id_dir_data = g_file_get_child (app_id_dir, "data");
  app_id_dir_config = g_file_get_child (app_id_dir, "config");
  app_id_dir_cache = g_file_get_child (app_id_dir, "cache");
  g_setenv ("XDG_DATA_HOME", gs_file_get_path_cached (app_id_dir_data), TRUE);
  g_setenv ("XDG_CONFIG_HOME", gs_file_get_path_cached (app_id_dir_config), TRUE);
  g_setenv ("XDG_CACHE_HOME", gs_file_get_path_cached (app_id_dir_cache), TRUE);

  xdg_app_run_in_transient_unit (app);

  if (execv (HELPER, (char **)argv_array->pdata) == -1)
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
