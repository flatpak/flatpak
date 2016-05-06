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

#include "flatpak-builtins.h"
#include "flatpak-utils.h"

static char *opt_arch;
static char *opt_var;
static char *opt_sdk_dir;
static char **opt_sdk_extensions;
static char **opt_tags;
static gboolean opt_writable_sdk;
static gboolean opt_update;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to use", "ARCH" },
  { "var", 'v', 0, G_OPTION_ARG_STRING, &opt_var, "Initialize var from named runtime", "RUNTIME" },
  { "writable-sdk", 'w', 0, G_OPTION_ARG_NONE, &opt_writable_sdk, "Initialize /usr with a writable copy of the sdk",  },
  { "tag", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_tags, "Add a tag",  },
  { "sdk-extension", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_sdk_extensions, "include this sdk extension in /usr",  "EXTENSION"},
  { "sdk-dir", 0, 0, G_OPTION_ARG_STRING, &opt_sdk_dir, "Where to store sdk (defaults to 'usr')", "DIR" },
  { "update", 0, 0, G_OPTION_ARG_NONE, &opt_update, "Re-initialize the sdk/var",  },
  { NULL }
};

gboolean
flatpak_builtin_build_init (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) var_deploy_base = NULL;
  g_autoptr(GFile) var_deploy_files = NULL;
  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) files_dir = NULL;
  g_autoptr(GFile) usr_dir = NULL;
  g_autoptr(GFile) var_dir = NULL;
  g_autoptr(GFile) var_tmp_dir = NULL;
  g_autoptr(GFile) var_run_dir = NULL;
  g_autoptr(GFile) metadata_file = NULL;
  g_autoptr(GString) metadata_contents = NULL;
  const char *app_id;
  const char *directory;
  const char *sdk;
  const char *runtime;
  const char *branch = "master";
  g_autofree char *runtime_ref = NULL;
  g_autofree char *var_ref = NULL;
  g_autofree char *sdk_ref = NULL;
  int i;

  context = g_option_context_new ("DIRECTORY APPNAME SDK RUNTIME [BRANCH] - Initialize a directory for building");

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 5)
    return usage_error (context, "RUNTIME must be specified", error);

  directory = argv[1];
  app_id = argv[2];
  sdk = argv[3];
  runtime = argv[4];
  if (argc >= 6)
    branch = argv[5];

  if (!flatpak_is_valid_name (app_id))
    return flatpak_fail (error, "'%s' is not a valid application name", app_id);

  if (!flatpak_is_valid_name (runtime))
    return flatpak_fail (error, "'%s' is not a valid runtime name", runtime);

  if (!flatpak_is_valid_name (sdk))
    return flatpak_fail (error, "'%s' is not a valid sdk name", sdk);

  if (!flatpak_is_valid_branch (branch))
    return flatpak_fail (error, "'%s' is not a valid branch name", branch);

  runtime_ref = flatpak_build_untyped_ref (runtime, branch, opt_arch);
  sdk_ref = flatpak_build_untyped_ref (sdk, branch, opt_arch);

  base = g_file_new_for_commandline_arg (directory);

  if (!gs_file_ensure_directory (base, TRUE, cancellable, error))
    return FALSE;

  files_dir = g_file_get_child (base, "files");
  if (opt_sdk_dir)
    usr_dir = g_file_get_child (base, opt_sdk_dir);
  else
    usr_dir = g_file_get_child (base, "usr");
  var_dir = g_file_get_child (base, "var");
  var_tmp_dir = g_file_get_child (var_dir, "tmp");
  var_run_dir = g_file_get_child (var_dir, "run");
  metadata_file = g_file_get_child (base, "metadata");

  if (!opt_update &&
      g_file_query_exists (files_dir, cancellable))
    return flatpak_fail (error, "Build directory %s already initialized", directory);

  if (opt_writable_sdk)
    {
      g_autofree char *full_sdk_ref = g_strconcat ("runtime/", sdk_ref, NULL);
      g_autoptr(GError) my_error = NULL;
      g_autoptr(GFile) sdk_deploy_files = NULL;
      g_autoptr(FlatpakDeploy) sdk_deploy = NULL;

      sdk_deploy = flatpak_find_deploy_for_ref (full_sdk_ref, cancellable, error);
      if (sdk_deploy == NULL)
        return FALSE;

      if (!gs_shutil_rm_rf (usr_dir, NULL, &my_error))
        {
          if (!g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_propagate_error (error, g_steal_pointer (&my_error));
              return FALSE;
            }

          g_clear_error (&my_error);
        }

      sdk_deploy_files = flatpak_deploy_get_files (sdk_deploy);
      if (!flatpak_cp_a (sdk_deploy_files, usr_dir, FLATPAK_CP_FLAGS_NO_CHOWN, cancellable, error))
        return FALSE;

      if (opt_sdk_extensions)
        {
          g_autoptr(GKeyFile) metakey = flatpak_deploy_get_metadata (sdk_deploy);
          GList *extensions = NULL, *l;

          /* We leak this on failure, as we have no autoptr for deep lists.. */
          extensions = flatpak_list_extensions (metakey,
                                                opt_arch,
                                                branch);

          for (i = 0; opt_sdk_extensions[i] != NULL; i++)
            {
              const char *requested_extension = opt_sdk_extensions[i];
              gboolean found = FALSE;

              for (l = extensions; l != NULL; l = l->next)
                {
                  FlatpakExtension *ext = l->data;

                  if (strcmp (ext->installed_id, requested_extension) == 0 ||
                      strcmp (ext->id, requested_extension) == 0)
                    {
                      g_autoptr(GFile) ext_deploy_dir = flatpak_find_deploy_dir_for_ref (ext->ref, cancellable, NULL);
                      if (ext_deploy_dir != NULL)
                        {
                          g_autoptr(GFile) ext_deploy_files = g_file_get_child (ext_deploy_dir, "files");
                          g_autoptr(GFile) target = g_file_resolve_relative_path (usr_dir, ext->directory);
                          g_autoptr(GFile) target_parent = g_file_get_parent (target);

                          if (!gs_file_ensure_directory (target_parent, TRUE, cancellable, error))
                            return FALSE;

                          /* An extension overrides whatever is there before, so we clean up first */
                          if (!gs_shutil_rm_rf (target, cancellable, error))
                            return FALSE;

                          if (!flatpak_cp_a (ext_deploy_files, target, FLATPAK_CP_FLAGS_NO_CHOWN, cancellable, error))
                            return FALSE;

                          found = TRUE;
                        }
                      else
                        {
                          g_list_free_full (extensions, (GDestroyNotify) flatpak_extension_free);
                          return flatpak_fail (error, "Requested extension %s not installed\n", requested_extension);
                        }
                    }
                }

              if (!found)
                return flatpak_fail (error, "No extension %s in sdk\n", requested_extension);
            }
          g_list_free_full (extensions, (GDestroyNotify) flatpak_extension_free);
        }
    }

  if (opt_var)
    {
      var_ref = flatpak_build_runtime_ref (opt_var, branch, opt_arch);

      var_deploy_base = flatpak_find_deploy_dir_for_ref (var_ref, cancellable, error);
      if (var_deploy_base == NULL)
        return FALSE;

      var_deploy_files = g_file_get_child (var_deploy_base, "files");
    }

  if (opt_update)
    return TRUE;

  if (!g_file_make_directory (files_dir, cancellable, error))
    return FALSE;

  if (var_deploy_files)
    {
      if (!gs_shutil_cp_a (var_deploy_files, var_dir, cancellable, error))
        return FALSE;
    }
  else
    {
      if (!g_file_make_directory (var_dir, cancellable, error))
        return FALSE;
    }

  if (!gs_file_ensure_directory (var_tmp_dir, FALSE, cancellable, error))
    return FALSE;

  if (!g_file_query_exists (var_run_dir, cancellable) &&
      !g_file_make_symbolic_link (var_run_dir, "/run", cancellable, error))
    return FALSE;


  metadata_contents = g_string_new ("[Application]\n");
  g_string_append_printf (metadata_contents,
                          "name=%s\n"
                          "runtime=%s\n"
                          "sdk=%s\n",
                          app_id, runtime_ref, sdk_ref);
  if (opt_tags != NULL)
    {
      g_string_append (metadata_contents, "tags=");
      for (i = 0; opt_tags[i] != NULL; i++)
        {
          g_string_append (metadata_contents, opt_tags[i]);
          g_string_append_c (metadata_contents, ';');
        }
      g_string_append_c (metadata_contents, '\n');
    }

  if (!g_file_replace_contents (metadata_file,
                                metadata_contents->str, metadata_contents->len, NULL, FALSE,
                                G_FILE_CREATE_REPLACE_DESTINATION,
                                NULL, cancellable, error))
    return FALSE;

  return TRUE;
}
