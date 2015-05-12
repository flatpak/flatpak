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
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "libgsystem.h"
#include "libglnx/libglnx.h"

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

typedef struct {
  char *name;
  char *value;
} EnvVar;

static void
free_envvar (void *p)
{
  EnvVar *v = (EnvVar *) p;
  g_free (v->name);
  g_free (v->value);
  g_free (v);
}

static void
add_extension_arg (const char *directory,
		   const char *type, const char *extension, const char *arch, const char *branch,
		   GPtrArray *argv_array, GCancellable *cancellable)
{
  g_autofree char *extension_ref;
  g_autoptr(GFile) deploy = NULL;
  g_autofree char *full_directory = NULL;
  gboolean is_app;

  is_app = strcmp (type, "app") == 0;

  full_directory = g_build_filename (is_app ? "/self" : "/usr", directory, NULL);

  extension_ref = g_build_filename (type, extension, arch, branch, NULL);
  deploy = xdg_app_find_deploy_dir_for_ref (extension_ref, cancellable, NULL);
  if (deploy != NULL)
    {
      g_autoptr(GFile) files = g_file_get_child (deploy, "files");
      g_ptr_array_add (argv_array, g_strdup ("-b"));
      g_ptr_array_add (argv_array, g_strdup_printf ("%s=%s", full_directory, gs_file_get_path_cached (files)));
    }
}

