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
static char *opt_sdk_dir;
static char *opt_metadata;
static gboolean opt_die_with_parent;

static GOptionEntry options[] = {
  { "runtime", 'r', 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Use Platform runtime rather than Sdk"), NULL },
  { "bind-mount", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_bind_mounts, N_("Add bind mount"), N_("DEST=SRC") },
  { "build-dir", 0, 0, G_OPTION_ARG_STRING, &opt_build_dir, N_("Start build in this directory"), N_("DIR") },
  { "sdk-dir", 0, 0, G_OPTION_ARG_STRING, &opt_sdk_dir, N_("Where to look for custom sdk dir (defaults to 'usr')"), N_("DIR") },
  { "metadata", 0, 0, G_OPTION_ARG_STRING, &opt_metadata, N_("Use alternative file for the metadata"), N_("FILE") },
  { "die-with-parent", 'p', 0, G_OPTION_ARG_NONE, &opt_die_with_parent, N_("Kill processes when the parent process dies"), NULL },
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
  g_autoptr(FlatpakDeploy) extensionof_deploy = NULL;
  g_autoptr(GFile) var = NULL;
  g_autoptr(GFile) usr = NULL;
  g_autoptr(GFile) res_deploy = NULL;
  g_autoptr(GFile) res_files = NULL;
  g_autoptr(GFile) app_files = NULL;
  gboolean app_files_ro = FALSE;
  g_autoptr(GFile) runtime_files = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_autofree char *metadata_contents = NULL;
  g_autofree char *runtime = NULL;
  g_autofree char *runtime_ref = NULL;
  g_autofree char *extensionof_ref = NULL;
  g_autofree char *extension_point = NULL;
  g_autofree char *extension_tmpfs_point = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GKeyFile) runtime_metakey = NULL;
  g_autoptr(GPtrArray) argv_array = NULL;
  g_auto(GStrv) envp = NULL;
  gsize metadata_size;
  const char *directory = NULL;
  const char *command = "/bin/sh";
  g_autofree char *id = NULL;
  int i;
  int rest_argv_start, rest_argc;
  g_autoptr(FlatpakContext) arg_context = NULL;
  g_autoptr(FlatpakContext) app_context = NULL;
  gboolean custom_usr;
  g_auto(GStrv) runtime_ref_parts = NULL;
  FlatpakRunFlags run_flags;
  const char *group = NULL;
  const char *runtime_key = NULL;
  const char *dest = NULL;
  gboolean is_app = FALSE;
  gboolean is_extension = FALSE;
  gboolean is_app_extension = FALSE;
  g_autofree char *app_info_path = NULL;

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

  res_deploy = g_file_new_for_commandline_arg (directory);
  metadata = g_file_get_child (res_deploy, opt_metadata ? opt_metadata : "metadata");

  if (!g_file_query_exists (res_deploy, NULL) ||
      !g_file_query_exists (metadata, NULL))
    return flatpak_fail (error, _("Build directory %s not initialized, use flatpak build-init"), directory);

  if (!g_file_load_contents (metadata, cancellable, &metadata_contents, &metadata_size, NULL, error))
    return FALSE;

  metakey = g_key_file_new ();
  if (!g_key_file_load_from_data (metakey, metadata_contents, metadata_size, 0, error))
    return FALSE;

  if (g_key_file_has_group (metakey, FLATPAK_METADATA_GROUP_APPLICATION))
    {
      group = FLATPAK_METADATA_GROUP_APPLICATION;
      is_app = TRUE;
    }
  else if (g_key_file_has_group (metakey, FLATPAK_METADATA_GROUP_RUNTIME))
    {
      group = FLATPAK_METADATA_GROUP_RUNTIME;
    }
  else
    return flatpak_fail (error, _("metadata invalid, not application or runtime"));

  extensionof_ref = g_key_file_get_string (metakey,
                                           FLATPAK_METADATA_GROUP_EXTENSION_OF,
                                           FLATPAK_METADATA_KEY_REF, NULL);
  if (extensionof_ref != NULL)
    {
      is_extension = TRUE;
      if (g_str_has_prefix (extensionof_ref, "app/"))
        is_app_extension = TRUE;
    }


  id = g_key_file_get_string (metakey, group, FLATPAK_METADATA_KEY_NAME, error);
  if (id == NULL)
    return FALSE;

  if (opt_runtime)
    runtime_key = FLATPAK_METADATA_KEY_RUNTIME;
  else
    runtime_key = FLATPAK_METADATA_KEY_SDK;

  runtime = g_key_file_get_string (metakey, group, runtime_key, error);
  if (runtime == NULL)
    return FALSE;

  runtime_ref = g_build_filename ("runtime", runtime, NULL);

  runtime_ref_parts = flatpak_decompose_ref (runtime_ref, error);
  if (runtime_ref_parts == NULL)
    return FALSE;

  custom_usr = FALSE;
  usr = g_file_get_child (res_deploy,  opt_sdk_dir ? opt_sdk_dir : "usr");
  if (g_file_query_exists (usr, cancellable))
    {
      custom_usr = TRUE;
      runtime_files = g_object_ref (usr);
    }
  else
    {
      runtime_deploy = flatpak_find_deploy_for_ref (runtime_ref, cancellable, error);
      if (runtime_deploy == NULL)
        return FALSE;

      runtime_metakey = flatpak_deploy_get_metadata (runtime_deploy);

      runtime_files = flatpak_deploy_get_files (runtime_deploy);
    }

  var = g_file_get_child (res_deploy, "var");
  if (!flatpak_mkdir_p (var, cancellable, error))
    return FALSE;

  res_files = g_file_get_child (res_deploy, "files");

  if (is_app)
    app_files = g_object_ref (res_files);
  else if (is_extension)
    {
      g_autoptr(GKeyFile) x_metakey = NULL;
      g_autofree char *x_group = NULL;
      g_autofree char *x_dir = NULL;
      g_autofree char *x_subdir_suffix = NULL;
      char *x_subdir = NULL;
      g_autofree char *bare_extension_point = NULL;

      extensionof_deploy = flatpak_find_deploy_for_ref (extensionof_ref, cancellable, error);
      if (extensionof_deploy == NULL)
        return FALSE;

      x_metakey = flatpak_deploy_get_metadata (extensionof_deploy);

      x_group = g_strconcat (FLATPAK_METADATA_GROUP_PREFIX_EXTENSION, id, NULL);
      if (!g_key_file_has_group (x_metakey, x_group))
        {
          /* Failed, look for subdirectories=true parent */
          char *last_dot = strrchr (id, '.');

          if (last_dot != NULL)
            {
              char *parent_id = g_strndup (id, last_dot - id);
              g_free (x_group);
              x_group = g_strconcat (FLATPAK_METADATA_GROUP_PREFIX_EXTENSION,
                                     parent_id, NULL);
              if (g_key_file_get_boolean (x_metakey, x_group,
                                          FLATPAK_METADATA_KEY_SUBDIRECTORIES,
                                          NULL))
                x_subdir = last_dot + 1;
            }

          if (x_subdir == NULL)
            return flatpak_fail (error, _("No extension point matching %s in %s"), id, extensionof_ref);
        }

      x_dir = g_key_file_get_string (x_metakey, x_group,
                                     FLATPAK_METADATA_KEY_DIRECTORY, error);
      if (x_dir == NULL)
        return FALSE;

      x_subdir_suffix = g_key_file_get_string (x_metakey, x_group,
                                               FLATPAK_METADATA_KEY_SUBDIRECTORY_SUFFIX,
                                               NULL);

      if (is_app_extension)
        {
          app_files = flatpak_deploy_get_files (extensionof_deploy);
          app_files_ro = TRUE;
          if (x_subdir != NULL)
            extension_tmpfs_point = g_build_filename ("/app", x_dir, NULL);
          bare_extension_point = g_build_filename ("/app", x_dir, x_subdir, NULL);
        }
      else
        {
          if (x_subdir != NULL)
            extension_tmpfs_point = g_build_filename ("/usr", x_dir, NULL);
          bare_extension_point = g_build_filename ("/usr", x_dir, x_subdir, NULL);
        }

      extension_point = g_build_filename (bare_extension_point, x_subdir_suffix, NULL);
    }

  argv_array = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (argv_array, g_strdup (flatpak_get_bwrap ()));

  run_flags =
    FLATPAK_RUN_FLAG_DEVEL | FLATPAK_RUN_FLAG_NO_SESSION_HELPER |
    FLATPAK_RUN_FLAG_SET_PERSONALITY;
  if (opt_die_with_parent)
    run_flags |= FLATPAK_RUN_FLAG_DIE_WITH_PARENT;
  if (custom_usr)
    run_flags |= FLATPAK_RUN_FLAG_WRITABLE_ETC;

  /* Unless manually specified, we disable dbus proxy */
  if (!flatpak_context_get_needs_session_bus_proxy (arg_context))
    run_flags |= FLATPAK_RUN_FLAG_NO_SESSION_BUS_PROXY;

  if (!flatpak_context_get_needs_system_bus_proxy (arg_context))
    run_flags |= FLATPAK_RUN_FLAG_NO_SYSTEM_BUS_PROXY;

  if (!flatpak_run_setup_base_argv (argv_array, NULL, runtime_files, NULL, runtime_ref_parts[2],
                                    run_flags, error))
    return FALSE;

  add_args (argv_array,
            custom_usr ? "--bind" : "--ro-bind", flatpak_file_get_path_cached (runtime_files), "/usr",
            NULL);

  if (!custom_usr)
    add_args (argv_array,
              "--lock-file", "/usr/.ref",
              NULL);

  if (app_files)
    add_args (argv_array,
              app_files_ro ? "--ro-bind" : "--bind", flatpak_file_get_path_cached (app_files), "/app",
              NULL);
  else
    add_args (argv_array,
              "--dir", "/app",
              NULL);

  if (extension_tmpfs_point)
    add_args (argv_array,
              "--tmpfs", extension_tmpfs_point,
              NULL);

  if (extension_point)
    add_args (argv_array,
              "--bind", flatpak_file_get_path_cached (res_files), extension_point,
              NULL);

  if (extension_point)
    dest = extension_point;
  else if (is_app)
    dest = g_strdup ("/app");
  else
    dest = g_strdup ("/usr");

  add_args (argv_array,
            "--setenv", "FLATPAK_DEST", dest,
            NULL);

  app_context = flatpak_app_compute_permissions (metakey,
                                                 runtime_metakey,
                                                 error);
  if (app_context == NULL)
    return FALSE;

  flatpak_context_allow_host_fs (app_context);
  flatpak_context_merge (app_context, arg_context);

  if (!flatpak_run_add_app_info_args (argv_array,
                                      NULL,
                                      app_files,
                                      runtime_files,
                                      id, NULL,
                                      runtime_ref,
                                      app_context,
                                      &app_info_path,
                                      error))
    return FALSE;

  envp = flatpak_run_get_minimal_env (TRUE);
  envp = flatpak_run_apply_env_vars (envp, app_context);

  if (!custom_usr && !(is_extension && !is_app_extension) &&
      !flatpak_run_add_extension_args (argv_array, &envp, runtime_metakey, runtime_ref, cancellable, error))
    return FALSE;

  if (!flatpak_run_add_environment_args (argv_array, NULL, &envp, app_info_path, run_flags, id,
                                         app_context, NULL, NULL, cancellable, error))
    return FALSE;

  /* After setup_base to avoid conflicts with /var symlinks */
  add_args (argv_array,
            "--bind", flatpak_file_get_path_cached (var), "/var",
            NULL);

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
