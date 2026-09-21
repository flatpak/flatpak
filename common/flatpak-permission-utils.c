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

#include "flatpak-permission-utils-private.h"
#include "flatpak-utils-private.h"

char **
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
      g_dir_close (dir);
    }

  g_ptr_array_add (tables, NULL);

  return (char **) g_ptr_array_free (tables, FALSE);
}

static gboolean
remove_for_app (XdpDbusPermissionStore *store,
                const char             *table,
                const char             *app_id,
                GError                **error)
{
  char **ids;
  int i;

  /* FIXME some portals cache their permission tables and assume that they're
   * the only writers, so they may miss these changes.
   * See https://github.com/flatpak/xdg-desktop-portal/issues/197
   */

  if (!xdp_dbus_permission_store_call_list_sync (store, table, &ids, NULL, error))
    return FALSE;

  for (i = 0; ids[i]; i++)
    {
      g_autoptr(GVariant) permissions = NULL;
      g_autoptr(GVariant) data = NULL;
      GVariantIter iter;
      char *key;
      GVariant *value;
      g_auto(GVariantBuilder) builder = FLATPAK_VARIANT_BUILDER_INITIALIZER;
      gboolean need_to_set = FALSE;

      g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sas}"));

      if (!xdp_dbus_permission_store_call_lookup_sync (store, table, ids[i],
                                                       &permissions, &data,
                                                       NULL, error))
        return FALSE;

      g_variant_iter_init (&iter, permissions);
      while (g_variant_iter_loop (&iter, "{s@as}", &key, &value))
        {
          if (app_id == NULL || strcmp (key, app_id) == 0)
            {
              need_to_set = TRUE;
              continue;
            }

          g_variant_builder_add (&builder, "{s@as}", key, value);
        }

      if (need_to_set)
        {
          if (!xdp_dbus_permission_store_call_set_sync (store, table, TRUE, ids[i],
                                                        g_variant_builder_end (&builder),
                                                        data ? data : g_variant_new_byte (0),
                                                        NULL, error))
            return FALSE;
        }
    }

  return TRUE;
}

gboolean
flatpak_reset_permissions_for_app (const char *app_id,
                                   GError    **error)
{
  g_autoptr(GDBusConnection) session_bus = NULL;
  XdpDbusPermissionStore *store = NULL;
  int i;
  g_auto(GStrv) tables = NULL;

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
