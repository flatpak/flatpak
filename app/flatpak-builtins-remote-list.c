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
#include <string.h>
#include <unistd.h>

#include <glib/gi18n.h>

#include "libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-utils-private.h"
#include "flatpak-table-printer.h"

static gboolean opt_show_details;
static gboolean opt_show_disabled;
static const char **opt_cols;
static gboolean opt_json;

static GOptionEntry options[] = {
  { "show-details", 'd', 0, G_OPTION_ARG_NONE, &opt_show_details, N_("Show remote details"), NULL },
  { "show-disabled", 0, 0, G_OPTION_ARG_NONE, &opt_show_disabled, N_("Show disabled remotes"), NULL },
  { "columns", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_cols, N_("What information to show"), N_("FIELD,…") },
  { "json", 'j', 0, G_OPTION_ARG_NONE, &opt_json, N_("Show output in JSON format"), NULL },
  { NULL }
};

static Column all_columns[] = {
  { "name",       N_("Name"),          N_("Show the name"),          0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "title",      N_("Title"),         N_("Show the title"),         0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "url",        N_("URL"),           N_("Show the URL"),           0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "collection", N_("Collection ID"), N_("Show the collection ID"), 0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "subset",     N_("Subset"),        N_("Show the subset"),        0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "filter",     N_("Filter"),        N_("Show filter file"),       0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "priority",   N_("Priority"),      N_("Show the priority"),      0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "options",    N_("Options"),       N_("Show options"),           0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "comment",    N_("Comment"),       N_("Show comment"),           0, FLATPAK_ELLIPSIZE_MODE_END,  1, 0 },
  { "description", N_("Description"),  N_("Show description"),       0, FLATPAK_ELLIPSIZE_MODE_END,  1, 0 },
  { "homepage",   N_("Homepage"),      N_("Show homepage"),          0, FLATPAK_ELLIPSIZE_MODE_NONE,  1, 0 },
  { "icon",       N_("Icon"),          N_("Show icon"),              0, FLATPAK_ELLIPSIZE_MODE_NONE,  1, 0 },
  { NULL }
};

