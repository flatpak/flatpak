/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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
#include <string.h>

#include <glib/gi18n.h>

#include "libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils-private.h"
#include "flatpak-error.h"
#include "flatpak-dbus-generated.h"
#include "flatpak-run-private.h"

static char *opt_arch;
static char *opt_branch;
static char *opt_command;
static char *opt_cwd;
static gboolean opt_devel;
static gboolean opt_userns;
static gboolean opt_log_session_bus;
static gboolean opt_log_system_bus;
static gboolean opt_log_a11y_bus;
static int opt_a11y_bus = -1;
static int opt_session_bus = -1;
static gboolean opt_no_documents_portal;
static gboolean opt_file_forwarding;
static gboolean opt_die_with_parent;
static gboolean opt_sandbox;
static char *opt_runtime;
static char *opt_runtime_version;
static char *opt_commit;
static char *opt_runtime_commit;
static int opt_parent_pid;
static gboolean opt_parent_expose_pids;
static gboolean opt_parent_share_pids;
static int opt_instance_id_fd = -1;
static char *opt_app_path;
static char *opt_usr_path;
static gboolean opt_clear_env;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to use"), N_("ARCH") },
  { "command", 0, 0, G_OPTION_ARG_STRING, &opt_command, N_("Command to run"), N_("COMMAND") },
  { "cwd", 0, 0, G_OPTION_ARG_STRING, &opt_cwd, N_("Directory to run the command in"), N_("DIR") },
  { "branch", 0, 0, G_OPTION_ARG_STRING, &opt_branch, N_("Branch to use"), N_("BRANCH") },
  { "devel", 'd', 0, G_OPTION_ARG_NONE, &opt_devel, N_("Use development runtime"), NULL },
  { "userns", 0, 0, G_OPTION_ARG_NONE, &opt_userns, N_("Allow user namespaces inside sandbox"), NULL },
  { "runtime", 0, 0, G_OPTION_ARG_STRING, &opt_runtime, N_("Runtime to use"), N_("RUNTIME") },
  { "runtime-version", 0, 0, G_OPTION_ARG_STRING, &opt_runtime_version, N_("Runtime version to use"), N_("VERSION") },
  { "log-session-bus", 0, 0, G_OPTION_ARG_NONE, &opt_log_session_bus, N_("Log session bus calls"), NULL },
  { "log-system-bus", 0, 0, G_OPTION_ARG_NONE, &opt_log_system_bus, N_("Log system bus calls"), NULL },
  { "log-a11y-bus", 0, 0, G_OPTION_ARG_NONE, &opt_log_a11y_bus, N_("Log accessibility bus calls"), NULL },
  { "no-a11y-bus", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &opt_a11y_bus, N_("Don't proxy accessibility bus calls"), NULL },
  { "a11y-bus", 0, 0, G_OPTION_ARG_NONE, &opt_a11y_bus, N_("Proxy accessibility bus calls (default except when sandboxed)"), NULL },
  { "no-session-bus", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &opt_session_bus, N_("Don't proxy session bus calls"), NULL },
  { "session-bus", 0, 0, G_OPTION_ARG_NONE, &opt_session_bus, N_("Proxy session bus calls (default except when sandboxed)"), NULL },
  { "no-documents-portal", 0, 0, G_OPTION_ARG_NONE, &opt_no_documents_portal, N_("Don't start portals"), NULL },
  { "file-forwarding", 0, 0, G_OPTION_ARG_NONE, &opt_file_forwarding, N_("Enable file forwarding"), NULL },
  { "commit", 0, 0, G_OPTION_ARG_STRING, &opt_commit, N_("Run specified commit"), NULL },
  { "runtime-commit", 0, 0, G_OPTION_ARG_STRING, &opt_runtime_commit, N_("Use specified runtime commit"), NULL },
  { "sandbox", 0, 0, G_OPTION_ARG_NONE, &opt_sandbox, N_("Run completely sandboxed"), NULL },
  { "die-with-parent", 'p', 0, G_OPTION_ARG_NONE, &opt_die_with_parent, N_("Kill processes when the parent process dies"), NULL },
  { "parent-pid", 0, 0, G_OPTION_ARG_INT, &opt_parent_pid, N_("Use PID as parent pid for sharing namespaces"), N_("PID") },
  { "parent-expose-pids", 0, 0, G_OPTION_ARG_NONE, &opt_parent_expose_pids, N_("Make processes visible in parent namespace"), NULL },
  { "parent-share-pids", 0, 0, G_OPTION_ARG_NONE, &opt_parent_share_pids, N_("Share process ID namespace with parent"), NULL },
  { "instance-id-fd", 0, 0, G_OPTION_ARG_INT, &opt_instance_id_fd, N_("Write the instance ID to the given file descriptor"), NULL },
  { "app-path", 0, 0, G_OPTION_ARG_FILENAME, &opt_app_path, N_("Use PATH instead of the app's /app"), N_("PATH") },
  { "usr-path", 0, 0, G_OPTION_ARG_FILENAME, &opt_usr_path, N_("Use PATH instead of the runtime's /usr"), N_("PATH") },
  { "clear-env", 0, 0, G_OPTION_ARG_NONE, &opt_clear_env, N_("Clear all outside environment variables"), NULL },
  { NULL }
};

