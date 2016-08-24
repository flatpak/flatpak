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
#include <string.h>

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils.h"
#include "flatpak-dbus.h"
#include "flatpak-run.h"

static char *opt_arch;
static char *opt_branch;
static char *opt_command;
static gboolean opt_devel;
static gboolean opt_log_session_bus;
static gboolean opt_log_system_bus;
static char *opt_runtime;
static char *opt_runtime_version;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to use"), N_("ARCH") },
  { "command", 0, 0, G_OPTION_ARG_STRING, &opt_command, N_("Command to run"), N_("COMMAND") },
  { "branch", 0, 0, G_OPTION_ARG_STRING, &opt_branch, N_("Branch to use"), N_("BRANCH") },
  { "devel", 'd', 0, G_OPTION_ARG_NONE, &opt_devel, N_("Use development runtime"), NULL },
  { "runtime", 0, 0, G_OPTION_ARG_STRING, &opt_runtime, N_("Runtime to use"), N_("RUNTIME") },
  { "runtime-version", 0, 0, G_OPTION_ARG_STRING, &opt_runtime_version, N_("Runtime version to use"), N_("VERSION") },
  { "log-session-bus", 0, 0, G_OPTION_ARG_NONE, &opt_log_session_bus, N_("Log session bus calls"), NULL },
  { "log-system-bus", 0, 0, G_OPTION_ARG_NONE, &opt_log_system_bus, N_("Log system bus calls"), NULL },
  { NULL }
};

gboolean
flatpak_builtin_run (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDeploy) app_deploy = NULL;
  g_autofree char *app_ref = NULL;
  char *app;
  int i;
  int rest_argv_start, rest_argc;
  g_autoptr(FlatpakContext) arg_context = NULL;

  context = g_option_context_new (_("APP [args...] - Run an app"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

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

  arg_context = flatpak_context_new ();
  g_option_context_add_group (context, flatpak_context_get_options (arg_context));

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (rest_argc == 0)
    return usage_error (context, _("APP must be specified"), error);

  app = argv[rest_argv_start];

  if (!flatpak_split_partial_ref_arg (app, &opt_arch, &opt_branch, error))
    return FALSE;

  if (opt_branch == NULL && opt_arch == NULL)
    {
      g_autoptr(FlatpakDir) user_dir = flatpak_dir_get_user ();
      g_autoptr(FlatpakDir) system_dir = flatpak_dir_get_system ();

      app_ref = flatpak_dir_current_ref (user_dir, app, cancellable);
      if (app_ref == NULL)
        app_ref = flatpak_dir_current_ref (system_dir, app, cancellable);
    }

  if (app_ref == NULL)
    {
      app_ref = flatpak_compose_ref (TRUE, app, opt_branch, opt_arch, error);
      if (app_ref == NULL)
        return FALSE;
    }

  app_deploy = flatpak_find_deploy_for_ref (app_ref, cancellable, error);
  if (app_deploy == NULL)
    return FALSE;

  if (!flatpak_run_app (app_ref, app_deploy,
                        arg_context,
                        opt_runtime,
                        opt_runtime_version,
                        (opt_devel ? FLATPAK_RUN_FLAG_DEVEL : 0) |
                        (opt_log_session_bus ? FLATPAK_RUN_FLAG_LOG_SESSION_BUS : 0) |
                        (opt_log_system_bus ? FLATPAK_RUN_FLAG_LOG_SYSTEM_BUS : 0),
                        opt_command,
                        &argv[rest_argv_start + 1],
                        rest_argc - 1,
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
  g_autoptr(FlatpakDir) system_dir = NULL;
  g_autoptr(GError) error = NULL;
  int i;
  g_autoptr(FlatpakContext) arg_context = NULL;

  context = g_option_context_new ("");

  arg_context = flatpak_context_new ();
  g_option_context_add_group (context, flatpak_context_get_options (arg_context));

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* NAME */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_context_complete (arg_context, completion);

      user_dir = flatpak_dir_get_user ();
      {
        g_auto(GStrv) refs = flatpak_dir_find_installed_refs (user_dir, NULL, NULL, opt_arch,
                                                              TRUE, FALSE, &error);
        if (refs == NULL)
          flatpak_completion_debug ("find local refs error: %s", error->message);
        for (i = 0; refs != NULL && refs[i] != NULL; i++)
          {
            g_auto(GStrv) parts = flatpak_decompose_ref (refs[i], NULL);
            if (parts)
              flatpak_complete_word (completion, "%s ", parts[1]);
          }
      }

      system_dir = flatpak_dir_get_system ();
      {
        g_auto(GStrv) refs = flatpak_dir_find_installed_refs (system_dir, NULL, NULL, opt_arch,
                                                              TRUE, FALSE, &error);
        if (refs == NULL)
          flatpak_completion_debug ("find local refs error: %s", error->message);
        for (i = 0; refs != NULL && refs[i] != NULL; i++)
          {
            g_auto(GStrv) parts = flatpak_decompose_ref (refs[i], NULL);
            if (parts)
              flatpak_complete_word (completion, "%s ", parts[1]);
          }
      }

      break;

    }

  return TRUE;
}
