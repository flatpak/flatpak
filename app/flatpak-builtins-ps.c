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
#include "flatpak-table-printer.h"
#include "flatpak-instance-private.h"

static gboolean opt_show_cols;
static const char **opt_cols;

static GOptionEntry options[] = {
  { "show-columns", 0, 0, G_OPTION_ARG_NONE, &opt_show_cols, N_("Show available columns"), NULL },
  { "columns", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_cols, N_("What information to show"), N_("FIELD,…") },
  { NULL }
};

static struct {
  const char *name;
  const char *title;
  const char *desc;
  int len;
} all_columns[] = {
  { "application",    N_("Application"),    N_("Show the application ID"),              0 },
  { "arch",           N_("Architecture"),   N_("Show the architecture"),                0 },
  { "branch",         N_("Branch"),         N_("Show the application branch"),          0 },
  { "commit",         N_("Commit"),         N_("Show the application commit"),         12 },
  { "runtime",        N_("Runtime"),        N_("Show the runtime ID"),                  0 },
  { "runtime-branch", N_("Runtime Branch"), N_("Show the runtime branch"),              0 },
  { "runtime-commit", N_("Runtime Commit"), N_("Show the runtime commit"),             12 },
  { "pid",            N_("PID"),            N_("Show the PID of the wrapper process"),  0 },
  { "child-pid",      N_("Child PID"),      N_("Show the PID of the sandbox process"),  0 },
};

#define ALL_COLUMNS "pid,child-pid,application,arch,branch,runtime,runtime-branch"
#define DEFAULT_COLUMNS "pid,application,runtime"

static int
find_column (const char *name,
             GError **error)
{
  int i;
  int candidate;

  candidate = -1;
  for (i = 0; i < G_N_ELEMENTS (all_columns); i++)
    {
      if (g_str_equal (all_columns[i].name, name))
        {
          return i;
        }
      else if (g_str_has_prefix (all_columns[i].name, name))
        {
          if (candidate == -1)
            {
              candidate = i;
            }
          else
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Ambiguous column: %s"), name);
              return -1;
            }
        }
    }

  if (candidate >= 0)
    return candidate;

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Unknown column: %s"), name);
  return -1;
}

static char *
column_help (void)
{
  GString *s = g_string_new ("");
  int len;
  int i;

  g_string_append (s, _("Available columns:\n"));
  
  len = 0;
  for (i = 0; i < G_N_ELEMENTS (all_columns); i++)
    len = MAX (len, strlen (all_columns[i].name));

  len += 4;
  for (i = 0; i < G_N_ELEMENTS (all_columns); i++)
    g_string_append_printf (s, "  %-*s %s\n", len, all_columns[i].name, all_columns[i].desc);

  g_string_append_printf (s, "  %-*s %s\n", len, "all", _("Show all columns"));
  g_string_append_printf (s, "  %-*s %s\n", len, "help", _("Show available columns"));

  return g_string_free (s, FALSE);
}

static void
show_columns (void)
{
  g_autofree char *col_help = column_help ();
  g_print ("%s", col_help);
}

static gboolean
enumerate_instances (const char *columns,
                     GError **error)
{
  g_autoptr(GPtrArray) instances = NULL;
  FlatpakTablePrinter *printer;
  g_auto(GStrv) cols = NULL;
  g_autofree int *col_idx = NULL;
  int n_cols;
  int i, j;

  cols = g_strsplit (columns, ",", 0);
  n_cols = g_strv_length (cols);
  col_idx = g_new (int, n_cols); 
  for (i = 0; i < n_cols; i++)
    {
      col_idx[i] = find_column (cols[i], error);
      if (col_idx[i] == -1)
        {
          return FALSE;
        }
    }

  printer = flatpak_table_printer_new ();
  flatpak_table_printer_set_column_title (printer, 0, _("Instance"));

  for (i = 0; i < n_cols; i++)
    flatpak_table_printer_set_column_title (printer, i + 1, all_columns[col_idx[i]].title);
 
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

      for (i = 0; i < n_cols; i++)
        {
          g_autofree char *freeme = NULL;
          const char *col = NULL;
          int idx = col_idx[i];
          int len;

          if (strcmp (all_columns[idx].name, "pid") == 0)
            col = freeme = g_strdup_printf ("%d", flatpak_instance_get_pid (instance));
          else if (strcmp (all_columns[idx].name, "child-pid") == 0)
            col = freeme = g_strdup_printf ("%d", flatpak_instance_get_child_pid (instance));
          else if (strcmp (all_columns[idx].name, "application") == 0)
            col = flatpak_instance_get_app (instance);
          else if (strcmp (all_columns[idx].name, "arch") == 0)
            col = flatpak_instance_get_arch (instance);
          else if (strcmp (all_columns[idx].name, "commit") == 0)
            col = flatpak_instance_get_commit (instance);
          else if (strcmp (all_columns[idx].name, "runtime") == 0)
            {
              const char *full_ref = flatpak_instance_get_runtime (instance);
              if (full_ref != NULL)
                {
                  g_auto(GStrv) ref = flatpak_decompose_ref (full_ref, NULL);
                  col = freeme = g_strdup (ref[1]);
                }
            }
          else if (strcmp (all_columns[idx].name, "runtime-branch") == 0)
            {
              const char *full_ref = flatpak_instance_get_runtime (instance);
              if (full_ref != NULL)
                {
                  g_auto(GStrv) ref = flatpak_decompose_ref (full_ref, NULL);
                  col = freeme = g_strdup (ref[3]);
                }
            }
          else if (strcmp (all_columns[idx].name, "runtime-commit") == 0)
            col = flatpak_instance_get_runtime_commit (instance);

          len = all_columns[idx].len;
          if (len == 0)
            flatpak_table_printer_add_column (printer, col);
          else
            flatpak_table_printer_add_column_len (printer, col, len);
        }

      flatpak_table_printer_finish_row (printer);
    }

  flatpak_table_printer_print (printer);
  flatpak_table_printer_free (printer);

  return TRUE;
}

static gboolean
list_has (const char *list,
          const char *term)
{
  const char *p;
  int len;

  p = strstr (list, term);
  len = strlen (term);
  if (p &&
      (p == list || p[-1] == ',') &&
      (p[len] == '\0' || p[len] == ','))
    return TRUE;
  return FALSE;
}

gboolean
flatpak_builtin_ps (int            argc,
                    char         **argv,
                    GCancellable  *cancellable,
                    GError       **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autofree char *col_help = NULL;
  g_autofree char *cols = NULL;

  context = g_option_context_new (_(" - Enumerate running sandboxes"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);
  col_help = column_help ();
  g_option_context_set_description (context, col_help);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc > 1)
    {
      usage_error (context, _("Extra arguments given"), error);
      return FALSE;
    }

  if (opt_show_cols)
    {
      show_columns ();
      return TRUE;
    }

  if (opt_cols)
    {
      gboolean show_all = FALSE;
      int i;

      for (i = 0; opt_cols[i]; i++)
        {
          if (list_has (opt_cols[i], "help"))
            {
              show_columns ();
              return TRUE;
            }
          if (list_has (opt_cols[i], "all"))
            {
              show_all = TRUE;
              break;
            }
        }

      if (show_all)
        {
          cols = g_strdup (ALL_COLUMNS);
        }
      else
        {
          cols = g_strjoinv (",", (char **)opt_cols);
        }
    }
  else
    cols = g_strdup (DEFAULT_COLUMNS);

  return enumerate_instances (cols, error);
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