gboolean
flatpak_builtin_run (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_auto(GStrv) run_environ = NULL;
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDeploy) app_deploy = NULL;
  g_autoptr(FlatpakDecomposed) app_ref = NULL;
  g_autoptr(FlatpakDecomposed) runtime_ref = NULL;
  const char *pref;
  int i;
  int rest_argv_start = 0, rest_argc = 0;
  g_autoptr(FlatpakContext) arg_context = NULL;
  g_autofree char *id = NULL;
  g_autofree char *arch = NULL;
  g_autofree char *branch = NULL;
  FlatpakKinds kinds;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  FlatpakRunFlags flags = 0;

  run_environ = g_get_environ ();

  context = g_option_context_new (_("APP [ARGUMENT…] - Run an app"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

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

  arg_context = flatpak_context_new ();
  g_option_context_add_group (context, flatpak_context_get_options (arg_context));

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, cancellable, error))
    return FALSE;

  /* Move the user dir to the front so it "wins" in case an app is in more than
   * one installation */
  if (dirs->len > 1)
    {
      /* Walk through the array backwards so we can safely remove */
      for (i = dirs->len; i > 0; i--)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i - 1);
          if (flatpak_dir_is_user (dir))
            {
              g_ptr_array_insert (dirs, 0, g_object_ref (dir));
              g_ptr_array_remove_index (dirs, i);
              break;
            }
        }
    }

  if (rest_argc == 0)
    return usage_error (context, _("APP must be specified"), error);

  /* If we get here, then rest_argv_start must have been set >= 1 */
  g_assert (rest_argv_start > 0);
  pref = argv[rest_argv_start];

  if (!flatpak_split_partial_ref_arg (pref, FLATPAK_KINDS_APP | FLATPAK_KINDS_RUNTIME,
                                      opt_arch, opt_branch,
                                      &kinds, &id, &arch, &branch, error))
    return FALSE;

  if (branch == NULL || arch == NULL)
    {
      g_autoptr(FlatpakDecomposed) current_ref = flatpak_find_current_ref (id, NULL, NULL);
      if (current_ref)
        {
          if (branch == NULL)
            branch = flatpak_decomposed_dup_branch (current_ref);
          if (arch == NULL)
            arch = flatpak_decomposed_dup_arch (current_ref);
        }
    }

  if ((kinds & FLATPAK_KINDS_APP) != 0)
    {
      app_ref = flatpak_decomposed_new_from_parts (FLATPAK_KINDS_APP, id, arch, branch, &local_error);
      if (app_ref != NULL)
        app_deploy = flatpak_find_deploy_for_ref_in (dirs, flatpak_decomposed_get_ref (app_ref), opt_commit, cancellable, &local_error);

      if (app_deploy == NULL &&
          (!g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED) ||
           (kinds & FLATPAK_KINDS_RUNTIME) == 0))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
      /* On error, local_error is set after this point so we can reuse
         this error rather than later errors, as the app-kind error
         is more likely interesting. */
    }

  if (app_deploy == NULL)
    {
      g_autoptr(FlatpakDeploy) runtime_deploy = NULL;
      g_autoptr(GError) local_error2 = NULL;
      g_autoptr(GPtrArray) ref_dir_pairs = NULL;
      RefDirPair *chosen_pair = NULL;
      const char *runtime_arch = arch;

      /* If arch is not specified, only run the default arch to avoid asking for prompts for non-primary arches,
         still asks for prompts if there are multiple branches though */
      if (runtime_arch == NULL)
        runtime_arch = flatpak_get_arch ();

      /* Whereas for apps we want to default to using the "current" one (see
       * flatpak-make-current(1)) runtimes don't have a concept of currentness.
       * So prompt if there's ambiguity about which branch to use */
      ref_dir_pairs = g_ptr_array_new_with_free_func ((GDestroyNotify) ref_dir_pair_free);
      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          g_autoptr(GPtrArray) refs = NULL;

          refs = flatpak_dir_find_installed_refs (dir, id, branch, runtime_arch, FLATPAK_KINDS_RUNTIME,
                                                  FIND_MATCHING_REFS_FLAGS_NONE, error);
          if (refs == NULL)
            return FALSE;
          else if (refs->len == 0)
            continue;

          for (int j = 0; j < refs->len; j++)
            {
              FlatpakDecomposed *ref = g_ptr_array_index (refs, j);
              RefDirPair *pair = ref_dir_pair_new (ref, dir);
              g_ptr_array_add (ref_dir_pairs, pair);
            }
        }

      if (ref_dir_pairs->len > 0)
        {
          g_autoptr(GPtrArray) chosen_pairs = NULL;
          g_autoptr(GPtrArray) chosen_dir_array = NULL;

          chosen_pairs = g_ptr_array_new ();

          if (!flatpak_resolve_matching_installed_refs (TRUE, TRUE, ref_dir_pairs, id, chosen_pairs, error))
            return FALSE;

          g_assert (chosen_pairs->len == 1);
          chosen_pair = g_ptr_array_index (chosen_pairs, 0);

          /* For runtimes we don't need to pass a FlatpakDeploy object to
           * flatpak_run_app(), but get it anyway because we don't want to run
           * something that's not deployed */
          chosen_dir_array = g_ptr_array_new ();
          g_ptr_array_add (chosen_dir_array, chosen_pair->dir);
          runtime_deploy = flatpak_find_deploy_for_ref_in (chosen_dir_array, flatpak_decomposed_get_ref (chosen_pair->ref),
                                                           opt_commit ? opt_commit : opt_runtime_commit,
                                                           cancellable, &local_error2);
        }

      if (runtime_deploy == NULL)
        {
          /* Report old app-kind error, as its more likely right */
          if (local_error != NULL)
            g_propagate_error (error, g_steal_pointer (&local_error));
          else if (local_error2 != NULL)
            g_propagate_error (error, g_steal_pointer (&local_error2));
          else
            flatpak_fail_error (error, FLATPAK_ERROR_NOT_INSTALLED,
                                _("runtime/%s/%s/%s not installed"),
                                id ?: "*unspecified*",
                                arch ?: "*unspecified*",
                                branch ?: "*unspecified*");
          return FALSE;
        }

      runtime_ref = flatpak_decomposed_ref (chosen_pair->ref);

      /* Clear app-kind error */
      g_clear_error (&local_error);
    }

  /* Default to TRUE, unless sandboxed */
  if (opt_a11y_bus == -1)
    opt_a11y_bus = !opt_sandbox;
  if (opt_session_bus == -1)
    opt_session_bus = !opt_sandbox;

  if (opt_sandbox)
    flags |= FLATPAK_RUN_FLAG_SANDBOX | FLATPAK_RUN_FLAG_NO_SYSTEM_BUS_PROXY;
  if (opt_die_with_parent)
    flags |= FLATPAK_RUN_FLAG_DIE_WITH_PARENT;
  if (opt_devel)
    flags |= FLATPAK_RUN_FLAG_DEVEL;
  if (opt_log_session_bus)
    flags |= FLATPAK_RUN_FLAG_LOG_SESSION_BUS;
  if (opt_log_system_bus)
    flags |= FLATPAK_RUN_FLAG_LOG_SYSTEM_BUS;
  if (opt_log_a11y_bus)
    flags |= FLATPAK_RUN_FLAG_LOG_A11Y_BUS;
  if (opt_file_forwarding)
    flags |= FLATPAK_RUN_FLAG_FILE_FORWARDING;
  if (opt_no_documents_portal)
    flags |= FLATPAK_RUN_FLAG_NO_DOCUMENTS_PORTAL;
  if (opt_parent_expose_pids)
    flags |= FLATPAK_RUN_FLAG_PARENT_EXPOSE_PIDS;
  if (opt_parent_share_pids)
    flags |= FLATPAK_RUN_FLAG_PARENT_SHARE_PIDS;
  if (!opt_a11y_bus)
    flags |= FLATPAK_RUN_FLAG_NO_A11Y_BUS_PROXY;
  if (!opt_session_bus)
    flags |= FLATPAK_RUN_FLAG_NO_SESSION_BUS_PROXY;
  if (!opt_clear_env)
    flags |= FLATPAK_RUN_FLAG_CLEAR_ENV;
  if (opt_userns)
    flags |= FLATPAK_RUN_FLAG_USERNS;

  if (!flatpak_run_app (app_deploy ? app_ref : runtime_ref,
                        app_deploy,
                        opt_app_path,
                        arg_context,
                        opt_runtime,
                        opt_runtime_version,
                        opt_runtime_commit,
                        opt_usr_path,
                        opt_parent_pid,
                        flags,
                        opt_cwd,
                        opt_command,
                        &argv[rest_argv_start + 1],
                        rest_argc - 1,
                        opt_instance_id_fd,
                        (const char * const *) run_environ,
                        NULL,
                        cancellable,
                        error))
    return FALSE;

  /* Not actually reached... */
  return TRUE;
}

