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
#include <unistd.h>
#include <string.h>

#include <glib/gi18n.h>

#include "libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-quiet-transaction.h"

static gboolean opt_force;
static gboolean opt_unused;

static GOptionEntry delete_options[] = {
  { "force", 0, 0, G_OPTION_ARG_NONE, &opt_force, N_("Remove remote even if in use"), NULL },
  { "unused", 0, 0, G_OPTION_ARG_NONE, &opt_unused, N_("Remove all remotes that are not in use"), NULL },
  { NULL }
};

static gboolean
remove_unused_remotes (GPtrArray    *dirs,
                       GCancellable *cancellable,
                       GError      **error)
{
  g_autoptr(GPtrArray) unused_remotes = NULL;

  unused_remotes = g_ptr_array_new_with_free_func ((GDestroyNotify) remote_dir_pair_free);

  for (int dir_index = 0; dir_index < dirs->len; dir_index++)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, dir_index);
      g_autoptr(GHashTable) used_remotes = NULL;
      g_autoptr(GPtrArray) refs = NULL;
      g_auto(GStrv) remotes = NULL;

      refs = flatpak_dir_find_installed_refs (dir, NULL, NULL, NULL,
                                              FLATPAK_KINDS_APP | FLATPAK_KINDS_RUNTIME,
                                              0, error);
      if (refs == NULL)
        return FALSE;

      used_remotes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      for (int ref_index = 0; ref_index < refs->len; ref_index++)
        {
          FlatpakDecomposed *ref = g_ptr_array_index (refs, ref_index);
          g_autofree char *origin = flatpak_dir_get_origin (dir, ref, cancellable, error);

          if (origin == NULL && error != NULL && *error != NULL)
            return FALSE;
          if (origin != NULL)
            g_hash_table_add (used_remotes, g_steal_pointer (&origin));
        }

      remotes = flatpak_dir_list_remotes (dir, cancellable, error);
      if (remotes == NULL)
        return FALSE;

      for (int remote_index = 0; remotes[remote_index] != NULL; remote_index++)
        {
          if (!g_hash_table_contains (used_remotes, remotes[remote_index]))
            g_ptr_array_add (unused_remotes, remote_dir_pair_new (remotes[remote_index], dir));
        }
    }

  if (unused_remotes->len == 0)
    return TRUE;

  for (int dir_index = 0; dir_index < dirs->len; dir_index++)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, dir_index);
      gboolean printed_heading = FALSE;

      for (int unused_index = 0; unused_index < unused_remotes->len; unused_index++)
        {
          RemoteDirPair *pair = g_ptr_array_index (unused_remotes, unused_index);

          if (pair->dir != dir)
            continue;

          if (!printed_heading)
            {
              g_print (_("Unused remotes in the %s installation:\n"),
                       flatpak_dir_get_name_cached (dir));
              printed_heading = TRUE;
            }
          g_print ("  %s\n", pair->remote_name);
        }

      if (printed_heading)
        g_print ("\n");
    }

  if (!flatpak_yes_no_prompt (TRUE, _("Remove all unused remotes?")))
    return TRUE;

  for (int removal_index = 0; removal_index < unused_remotes->len; removal_index++)
    {
      RemoteDirPair *pair = g_ptr_array_index (unused_remotes, removal_index);

      if (!flatpak_dir_remove_remote (pair->dir, FALSE, pair->remote_name,
                                      cancellable, error))
        return FALSE;
    }

  g_print (ngettext ("Removed %u unused remote.\n",
                     "Removed %u unused remotes.\n",
                     unused_remotes->len),
           unused_remotes->len);

  return TRUE;
}


gboolean
flatpak_builtin_remote_delete (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(FlatpakDir) preferred_dir = NULL;
  gboolean removed_all_refs = FALSE;
  const char *remote_name;

  context = g_option_context_new (_("[NAME] - Delete a remote repository"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  g_option_context_add_main_entries (context, delete_options, NULL);

  if (!flatpak_option_context_parse (context, NULL, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS, &dirs, cancellable, error))
    return FALSE;

  if (opt_unused)
    {
      if (argc > 1)
        return usage_error (context, _("NAME and --unused are mutually exclusive"), error);

      return remove_unused_remotes (dirs, cancellable, error);
    }

  if (argc < 2)
    return usage_error (context, _("NAME must be specified"), error);

  remote_name = argv[1];

  if (argc > 2)
    return usage_error (context, _("Too many arguments"), error);

  if (!flatpak_resolve_duplicate_remotes (dirs, remote_name, FALSE, &preferred_dir, cancellable, error))
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
          g_autofree char *origin = flatpak_dir_get_origin (preferred_dir, ref, NULL, NULL);
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

      if (!opt_unused)
        {
          for (int dir_index = 0; dir_index < dirs->len; dir_index++)
            {
              FlatpakDir *dir = g_ptr_array_index (dirs, dir_index);
              g_auto(GStrv) remotes = flatpak_dir_list_remotes (dir, NULL, NULL);
              if (remotes == NULL)
                return FALSE;
              for (int remote_index = 0; remotes[remote_index] != NULL; remote_index++)
                flatpak_complete_word (completion, "%s ", remotes[remote_index]);
            }
        }

      break;
    }

  return TRUE;
}
