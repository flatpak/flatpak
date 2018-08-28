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
#include "flatpak-run-private.h"

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

static char *
get_instance_pid (const char *instance)
{
  g_autofree char *path = NULL;
  char *pid;
  g_autoptr(GError) error = NULL;

  path = g_build_filename (g_get_user_runtime_dir (), ".flatpak", instance, "pid", NULL);

  if (!g_file_get_contents (path, &pid, NULL, &error))
    {
      g_debug ("Failed to load pid file for instance '%s': %s", instance, error->message);
      return NULL;
    }

  return pid; 
}

static int
get_child_pid (const char *instance)
{
  g_autofree char *path = NULL;
  g_autofree char *contents = NULL;
  gsize length;
  g_autoptr(GError) error = NULL;
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(JsonNode) node = NULL;
  g_autoptr(JsonObject) obj = NULL;

  path = g_build_filename (g_get_user_runtime_dir (), ".flatpak", instance, "bwrapinfo.json", NULL);

  if (!g_file_get_contents (path, &contents, &length, &error))
    {
      g_debug ("Failed to load bwrapinfo.json file for instance '%s': %s", instance, error->message);
      return 0;
    }

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, contents, length, &error))
    {
      g_debug ("Failed to parse bwrapinfo.json file for instance '%s': %s", instance, error->message);
      return 0;
    }

  node = json_parser_get_root (parser);
  obj = json_node_get_object (node);

  return json_object_get_int_member (obj, "child-pid");
}

static GKeyFile *
get_instance_info (const char *instance)
{
  g_autofree char *path = NULL;
  GKeyFile *key_file = NULL;
  g_autoptr(GError) error = NULL;

  path = g_build_filename (g_get_user_runtime_dir (), ".flatpak", instance, "info", NULL);

  key_file = g_key_file_new ();
  if (!g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, &error))
    {
      g_debug ("Failed to load info file for instance '%s': %s", instance, error->message);
      return NULL;
    }

  return key_file;
}

static char *
get_instance_column (GKeyFile *info,
                     const char *name)
{
  if (info == NULL)
    return NULL;

  if (strcmp (name, "application") == 0)
    return g_key_file_get_string (info, "Application", "name", NULL);
  if (strcmp (name, "arch") == 0)
    return g_key_file_get_string (info, "Instance", "arch", NULL);
  if (strcmp (name, "branch") == 0)
    return g_key_file_get_string (info, "Instance", "branch", NULL);
  else if (strcmp (name, "commit") == 0)
    return g_key_file_get_string (info, "Instance", "app-commit", NULL);
  else if (strcmp (name, "runtime") == 0)
    {
      g_autofree char *full_ref = g_key_file_get_string (info, "Application", "runtime", NULL);
      g_auto(GStrv) ref = flatpak_decompose_ref (full_ref, NULL);
      return g_strdup (ref[1]);
    }
  else if (strcmp (name, "runtime-branch") == 0)
    {
      g_autofree char *full_ref = g_key_file_get_string (info, "Application", "runtime", NULL);
      g_auto(GStrv) ref = flatpak_decompose_ref (full_ref, NULL);
      return g_strdup (ref[3]);
    }
  else if (strcmp (name, "runtime-commit") == 0)
    return g_key_file_get_string (info, "Instance", "runtime-commit", NULL);

  return NULL;
}

static gboolean
enumerate_instances (const char *columns,
                     GError **error)
{
  g_autofree char *base_dir = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GFileEnumerator) enumerator = NULL;
  GFileInfo *dir_info;
  FlatpakTablePrinter *printer;
  g_auto(GStrv) cols = NULL;
  g_autofree int *col_idx = NULL;
  int n_cols;
  int i;

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
 
  base_dir = g_build_filename (g_get_user_runtime_dir (), ".flatpak", NULL);
  file = g_file_new_for_path (base_dir);
  enumerator = g_file_enumerate_children (file, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, error);
  if (enumerator == NULL)
    {
      if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          /* nothing to show */
          g_clear_error (error);
          return TRUE;
        }
      return FALSE;
    }

  while ((dir_info = g_file_enumerator_next_file (enumerator, NULL, error)) != NULL)
    {
      g_autofree char *instance = g_file_info_get_attribute_as_string (dir_info, "standard::name");
      g_autoptr(GKeyFile) info = get_instance_info (instance);

      flatpak_table_printer_add_column (printer, instance);

      for (i = 0; i < n_cols; i++)
        {
          g_autofree char *col = NULL;
          int len;

          if (strcmp (all_columns[col_idx[i]].name, "pid") == 0)
            col = get_instance_pid (instance);
          else if (strcmp (all_columns[col_idx[i]].name, "child-pid") == 0)
            col = g_strdup_printf ("%d", get_child_pid (instance));
          else 
            col = get_instance_column (info, all_columns[col_idx[i]].name);

          len = all_columns[col_idx[i]].len;
          if (len == 0)
            flatpak_table_printer_add_column (printer, col);
          else
            flatpak_table_printer_add_column_len (printer, col, len);
        }

      flatpak_table_printer_finish_row (printer);

      g_object_unref (dir_info);
    }

  flatpak_table_printer_print (printer);
  flatpak_table_printer_free (printer);

  if (*error)
    return FALSE;

  return TRUE;
}

gboolean
flatpak_builtin_ps (int           argc,
                    char        **argv,
                    GCancellable *cancellable,
                    GError      **error)
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

  flatpak_run_gc_ids ();

  if (opt_show_cols)
    {
      show_columns ();
      return TRUE;
    }

  if (opt_cols)
    {
      gboolean show_help = FALSE;
      gboolean show_all = FALSE;
      int i;

      for (i = 0; opt_cols[i]; i++)
        {
          const char *p;
          p = strstr (opt_cols[i], "help");
          if (p &&
              (p == opt_cols[i] || p[-1] == ',') &&
              (p[strlen("help")] == '\0' || p[strlen("help")] == ','))
            {
              show_help = TRUE;
              break;
            }

          p = strstr (opt_cols[i], "all");
          if (p &&
              (p == opt_cols[i] || p[-1] == ',') &&
              (p[strlen("all")] == '\0' || p[strlen("all")] == ','))
            {
              show_all = TRUE;
              break;
            }
        }

      if (show_help)
        {
          show_columns ();
          return TRUE;
        }
      else if (show_all)
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
