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
#include "flatpak-quiet-transaction.h"

static gboolean opt_force;

static GOptionEntry delete_options[] = {
  { "force", 0, 0, G_OPTION_ARG_NONE, &opt_force, N_("Remove remote even if in use"), NULL },
  { NULL }
};


gboolean
flatpak_builtin_remote_delete (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(FlatpakDir) preferred_dir = NULL;
  gboolean removed_all_refs = FALSE;
  const char *remote_name;

  context = g_option_context_new (_("NAME - Delete a remote repository"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  g_option_context_add_main_entries (context, delete_options, NULL);

  if (!flatpak_option_context_parse (context, NULL, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS, &dirs, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("NAME must be specified"), error);

  remote_name = argv[1];

  if (argc > 2)
    return usage_error (context, _("Too many arguments"), error);

  if (!flatpak_resolve_duplicate_remotes (dirs, remote_name, &preferred_dir, cancellable, error))
    return FALSE;

  if (!opt_force)
    {
      g_autoptr(GPtrArray) refs = NULL;
      g_autoptr(GPtrArray) refs_to_remove = NULL;
      int i;

      refs = flatpak_dir_find_installed_refs (preferred_dir, NULL, NULL, NULL, FLATPAK_KINDS_APP | FLATPAK_KINDS_RUNTIME, 0, error);
      if (refs == NULL)
        return FALSE;

      refs_to_remove = g_ptr_array_new_with_free_func (g_free);

      for (i = 0; refs != NULL && i < refs->len; i++)
        {
          FlatpakDecomposed *ref = g_ptr_array_index (refs, i);
          g_autofree char *origin = flatpak_dir_get_origin (preferred_dir, flatpak_decomposed_get_ref (ref), NULL, NULL);
          if (g_strcmp0 (origin, remote_name) == 0)
            g_ptr_array_add (refs_to_remove, flatpak_decomposed_dup_ref (ref));
        }

      if (refs_to_remove->len > 0)
        {
          g_autoptr(FlatpakTransaction) transaction = NULL;

          g_ptr_array_add (refs_to_remove, NULL);

          flatpak_format_choices ((const char **) refs_to_remove->pdata,
                                  _("The following refs are installed from remote '%s':"), remote_name);
          if (!flatpak_yes_no_prompt (FALSE, _("Remove them?")))
            return flatpak_fail_error (error, FLATPAK_ERROR_REMOTE_USED,
                                       _("Can't remove remote '%s' with installed refs"), remote_name);

          transaction = flatpak_quiet_transaction_new (preferred_dir, error);
          if (transaction == NULL)
            return FALSE;

          for (i = 0; i < refs_to_remove->len - 1; i++)
            {
              const char *ref = g_ptr_array_index (refs_to_remove, i);
              if (!flatpak_transaction_add_uninstall (transaction, ref, error))
                return FALSE;
            }

          if (!flatpak_transaction_run (transaction, cancellable, error))
            {
              if (g_error_matches (*error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED))
                g_clear_error (error);  /* Don't report on stderr */

              return FALSE;
            }

          removed_all_refs = TRUE;
        }
    }

  if (g_str_has_suffix (remote_name, "-origin") && removed_all_refs)
    // The remote has already been deleted because all its refs were deleted.
    return TRUE;

  if (!flatpak_dir_remove_remote (preferred_dir, opt_force, remote_name,
                                  cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_complete_remote_delete (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  int i;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, delete_options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS, &dirs, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* REMOTE */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, delete_options);
      flatpak_complete_options (completion, user_entries);

      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          int j;
          g_auto(GStrv) remotes = flatpak_dir_list_remotes (dir, NULL, NULL);
          if (remotes == NULL)
            return FALSE;
          for (j = 0; remotes[j] != NULL; j++)
            flatpak_complete_word (completion, "%s ", remotes[j]);
        }

      break;
    }

  return TRUE;
}