gboolean
flatpak_complete_run (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) user_dir = NULL;
  g_autoptr(GPtrArray) system_dirs = NULL;
  g_autoptr(GError) error = NULL;
  int i;
  g_autoptr(FlatpakContext) arg_context = NULL;

  context = g_option_context_new ("");

  arg_context = flatpak_context_new ();
  g_option_context_add_group (context, flatpak_context_get_options (arg_context));

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* NAME */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, user_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_context (completion);

      user_dir = flatpak_dir_get_user ();
      {
        g_autoptr(GPtrArray) refs = flatpak_dir_find_installed_refs (user_dir, NULL, NULL, opt_arch,
                                                                     FLATPAK_KINDS_APP,
                                                                     FIND_MATCHING_REFS_FLAGS_NONE,
                                                                     &error);
        if (refs == NULL)
          flatpak_completion_debug ("find local refs error: %s", error->message);

        flatpak_complete_ref_id (completion, refs);
      }

      system_dirs = flatpak_dir_get_system_list (NULL, &error);
      if (system_dirs == NULL)
        {
          flatpak_completion_debug ("find system installations error: %s", error->message);
          break;
        }

      for (i = 0; i < system_dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (system_dirs, i);
          g_autoptr(GPtrArray) refs = flatpak_dir_find_installed_refs (dir, NULL, NULL, opt_arch,
                                                                       FLATPAK_KINDS_APP,
                                                                       FIND_MATCHING_REFS_FLAGS_NONE,
                                                                       &error);
          if (refs == NULL)
            flatpak_completion_debug ("find local refs error: %s", error->message);

          flatpak_complete_ref_id (completion, refs);
        }

      break;
    }

  return TRUE;
}
