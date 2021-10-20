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
#include <unistd.h>
#include <string.h>

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-utils-private.h"
#include "flatpak-ref.h"
#include "flatpak-run-private.h"

static char *opt_arch;
static char *opt_var;
static char *opt_type;
static char *opt_sdk_dir;
static char **opt_sdk_extensions;
static char **opt_extensions;
static char **opt_tags;
static char *opt_extension_tag;
static char *opt_base;
static char *opt_base_version;
static char **opt_base_extensions;
static gboolean opt_writable_sdk;
static gboolean opt_update;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to use"), N_("ARCH") },
  { "var", 'v', 0, G_OPTION_ARG_STRING, &opt_var, N_("Initialize var from named runtime"), N_("RUNTIME") },
  { "base", 0, 0, G_OPTION_ARG_STRING, &opt_base, N_("Initialize apps from named app"), N_("APP") },
  { "base-version", 0, 0, G_OPTION_ARG_STRING, &opt_base_version, N_("Specify version for --base"), N_("VERSION") },
  { "base-extension", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_base_extensions, N_("Include this base extension"), N_("EXTENSION") },
  { "extension-tag", 0, 0, G_OPTION_ARG_STRING, &opt_extension_tag, N_("Extension tag to use if building extension"), N_("EXTENSION_TAG") },
  { "writable-sdk", 'w', 0, G_OPTION_ARG_NONE, &opt_writable_sdk, N_("Initialize /usr with a writable copy of the sdk"), NULL },
  { "type", 0, 0, G_OPTION_ARG_STRING, &opt_type, N_("Specify the build type (app, runtime, extension)"), N_("TYPE") },
  { "tag", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_tags, N_("Add a tag"), N_("TAG") },
  { "sdk-extension", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_sdk_extensions, N_("Include this sdk extension in /usr"), N_("EXTENSION") },
  { "extension", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_extensions, N_("Add extension point info"),  N_("NAME=VARIABLE[=VALUE]") },
  { "sdk-dir", 0, 0, G_OPTION_ARG_STRING, &opt_sdk_dir, N_("Where to store sdk (defaults to 'usr')"), N_("DIR") },
  { "update", 0, 0, G_OPTION_ARG_NONE, &opt_update, N_("Re-initialize the sdk/var"), NULL },
  { NULL }
};

static gboolean
ensure_extensions (FlatpakDeploy *src_deploy, const char *default_arch, const char *default_branch,
                   char *src_extensions[], GFile *top_dir, GCancellable *cancellable, GError **error)
{
  g_autoptr(GKeyFile) metakey = flatpak_deploy_get_metadata (src_deploy);
  GList *extensions = NULL, *l;
  int i;

  /* We leak this on failure, as we have no autoptr for deep lists.. */
  extensions = flatpak_list_extensions (metakey, default_arch, default_branch);

  for (i = 0; src_extensions[i] != NULL; i++)
    {
      const char *requested_extension = src_extensions[i];
      g_autofree char *requested_extension_name = NULL;
      gboolean found = FALSE;

      /* Remove any '@' from the name */
      flatpak_parse_extension_with_tag (requested_extension,
                                        &requested_extension_name,
                                        NULL);

      for (l = extensions; l != NULL; l = l->next)
        {
          FlatpakExtension *ext = l->data;

          if (strcmp (ext->installed_id, requested_extension_name) == 0 ||
              strcmp (ext->id, requested_extension_name) == 0)
            {
              if (!ext->is_unmaintained)
                {
                  g_autoptr(FlatpakDir) src_dir = NULL;
                  g_autoptr(GFile) deploy = NULL;
                  g_autoptr(GBytes) deploy_data = NULL;
                  g_autofree const char **subpaths = NULL;

                  deploy = flatpak_find_deploy_dir_for_ref (ext->ref, &src_dir, cancellable, error);
                  if (deploy == NULL)
                    return FALSE;
                  deploy_data = flatpak_dir_get_deploy_data (src_dir, ext->ref, FLATPAK_DEPLOY_VERSION_ANY, cancellable, error);
                  if (deploy_data == NULL)
                    return FALSE;

                  subpaths = flatpak_deploy_data_get_subpaths (deploy_data);
                  if (subpaths[0] != NULL)
                    return flatpak_fail (error, _("Requested extension %s is only partially installed"), ext->installed_id);
                }

              if (top_dir)
                {
                  g_autoptr(GFile) target = g_file_resolve_relative_path (top_dir, ext->directory);
                  g_autoptr(GFile) target_parent = g_file_get_parent (target);
                  g_autoptr(GFile) ext_deploy_files = g_file_new_for_path (ext->files_path);

                  if (!flatpak_mkdir_p (target_parent, cancellable, error))
                    return FALSE;

                  /* An extension overrides whatever is there before, so we clean up first */
                  if (!flatpak_rm_rf (target, cancellable, error))
                    return FALSE;

                  if (!flatpak_cp_a (ext_deploy_files, target,
                                     FLATPAK_CP_FLAGS_NO_CHOWN,
                                     cancellable, error))
                    return FALSE;
                }

              found = TRUE;
            }
        }

      if (!found)
        {
          g_list_free_full (extensions, (GDestroyNotify) flatpak_extension_free);
          return flatpak_fail (error, _("Requested extension %s not installed"), requested_extension_name);
        }
    }

  g_list_free_full (extensions, (GDestroyNotify) flatpak_extension_free);

  return TRUE;
}