static gboolean
list_remotes (GPtrArray *dirs, Column *columns, GCancellable *cancellable, GError **error)
{
  g_autoptr(FlatpakTablePrinter) printer = NULL;
  int i, j, k;

  if (columns[0].name == NULL)
    return TRUE;

  printer = flatpak_table_printer_new ();
  flatpak_table_printer_set_columns (printer, columns,
                                     opt_cols == NULL && !opt_show_details);

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

          disabled = flatpak_dir_get_remote_disabled (dir, remote_name);
          if (disabled && !opt_show_disabled)
            continue;

          for (k = 0; columns[k].name; k++)
            {
              if (strcmp (columns[k].name, "name") == 0)
                flatpak_table_printer_add_column (printer, remote_name);
              else if (strcmp (columns[k].name, "title") == 0)
                {
                  g_autofree char *title = flatpak_dir_get_remote_title (dir, remote_name);
                  if (title)
                    flatpak_table_printer_add_column (printer, title);
                  else
                    flatpak_table_printer_add_column (printer, "-");
                }
              else if (strcmp (columns[k].name, "subset") == 0)
                {
                  g_autofree char *subset = flatpak_dir_get_remote_subset (dir, remote_name);
                  if (subset)
                    flatpak_table_printer_add_column (printer, subset);
                  else
                    flatpak_table_printer_add_column (printer, "-");
                }
              else if (strcmp (columns[k].name, "comment") == 0)
                {
                  g_autofree char *comment = flatpak_dir_get_remote_comment (dir, remote_name);
                  if (comment)
                    flatpak_table_printer_add_column (printer, comment);
                  else
                    flatpak_table_printer_add_column (printer, "-");
                }
              else if (strcmp (columns[k].name, "description") == 0)
                {
                  g_autofree char *description = flatpak_dir_get_remote_description (dir, remote_name);
                  if (description)
                    flatpak_table_printer_add_column (printer, description);
                  else
                    flatpak_table_printer_add_column (printer, "-");
                }
              else if (strcmp (columns[k].name, "filter") == 0)
                {
                  g_autofree char *filter = flatpak_dir_get_remote_filter (dir, remote_name);
                  if (filter)
                    flatpak_table_printer_add_column (printer, filter);
                  else
                    flatpak_table_printer_add_column (printer, "-");
                }
              else if (strcmp (columns[k].name, "homepage") == 0)
                {
                  g_autofree char *homepage = flatpak_dir_get_remote_homepage (dir, remote_name);
                  if (homepage)
                    flatpak_table_printer_add_column (printer, homepage);
                  else
                    flatpak_table_printer_add_column (printer, "-");
                }
              else if (strcmp (columns[k].name, "icon") == 0)
                {
                  g_autofree char *icon = flatpak_dir_get_remote_icon (dir, remote_name);
                  if (icon)
                    flatpak_table_printer_add_column (printer, icon);
                  else
                    flatpak_table_printer_add_column (printer, "-");
                }
              else if (strcmp (columns[k].name, "url") == 0)
                {
                  g_autofree char *remote_url = NULL;

                  if (ostree_repo_remote_get_url (flatpak_dir_get_repo (dir), remote_name, &remote_url, NULL))
                    flatpak_table_printer_add_column (printer, remote_url);
                  else
                    flatpak_table_printer_add_column (printer, "-");
                }
              else if (strcmp (columns[k].name, "collection") == 0)
                {
                  g_autofree char *id = flatpak_dir_get_remote_collection_id (dir, remote_name);
                  if (id != NULL)
                    flatpak_table_printer_add_column (printer, id);
                  else
                    flatpak_table_printer_add_column (printer, "-");
                }
              else if (strcmp (columns[k].name, "priority") == 0)
                {
                  int prio = flatpak_dir_get_remote_prio (dir, remote_name);
                  g_autofree char *prio_as_string = g_strdup_printf ("%d", prio);
                  flatpak_table_printer_add_column (printer, prio_as_string);
                }
              else if (strcmp (columns[k].name, "options") == 0)
                {
                  gboolean gpg_verify = TRUE;
                  g_autofree char *filter = flatpak_dir_get_remote_filter (dir, remote_name);

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

                  if (!ostree_repo_remote_get_gpg_verify (flatpak_dir_get_repo (dir), remote_name,
                                                          &gpg_verify, error))
                      return FALSE; /* shouldn't happen unless repo config is modified out-of-band */

                  if (!gpg_verify)
                    flatpak_table_printer_append_with_comma (printer, "no-gpg-verify");

                  if (filter != NULL && *filter != 0)
                    flatpak_table_printer_append_with_comma (printer, "filtered");
                }
            }

          flatpak_table_printer_finish_row (printer);
        }
    }

  opt_json ? flatpak_table_printer_print_json (printer) : flatpak_table_printer_print (printer);

  return TRUE;
}

gboolean
flatpak_builtin_remote_list (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autofree char *col_help = NULL;
  g_autofree Column *columns = NULL;
  g_autoptr(GPtrArray) dirs = NULL;

  context = g_option_context_new (_(" - List remote repositories"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);
  col_help = column_help (all_columns);
  g_option_context_set_description (context, col_help);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO, &dirs, cancellable, error))
    return FALSE;

  if (argc > 1)
    return usage_error (context, _("Too many arguments"), error);

  columns = handle_column_args (all_columns, opt_show_details, opt_cols, error);
  if (columns == NULL)
    return FALSE;

  return list_remotes (dirs, columns, cancellable, error);
}

gboolean
flatpak_complete_remote_list (FlatpakCompletion *completion)
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
      flatpak_complete_columns (completion, all_columns);

      break;
    }

  return TRUE;
}
