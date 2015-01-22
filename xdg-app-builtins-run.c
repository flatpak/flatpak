#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "libgsystem.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static char *opt_arch;
static char *opt_branch;
static char *opt_command;
static gboolean opt_devel;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to use", "ARCH" },
  { "command", 0, 0, G_OPTION_ARG_STRING, &opt_command, "Command to run", "COMMAND" },
  { "branch", 0, 0, G_OPTION_ARG_STRING, &opt_branch, "Branch to use", "BRANCH" },
  { "devel", 'd', 0, G_OPTION_ARG_NONE, &opt_devel, "Use development runtime", NULL },
  { NULL }
};

static char *
extract_unix_path_from_dbus_address (const char *address)
{
  const char *path, *path_end;

  if (address == NULL)
    return NULL;

  if (!g_str_has_prefix (address, "unix:"))
    return NULL;

  path = strstr (address, "path=");
  if (path == NULL)
    return NULL;
  path += strlen ("path=");
  path_end = path;
  while (*path_end != 0 && *path_end != ',')
    path_end++;

  return g_strndup (path, path_end - path);
}

void
xdg_app_run_add_x11_args (GPtrArray *argv_array)
{
  char *x11_socket = NULL;
  const char *display = g_getenv ("DISPLAY");

  if (display && display[0] == ':' && g_ascii_isdigit (display[1]))
    {
      const char *display_nr = &display[1];
      const char *display_nr_end = display_nr;
      gs_free char *d = NULL;

      while (g_ascii_isdigit (*display_nr_end))
        display_nr_end++;

      d = g_strndup (display_nr, display_nr_end - display_nr);
      x11_socket = g_strdup_printf ("/tmp/.X11-unix/X%s", d);

      g_ptr_array_add (argv_array, g_strdup ("-x"));
      g_ptr_array_add (argv_array, x11_socket);
    }
}

void
xdg_app_run_add_wayland_args (GPtrArray *argv_array)
{
  char *wayland_socket = g_build_filename (g_get_user_runtime_dir (), "wayland-0", NULL);
  if (g_file_test (wayland_socket, G_FILE_TEST_EXISTS))
    {
      g_ptr_array_add (argv_array, g_strdup ("-y"));
      g_ptr_array_add (argv_array, wayland_socket);
    }
  else
    g_free (wayland_socket);
}

void
xdg_app_run_add_no_x11_args (GPtrArray *argv_array)
{
  g_unsetenv ("DISPLAY");
}

void
xdg_app_run_add_pulseaudio_args (GPtrArray *argv_array)
{
  char *pulseaudio_socket = g_build_filename (g_get_user_runtime_dir (), "pulse/native", NULL);
  if (g_file_test (pulseaudio_socket, G_FILE_TEST_EXISTS))
    {
      g_ptr_array_add (argv_array, g_strdup ("-p"));
      g_ptr_array_add (argv_array, pulseaudio_socket);
    }
}

void
xdg_app_run_add_system_dbus_args (GPtrArray *argv_array)
{
  const char *dbus_address = g_getenv ("DBUS_SYSTEM_BUS_ADDRESS");
  char *dbus_system_socket = NULL;

  dbus_system_socket = extract_unix_path_from_dbus_address (dbus_address);
  if (dbus_system_socket == NULL &&
      g_file_test ("/var/run/dbus/system_bus_socket", G_FILE_TEST_EXISTS))
    {
      dbus_system_socket = g_strdup ("/var/run/dbus/system_bus_socket");
    }

  if (dbus_system_socket != NULL)
    {
      g_ptr_array_add (argv_array, g_strdup ("-D"));
      g_ptr_array_add (argv_array, dbus_system_socket);
    }
}

