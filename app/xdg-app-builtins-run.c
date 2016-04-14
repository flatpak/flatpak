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
static gboolean opt_log_session_bus;
static gboolean opt_log_system_bus;
static char *opt_runtime;
static char *opt_runtime_version;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to use", "ARCH" },
  { "command", 0, 0, G_OPTION_ARG_STRING, &opt_command, "Command to run", "COMMAND" },
  { "branch", 0, 0, G_OPTION_ARG_STRING, &opt_branch, "Branch to use", "BRANCH" },
  { "devel", 'd', 0, G_OPTION_ARG_NONE, &opt_devel, "Use development runtime", NULL },
  { "runtime", 0, 0, G_OPTION_ARG_STRING, &opt_runtime, "Runtime to use", "RUNTIME" },
  { "runtime-version", 0, 0, G_OPTION_ARG_STRING, &opt_runtime_version, "Runtime version to use", "VERSION" },
  { "log-session-bus", 0, 0, G_OPTION_ARG_NONE, &opt_log_session_bus, "Log session bus calls", NULL },
  { "log-system-bus", 0, 0, G_OPTION_ARG_NONE, &opt_log_system_bus, "Log system bus calls", NULL },
  { NULL }
};

gboolean
xdg_app_builtin_run (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(XdgAppDeploy) app_deploy = NULL;
  g_autofree char *app_ref = NULL;
  const char *app;
  const char *branch = "master";
  int i;
  int rest_argv_start, rest_argc;
  g_autoptr(XdgAppContext) arg_context = NULL;

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

  arg_context = xdg_app_context_new ();
  g_option_context_add_group (context, xdg_app_context_get_options (arg_context));

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (rest_argc == 0)
    return usage_error (context, "APP must be specified", error);

  app = argv[rest_argv_start];

  if (opt_branch)
    branch = opt_branch;

  if (opt_branch == NULL && opt_arch == NULL)
    {
      g_autoptr(XdgAppDir) user_dir = xdg_app_dir_get_user ();
      g_autoptr(XdgAppDir) system_dir = xdg_app_dir_get_system ();

      app_ref = xdg_app_dir_current_ref (user_dir, app, cancellable);
      if (app_ref == NULL)
        app_ref = xdg_app_dir_current_ref (system_dir, app, cancellable);
    }

  if (app_ref == NULL)
    {
      app_ref = xdg_app_compose_ref (TRUE, app, branch, opt_arch, error);
      if (app_ref == NULL)
        return FALSE;
    }

  app_deploy = xdg_app_find_deploy_for_ref (app_ref, cancellable, error);
  if (app_deploy == NULL)
    return FALSE;

  if (!xdg_app_run_app (app_ref, app_deploy,
                        arg_context,
                        opt_runtime,
                        opt_runtime_version,
                        (opt_devel ? XDG_APP_RUN_FLAG_DEVEL : 0) |
                        (opt_log_session_bus ? XDG_APP_RUN_FLAG_LOG_SESSION_BUS : 0) |
                        (opt_log_system_bus ? XDG_APP_RUN_FLAG_LOG_SYSTEM_BUS : 0),
                        opt_command,
                        &argv[rest_argv_start + 1],
                        rest_argc - 1,
                        cancellable,
                        error))
    return FALSE;

  /* Not actually reached... */
  return TRUE;
}
