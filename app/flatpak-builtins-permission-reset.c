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
#include "flatpak-permission-utils-private.h"
#include "flatpak-utils-private.h"
#include "flatpak-run-private.h"

static gboolean opt_all;

static GOptionEntry options[] = {
  { "all", 0, 0, G_OPTION_ARG_NONE, &opt_all, N_("Reset all permissions"), NULL },
  { NULL }
};

gboolean
flatpak_builtin_permission_reset (int argc, char **argv,
                                  GCancellable *cancellable,
                                  GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  const char *app_id;

  context = g_option_context_new (_("APP_ID - Reset permissions for an app"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR,
                                     NULL, cancellable, error))
    return FALSE;

  if ((opt_all && argc != 1) || (!opt_all && argc != 2))
    return usage_error (context, _("Wrong number of arguments"), error);

  if (opt_all)
    app_id = NULL;
  else
    app_id = argv[1];

  return flatpak_reset_permissions_for_app (app_id, cancellable, error);
}

gboolean
flatpak_complete_permission_reset (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  XdpDbusPermissionStore *store = NULL;
  g_autoptr(FlatpakDir) user_dir = NULL;
  g_autoptr(FlatpakDir) system_dir = NULL;

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


      user_dir = flatpak_dir_get_user ();
      system_dir = flatpak_dir_get_system_default ();
      flatpak_complete_partial_ref (completion, FLATPAK_KINDS_APP, FALSE, user_dir, NULL);
      flatpak_complete_partial_ref (completion, FLATPAK_KINDS_APP, FALSE, system_dir, NULL);

      break;

    default:
      break;
    }

  return TRUE;
}