static gboolean
add_extension_args (GKeyFile *metakey, const char *full_ref,
		    GPtrArray *argv_array, GCancellable *cancellable, GError **error)
{
  glnx_strfreev gchar **groups = NULL;
  glnx_strfreev gchar **parts = NULL;
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
	  g_autofree char *directory = g_key_file_get_string (metakey, groups[i], "directory", NULL);

	  if (directory == NULL)
	    continue;

	  if (g_key_file_get_boolean (metakey, groups[i],
				      "subdirectories", NULL))
	    {
	      g_autofree char *prefix = g_strconcat (extension, ".", NULL);
	      glnx_strfreev char **refs = NULL;
	      int i;

	      refs = xdg_app_list_deployed_refs (parts[0], prefix, parts[2], parts[3],
						 cancellable, error);
	      if (refs == NULL)
		goto out;

	      for (i = 0; refs[i] != NULL; i++)
		{
		  g_autofree char *extended_dir = g_build_filename (directory, refs[i] + strlen (prefix), NULL);
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

static void
add_env_overrides (GKeyFile *metakey, GPtrArray *env_array)
{
  gsize i, keys_count;
  /* Only free the array of keys, not the actual values */
  g_autofree char **keys;

  if (!g_key_file_has_group (metakey, "Vars"))
    return;

  keys = g_key_file_get_keys (metakey, "Vars", &keys_count, NULL);
  if (!keys)
    return;

  for (i = 0; i < keys_count; i++)
    {
        EnvVar *var = malloc (sizeof (EnvVar));
        var->name = keys[i];
        var->value = g_key_file_get_string (metakey, "Vars", keys[i], NULL);

        g_ptr_array_add (env_array, var);
    }
}

static void
dbus_spawn_child_setup (gpointer user_data)
{
  int fd = GPOINTER_TO_INT (user_data);
  fcntl (fd, F_SETFD, 0);
}

gboolean
xdg_app_builtin_run (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  g_autoptr(GVariantBuilder) optbuilder = NULL;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(XdgAppDeploy) app_deploy = NULL;
  g_autoptr(XdgAppDeploy) runtime_deploy = NULL;
  g_autoptr(GFile) app_files = NULL;
  g_autoptr(GFile) runtime_files = NULL;
  g_autoptr(GFile) app_id_dir = NULL;
  g_autoptr(GFile) app_id_dir_data = NULL;
  g_autoptr(GFile) app_id_dir_config = NULL;
  g_autoptr(GFile) app_id_dir_cache = NULL;
  g_autoptr(XdgAppSessionHelper) session_helper = NULL;
  g_autofree char *runtime = NULL;
  g_autofree char *default_command = NULL;
  g_autofree char *runtime_ref = NULL;
  g_autofree char *app_ref = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GKeyFile) runtime_metakey = NULL;
  g_autoptr(GPtrArray) argv_array = NULL;
  g_autoptr(GPtrArray) env_array = NULL;
  g_autoptr(GPtrArray) dbus_proxy_argv = NULL;
  g_autofree char *monitor_path = NULL;
  const char *app;
  const char *branch = "master";
  const char *command = "/bin/sh";
  int i;
  int rest_argv_start, rest_argc;
  int sync_proxy_pipes[2];

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

  app_deploy = xdg_app_find_deploy_for_ref (app_ref, cancellable, error);
  if (app_deploy == NULL)
    goto out;

  metakey = xdg_app_deploy_get_metadata (app_deploy);

  argv_array = g_ptr_array_new_with_free_func (g_free);
  dbus_proxy_argv = g_ptr_array_new_with_free_func (g_free);
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

  runtime_deploy = xdg_app_find_deploy_for_ref (runtime_ref, cancellable, error);
  if (runtime_deploy == NULL)
    goto out;

  env_array = g_ptr_array_new_with_free_func (free_envvar);

  runtime_metakey = xdg_app_deploy_get_metadata (runtime_deploy);

  if (!add_extension_args (runtime_metakey, runtime_ref, argv_array, cancellable, error))
    goto out;

  add_env_overrides (runtime_metakey, env_array);

  /* Load application environment overrides *after* runtime */
  add_env_overrides (metakey, env_array);

  if ((app_id_dir = xdg_app_ensure_data_dir (app, cancellable, error)) == NULL)
      goto out;

  app_files = xdg_app_deploy_get_files (app_deploy);
  runtime_files = xdg_app_deploy_get_files (runtime_deploy);

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

  xdg_app_run_add_environment_args (argv_array, dbus_proxy_argv, metakey,
				    (const char **)opt_allow,
				    (const char **)opt_forbid);

  g_ptr_array_add (argv_array, g_strdup ("-b"));
  g_ptr_array_add (argv_array, g_strdup_printf ("/run/host/fonts=%s", SYSTEM_FONTS_DIR));

  /* Must run this before spawning the dbus proxy, to ensure it
     ends up in the app cgroup */
  xdg_app_run_in_transient_unit (app);

  if (dbus_proxy_argv->len > 0)
    {
      char x;

      if (pipe (sync_proxy_pipes) < 0)
	{
	  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Unable to create sync pipe");
	  goto out;
	}

      g_ptr_array_insert (dbus_proxy_argv, 0, g_strdup ("xdg-dbus-proxy"));
      g_ptr_array_insert (dbus_proxy_argv, 1, g_strdup_printf ("--fd=%d", sync_proxy_pipes[1]));

      g_ptr_array_add (dbus_proxy_argv, NULL); /* NULL terminate */

      if (!g_spawn_async (NULL,
			  (char **)dbus_proxy_argv->pdata,
			  NULL,
			  G_SPAWN_SEARCH_PATH,
			  dbus_spawn_child_setup,
			  GINT_TO_POINTER (sync_proxy_pipes[1]),
			  NULL, error))
	goto out;

      close (sync_proxy_pipes[1]);

      /* Sync with proxy, i.e. wait until its listening on the sockets */
      if (read (sync_proxy_pipes[0], &x, 1) != 1)
	{
	  g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Failed to sync with dbus proxy");
	  goto out;
	}

      g_ptr_array_add (argv_array, g_strdup ("-S"));
      g_ptr_array_add (argv_array, g_strdup_printf ("%d", sync_proxy_pipes[0]));
    }

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

  for (i = 0; i < env_array->len; i++)
    {
       EnvVar *var = g_ptr_array_index (env_array, i);
       if (!var->value || !var->value[0])
         g_unsetenv (var->name);
       else
         g_setenv (var->name, var->value, TRUE);
    }

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
