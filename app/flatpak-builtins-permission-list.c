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

#include <glib/gi18n.h>

#include "libglnx.h"
#include "flatpak-permission-dbus-generated.h"

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-table-printer.h"
#include "flatpak-utils-private.h"
#include "flatpak-run-private.h"

static gboolean opt_json;

static GOptionEntry options[] = {
  { "json", 'j', 0, G_OPTION_ARG_NONE, &opt_json, N_("Show output in JSON format"), NULL },
  { NULL }
};

static char **
get_ids_for_table (XdpDbusPermissionStore *store,
                   const char             *table)
{
  char **ids = NULL;

  xdp_dbus_permission_store_call_list_sync (store, table, &ids, NULL, NULL);

  return ids;
}

static gboolean
list_table (XdpDbusPermissionStore *store,
            const char             *table,
            const char             *id,
            FlatpakTablePrinter    *printer,
            GError                **error)
{
  const char *one_id[2];
  g_auto(GStrv) store_ids = NULL;
  char **ids;
  int i;

  if (id)
    {
      one_id[0] = id;
      one_id[1] = NULL;
      ids = (char **) one_id;
    }
  else
    {
      if (!xdp_dbus_permission_store_call_list_sync (store, table, &store_ids, NULL, error))
        return FALSE;
      ids = store_ids;
    }

  for (i = 0; ids[i]; i++)
    {
      g_autoptr(GVariant) permissions = NULL;
      g_autoptr(GVariant) data = NULL;
      g_autoptr(GVariant) d = NULL;
      g_autofree char *txt = NULL;
      GVariantIter iter;
      char *key;
      GVariantIter *val;

      if (!xdp_dbus_permission_store_call_lookup_sync (store, table, ids[i], &permissions, &data, NULL, error))
        return FALSE;

      d = g_variant_get_child_value (data, 0);
      txt = g_variant_print (d, FALSE);

      if (g_variant_iter_init (&iter, permissions) == 0)
        {
          flatpak_table_printer_add_column (printer, table);
          flatpak_table_printer_add_column (printer, ids[i]);
          flatpak_table_printer_add_column (printer, "");
          flatpak_table_printer_add_column (printer, "");
          flatpak_table_printer_add_column (printer, txt);
          flatpak_table_printer_finish_row (printer);
        }

      while (g_variant_iter_loop (&iter, "{sas}", &key, &val))
        {
          char *p;

          flatpak_table_printer_add_column (printer, table);
          flatpak_table_printer_add_column (printer, ids[i]);
          flatpak_table_printer_add_column (printer, key);
          flatpak_table_printer_add_column (printer, "");

          while (g_variant_iter_loop (val, "s", &p))
            {
              flatpak_table_printer_append_with_comma (printer, p);
            }

          flatpak_table_printer_add_column (printer, txt);
          flatpak_table_printer_finish_row (printer);
        }
    }

  return TRUE;
}

gboolean
flatpak_builtin_permission_list (int argc, char **argv,
                                 GCancellable *cancellable,
                                 GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  XdpDbusPermissionStore *store = NULL;
  const char *table;
  const char *id;
  g_autoptr(FlatpakTablePrinter) printer = NULL;

  context = g_option_context_new (_("[TABLE] [ID] - List permissions"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR,
                                     NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    table = NULL;
  else
    table = argv[1];

  if (argc < 3)
    id = NULL;
  else
    id = argv[2];

  if (argc > 3)
    return usage_error (context, _("Too many arguments"), error);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (session_bus == NULL)
    return FALSE;

  store = xdp_dbus_permission_store_proxy_new_sync (session_bus, 0,
                                                    "org.freedesktop.impl.portal.PermissionStore",
                                                    "/org/freedesktop/impl/portal/PermissionStore",
                                                    NULL, error);
  if (store == NULL)
    return FALSE;

  printer = flatpak_table_printer_new ();
  int i;

  i = 0;
  flatpak_table_printer_set_column_title (printer, i++, _("Table"));
  flatpak_table_printer_set_column_title (printer, i++, _("Object"));
  flatpak_table_printer_set_column_title (printer, i++, _("App"));
  flatpak_table_printer_set_column_title (printer, i++, _("Permissions"));
  flatpak_table_printer_set_column_title (printer, i++, _("Data"));

  if (table)
    {
      if (!list_table (store, table, id, printer, error))
        return FALSE;
    }
  else
    {
      g_auto(GStrv) tables = get_permission_tables (store);

      for (i = 0; tables[i]; i++)
        {
          if (!list_table (store, tables[i], NULL, printer, error))
            return FALSE;
        }
    }

  opt_json ? flatpak_table_printer_print_json (printer) : flatpak_table_printer_print (printer);

  return TRUE;
}

gboolean
flatpak_complete_permission_list (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  XdpDbusPermissionStore *store = NULL;
  int i;

  context = g_option_context_new ("");

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (session_bus == NULL)
    return FALSE;

  store = xdp_dbus_permission_store_proxy_new_sync (session_bus, 0,
                                                    "org.freedesktop.impl.portal.PermissionStore",
                                                    "/org/freedesktop/impl/portal/PermissionStore",
                                                    NULL, NULL);

  if (store == NULL)
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* TABLE */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      {
        g_auto(GStrv) tables = get_permission_tables (store);
        for (i = 0; tables != NULL && tables[i] != NULL; i++)
          {
            flatpak_complete_word (completion, "%s ", tables[i]);
          }
      }

      break;

    case 2:
      {
        g_auto(GStrv) ids = get_ids_for_table (store, completion->argv[1]);
        for (i = 0; ids != NULL && ids[i] != NULL; i++)
          {
            flatpak_complete_word (completion, "%s ", ids[i]);
          }
      }

      break;
    }

  return TRUE;
}
