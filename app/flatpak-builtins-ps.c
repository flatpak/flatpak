/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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

#include "libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-table-printer.h"
#include "flatpak-instance.h"

static const char **opt_cols;
static gboolean opt_json;

static GOptionEntry options[] = {
  { "columns", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_cols, N_("What information to show"), N_("FIELD,…") },
  { "json", 'j', 0, G_OPTION_ARG_NONE, &opt_json, N_("Show output in JSON format"), NULL },
  { NULL }
};

static Column all_columns[] = {
  { "instance",       N_("Instance"),    N_("Show the instance ID"),                0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "pid",            N_("PID"),         N_("Show the PID of the wrapper process"), 0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "child-pid",      N_("Child-PID"),   N_("Show the PID of the sandbox process"), 0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "application",    N_("Application"), N_("Show the application ID"),             0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "arch",           N_("Arch"),        N_("Show the architecture"),               0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "branch",         N_("Branch"),      N_("Show the application branch"),         0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "commit",         N_("Commit"),      N_("Show the application commit"),         0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "runtime",        N_("Runtime"),     N_("Show the runtime ID"),                 0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "runtime-branch", N_("R.-Branch"),   N_("Show the runtime branch"),             0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "runtime-commit", N_("R.-Commit"),   N_("Show the runtime commit"),             0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "active",         N_("Active"),      N_("Show whether the app is active"),      0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "background",     N_("Background"),  N_("Show whether the app is background"),  0, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { NULL }
};

enum {
  BACKGROUND,
  RUNNING,
  ACTIVE
};

static GVariant *
get_compositor_apps (void)
{
  g_autoptr(GDBusConnection) bus = NULL;
  g_autoptr(GVariant) ret = NULL;
  GVariant *list = NULL;
  const char *backends[] = {
    "org.freedesktop.impl.portal.desktop.gnome",
    /* Background portal was removed in 1.15.0, retained for compatibility */
    "org.freedesktop.impl.portal.desktop.gtk",
    "org.freedesktop.impl.portal.desktop.kde",
    NULL
  };
  int i;

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (!bus)
    return NULL;

  for (i = 0; backends[i]; i++)
    {
      g_autoptr(GError) error = NULL;

      ret = g_dbus_connection_call_sync (bus,
                                         backends[i],
                                         "/org/freedesktop/portal/desktop",
                                         "org.freedesktop.impl.portal.Background",
                                         "GetAppState",
                                         g_variant_new ("()"),
                                         G_VARIANT_TYPE ("(a{sv})"),
                                         G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                         -1,
                                         NULL,
                                         &error);
      if (ret)
        break;
      if (error &&
          !g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) &&
          !g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
        return NULL;
    }

  if (ret)
    g_variant_get (ret, "(@a{sv})", &list);
  else
    g_info ("Failed to get information about running apps from background portal backends");

  return list;
}

static gboolean
enumerate_instances (Column *columns, GError **error)
{
  g_autoptr(GPtrArray) instances = NULL;
  g_autoptr(FlatpakTablePrinter) printer = NULL;
  int i, j;
  g_autoptr(GVariant) compositor_apps = NULL;

  if (columns[0].name == NULL)
    return TRUE;

  printer = flatpak_table_printer_new ();
  flatpak_table_printer_set_columns (printer, columns, opt_cols == NULL);

  instances = flatpak_instance_get_all ();
  if (instances->len == 0)
    {
      /* nothing to show */
      return TRUE;
    }

  for (i = 0; columns[i].name; i++)
    {
      if (strcmp (columns[i].name, "active") == 0 ||
          strcmp (columns[i].name, "background") == 0)
        {
          compositor_apps = get_compositor_apps ();
          break;
        }
    }

  for (j = 0; j < instances->len; j++)
    {
      FlatpakInstance *instance = (FlatpakInstance *) g_ptr_array_index (instances, j);

      for (i = 0; columns[i].name; i++)
        {
          if (strcmp (columns[i].name, "instance") == 0)
            flatpak_table_printer_add_column (printer, flatpak_instance_get_id (instance));
          else if (strcmp (columns[i].name, "pid") == 0)
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
              const char *ref_str = flatpak_instance_get_runtime (instance);
              if (ref_str != NULL)
                {
                  g_autoptr(FlatpakDecomposed) ref = flatpak_decomposed_new_from_ref (ref_str, NULL);
                  if (ref)
                    {
                      g_autofree char *id = flatpak_decomposed_dup_id (ref);
                      flatpak_table_printer_add_column (printer, id);
                    }
                }
            }
          else if (strcmp (columns[i].name, "runtime-branch") == 0)
            {
              const char *ref_str = flatpak_instance_get_runtime (instance);
              if (ref_str != NULL)
                {
                  g_autoptr(FlatpakDecomposed) ref = flatpak_decomposed_new_from_ref (ref_str, NULL);
                  if (ref)
                    flatpak_table_printer_add_column (printer, flatpak_decomposed_get_branch (ref));
                }
            }
          else if (strcmp (columns[i].name, "runtime-commit") == 0)
            flatpak_table_printer_add_column_len (printer,
                                                  flatpak_instance_get_runtime_commit (instance),
                                                  12);
          else if (strcmp (columns[i].name, "active") == 0 ||
                   strcmp (columns[i].name, "background") == 0)
           {
             const char *app = flatpak_instance_get_app (instance);
             if (compositor_apps && app)
               {
                 guint state;

                 if (!g_variant_lookup (compositor_apps, app, "u", &state))
                   state = BACKGROUND;

                 if ((strcmp (columns[i].name, "background") == 0 && state == BACKGROUND) ||
                     (strcmp (columns[i].name, "active") == 0 && state == ACTIVE))
                   flatpak_table_printer_add_column (printer, "🗸");
                 else
                   flatpak_table_printer_add_column (printer, "");
               }
             else
               flatpak_table_printer_add_column (printer, "?");
           }
        }

      flatpak_table_printer_finish_row (printer);
    }

  opt_json ? flatpak_table_printer_print_json (printer) : flatpak_table_printer_print (printer);

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
