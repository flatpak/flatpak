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
#include <string.h>
#include <unistd.h>

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static gboolean opt_show_details;

static GOptionEntry options[] = {
  { "show-details", 'd', 0, G_OPTION_ARG_NONE, &opt_show_details, "Show remote details", NULL },
  { NULL }
};

gboolean
xdg_app_builtin_list_remotes (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(XdgAppDir) dir = NULL;
  g_auto(GStrv) remotes = NULL;
  guint ii, n_remotes = 0;

  context = g_option_context_new (" - List remote repositories");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  remotes = ostree_repo_remote_list (xdg_app_dir_get_repo (dir), &n_remotes);

  if (opt_show_details)
    {
      XdgAppTablePrinter *printer = xdg_app_table_printer_new ();
      GKeyFile *config = ostree_repo_get_config (xdg_app_dir_get_repo (dir));;
      g_autoptr(GString) options = g_string_new ("");

      for (ii = 0; ii < n_remotes; ii++)
        {
          char *remote_name = remotes[ii];
          g_autofree char *remote_url = NULL;
          g_autofree char *group = NULL;
          g_autofree char *title = NULL;
          gboolean gpg_verify = TRUE;

          group = g_strdup_printf ("remote \"%s\"", remote_name);

          xdg_app_table_printer_add_column (printer, remote_name);

          title = g_key_file_get_string (config, group, "xa.title", NULL);
          if (title)
            xdg_app_table_printer_add_column (printer, title);
          else
            xdg_app_table_printer_add_column (printer, "-");

          ostree_repo_remote_get_url (xdg_app_dir_get_repo (dir), remote_name, &remote_url, NULL);

          xdg_app_table_printer_add_column (printer, remote_url);

          ostree_repo_remote_get_gpg_verify (xdg_app_dir_get_repo (dir), remote_name,
                                             &gpg_verify, NULL);
            if (!gpg_verify)
              {
                if (options->len != 0)
                  g_string_append_c (options, ',');
                g_string_append (options, "no-gpg-verify");
              }

          if (options->len != 0)
            xdg_app_table_printer_add_column (printer, options->str);

          g_string_truncate (options, 0);

          xdg_app_table_printer_finish_row (printer);
        }

      xdg_app_table_printer_print (printer);
      xdg_app_table_printer_free (printer);
    }
  else
    {
      for (ii = 0; ii < n_remotes; ii++)
        g_print ("%s\n", remotes[ii]);
    }

  return TRUE;
}
