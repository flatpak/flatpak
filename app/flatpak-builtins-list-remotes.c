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

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils-private.h"
#include "flatpak-table-printer.h"

static gboolean opt_show_details;
static gboolean opt_show_disabled;

static GOptionEntry options[] = {
  { "show-details", 'd', 0, G_OPTION_ARG_NONE, &opt_show_details, N_("Show remote details"), NULL },
  { "show-disabled", 0, 0, G_OPTION_ARG_NONE, &opt_show_disabled, N_("Show disabled remotes"), NULL },
  { NULL }
};

gboolean
flatpak_builtin_list_remotes (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  guint i = 0, j;
  FlatpakTablePrinter *printer;

  context = g_option_context_new (_(" - List remote repositories"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO, &dirs, cancellable, error))
    return FALSE;

  if (argc > 1)
    return usage_error (context, _("Too many arguments"), error);

  printer = flatpak_table_printer_new ();

  j = 0;
  flatpak_table_printer_set_column_title (printer, j++, _("Name"));
  if (opt_show_details)
    {
      flatpak_table_printer_set_column_title (printer, j++, _("Title"));
      flatpak_table_printer_set_column_title (printer, j++, _("URL"));
      flatpak_table_printer_set_column_title (printer, j++, _("Collection ID"));
      flatpak_table_printer_set_column_title (printer, j++, _("Priority"));
    }
  flatpak_table_printer_set_column_title (printer, j++, _("Options"));

  for (j = 0; j < dirs->len; j++)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, j);
      g_auto(GStrv) remotes = NULL;

      remotes = flatpak_dir_list_remotes (dir, cancellable, error);
      if (remotes == NULL)
        return FALSE;

      for (i = 0; remotes[i] != NULL; i++)
        {
          char *remote_name = remotes[i];
          gboolean disabled;
          g_autofree char *remote_url = NULL;
          g_autofree char *title = NULL;
          g_autofree char *collection_id = NULL;
          int prio;
          g_autofree char *prio_as_string = NULL;
          gboolean gpg_verify = TRUE;

          disabled = flatpak_dir_get_remote_disabled (dir, remote_name);
          if (disabled && !opt_show_disabled)
            continue;

          flatpak_table_printer_add_column (printer, remote_name);

          if (opt_show_details)
            {
              title = flatpak_dir_get_remote_title (dir, remote_name);
              if (title)
                flatpak_table_printer_add_column (printer, title);
              else
                flatpak_table_printer_add_column (printer, "-");

              if (ostree_repo_remote_get_url (flatpak_dir_get_repo (dir), remote_name, &remote_url, NULL))
                flatpak_table_printer_add_column (printer, remote_url);
              else
                flatpak_table_printer_add_column (printer, "-");

              collection_id = flatpak_dir_get_remote_collection_id (dir, remote_name);
              if (collection_id != NULL)
                flatpak_table_printer_add_column (printer, collection_id);
              else
                flatpak_table_printer_add_column (printer, "-");

              prio = flatpak_dir_get_remote_prio (dir, remote_name);
              prio_as_string = g_strdup_printf ("%d", prio);
              flatpak_table_printer_add_column (printer, prio_as_string);
            }

          flatpak_table_printer_add_column (printer, ""); /* Options */

          if (dirs->len > 1)
            {
              g_autofree char *dir_id = flatpak_dir_get_name (dir);
              flatpak_table_printer_append_with_comma (printer, dir_id);
            }

          if (disabled)
            flatpak_table_printer_append_with_comma (printer, "disabled");

          if (flatpak_dir_get_remote_oci (dir, remote_name))
            flatpak_table_printer_append_with_comma (printer, "oci");

          if (flatpak_dir_get_remote_noenumerate (dir, remote_name))
            flatpak_table_printer_append_with_comma (printer, "no-enumerate");

          ostree_repo_remote_get_gpg_verify (flatpak_dir_get_repo (dir), remote_name,
                                             &gpg_verify, NULL);
          if (!gpg_verify)
            flatpak_table_printer_append_with_comma (printer, "no-gpg-verify");

          flatpak_table_printer_finish_row (printer);
        }
    }

  flatpak_table_printer_print (printer);
  flatpak_table_printer_free (printer);

  return TRUE;
}

gboolean
flatpak_complete_list_remotes (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1:
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      break;
    }

  return TRUE;
}
