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
#include <unistd.h>
#include <string.h>

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static char *opt_arch;
static char *opt_var;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to use", "ARCH" },
  { "var", 'v', 0, G_OPTION_ARG_STRING, &opt_var, "Initialize var from named runtime", "RUNTIME" },
  { NULL }
};

gboolean
xdg_app_builtin_build_init (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  g_autoptr(GFile) runtime_deploy_base = NULL;
  g_autoptr(GFile) sdk_deploy_base = NULL;
  g_autoptr(GFile) var_deploy_base = NULL;
  g_autoptr(GFile) var_deploy_files = NULL;
  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) files_dir = NULL;
  g_autoptr(GFile) var_dir = NULL;
  g_autoptr(GFile) var_tmp_dir = NULL;
  g_autoptr(GFile) var_run_dir = NULL;
  g_autoptr(GFile) metadata_file = NULL;
  const char *app_id;
  const char *directory;
  const char *sdk;
  const char *runtime;
  const char *branch = "master";
  g_autofree char *runtime_ref = NULL;
  g_autofree char *var_ref = NULL;
  g_autofree char *sdk_ref = NULL;
  g_autofree char *metadata_contents = NULL;

  context = g_option_context_new ("DIRECTORY APPNAME SDK RUNTIME [BRANCH] - Initialize a directory for building");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    goto out;

  if (argc < 5)
    {
      usage_error (context, "RUNTIME must be specified", error);
      goto out;
    }

  directory = argv[1];
  app_id = argv[2];
  sdk = argv[3];
  runtime = argv[4];
  if (argc >= 6)
    branch = argv[5];

  if (!xdg_app_is_valid_name (app_id))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "'%s' is not a valid application name", app_id);
      goto out;
    }

  if (!xdg_app_is_valid_name (runtime))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "'%s' is not a valid runtime name", runtime);
      goto out;
    }

  if (!xdg_app_is_valid_name (sdk))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "'%s' is not a valid sdk name", sdk);
      goto out;
    }

  if (!xdg_app_is_valid_branch (branch))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "'%s' is not a valid branch name", branch);
      goto out;
    }

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
                                      "name=%s\n"
                                      "runtime=%s\n"
                                      "sdk=%s\n",
                                      app_id, runtime_ref, sdk_ref);
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
