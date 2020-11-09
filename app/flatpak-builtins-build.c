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
#include "flatpak-utils-private.h"
#include "flatpak-run-private.h"

static gboolean opt_runtime;
static char *opt_build_dir;
static char **opt_bind_mounts;
static char *opt_sdk_dir;
static char *opt_metadata;
static gboolean opt_log_session_bus;
static gboolean opt_log_system_bus;
static gboolean opt_die_with_parent;
static gboolean opt_with_appdir;
static gboolean opt_readonly;

static GOptionEntry options[] = {
  { "runtime", 'r', 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Use Platform runtime rather than Sdk"), NULL },
  { "readonly", 0, 0, G_OPTION_ARG_NONE, &opt_readonly, N_("Make destination readonly"), NULL },
  { "bind-mount", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_bind_mounts, N_("Add bind mount"), N_("DEST=SRC") },
  { "build-dir", 0, 0, G_OPTION_ARG_STRING, &opt_build_dir, N_("Start build in this directory"), N_("DIR") },
  { "sdk-dir", 0, 0, G_OPTION_ARG_STRING, &opt_sdk_dir, N_("Where to look for custom sdk dir (defaults to 'usr')"), N_("DIR") },
  { "metadata", 0, 0, G_OPTION_ARG_STRING, &opt_metadata, N_("Use alternative file for the metadata"), N_("FILE") },
  { "die-with-parent", 'p', 0, G_OPTION_ARG_NONE, &opt_die_with_parent, N_("Kill processes when the parent process dies"), NULL },
  { "with-appdir", 0, 0, G_OPTION_ARG_NONE, &opt_with_appdir, N_("Export application homedir directory to build"), NULL },
  { "log-session-bus", 0, 0, G_OPTION_ARG_NONE, &opt_log_session_bus, N_("Log session bus calls"), NULL },
  { "log-system-bus", 0, 0, G_OPTION_ARG_NONE, &opt_log_system_bus, N_("Log system bus calls"), NULL },
  { NULL }
};

/* Unset FD_CLOEXEC on the array of fds passed in @user_data */
static void
child_setup (gpointer user_data)
{
  GArray *fd_array = user_data;
  int i;

  /* If no fd_array was specified, don't care. */
  if (fd_array == NULL)
    return;

  /* Otherwise, mark not - close-on-exec all the fds in the array */
  for (i = 0; i < fd_array->len; i++)
    fcntl (g_array_index (fd_array, int, i), F_SETFD, 0);
}

static gboolean
find_matching_extension_group_in_metakey (GKeyFile   *metakey,
                                          const char *id,
                                          const char *specified_tag,
                                          char      **out_extension_group,
                                          GError    **error)
{
  g_auto(GStrv) groups = NULL;
  g_autofree char *extension_prefix = NULL;
  const char *last_seen_group = NULL;
  guint n_extension_groups = 0;
  GStrv iter = NULL;

  g_return_val_if_fail (out_extension_group != NULL, FALSE);

  groups =  g_key_file_get_groups (metakey, NULL);
  extension_prefix = g_strconcat (FLATPAK_METADATA_GROUP_PREFIX_EXTENSION,
                                  id,
                                  NULL);

  for (iter = groups; *iter != NULL; ++iter)
    {
      const char *group_name = *iter;
      const char *extension_name = NULL;
      g_autofree char *extension_tag = NULL;

      if (!g_str_has_prefix (group_name, extension_prefix))
        continue;

      ++n_extension_groups;
      extension_name = group_name + strlen (FLATPAK_METADATA_GROUP_PREFIX_EXTENSION);

      flatpak_parse_extension_with_tag (extension_name,
                                        NULL,
                                        &extension_tag);

      /* Check 1: Does this extension have the same tag as the
       * specified tag (including if both are NULL)? If so, use it */
      if (g_strcmp0 (extension_tag, specified_tag) == 0)
        {
          *out_extension_group = g_strdup (group_name);
          return TRUE;
        }

      /* Check 2: Keep track of this extension group as the last
       * seen one. If it was the only one then we can use it. */
      last_seen_group = group_name;
    }

  if (n_extension_groups == 1 && last_seen_group != NULL)
    {
      *out_extension_group = g_strdup (last_seen_group);
      return TRUE;
    }
  else if (n_extension_groups == 0)
    {
      /* Check 2: No extension groups, this is not an error case as
       * we check the parent later. */
      *out_extension_group = NULL;
      return TRUE;
    }

  g_set_error (error,
               G_IO_ERROR,
               G_IO_ERROR_FAILED,
               "Unable to resolve extension %s to a unique "
               "extension point in the parent app or runtime. Consider "
               "using the 'tag' key in ExtensionOf to disambiguate which "
               "extension point to build against.",
               id);

  return FALSE;
}

gboolean
flatpak_builtin_build (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDeploy) runtime_deploy = NULL;
  g_autoptr(GBytes) runtime_deploy_data = NULL;
  g_autoptr(FlatpakDeploy) extensionof_deploy = NULL;
  g_autoptr(GFile) var = NULL;
  g_autoptr(GFile) var_tmp = NULL;
  g_autoptr(GFile) var_lib = NULL;
  g_autoptr(GFile) usr = NULL;
  g_autoptr(GFile) res_deploy = NULL;
  g_autoptr(GFile) res_files = NULL;
  g_autoptr(GFile) app_files = NULL;
  gboolean app_files_ro = FALSE;
  g_autoptr(GFile) runtime_files = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_autofree char *metadata_contents = NULL;
  g_autofree char *runtime_pref = NULL;
  g_autoptr(FlatpakDecomposed) runtime_ref = NULL;
  g_autofree char *extensionof_ref = NULL;
  g_autofree char *extensionof_tag = NULL;
  g_autofree char *extension_point = NULL;
  g_autofree char *extension_tmpfs_point = NULL;
  g_autoptr(GKeyFile) metakey = NULL;
  g_autoptr(GKeyFile) runtime_metakey = NULL;
  g_autoptr(FlatpakBwrap) bwrap = NULL;
  g_auto(GStrv) minimal_envp = NULL;
  gsize metadata_size;
  const char *directory = NULL;
  const char *command = "/bin/sh";
  g_autofree char *id = NULL;
  int i;
  int rest_argv_start, rest_argc;
  g_autoptr(FlatpakContext) arg_context = NULL;
  g_autoptr(FlatpakContext) app_context = NULL;
  gboolean custom_usr;
  FlatpakRunFlags run_flags;
  const char *group = NULL;
  const char *runtime_key = NULL;
  const char *dest = NULL;
  gboolean is_app = FALSE;
  gboolean is_extension = FALSE;
  gboolean is_app_extension = FALSE;
  g_autofree char *arch = NULL;
  g_autofree char *app_info_path = NULL;
  g_autofree char *app_extensions = NULL;
  g_autofree char *runtime_extensions = NULL;
  g_autofree char *instance_id_host_dir = NULL;
  char pid_str[64];
  g_autofree char *pid_path = NULL;
  g_autoptr(GFile) app_id_dir = NULL;

  context = g_option_context_new (_("DIRECTORY [COMMAND [ARGUMENT…]] - Build in directory"));
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

  extensionof_tag = g_key_file_get_string (metakey,
                                           FLATPAK_METADATA_GROUP_EXTENSION_OF,
                                           FLATPAK_METADATA_KEY_TAG, NULL);

  id = g_key_file_get_string (metakey, group, FLATPAK_METADATA_KEY_NAME, error);
  if (id == NULL)
    return FALSE;

  if (opt_runtime)
    runtime_key = FLATPAK_METADATA_KEY_RUNTIME;
  else
    runtime_key = FLATPAK_METADATA_KEY_SDK;

  runtime_pref = g_key_file_get_string (metakey, group, runtime_key, error);
  if (runtime_pref == NULL)
    return FALSE;

  runtime_ref = flatpak_decomposed_new_from_pref (FLATPAK_KINDS_RUNTIME, runtime_pref, error);
  if (runtime_ref == NULL)
    return FALSE;

  arch = flatpak_decomposed_dup_arch (runtime_ref);

  custom_usr = FALSE;
  usr = g_file_get_child (res_deploy,  opt_sdk_dir ? opt_sdk_dir : "usr");
  if (g_file_query_exists (usr, cancellable))
    {
      custom_usr = TRUE;
      runtime_files = g_object_ref (usr);
    }
  else
    {
      runtime_deploy = flatpak_find_deploy_for_ref (flatpak_decomposed_get_ref (runtime_ref), NULL, cancellable, error);
      if (runtime_deploy == NULL)
        return FALSE;

      runtime_deploy_data = flatpak_deploy_get_deploy_data (runtime_deploy, FLATPAK_DEPLOY_VERSION_ANY, cancellable, error);
      if (runtime_deploy_data == NULL)
        return FALSE;

      runtime_metakey = flatpak_deploy_get_metadata (runtime_deploy);

      runtime_files = flatpak_deploy_get_files (runtime_deploy);
    }

  var = g_file_get_child (res_deploy, "var");
  var_tmp = g_file_get_child (var, "tmp");
  if (!flatpak_mkdir_p (var_tmp, cancellable, error))
    return FALSE;
  var_lib = g_file_get_child (var, "lib");
  if (!flatpak_mkdir_p (var_lib, cancellable, error))
    return FALSE;

  res_files = g_file_get_child (res_deploy, "files");

  if (is_app)
    {
      app_files = g_object_ref (res_files);
      if (opt_with_appdir)
        {
          app_id_dir = flatpak_get_data_dir (id);
          if (!flatpak_ensure_data_dir (app_id_dir, cancellable, NULL))
            g_clear_object (&app_id_dir);
        }
    }
  else if (is_extension)
    {
      g_autoptr(GKeyFile) x_metakey = NULL;
      g_autofree char *x_group = NULL;
      g_autofree char *x_dir = NULL;
      g_autofree char *x_subdir_suffix = NULL;
      char *x_subdir = NULL;
      g_autofree char *bare_extension_point = NULL;

      extensionof_deploy = flatpak_find_deploy_for_ref (extensionof_ref, NULL, cancellable, error);
      if (extensionof_deploy == NULL)
        return FALSE;

      x_metakey = flatpak_deploy_get_metadata (extensionof_deploy);

      /* Since we have tagged extensions, it is possible that an extension could
       * be listed more than once in the "parent" flatpak. In that case, we should
       * try and disambiguate using the following rules:
       *
       * 1. Use the 'tag=' key in the ExtensionOfSection and if not found:
       * 2. Use the only extension point available if there is only one.
       * 3. If there are no matching groups, return NULL.
       * 4. In all other cases, error out.
       */
      if (!find_matching_extension_group_in_metakey (x_metakey,
                                                     id,
                                                     extensionof_tag,
                                                     &x_group,
                                                     error))
        return FALSE;

      if (x_group == NULL)
        {
          /* Failed, look for subdirectories=true parent */
          char *last_dot = strrchr (id, '.');

          if (last_dot != NULL)
            {
              char *parent_id = g_strndup (id, last_dot - id);
              if (!find_matching_extension_group_in_metakey (x_metakey,
                                                             parent_id,
                                                             extensionof_tag,
                                                             &x_group,
                                                             error))
                return FALSE;

              if (x_group != NULL &&
                  g_key_file_get_boolean (x_metakey, x_group,
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

  app_context = flatpak_app_compute_permissions (metakey,
                                                 runtime_metakey,
                                                 error);
  if (app_context == NULL)
    return FALSE;

  flatpak_context_allow_host_fs (app_context);
  flatpak_context_merge (app_context, arg_context);

  minimal_envp = flatpak_run_get_minimal_env (TRUE, FALSE);
  bwrap = flatpak_bwrap_new (minimal_envp);
  flatpak_bwrap_add_args (bwrap, flatpak_get_bwrap (), NULL);

  run_flags =
    FLATPAK_RUN_FLAG_DEVEL | FLATPAK_RUN_FLAG_MULTIARCH | FLATPAK_RUN_FLAG_NO_SESSION_HELPER |
    FLATPAK_RUN_FLAG_SET_PERSONALITY | FLATPAK_RUN_FLAG_NO_A11Y_BUS_PROXY;
  if (opt_die_with_parent)
    run_flags |= FLATPAK_RUN_FLAG_DIE_WITH_PARENT;
  if (custom_usr)
    run_flags |= FLATPAK_RUN_FLAG_WRITABLE_ETC;

  run_flags |= flatpak_context_get_run_flags (app_context);

  /* Unless manually specified, we disable dbus proxy */
  if (!flatpak_context_get_needs_session_bus_proxy (arg_context))
    run_flags |= FLATPAK_RUN_FLAG_NO_SESSION_BUS_PROXY;

  if (!flatpak_context_get_needs_system_bus_proxy (arg_context))
    run_flags |= FLATPAK_RUN_FLAG_NO_SYSTEM_BUS_PROXY;

  if (opt_log_session_bus)
    run_flags |= FLATPAK_RUN_FLAG_LOG_SESSION_BUS;

  if (opt_log_system_bus)
    run_flags |= FLATPAK_RUN_FLAG_LOG_SYSTEM_BUS;

  /* Never set up an a11y bus for builds */
  run_flags |= FLATPAK_RUN_FLAG_NO_A11Y_BUS_PROXY;

  if (!flatpak_run_setup_base_argv (bwrap, runtime_files, app_id_dir, arch,
                                    run_flags, error))
    return FALSE;

  flatpak_bwrap_add_args (bwrap,
                          (custom_usr && !opt_readonly)  ? "--bind" : "--ro-bind", flatpak_file_get_path_cached (runtime_files), "/usr",
                          NULL);

  if (!custom_usr)
    flatpak_bwrap_add_args (bwrap,
                            "--lock-file", "/usr/.ref",
                            NULL);

  if (app_files)
    flatpak_bwrap_add_args (bwrap,
                            (app_files_ro || opt_readonly) ? "--ro-bind" : "--bind", flatpak_file_get_path_cached (app_files), "/app",
                            NULL);
  else
    flatpak_bwrap_add_args (bwrap,
                            "--dir", "/app",
                            NULL);

  if (extension_tmpfs_point)
    flatpak_bwrap_add_args (bwrap,
                            "--tmpfs", extension_tmpfs_point,
                            NULL);

  /* We add the actual bind below so that we're not shadowed by other extensions or their tmpfs */

  if (extension_point)
    dest = extension_point;
  else if (is_app)
    dest = g_strdup ("/app");
  else
    dest = g_strdup ("/usr");

  flatpak_bwrap_add_args (bwrap,
                          "--setenv", "FLATPAK_DEST", dest,
                          "--setenv", "FLATPAK_ID", id,
                          "--setenv", "FLATPAK_ARCH", arch,
                          NULL);

  /* Persist some stuff in /var. We can't persist everything because  that breaks /var things
   * from the host to work. For example the /home -> /var/home on atomic.
   * The interesting things to contain during the build is /var/tmp (for tempfiles shared during builds)
   * and things like /var/lib/rpm, if the installation uses packages.
   */
  flatpak_bwrap_add_args (bwrap,
                          "--bind", flatpak_file_get_path_cached (var_lib), "/var/lib",
                          NULL);
  flatpak_bwrap_add_args (bwrap,
                          "--bind", flatpak_file_get_path_cached (var_tmp), "/var/tmp",
                          NULL);

  flatpak_run_apply_env_vars (bwrap, app_context);

  if (is_app)
    {
      /* We don't actually know the final branchname yet, so use "nobranch" as fallback to avoid unexpected matches.
         This means any extension point used at build time must have explicit versions to work. */
      g_autoptr(FlatpakDecomposed) fake_ref =
        flatpak_decomposed_new_from_parts (FLATPAK_KINDS_APP, id, arch, "nobranch", NULL);
      if (fake_ref != NULL &&
          !flatpak_run_add_extension_args (bwrap, metakey, fake_ref, FALSE, &app_extensions, cancellable, error))
        return FALSE;
    }

  if (!custom_usr &&
      !flatpak_run_add_extension_args (bwrap, runtime_metakey, runtime_ref, FALSE, &runtime_extensions, cancellable, error))
    return FALSE;

  /* Mount this after the above extensions so we always win */
  if (extension_point)
    flatpak_bwrap_add_args (bwrap,
                            "--bind", flatpak_file_get_path_cached (res_files), extension_point,
                            NULL);

  if (!flatpak_run_add_app_info_args (bwrap,
                                      app_files, NULL, app_extensions,
                                      runtime_files, runtime_deploy_data, runtime_extensions,
                                      id, NULL,
                                      runtime_ref,
                                      app_id_dir, app_context, NULL,
                                      FALSE, TRUE, TRUE,
                                      &app_info_path, -1,
                                      &instance_id_host_dir,
                                      error))
    return FALSE;

  if (!flatpak_run_add_environment_args (bwrap, app_info_path, run_flags, id,
                                         app_context, app_id_dir, NULL, NULL, cancellable, error))
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
      flatpak_bwrap_add_args (bwrap,
                              "--bind", split, opt_bind_mounts[i],
                              NULL);
    }

  if (opt_build_dir != NULL)
    {
      flatpak_bwrap_add_args (bwrap,
                              "--chdir", opt_build_dir,
                              NULL);
    }

  if (!flatpak_bwrap_bundle_args (bwrap, 1, -1, FALSE, error))
    return FALSE;

  flatpak_bwrap_add_args (bwrap, command, NULL);
  flatpak_bwrap_append_argsv (bwrap,
                              &argv[rest_argv_start + 2],
                              rest_argc - 2);

  g_ptr_array_add (bwrap->argv, NULL);

  g_snprintf (pid_str, sizeof (pid_str), "%d", getpid ());
  pid_path = g_build_filename (instance_id_host_dir, "pid", NULL);
  g_file_set_contents (pid_path, pid_str, -1, NULL);

  /* Ensure we unset O_CLOEXEC */
  child_setup (bwrap->fds);
  if (execvpe (flatpak_get_bwrap (), (char **) bwrap->argv->pdata, bwrap->envp) == -1)
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
