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
#include "flatpak-utils.h"

static gboolean opt_show_details;
static gboolean opt_user;
static gboolean opt_system;
static gboolean opt_show_disabled;
static char **opt_installations;

static GOptionEntry options[] = {
  { "user", 0, 0, G_OPTION_ARG_NONE, &opt_user, N_("Show user installations"), NULL },
  { "system", 0, 0, G_OPTION_ARG_NONE, &opt_system, N_("Show system-wide installations"), NULL },
  { "installation", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_installations, N_("Show specific system-wide installations"), NULL },
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

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc > 1)
    return usage_error (context, _("Too many arguments"), error);

  if (!opt_user && !opt_system && opt_installations == NULL)
    {
      /* Default: All system and user remotes */
      opt_user = opt_system = TRUE;
    }

  dirs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

  if (opt_user)
    g_ptr_array_add (dirs, flatpak_dir_get_user ());

  if (opt_system)
    g_ptr_array_add (dirs, flatpak_dir_get_system_default ());

  if (opt_installations != NULL)
    {
      int i = 0;

      for (i = 0; opt_installations[i] != NULL; i++)
        {
          FlatpakDir *installation_dir = NULL;

          /* Already included the default system installation. */
          if (opt_system && g_strcmp0 (opt_installations[i], "default") == 0)
            continue;

          installation_dir = flatpak_dir_get_system_by_id (opt_installations[i], cancellable, error);
          if (installation_dir == NULL)
            return FALSE;

          g_ptr_array_add (dirs, installation_dir);
        }
    }

  printer = flatpak_table_printer_new ();

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

              ostree_repo_remote_get_url (flatpak_dir_get_repo (dir), remote_name, &remote_url, NULL);

              flatpak_table_printer_add_column (printer, remote_url);

              prio = flatpak_dir_get_remote_prio (dir, remote_name);
              prio_as_string = g_strdup_printf ("%d", prio);
              flatpak_table_printer_add_column (printer, prio_as_string);
            }

          flatpak_table_printer_add_column (printer, ""); /* Options */

          if ((opt_user && opt_system) || (opt_user && opt_installations != NULL)
              || (opt_system && opt_installations != NULL)
              || (opt_installations != NULL && g_strv_length (opt_installations) > 1))
            {
              g_autofree char *dir_id = flatpak_dir_get_name (dir);
              flatpak_table_printer_append_with_comma (printer, dir_id);
            }

          if (disabled)
            flatpak_table_printer_append_with_comma (printer, "disabled");

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
  g_autoptr(FlatpakDir) dir = NULL;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv, 0, &dir, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* REMOTE */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      break;
    }

  return TRUE;
}
