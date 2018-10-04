/*
 * Copyright © 2018 Red Hat, Inc
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-table-printer.h"
#include "flatpak-instance-private.h"

static gboolean opt_show_cols;
static const char **opt_cols;

static GOptionEntry options[] = {
  { "show-columns", 0, 0, G_OPTION_ARG_NONE, &opt_show_cols, N_("Show available columns"), NULL },
  { "columns", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_cols, N_("What information to show"), N_("FIELD,…") },
  { NULL }
};

static Column all_columns[] = {
  { "instance",       N_("Instance"),       N_("Show the instance ID"),                1, 1 },
  { "pid",            N_("PID"),            N_("Show the PID of the wrapper process"), 1, 1 },
  { "child-pid",      N_("Child PID"),      N_("Show the PID of the sandbox process"), 1, 0 },
  { "application",    N_("Application"),    N_("Show the application ID"),             1, 1 },
  { "arch",           N_("Architecture"),   N_("Show the architecture"),               1, 0 },
  { "branch",         N_("Branch"),         N_("Show the application branch"),         1, 0 },
  { "commit",         N_("Commit"),         N_("Show the application commit"),         1, 0 },
  { "runtime",        N_("Runtime"),        N_("Show the runtime ID"),                 1, 1 },
  { "runtime-branch", N_("Runtime Branch"), N_("Show the runtime branch"),             1, 0 },
  { "runtime-commit", N_("Runtime Commit"), N_("Show the runtime commit"),             1, 0 },
  { NULL }
};

static gboolean
enumerate_instances (Column *columns, GError **error)
{
  g_autoptr(GPtrArray) instances = NULL;
  FlatpakTablePrinter *printer;
  int i, j;

  if (columns[0].name == NULL)
    return TRUE;

  printer = flatpak_table_printer_new ();

  for (i = 0; columns[i].name; i++)
    flatpak_table_printer_set_column_title (printer, i, columns[i].title);
 
  instances = flatpak_instance_get_all ();
  if (instances->len == 0)
    {
      /* nothing to show */
      return TRUE;
    }

  for (j = 0; j < instances->len; j++)
    {
      FlatpakInstance *instance = (FlatpakInstance *)g_ptr_array_index (instances, j);

      flatpak_table_printer_add_column (printer, flatpak_instance_get_id (instance));

      for (i = 0; columns[i].name; i++)
        {
          if (strcmp (columns[i].name, "pid") == 0)
            {
              g_autofree char *pid = g_strdup_printf ("%d", flatpak_instance_get_pid (instance));
              flatpak_table_printer_add_column (printer, pid);
            }
          else if (strcmp (columns[i].name, "child-pid") == 0)
            {
              g_autofree char *pid = g_strdup_printf ("%d", flatpak_instance_get_child_pid (instance));
              flatpak_table_printer_add_column (printer, pid);
            }
          else if (strcmp (columns[i].name, "application") == 0)
            flatpak_table_printer_add_column (printer, flatpak_instance_get_app (instance));
          else if (strcmp (columns[i].name, "arch") == 0)
            flatpak_table_printer_add_column (printer, flatpak_instance_get_arch (instance));
          else if (strcmp (columns[i].name, "commit") == 0)
            flatpak_table_printer_add_column_len (printer,
                                                  flatpak_instance_get_commit (instance),
                                                  12);
          else if (strcmp (columns[i].name, "runtime") == 0)
            {
              const char *full_ref = flatpak_instance_get_runtime (instance);
              if (full_ref != NULL)
                {
                  g_auto(GStrv) ref = flatpak_decompose_ref (full_ref, NULL);
                  flatpak_table_printer_add_column (printer, ref[1]);
                }
            }
          else if (strcmp (columns[i].name, "runtime-branch") == 0)
            {
              const char *full_ref = flatpak_instance_get_runtime (instance);
              if (full_ref != NULL)
                {
                  g_auto(GStrv) ref = flatpak_decompose_ref (full_ref, NULL);
                  flatpak_table_printer_add_column (printer, ref[3]);
                }
            }
          else if (strcmp (columns[i].name, "runtime-commit") == 0)
            flatpak_table_printer_add_column_len (printer,
                                                  flatpak_instance_get_runtime_commit (instance),
                                                  12);
        }

      flatpak_table_printer_finish_row (printer);
    }

  flatpak_table_printer_print (printer);
  flatpak_table_printer_free (printer);

  return TRUE;
}

gboolean
flatpak_builtin_ps (int            argc,
                    char         **argv,
                    GCancellable  *cancellable,
                    GError       **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autofree char *col_help = NULL;
  g_autofree Column *columns = NULL;

  context = g_option_context_new (_(" - Enumerate running sandboxes"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);
  col_help = column_help (all_columns);
  g_option_context_set_description (context, col_help);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc > 1)
    {
      usage_error (context, _("Extra arguments given"), error);
      return FALSE;
    }

  columns = handle_column_args (all_columns,
                                opt_show_cols, FALSE, opt_cols,
                                error);
  if (columns == NULL)
    return FALSE;

  return enumerate_instances (columns, error);
}

gboolean
flatpak_complete_ps (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1:
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      break;

    default:
      break;
    }

  return TRUE;
}
