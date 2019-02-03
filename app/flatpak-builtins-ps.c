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
#include "flatpak-instance.h"
#include "gnome-shell-introspect-dbus-generated.h"


static const char **opt_cols;

static GOptionEntry options[] = {
  { "columns", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_cols, N_("What information to show"), N_("FIELD,…") },
  { NULL }
};

static Column all_columns[] = {
  { "instance",       N_("Instance"),    N_("Show the instance ID"),                0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "pid",            N_("PID"),         N_("Show the PID of the wrapper process"), 0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "child-pid",      N_("Child-PID"),   N_("Show the PID of the sandbox process"), 0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "application",    N_("Application"), N_("Show the application ID"),             0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "state",          N_("State"),       N_("Show the application state"),          0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "arch",           N_("Arch"),        N_("Show the architecture"),               0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "branch",         N_("Branch"),      N_("Show the application branch"),         0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "commit",         N_("Commit"),      N_("Show the application commit"),         0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "runtime",        N_("Runtime"),     N_("Show the runtime ID"),                 0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "runtime-branch", N_("R.-Branch"),   N_("Show the runtime branch"),             0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "runtime-commit", N_("R.-Commit"),   N_("Show the runtime commit"),             0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { NULL }
};

typedef enum { UNKNOWN, BACKGROUND, RUNNING, ACTIVE } AppState;

static GHashTable *
get_app_states (void)
{
  GnomeShellIntrospect *shell = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) windows = NULL;
  g_autoptr(GHashTable) app_states = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  shell = gnome_shell_introspect_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                         G_DBUS_PROXY_FLAGS_NONE,
                                                         "org.gnome.Shell",
                                                         "/org/gnome/Shell/Introspect",
                                                         NULL, NULL);
  if (shell == NULL)
    return g_steal_pointer (&app_states);

  if (!gnome_shell_introspect_call_get_windows_sync (shell, &windows, NULL, &error))
    g_debug ("Could not get window list: %s", error->message);

  if (windows)
    {
      g_autoptr(GVariantIter) iter = g_variant_iter_new (windows);
      GVariant *dict;
      while (g_variant_iter_loop (iter, "{t@a{sv}}", NULL, &dict))
        {
          const char *app_id = NULL;
          gboolean hidden = FALSE;
          gboolean focus = FALSE;
          AppState state = BACKGROUND;

          g_variant_lookup (dict, "app-id", "&s", &app_id);
          g_variant_lookup (dict, "is-hidden", "b", &hidden);
          g_variant_lookup (dict, "has-focus", "b", &focus);

          if (app_id == NULL)
            continue;

          state = GPOINTER_TO_INT (g_hash_table_lookup (app_states, app_id));

          if (!hidden)
            state = MAX (state, RUNNING);
          if (focus)
            state = MAX (state, ACTIVE);

          g_hash_table_insert (app_states, g_strdup (app_id), GINT_TO_POINTER (state));
        }
    }

  g_object_unref (shell);

  return g_steal_pointer (&app_states);
}

static gboolean
enumerate_instances (Column *columns, GError **error)
{
  g_autoptr(GHashTable) app_states = NULL;
  g_autoptr(GPtrArray) instances = NULL;
  FlatpakTablePrinter *printer;
  int i, j;

  if (columns[0].name == NULL)
    return TRUE;

  printer = flatpak_table_printer_new ();
  flatpak_table_printer_set_columns (printer, columns);
 
  instances = flatpak_instance_get_all ();
  if (instances->len == 0)
    {
      /* nothing to show */
      return TRUE;
    }

  if (find_column (columns, "state", NULL) != -1)
    app_states = get_app_states ();

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
          else if (strcmp (columns[i].name, "branch") == 0)
            flatpak_table_printer_add_column (printer, flatpak_instance_get_branch (instance));
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
          else if (strcmp (columns[i].name, "state") == 0)
            {
              AppState state = GPOINTER_TO_INT (g_hash_table_lookup (app_states, flatpak_instance_get_app (instance)));
              const char *names[] = { N_("Unknown"), N_("Background"), N_("Running"), N_("Active") };
              flatpak_table_printer_add_column (printer, names[state]);
            }
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

  columns = handle_column_args (all_columns, FALSE, opt_cols, error);
  if (columns == NULL)
    return FALSE;

  return enumerate_instances (columns, error);
}

gboolean
flatpak_complete_ps (FlatpakCompletion *completion)
{
  flatpak_complete_options (completion, global_entries);
  flatpak_complete_options (completion, options);
  flatpak_complete_columns (completion, all_columns);

  return TRUE;
}
