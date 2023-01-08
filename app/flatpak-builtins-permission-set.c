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
#include "flatpak-table-printer.h"
#include "flatpak-utils-private.h"
#include "flatpak-run-private.h"

static char *opt_data;

static GOptionEntry options[] = {
  { "data", 0, 0, G_OPTION_ARG_STRING, &opt_data, N_("Associate DATA with the entry"), N_("DATA") },
  { NULL }
};

static const char *tables[] = { "documents", "notifications", "desktop-used-apps", "devices",
                                "location", "inhibit", "background", NULL };
static const char *notification_ids[] = { "notification", NULL };
static const char *device_ids[] = { "speakers", "microphone", "camera", NULL };
static const char *location_ids[] = { "location", NULL };
static const char *inhibit_ids[] = { "inhibit", NULL };
static const char *background_ids[] = { "background", NULL };

static const char *document_perms[] = { "read", "write", "delete", "grant-permissions", NULL };
static const char *notification_perms[] = { "yes", "no", NULL };
static const char *device_perms[] = { "yes", "no", "ask", NULL };
static const char *inhibit_perms[] = { "logout", "switch", "suspend", "idle", NULL };

static const char **
get_known_permission_tables (void)
{
  return tables;
}

static const char **
get_known_ids_for_table (const char *table)
{
  if (strcmp (table, "notifications") == 0)
    return notification_ids;
  else if (strcmp (table, "devices") == 0)
    return device_ids;
  else if (strcmp (table, "location") == 0)
    return location_ids;
  else if (strcmp (table, "inhibit") == 0)
    return inhibit_ids;
  else if (strcmp (table, "background") == 0)
    return background_ids;

  return NULL;
}

static const char **
get_permission_values_for_table (const char *table)
{
  if (strcmp (table, "devices") == 0)
    return device_perms;
  else if (strcmp (table, "documents") == 0)
    return document_perms;
  else if (strcmp (table, "notifications") == 0)
    return notification_perms;
  else if (strcmp (table, "inhibit") == 0)
    return inhibit_perms;

  return NULL;
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
  g_autoptr(GVariant) data = NULL;

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

  if (opt_data)
    {
      data = g_variant_parse (NULL, opt_data, NULL, NULL, error);
      if (!data)
        {
          g_prefix_error (error, _("Failed to parse '%s' as GVariant: "), opt_data);
          return FALSE;
        }
    }

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (session_bus == NULL)
    return FALSE;

  store = xdp_dbus_permission_store_proxy_new_sync (session_bus, 0,
                                                    "org.freedesktop.impl.portal.PermissionStore",
                                                    "/org/freedesktop/impl/portal/PermissionStore",
                                                    NULL, error);
  if (store == NULL)
    return FALSE;

  if (!xdp_dbus_permission_store_call_set_permission_sync (store, table, TRUE,
                                                           id, app_id, perms, 
                                                           NULL, error))
    return FALSE;

  if (data)
    {
      if (!xdp_dbus_permission_store_call_set_value_sync (store, table, FALSE,
                                                          id, g_variant_new_variant (data), NULL, error))
        return FALSE;
    }

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
        const char **known_tables = get_known_permission_tables ();
        for (i = 0; known_tables != NULL && known_tables[i] != NULL; i++)
          {
            flatpak_complete_word (completion, "%s ", known_tables[i]);
          }
      }

      break;

    case 2:
      {
        const char **ids = get_known_ids_for_table (completion->argv[1]);
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
