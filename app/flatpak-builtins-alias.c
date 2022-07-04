/*
 * Copyright © 2022 Matthew Leeds
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
 *       Matthew Leeds <mwleeds@protonmail.com>
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
#include "flatpak-cli-transaction.h"
#include "flatpak-quiet-transaction.h"
#include "flatpak-utils-private.h"
#include "flatpak-error.h"
#include "flatpak-table-printer.h"

static gboolean opt_remove;

static GOptionEntry options[] = {
  { "remove", 0, 0, G_OPTION_ARG_NONE, &opt_remove, N_("Remove the specified alias"), NULL },
  { NULL }
};

gboolean
flatpak_builtin_alias (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  int i;

  context = g_option_context_new (_("ALIAS [APP] - Add an alias for running the app APP"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

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

  if (argc == 1)
    {
      g_autoptr(FlatpakTablePrinter) printer = NULL;

      printer = flatpak_table_printer_new ();
      flatpak_table_printer_set_column_title (printer, 0, _("Alias"));
      flatpak_table_printer_set_column_title (printer, 1, _("App"));
      flatpak_table_printer_set_column_title (printer, 2, _("Installation"));

      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          g_autoptr(GHashTable) aliases = NULL; /* alias → app-id */

          aliases = flatpak_dir_get_aliases (dir);

          GLNX_HASH_TABLE_FOREACH_KV (aliases, const char *, alias, const char *, app_id)
            {
              flatpak_table_printer_add_column (printer, alias);
              flatpak_table_printer_add_column (printer, app_id);
              flatpak_table_printer_add_column (printer, flatpak_dir_get_name_cached (dir));
              flatpak_table_printer_finish_row (printer);
            }
        }

      if (flatpak_table_printer_get_current_row (printer) > 0)
        flatpak_table_printer_print (printer);
      else if (flatpak_fancy_output ())
        g_print (_("No aliases\n"));
    }
  else if (opt_remove && argc == 2)
    {
      const char *alias = argv[1];
      g_autoptr(GError) saved_error = NULL;

      for (i = 0; i < dirs->len; i++)
        {
          g_autoptr(GError) local_error = NULL;
          FlatpakDir *dir = g_ptr_array_index (dirs, i);

          if (flatpak_dir_remove_alias (dir, alias, &local_error))
            return TRUE;
          else
            {
              if (g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_ALIAS_NOT_FOUND))
                {
                  if (saved_error == NULL)
                    g_propagate_error (&saved_error, g_steal_pointer (&local_error));
                  continue;
                }
              else
                {
                  g_propagate_error (error, g_steal_pointer (&local_error));
                  return FALSE;
                }
            }
        }

      g_propagate_error (error, g_steal_pointer (&saved_error));
      return FALSE;
    }
  else if (!opt_remove && argc == 3)
    {
      const char *alias = argv[1];
      const char *app = argv[2];
      FlatpakDir *dir_to_use = NULL;
      g_autoptr(FlatpakDecomposed) current = NULL;

      /* Check if the named app is deployed */
      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          g_autoptr(GFile) deploy = NULL;

          g_clear_pointer (&current, flatpak_decomposed_unref);
          current = flatpak_dir_current_ref (dir, app, cancellable);
          if (current)
            deploy = flatpak_dir_get_if_deployed (dir, current, NULL, cancellable);

          if (deploy != NULL)
            {
              dir_to_use = dir;
              break;
            }
        }
      if (dir_to_use == NULL)
        return flatpak_fail_error (error, FLATPAK_ERROR_NOT_INSTALLED,
                                   _("App %s not installed"), app);

      if (!flatpak_dir_make_alias (dir_to_use, current, alias, error))
        return FALSE;
    }
  else
    return usage_error (context, _("Wrong number of arguments"), error);

  return TRUE;
}

gboolean
flatpak_complete_alias (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(GError) error = NULL;
  int i;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* ALIAS */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      if (opt_remove)
        {
          for (i = 0; i < dirs->len; i++)
            {
              FlatpakDir *dir = g_ptr_array_index (dirs, i);
              g_autoptr(GHashTable) aliases = NULL; /* alias → app-id */

              aliases = flatpak_dir_get_aliases (dir);
              GLNX_HASH_TABLE_FOREACH (aliases, const char *, alias)
                flatpak_complete_word (completion, "%s", alias);
            }
        }
      break;
    case 2: /* APP */
      if (!opt_remove)
        {
          for (i = 0; i < dirs->len; i++)
            {
              FlatpakDir *dir = g_ptr_array_index (dirs, i);
              g_autoptr(GPtrArray) refs = NULL;
              refs = flatpak_dir_find_installed_refs (dir, NULL, NULL, NULL,
                                                      FLATPAK_KINDS_APP,
                                                      FIND_MATCHING_REFS_FLAGS_NONE,
                                                      &error);
              if (refs == NULL)
                flatpak_completion_debug ("find installed refs error: %s", error->message);

              flatpak_complete_ref_id (completion, refs);
            }
        }
      break;
    }

  return TRUE;
}
