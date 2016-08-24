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
#include "flatpak-utils.h"

static char *opt_arch;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to make current for"), N_("ARCH") },
  { NULL }
};

gboolean
flatpak_builtin_make_current_app (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GFile) deploy_base = NULL;
  char *app;
  char *branch = NULL;
  g_autofree char *ref = NULL;
  g_auto(GLnxLockFile) lock = GLNX_LOCK_FILE_INIT;

  context = g_option_context_new (_("APP BRANCH - Make branch of application current"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("APP must be specified"), error);

  app  = argv[1];

  if (argc >= 3)
    branch = argv[2];

  if (!flatpak_split_partial_ref_arg (app, &opt_arch, &branch, error))
    return FALSE;

  if (branch == NULL)
    return usage_error (context, _("BRANCH must be specified"), error);

  ref = flatpak_dir_find_installed_ref (dir,
                                        app,
                                        branch,
                                        opt_arch,
                                        TRUE, FALSE, NULL,
                                        error);
  if (ref == NULL)
    return FALSE;

  if (!flatpak_dir_lock (dir, &lock,
                         cancellable, error))
    return FALSE;

  deploy_base = flatpak_dir_get_deploy_dir (dir, ref);
  if (!g_file_query_exists (deploy_base, cancellable))
    return flatpak_fail (error, _("App %s branch %s is not installed"), app, branch);

  if (!flatpak_dir_make_current_ref (dir, ref, cancellable, error))
    return FALSE;

  if (!flatpak_dir_update_exports (dir, app, cancellable, error))
    return FALSE;

  glnx_release_lock_file (&lock);

  if (!flatpak_dir_mark_changed (dir, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_complete_make_current_app (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) refs = NULL;
  int i;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv, 0, &dir, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* NAME */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      refs = flatpak_dir_find_installed_refs (dir, NULL, NULL, opt_arch,
                                              TRUE, FALSE, &error);
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
                                              TRUE, FALSE, &error);
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
