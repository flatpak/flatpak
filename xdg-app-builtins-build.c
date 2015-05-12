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

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"
#include "xdg-app-run.h"

static gboolean opt_runtime;
static char **opt_allow;
static char **opt_forbid;

static GOptionEntry options[] = {
  { "runtime", 'r', 0, G_OPTION_ARG_NONE, &opt_runtime, "Use non-devel runtime", NULL },
  { "allow", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_allow, "Environment options to set to true", "KEY" },
  { "forbid", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_forbid, "Environment options to set to false", "KEY" },
  { NULL }
};

gboolean
xdg_app_builtin_build (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  GOptionContext *context;
  gboolean ret = FALSE;
  g_autoptr(XdgAppDir) user_dir = NULL;
  g_autoptr(GVariantBuilder) optbuilder = NULL;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) var = NULL;
  g_autoptr(GFile) var_tmp = NULL;
  g_autoptr(GFile) var_run = NULL;
  g_autoptr(GFile) app_deploy = NULL;
  g_autoptr(GFile) app_files = NULL;
  g_autoptr(GFile) runtime_deploy = NULL;
  g_autoptr(GFile) runtime_files = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_autofree char *metadata_contents = NULL;
  g_autofree char *runtime = NULL;
  g_autofree char *default_command = NULL;
  g_autofree char *runtime_ref = NULL;
  g_autofree char *app_ref = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr (GError) my_error = NULL;
  g_autoptr (GError) my_error2 = NULL;
  g_autoptr(GPtrArray) argv_array = NULL;
  glnx_strfreev char **envp = NULL;
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

  if (!xdg_app_run_verify_environment_keys ((const char **)opt_forbid, error))
    goto out;

  if (!xdg_app_run_verify_environment_keys ((const char **)opt_allow, error))
    goto out;

  xdg_app_run_add_environment_args (argv_array, NULL, metakey,
				    (const char **)opt_allow,
				    (const char **)opt_forbid);

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

  envp = xdg_app_run_get_minimal_env (TRUE);

  if (!execve (HELPER, (char **)argv_array->pdata, envp))
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
