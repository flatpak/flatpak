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

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"
#include "flatpak-permission-dbus-generated.h"

#include "flatpak-builtins.h"
#include "flatpak-table-printer.h"
#include "flatpak-utils-private.h"
#include "flatpak-run-private.h"

static GOptionEntry options[] = {
  { NULL }
};

static char **
get_permission_tables (XdpDbusPermissionStore *store)
{
  g_autofree char *path = NULL;
  GDir *dir;
  const char *name;
  GPtrArray *tables = NULL;

  tables = g_ptr_array_new ();

  path = g_build_filename (g_get_user_data_dir (), "flatpak/db", NULL);
  dir = g_dir_open (path, 0, NULL);
  if (dir != NULL)
    {
      while ((name = g_dir_read_name (dir)) != NULL)
        {
          g_ptr_array_add (tables, g_strdup (name));
        }
    }

  g_dir_close (dir);

  g_ptr_array_add (tables, NULL);

  return (char **)g_ptr_array_free (tables, FALSE);
}

static gboolean
remove_for_app (XdpDbusPermissionStore  *store,
                const char              *table,
                const char              *app_id,
                GError                 **error)
{
  char **ids;
  int i;

  if (!xdp_dbus_permission_store_call_list_sync (store, table, &ids, NULL, error))
    return FALSE;

  for (i = 0; ids[i]; i++)
    {
      g_autoptr(GVariant) permissions = NULL;
      g_autoptr(GVariant) data = NULL;
      g_autofree char *perms = NULL;
      GVariantIter iter;
      char *key;
      GVariant *value;
      GVariantBuilder builder;

      g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sas}"));

      if (!xdp_dbus_permission_store_call_lookup_sync (store, table, ids[i],
                                                       &permissions, &data,
                                                       NULL, error))
        return FALSE;

      g_variant_iter_init (&iter, permissions);
      while (g_variant_iter_loop (&iter, "{s@as}", &key, &value))
        {
          if (strcmp (key, app_id) == 0)
            continue;

          g_variant_builder_add (&builder, "{s@as}", key, value);
        }

      if (!xdp_dbus_permission_store_call_set_sync (store, table, TRUE, ids[i],
                                                    g_variant_builder_end (&builder),
                                                    data ? data : g_variant_new_byte (0),
                                                    NULL, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_builtin_permission_reset (int argc, char **argv,
                                  GCancellable *cancellable,
                                  GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  XdpDbusPermissionStore *store = NULL;
  const char *app_id;
  int i;
  g_auto(GStrv) tables = NULL;

  context = g_option_context_new (_("APP_ID - Reset permissions for an app"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR,
                                     NULL, cancellable, error))
    return FALSE;

  if (argc != 2)
    return usage_error (context, _("Wrong number of arguments"), error);

  app_id = argv[1];

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (session_bus == NULL)
    return FALSE;

  store = xdp_dbus_permission_store_proxy_new_sync (session_bus, 0,
                                                    "org.freedesktop.impl.portal.PermissionStore",
                                                    "/org/freedesktop/impl/portal/PermissionStore",
                                                    NULL, error);
  if (store == NULL)
    return FALSE;

  tables = get_permission_tables (store);
  for (i = 0; tables[i]; i++)
    {
      if (!remove_for_app (store, tables[i], app_id, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_complete_permission_reset (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  XdpDbusPermissionStore *store = NULL;

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
    case 1: /* APP_ID */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      flatpak_complete_partial_ref (completion, FLATPAK_KINDS_APP, FALSE, flatpak_dir_get_user (), NULL);
      flatpak_complete_partial_ref (completion, FLATPAK_KINDS_APP, FALSE, flatpak_dir_get_system_default (), NULL);

      break;

    default:
      break;
    }

  return TRUE;
}