void
xdg_app_run_add_session_dbus_args (GPtrArray *argv_array)
{
  const char *dbus_address = g_getenv ("DBUS_SESSION_BUS_ADDRESS");
  char *dbus_session_socket = NULL;

  dbus_session_socket = extract_unix_path_from_dbus_address (dbus_address);
  if (dbus_session_socket != NULL)
    {
      g_ptr_array_add (argv_array, g_strdup ("-d"));
      g_ptr_array_add (argv_array, dbus_session_socket);
    }
}

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
  gs_unref_object GFile *runtime_metadata = NULL;
  gs_free char *metadata_contents = NULL;
  gs_free char *runtime_metadata_contents = NULL;
  gs_free char *runtime = NULL;
  gs_free char *default_command = NULL;
  gs_free char *runtime_ref = NULL;
  gs_free char *app_ref = NULL;
  gs_unref_keyfile GKeyFile *metakey = NULL;
  gs_unref_keyfile GKeyFile *runtime_metakey = NULL;
  gs_free_error GError *my_error = NULL;
  gs_free_error GError *my_error2 = NULL;
  gs_unref_ptrarray GPtrArray *argv_array = NULL;
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

  app_ref = xdg_app_build_app_ref (app, branch, opt_arch);

  user_dir = xdg_app_dir_get_user ();

  app_deploy = xdg_app_find_deploy_dir_for_ref (app_ref, cancellable, error);
  if (app_deploy == NULL)
    goto out;

  metadata = g_file_get_child (app_deploy, "metadata");
  if (!g_file_load_contents (metadata, cancellable, &metadata_contents, &metadata_size, NULL, error))
    goto out;

  metakey = g_key_file_new ();
  if (!g_key_file_load_from_data (metakey, metadata_contents, metadata_size, 0, error))
    goto out;

  argv_array = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv_array, g_strdup (HELPER));

  if (!add_extension_args (metakey, app_ref, argv_array, cancellable, error))
    goto out;

  runtime = g_key_file_get_string (metakey, "Application", opt_devel ? "sdk" : "runtime", error);
  if (*error)
    goto out;

  runtime_ref = g_build_filename ("runtime", runtime, NULL);

  runtime_deploy = xdg_app_find_deploy_dir_for_ref (runtime_ref, cancellable, error);
  if (runtime_deploy == NULL)
    goto out;

  runtime_metadata = g_file_get_child (runtime_deploy, "metadata");
  if (g_file_load_contents (runtime_metadata, cancellable, &runtime_metadata_contents, &runtime_metadata_size, NULL, error))
    {

      runtime_metakey = g_key_file_new ();
      if (!g_key_file_load_from_data (runtime_metakey, runtime_metadata_contents, runtime_metadata_size, 0, error))
	goto out;

      if (!add_extension_args (runtime_metakey, runtime_ref, argv_array, cancellable, error))
	goto out;
    }

  if (!xdg_app_dir_ensure_path (user_dir, cancellable, error))
    goto out;

  var = xdg_app_dir_get_app_data (user_dir, app);
  if (!gs_file_ensure_directory (var, TRUE, cancellable, error))
    goto out;

  var_tmp = g_file_get_child (var, "tmp");
  var_run = g_file_get_child (var, "run");

  if (!g_file_make_symbolic_link (var_tmp, "/tmp", cancellable, &my_error) &&
      !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      g_propagate_error (error, my_error);
      my_error = NULL;
      goto out;
    }

  if (!g_file_make_symbolic_link (var_run, "/run", cancellable, &my_error2) &&
      !g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      g_propagate_error (error, my_error2);
      my_error2 = NULL;
      goto out;
    }

  app_files = g_file_get_child (app_deploy, "files");
  runtime_files = g_file_get_child (runtime_deploy, "files");

  default_command = g_key_file_get_string (metakey, "Application", "command", error);
  if (*error)
    goto out;
  if (opt_command)
    command = opt_command;
  else
    command = default_command;

  if (g_key_file_get_boolean (metakey, "Environment", "ipc", NULL))
    g_ptr_array_add (argv_array, g_strdup ("-i"));

  if (g_key_file_get_boolean (metakey, "Environment", "host-fs", NULL))
    g_ptr_array_add (argv_array, g_strdup ("-f"));

  if (g_key_file_get_boolean (metakey, "Environment", "homedir", NULL))
    g_ptr_array_add (argv_array, g_strdup ("-H"));

  if (g_key_file_get_boolean (metakey, "Environment", "network", NULL))
    g_ptr_array_add (argv_array, g_strdup ("-n"));

  if (g_key_file_get_boolean (metakey, "Environment", "x11", NULL))
    xdg_app_run_add_x11_args (argv_array);
  else
    xdg_app_run_add_no_x11_args (argv_array);

  if (g_key_file_get_boolean (metakey, "Environment", "wayland", NULL))
    xdg_app_run_add_wayland_args (argv_array);

  if (g_key_file_get_boolean (metakey, "Environment", "pulseaudio", NULL))
    xdg_app_run_add_pulseaudio_args (argv_array);

  if (g_key_file_get_boolean (metakey, "Environment", "system-dbus", NULL))
    xdg_app_run_add_system_dbus_args (argv_array);

  if (g_key_file_get_boolean (metakey, "Environment", "session-dbus", NULL))
    xdg_app_run_add_session_dbus_args (argv_array);

  g_ptr_array_add (argv_array, g_strdup ("-a"));
  g_ptr_array_add (argv_array, g_file_get_path (app_files));
  g_ptr_array_add (argv_array, g_strdup ("-v"));
  g_ptr_array_add (argv_array, g_file_get_path (var));
  g_ptr_array_add (argv_array, g_file_get_path (runtime_files));

  g_ptr_array_add (argv_array, g_strdup (command));
  for (i = 1; i < rest_argc; i++)
    g_ptr_array_add (argv_array, g_strdup (argv[rest_argv_start + i]));

  g_ptr_array_add (argv_array, NULL);

  g_setenv ("XDG_DATA_DIRS", "/self/share:/usr/share", TRUE);
  g_unsetenv ("LD_LIBRARY_PATH");
  g_setenv ("PATH", "/self/bin:/usr/bin", TRUE);

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
