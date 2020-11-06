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
#include "flatpak-utils-private.h"

static char *opt_arch;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to make current for"), N_("ARCH") },
  { NULL }
};

gboolean
flatpak_builtin_make_current_app (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  FlatpakDir *dir;
  g_autoptr(GFile) deploy_base = NULL;
  const char *pref;
  const char *default_branch = NULL;
  g_auto(GLnxLockFile) lock = { 0, };
  g_autofree char *id = NULL;
  g_autofree char *arch = NULL;
  g_autofree char *branch = NULL;
  g_autoptr(FlatpakDecomposed) ref = NULL;
  FlatpakKinds kinds;

  context = g_option_context_new (_("APP BRANCH - Make branch of application current"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR,
                                     &dirs, cancellable, error))
    return FALSE;

  dir = g_ptr_array_index (dirs, 0);

  if (argc < 2)
    return usage_error (context, _("APP must be specified"), error);

  if (argc > 3)
    return usage_error (context, _("Too many arguments"), error);

  pref = argv[1];

  if (argc >= 3)
    default_branch = argv[2];

  if (!flatpak_split_partial_ref_arg (pref, FLATPAK_KINDS_APP, opt_arch, default_branch,
                                      &kinds, &id, &arch, &branch, error))
    return FALSE;

  if (branch == NULL)
    return usage_error (context, _("BRANCH must be specified"), error);

  ref = flatpak_dir_find_installed_ref (dir, id, branch, arch, FLATPAK_KINDS_APP,
                                        error);
  if (ref == NULL)
    return FALSE;

  if (!flatpak_dir_lock (dir, &lock,
                         cancellable, error))
    return FALSE;

  deploy_base = flatpak_dir_get_deploy_dir (dir, flatpak_decomposed_get_ref (ref));
  if (!g_file_query_exists (deploy_base, cancellable))
    return flatpak_fail (error, _("App %s branch %s is not installed"), id, branch);

  if (!flatpak_dir_make_current_ref (dir, ref, cancellable, error))
    return FALSE;

  if (!flatpak_dir_update_exports (dir, id, cancellable, error))
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
  g_autoptr(GPtrArray) dirs = NULL;
  FlatpakDir *dir;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) refs = NULL;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_ONE_DIR | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, NULL, NULL))
    return FALSE;

  dir = g_ptr_array_index (dirs, 0);

  switch (completion->argc)
    {
    case 0:
    case 1: /* NAME */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      refs = flatpak_dir_find_installed_refs (dir, NULL, NULL, opt_arch,
                                              FLATPAK_KINDS_APP,
                                              FIND_MATCHING_REFS_FLAGS_NONE,
                                              &error);
      if (refs == NULL)
        flatpak_completion_debug ("find installed refs error: %s", error->message);
      flatpak_complete_ref_id (completion, refs);
      break;

    case 2: /* Branch */
      refs = flatpak_dir_find_installed_refs (dir, completion->argv[1], NULL, opt_arch,
                                              FLATPAK_KINDS_APP,
                                              FIND_MATCHING_REFS_FLAGS_NONE,
                                              &error);
      if (refs == NULL)
        flatpak_completion_debug ("find installed refs error: %s", error->message);

      flatpak_complete_ref_branch (completion, refs);
      break;

    default:
      break;
    }

  return TRUE;
}