static char *
maybe_format_extension_tag (const char *extension_tag)
{
  if (extension_tag != NULL)
    {
      return g_strdup_printf ("tag=%s\n", extension_tag);
    }

  return g_strdup ("");
}

gboolean
flatpak_builtin_build_init (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) var_deploy_files = NULL;
  g_autoptr(GFile) base = NULL;
  g_autoptr(GFile) files_dir = NULL;
  g_autoptr(GFile) usr_dir = NULL;
  g_autoptr(GFile) var_dir = NULL;
  g_autoptr(GFile) var_tmp_dir = NULL;
  g_autoptr(GFile) var_run_dir = NULL;
  g_autoptr(GFile) metadata_file = NULL;
  g_autoptr(GString) metadata_contents = NULL;
  g_autoptr(GError) my_error = NULL;
  g_autoptr(FlatpakDeploy) runtime_deploy = NULL;
  g_autoptr(FlatpakDeploy) sdk_deploy = NULL;
  const char *app_id;
  const char *directory;
  const char *sdk_pref;
  const char *runtime_pref;
  const char *default_branch = NULL;
  g_autofree char *sdk_branch = NULL;
  g_autofree char *sdk_arch = NULL;
  g_autofree char *base_ref = NULL;
  g_autoptr(FlatpakDecomposed) runtime_ref = NULL;
  g_autofree char *extension_runtime_pref = NULL;
  g_autoptr(FlatpakDecomposed) var_ref = NULL;
  g_autoptr(FlatpakDecomposed) sdk_ref = NULL;
  FlatpakKinds kinds;
  int i;
  g_autoptr(FlatpakDir) sdk_dir = NULL;
  g_autoptr(FlatpakDir) runtime_dir = NULL;
  gboolean is_app = FALSE;
  gboolean is_extension = FALSE;
  gboolean is_runtime = FALSE;
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  g_autofree char *keyfile_data = NULL;
  gsize keyfile_data_len;

  context = g_option_context_new (_("DIRECTORY APPNAME SDK RUNTIME [BRANCH] - Initialize a directory for building"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 5)
    return usage_error (context, _("RUNTIME must be specified"), error);

  if (argc > 6)
    return usage_error (context, _("Too many arguments"), error);

  directory = argv[1];
  app_id = argv[2];
  sdk_pref = argv[3];
  runtime_pref = argv[4];
  if (argc >= 6)
    default_branch = argv[5];

  if (opt_type != NULL)
    {
      if (strcmp (opt_type, "app") == 0)
        is_app = TRUE;
      else if (strcmp (opt_type, "extension") == 0)
        is_extension = TRUE;
      else if (strcmp (opt_type, "runtime") == 0)
        is_runtime = TRUE;
      else
        return flatpak_fail (error, _("'%s' is not a valid build type name, use app, runtime or extension"), opt_type);
    }
  else
    is_app = TRUE;

  if (!flatpak_is_valid_name (app_id, -1, &my_error))
    return flatpak_fail (error, _("'%s' is not a valid application name: %s"), app_id, my_error->message);


  kinds = FLATPAK_KINDS_RUNTIME;
  sdk_dir = flatpak_find_installed_pref (sdk_pref, kinds, opt_arch, default_branch, TRUE, FALSE, FALSE, NULL,
                                         &sdk_ref, cancellable, error);
  if (sdk_dir == NULL)
    return FALSE;

  kinds = FLATPAK_KINDS_RUNTIME;
  if (is_extension)
    kinds |= FLATPAK_KINDS_APP;

  runtime_dir = flatpak_find_installed_pref (runtime_pref, kinds, opt_arch, default_branch, TRUE, FALSE, FALSE, NULL,
                                             &runtime_ref, cancellable, error);
  if (runtime_dir == NULL)
    return FALSE;

  if (is_extension)
    {
      /* The "runtime" can be an app in case we're building an extension */
      if (flatpak_decomposed_is_app (runtime_ref))
        {
          g_autoptr(GKeyFile) runtime_metadata = NULL;

          runtime_deploy = flatpak_dir_load_deployed (runtime_dir, runtime_ref, NULL, cancellable, error);
          if (runtime_deploy == NULL)
            return FALSE;

          runtime_metadata = flatpak_deploy_get_metadata (runtime_deploy);
          extension_runtime_pref = g_key_file_get_string (runtime_metadata, FLATPAK_METADATA_GROUP_APPLICATION,
                                                          FLATPAK_METADATA_KEY_RUNTIME, NULL);
          g_assert (extension_runtime_pref);
        }
      else
        extension_runtime_pref = flatpak_decomposed_dup_pref (runtime_ref);
    }

  base = g_file_new_for_commandline_arg (directory);
  if (flatpak_file_get_path_cached (base) == NULL)
    return flatpak_fail (error, _("'%s' is not a valid filename"), directory);

  if (!flatpak_mkdir_p (base, cancellable, error))
    return FALSE;

  files_dir = g_file_get_child (base, "files");
  var_dir = g_file_get_child (base, "var");
  var_tmp_dir = g_file_get_child (var_dir, "tmp");
  var_run_dir = g_file_get_child (var_dir, "run");
  metadata_file = g_file_get_child (base, "metadata");

  if (!opt_update &&
      g_file_query_exists (files_dir, cancellable))
    return flatpak_fail (error, _("Build directory %s already initialized"), directory);

  sdk_deploy = flatpak_dir_load_deployed (sdk_dir, sdk_ref, NULL, cancellable, error);
  if (sdk_deploy == NULL)
    return FALSE;

  if (opt_writable_sdk || is_runtime)
    {
      g_autoptr(GFile) sdk_deploy_files = NULL;

      if (opt_sdk_dir)
        usr_dir = g_file_get_child (base, opt_sdk_dir);
      else
        usr_dir = g_file_get_child (base, "usr");

      if (!flatpak_rm_rf (usr_dir, NULL, &my_error))
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
    }

  sdk_branch = flatpak_decomposed_dup_branch (sdk_ref);
  sdk_arch = flatpak_decomposed_dup_arch (sdk_ref);

  if (opt_sdk_extensions &&
      !ensure_extensions (sdk_deploy, sdk_arch, sdk_branch,
                          opt_sdk_extensions, usr_dir, cancellable, error))
    return FALSE;

  if (opt_var)
    {
      var_ref = flatpak_decomposed_new_from_parts (FLATPAK_KINDS_RUNTIME, opt_var, opt_arch, default_branch, error);
      if (var_ref == NULL)
        return FALSE;

      var_deploy_files = flatpak_find_files_dir_for_ref (var_ref, cancellable, error);
      if (var_deploy_files == NULL)
        return FALSE;
    }

  if (opt_update)
    return TRUE;

  if (!g_file_make_directory (files_dir, cancellable, error))
    return FALSE;

  if (opt_base)
    {
      const char *base_branch;
      g_autoptr(GFile) base_deploy_files = NULL;
      g_autoptr(FlatpakDeploy) base_deploy = NULL;

      base_branch = opt_base_version ? opt_base_version : "master";
      base_ref = flatpak_build_app_ref (opt_base, base_branch, opt_arch);
      base_deploy = flatpak_find_deploy_for_ref (base_ref, NULL, NULL, cancellable, error);
      if (base_deploy == NULL)
        return FALSE;

      base_deploy_files = flatpak_deploy_get_files (base_deploy);
      if (!flatpak_cp_a (base_deploy_files, files_dir,
                         FLATPAK_CP_FLAGS_MERGE | FLATPAK_CP_FLAGS_NO_CHOWN,
                         cancellable, error))
        return FALSE;


      if (opt_base_extensions &&
          !ensure_extensions (base_deploy, opt_arch, base_branch,
                              opt_base_extensions, files_dir, cancellable, error))
        return FALSE;
    }

  if (var_deploy_files)
    {
      if (!flatpak_cp_a (var_deploy_files, var_dir, FLATPAK_CP_FLAGS_NONE, cancellable, error))
        return FALSE;
    }
  else
    {
      if (!g_file_make_directory (var_dir, cancellable, error))
        return FALSE;
    }

  if (!flatpak_mkdir_p (var_tmp_dir, cancellable, error))
    return FALSE;

  if (!g_file_query_exists (var_run_dir, cancellable) &&
      !g_file_make_symbolic_link (var_run_dir, "/run", cancellable, error))
    return FALSE;


  metadata_contents = g_string_new ("");
  if (is_app)
    g_string_append (metadata_contents, "[Application]\n");
  else
    g_string_append (metadata_contents, "[Runtime]\n");

  g_string_append_printf (metadata_contents,
                          "name=%s\n",
                          app_id);

  /* The "runtime" can be an app in case we're building an extension */
  if (flatpak_decomposed_is_runtime (runtime_ref))
    g_string_append_printf (metadata_contents,
                            "runtime=%s\n",
                            flatpak_decomposed_get_pref (runtime_ref));

  if (flatpak_decomposed_is_runtime (sdk_ref))
    g_string_append_printf (metadata_contents,
                            "sdk=%s\n",
                            flatpak_decomposed_get_pref (sdk_ref));

  if (base_ref)
    g_string_append_printf (metadata_contents,
                            "base=%s\n", base_ref);


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

  if (is_extension)
    {
      g_autofree char *optional_extension_tag = maybe_format_extension_tag (opt_extension_tag);
      g_string_append_printf (metadata_contents,
                              "\n"
                              "[ExtensionOf]\n"
                              "ref=%s\n"
                              "runtime=%s\n"
                              "%s\n",
                              flatpak_decomposed_get_ref (runtime_ref),
                              extension_runtime_pref,
                              optional_extension_tag);
    }

  /* Do the rest of the work as a keyfile, as we need things like full escaping, etc.
   * We should probably do everything this way actually...   */
  if (!g_key_file_load_from_data (keyfile, metadata_contents->str, metadata_contents->len, 0, NULL))
    return flatpak_fail (error, "Internal error parsing generated keyfile");

  for (i = 0; opt_extensions != NULL && opt_extensions[i] != NULL; i++)
    {
      g_auto(GStrv) elements = NULL;
      g_autofree char *groupname = NULL;

      elements = g_strsplit (opt_extensions[i], "=", 3);
      if (g_strv_length (elements) < 2)
        return flatpak_fail (error, _("Too few elements in --extension argument %s, format should be NAME=VAR[=VALUE]"), opt_extensions[i]);

      if (!flatpak_is_valid_name (elements[0], -1, error))
        return glnx_prefix_error (error, _("Invalid extension name %s"), elements[0]);

      groupname = g_strconcat (FLATPAK_METADATA_GROUP_PREFIX_EXTENSION,
                               elements[0], NULL);

      g_key_file_set_string (keyfile, groupname, elements[1], elements[2] ? elements[2] : "true");
    }

  keyfile_data = g_key_file_to_data (keyfile, &keyfile_data_len, NULL);

  if (!g_file_replace_contents (metadata_file,
                                keyfile_data, keyfile_data_len, NULL, FALSE,
                                G_FILE_CREATE_REPLACE_DESTINATION,
                                NULL, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_complete_build_init (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) user_dir = NULL;
  g_autoptr(FlatpakDir) system_dir = NULL;

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

    case 2: /* APP */
      break;

    case 3: /* RUNTIME */
    case 4: /* SDK */
      user_dir = flatpak_dir_get_user ();
      {
        g_autoptr(GError) error = NULL;
        g_autoptr(GPtrArray) refs = flatpak_dir_find_installed_refs (user_dir, NULL, NULL, opt_arch,
                                                                     FLATPAK_KINDS_RUNTIME,
                                                                     FIND_MATCHING_REFS_FLAGS_NONE,
                                                                     &error);
        if (refs == NULL)
          flatpak_completion_debug ("find local refs error: %s", error->message);

        flatpak_complete_ref_id (completion, refs);
      }

      system_dir = flatpak_dir_get_system_default ();
      {
        g_autoptr(GError) error = NULL;
        g_autoptr(GPtrArray) refs = flatpak_dir_find_installed_refs (system_dir, NULL, NULL, opt_arch,
                                                                     FLATPAK_KINDS_RUNTIME,
                                                                     FIND_MATCHING_REFS_FLAGS_NONE,
                                                                     &error);
        if (refs == NULL)
          flatpak_completion_debug ("find local refs error: %s", error->message);

        flatpak_complete_ref_id (completion, refs);
      }

      break;

    case 5: /* BRANCH */
      user_dir = flatpak_dir_get_user ();
      {
        g_autoptr(GError) error = NULL;
        g_autoptr(GPtrArray) refs = flatpak_dir_find_installed_refs (user_dir, completion->argv[3], NULL, opt_arch,
                                                                     FLATPAK_KINDS_RUNTIME,
                                                                     FIND_MATCHING_REFS_FLAGS_NONE,
                                                                     &error);
        if (refs == NULL)
          flatpak_completion_debug ("find local refs error: %s", error->message);

        flatpak_complete_ref_branch (completion, refs);
      }

      system_dir = flatpak_dir_get_system_default ();
      {
        g_autoptr(GError) error = NULL;
        g_autoptr(GPtrArray) refs = flatpak_dir_find_installed_refs (system_dir, completion->argv[3], NULL, opt_arch,
                                                                     FLATPAK_KINDS_RUNTIME,
                                                                     FIND_MATCHING_REFS_FLAGS_NONE,
                                                                     &error);
        if (refs == NULL)
          flatpak_completion_debug ("find local refs error: %s", error->message);

        flatpak_complete_ref_branch (completion, refs);
      }

      break;
    }

  return TRUE;
}
