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

static char **
get_ids_for_table (XdpDbusPermissionStore *store,
                   const char             *table)
{
  char **ids = NULL;

  xdp_dbus_permission_store_call_list_sync (store, table, &ids, NULL, NULL);

  return ids;
}

static const char *device_perms[] = { "yes", "no", "ask", NULL };
static const char *document_perms[] = { "read", "write", "delete", "grant-permissions", NULL };
static const char *notification_perms[] = { "yes", "no", NULL };

static const char **
get_permission_values_for_table (const char *table)
{
  if (strcmp (table, "devices") == 0)
    return device_perms;
  else if (strcmp (table, "documents") == 0)
    return document_perms;
  else if (strcmp (table, "notifications") == 0)
    return notification_perms;

  return NULL;
}

static gboolean
set_permissions (XdpDbusPermissionStore  *store,
                 const char              *table,
                 const char              *id,
                 const char              *app_id,
                 const char             **permissions,
                 GError                 **error)
{
  GVariant *perms = NULL;
  GVariant *data = NULL;
  GVariantBuilder builder;
  int i;
  GVariant *new_perms = g_variant_new_strv (permissions, -1);

  if (!xdp_dbus_permission_store_call_lookup_sync (store, table, id,
                                                   &perms, &data,
                                                   NULL, error))
    return FALSE;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sas}"));
  for (i = 0; perms && i < g_variant_n_children (perms); i++)
    {
      const char *key;
      GVariant *value = NULL;

      g_variant_get_child (perms, i, "{&s@as}", &key, &value);
      if (strcmp (key, app_id) == 0)
        {
          if (permissions[0] != NULL)
            g_variant_builder_add (&builder, "{s@as}", key, new_perms);
          new_perms = NULL;
        }
      else
        g_variant_builder_add (&builder, "{s@as}", key, value);
    }

  if (new_perms != NULL)
    {
      if (permissions[0] != NULL)
        g_variant_builder_add (&builder, "{s@as}", app_id, new_perms);
    }

  if (!xdp_dbus_permission_store_call_set_sync (store, table, TRUE, id,
                                                g_variant_builder_end (&builder),
                                                data ? data : g_variant_new_byte (0),
                                                NULL, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_builtin_permission_set (int argc, char **argv,
                                GCancellable *cancellable,
                                GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  XdpDbusPermissionStore *store = NULL;
  const char *table;
  const char *id;
  const char *app_id;
  const char **perms;

  context = g_option_context_new (_("TABLE ID APP_ID [PERMISSION...] - Set permissions"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR,
                                     NULL, cancellable, error))
    return FALSE;

  if (argc < 4)
    return usage_error (context, _("Too few arguments"), error);

  table = argv[1];
  id = argv[2];
  app_id = argv[3];
  perms = (const char **)&argv[4];

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (session_bus == NULL)
    return FALSE;

  store = xdp_dbus_permission_store_proxy_new_sync (session_bus, 0,
                                                    "org.freedesktop.impl.portal.PermissionStore",
                                                    "/org/freedesktop/impl/portal/PermissionStore",
                                                    NULL, error);
  if (store == NULL)
    return FALSE;

  if (!set_permissions (store, table, id, app_id, perms, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_complete_permission_set (FlatpakCompletion *completion)
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

    case 3:
      flatpak_complete_partial_ref (completion, FLATPAK_KINDS_APP, FALSE, flatpak_dir_get_user (), NULL);
      flatpak_complete_partial_ref (completion, FLATPAK_KINDS_APP, FALSE, flatpak_dir_get_system_default (), NULL);
      break;

    default:
      {
        const char **vals = get_permission_values_for_table (completion->argv[1]);
        for (i = 0; vals != NULL && vals[i] != NULL; i++)
          {
            int j;
            for (j = 4; j < completion->argc; j++)
              {
                if (strcmp (completion->argv[j], vals[i]) == 0)
                  break;
              }
            if (j == completion->argc)
              flatpak_complete_word (completion, "%s ", vals[i]);
          }
      }

      break;
    }

  return TRUE;
}
