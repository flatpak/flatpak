/*
 * Copyright © 2016 Red Hat, Inc
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

static gboolean opt_user;
static gboolean opt_system;
static gboolean opt_runtime;
static gboolean opt_app;
static gboolean opt_show_ref;
static gboolean opt_show_commit;
static gboolean opt_show_origin;
static char *opt_arch;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to use", "ARCH" },
  { "user", 0, 0, G_OPTION_ARG_NONE, &opt_user, "Show user installations", NULL },
  { "system", 0, 0, G_OPTION_ARG_NONE, &opt_system, "Show system-wide installations", NULL },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, "List installed runtimes", },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, "List installed applications", },
  { "show-ref", 'r', 0, G_OPTION_ARG_NONE, &opt_show_ref, "Show ref", },
  { "show-commit", 'c', 0, G_OPTION_ARG_NONE, &opt_show_commit, "Show commit", },
  { "show-origin", 'o', 0, G_OPTION_ARG_NONE, &opt_show_origin, "Show origin", },
  { NULL }
};

gboolean
xdg_app_builtin_info (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autofree char *ref = NULL;
  g_autoptr(XdgAppDir) user_dir = NULL;
  g_autoptr(XdgAppDir) system_dir = NULL;
  XdgAppDir *dir = NULL;
  g_autoptr(GError) lookup_error = NULL;
  const char *name;
  const char *branch = "master";
  g_autofree char *commit = NULL;
  g_autofree char *origin = NULL;
  gboolean is_app = FALSE;
  gboolean first = TRUE;

  context = g_option_context_new ("NAME [BRANCH] - Get info about installed app and/or runtime");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, "NAME must be specified", error);
  name = argv[1];

  if (argc >= 3)
    branch = argv[2];

  if (!opt_app && !opt_runtime)
    opt_app = opt_runtime = TRUE;

  if (!opt_user && !opt_system)
    opt_user = opt_system = TRUE;

  if (opt_user)
    {
      user_dir = xdg_app_dir_get_user ();

      ref = xdg_app_dir_find_installed_ref (user_dir,
                                            name,
                                            branch,
                                            opt_arch,
                                            opt_app, opt_runtime, &is_app,
                                            &lookup_error);
      if (ref)
        dir = user_dir;
    }

  if (ref == NULL && opt_system)
    {
      system_dir = xdg_app_dir_get_user ();

      ref = xdg_app_dir_find_installed_ref (system_dir,
                                            name,
                                            branch,
                                            opt_arch,
                                            opt_app, opt_runtime, &is_app,
                                            lookup_error == NULL ? &lookup_error : NULL);
      if (ref)
        dir = system_dir;
    }

  if (ref == NULL)
    {
      g_propagate_error (error, g_steal_pointer (&lookup_error));
      return FALSE;
    }

  commit = xdg_app_dir_read_active (dir, ref, NULL);
  if (commit == NULL)
    return FALSE;

  origin = xdg_app_dir_get_origin (dir, ref, NULL, NULL);

  if (!opt_show_ref && !opt_show_origin && !opt_show_commit)
    opt_show_ref = opt_show_origin = opt_show_commit = TRUE;

  if (opt_show_ref)
    {
      if (first)
        first = FALSE;
      else
        g_print (" ");

      g_print ("%s", ref);
    }

  if (opt_show_origin)
    {
      if (first)
        first = FALSE;
      else
        g_print (" ");

      g_print ("%s", origin ? origin : "-");
    }

  if (opt_show_commit)
    {
      if (first)
        first = FALSE;
      else
        g_print (" ");

      g_print ("%s", commit);
    }

  g_print ("\n");

  return TRUE;
}
