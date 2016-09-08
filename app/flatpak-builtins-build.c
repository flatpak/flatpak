/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
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

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils.h"
#include "flatpak-run.h"

static gboolean opt_runtime;
static char *opt_build_dir;
static char **opt_bind_mounts;

static GOptionEntry options[] = {
  { "runtime", 'r', 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Use Platform runtime rather than Sdk"), NULL },
  { "bind-mount", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_bind_mounts, N_("Add bind mount"), N_("DEST=SRC") },
  { "build-dir", 0, 0, G_OPTION_ARG_STRING, &opt_build_dir, N_("Start build in this directory"), N_("DIR") },
  { NULL }
};

static void
add_args (GPtrArray *argv_array, ...)
{
  va_list args;
  const gchar *arg;

  va_start (args, argv_array);
  while ((arg = va_arg (args, const gchar *)))
    g_ptr_array_add (argv_array, g_strdup (arg));
  va_end (args);
}

gboolean
flatpak_builtin_build (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDeploy) runtime_deploy = NULL;
  g_autoptr(GFile) var = NULL;
  g_autoptr(GFile) usr = NULL;
  g_autoptr(GFile) app_deploy = NULL;
  g_autoptr(GFile) app_files = NULL;
  g_autoptr(GFile) runtime_files = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_autofree char *metadata_contents = NULL;
  g_autofree char *runtime = NULL;
  g_autofree char *runtime_ref = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GKeyFile) runtime_metakey = NULL;
  g_autoptr(GPtrArray) argv_array = NULL;
  g_auto(GStrv) envp = NULL;
  gsize metadata_size;
  const char *directory = NULL;
  const char *command = "/bin/sh";
  g_autofree char *app_id = NULL;
  int i;
  int rest_argv_start, rest_argc;
  g_autoptr(FlatpakContext) arg_context = NULL;
  g_autoptr(FlatpakContext) app_context = NULL;
  gboolean custom_usr;
  g_auto(GStrv) runtime_ref_parts = NULL;

  context = g_option_context_new (_("DIRECTORY [COMMAND [args...]] - Build in directory"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

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

  arg_context = flatpak_context_new ();
  g_option_context_add_group (context, flatpak_context_get_options (arg_context));

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (rest_argc == 0)
    return usage_error (context, _("DIRECTORY must be specified"), error);

  directory = argv[rest_argv_start];
  if (rest_argc >= 2)
    command = argv[rest_argv_start + 1];

  app_deploy = g_file_new_for_commandline_arg (directory);

  metadata = g_file_get_child (app_deploy, "metadata");
  if (!g_file_load_contents (metadata, cancellable, &metadata_contents, &metadata_size, NULL, error))
    return FALSE;

  metakey = g_key_file_new ();
  if (!g_key_file_load_from_data (metakey, metadata_contents, metadata_size, 0, error))
    return FALSE;

  app_id = g_key_file_get_string (metakey, "Application", "name", error);
  if (app_id == NULL)
    return FALSE;

  runtime = g_key_file_get_string (metakey, "Application", opt_runtime ? "runtime" : "sdk", error);
  if (runtime == NULL)
    return FALSE;

  runtime_ref = g_build_filename ("runtime", runtime, NULL);

  runtime_ref_parts = flatpak_decompose_ref (runtime_ref, error);
  if (runtime_ref_parts == NULL)
    return FALSE;

  runtime_deploy = flatpak_find_deploy_for_ref (runtime_ref, cancellable, error);
  if (runtime_deploy == NULL)
    return FALSE;

  runtime_metakey = flatpak_deploy_get_metadata (runtime_deploy);

  var = g_file_get_child (app_deploy, "var");
  if (!flatpak_mkdir_p (var, cancellable, error))
    return FALSE;

  app_files = g_file_get_child (app_deploy, "files");

  argv_array = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv_array, g_strdup (flatpak_get_bwrap ()));

  custom_usr = FALSE;
  usr = g_file_get_child (app_deploy, "usr");
  if (g_file_query_exists (usr, cancellable))
    {
      custom_usr = TRUE;
      runtime_files = g_object_ref (usr);
    }
  else
    {
      runtime_files = flatpak_deploy_get_files (runtime_deploy);
    }

  add_args (argv_array,
            custom_usr ? "--bind" : "--ro-bind", flatpak_file_get_path_cached (runtime_files), "/usr",
            "--bind", flatpak_file_get_path_cached (app_files), "/app",
            NULL);

  if (!flatpak_run_setup_base_argv (argv_array, NULL, runtime_files, NULL, runtime_ref_parts[2],
                                    FLATPAK_RUN_FLAG_DEVEL | FLATPAK_RUN_FLAG_NO_SESSION_HELPER,
                                    error))
    return FALSE;

  /* After setup_base to avoid conflicts with /var symlinks */
  add_args (argv_array,
            "--bind", flatpak_file_get_path_cached (var), "/var",
            NULL);

  app_context = flatpak_context_new ();
  if (!flatpak_context_load_metadata (app_context, runtime_metakey, error))
    return FALSE;
  if (!flatpak_context_load_metadata (app_context, metakey, error))
    return FALSE;
  flatpak_context_allow_host_fs (app_context);
  flatpak_context_merge (app_context, arg_context);

  if (!flatpak_run_add_app_info_args (argv_array,
                                      NULL,
                                      app_files,
                                      runtime_files,
                                      app_id, NULL,
                                      runtime_ref,
                                      app_context,
                                      NULL,
                                      error))
    return FALSE;

  envp = flatpak_run_get_minimal_env (TRUE);
  envp = flatpak_run_apply_env_vars (envp, app_context);
  flatpak_run_add_environment_args (argv_array, NULL, &envp, NULL, NULL, app_id,
                                    app_context, NULL);

  if (!custom_usr &&
      !flatpak_run_add_extension_args (argv_array, runtime_metakey, runtime_ref, cancellable, error))
    return FALSE;

  for (i = 0; opt_bind_mounts != NULL && opt_bind_mounts[i] != NULL; i++)
    {
      char *split = strchr (opt_bind_mounts[i], '=');
      if (split == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       _("Missing '=' in bind mount option '%s'"), opt_bind_mounts[i]);
          return FALSE;
        }

      *split++ = 0;
      add_args (argv_array,
                "--bind", split, opt_bind_mounts[i],
                NULL);
    }

  if (opt_build_dir != NULL)
    {
      add_args (argv_array,
                "--chdir", opt_build_dir,
                NULL);
    }

  g_ptr_array_add (argv_array, g_strdup (command));
  for (i = 2; i < rest_argc; i++)
    g_ptr_array_add (argv_array, g_strdup (argv[rest_argv_start + i]));

  g_ptr_array_add (argv_array, NULL);

  if (execvpe (flatpak_get_bwrap (), (char **) argv_array->pdata, envp) == -1)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   _("Unable to start app"));
      return FALSE;
    }

  /* Not actually reached... */
  return TRUE;
}

gboolean
flatpak_complete_build (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new ("");

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* DIR */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      flatpak_complete_dir (completion);
      break;
    }

  return TRUE;
}
