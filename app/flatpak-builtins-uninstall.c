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
static gboolean opt_keep_ref;
static gboolean opt_force_remove;
static gboolean opt_runtime;
static gboolean opt_app;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to uninstall", "ARCH" },
  { "keep-ref", 0, 0, G_OPTION_ARG_NONE, &opt_keep_ref, "Keep ref in local repository", NULL },
  { "force-remove", 0, 0, G_OPTION_ARG_NONE, &opt_force_remove, "Remove files even if running", NULL },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, "Look for runtime with the specified name", },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, "Look for app with the specified name", },
  { NULL }
};

gboolean
flatpak_builtin_uninstall (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  const char *name = NULL;
  const char *branch = NULL;
  g_autofree char *ref = NULL;
  gboolean is_app;
  FlatpakHelperUninstallFlags flags = 0;

  context = g_option_context_new ("NAME [BRANCH] - Uninstall an application");

  if (!flatpak_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, "APP must be specified", error);

  name = argv[1];
  if (argc > 2)
    branch = argv[2];

  if (!opt_app && !opt_runtime)
    opt_app = opt_runtime = TRUE;

  ref = flatpak_dir_find_installed_ref (dir,
                                        name,
                                        branch,
                                        opt_arch,
                                        opt_app, opt_runtime, &is_app,
                                        error);
  if (ref == NULL)
    return FALSE;

  /* TODO: when removing runtimes, look for apps that use it, require --force */

  if (opt_keep_ref)
    flags |= FLATPAK_HELPER_UNINSTALL_FLAGS_KEEP_REF;
  if (opt_force_remove)
    flags |= FLATPAK_HELPER_UNINSTALL_FLAGS_FORCE_REMOVE;

  if (!flatpak_dir_uninstall (dir, ref, flags,
                              cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_complete_uninstall (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) refs = NULL;
  int i;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv, 0, &dir, NULL, NULL))
    return FALSE;

  if (!opt_app && !opt_runtime)
    opt_app = opt_runtime = TRUE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* NAME */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      refs = flatpak_dir_find_installed_refs (dir, NULL, NULL, opt_arch,
                                              opt_app, opt_runtime, &error);
      if (refs == NULL)
        flatpak_completion_debug ("find installed refs error: %s", error->message);
      for (i = 0; refs != NULL && refs[i] != NULL; i++)
        {
          g_auto(GStrv) parts = flatpak_decompose_ref (refs[i], NULL);
          if (parts)
            flatpak_complete_word (completion, "%s ", parts[1]);
        }
      break;

    case 2: /* Branch */
      refs = flatpak_dir_find_installed_refs (dir, completion->argv[1], NULL, opt_arch,
                                              opt_app, opt_runtime, &error);
      if (refs == NULL)
        flatpak_completion_debug ("find installed refs error: %s", error->message);
      for (i = 0; refs != NULL && refs[i] != NULL; i++)
        {
          g_auto(GStrv) parts = flatpak_decompose_ref (refs[i], NULL);
          if (parts)
            flatpak_complete_word (completion, "%s ", parts[3]);
        }
      break;

    default:
      break;
    }

  return TRUE;
}
