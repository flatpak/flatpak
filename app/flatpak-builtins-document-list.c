/*
 * Copyright © 2016 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#include "libgsystem.h"
#include "libglnx/libglnx.h"
#include "document-portal/xdp-dbus.h"

#include "flatpak-builtins.h"
#include "flatpak-utils.h"
#include "flatpak-run.h"

static GOptionEntry options[] = {
  { NULL }
};

gboolean
flatpak_builtin_document_list (int argc, char **argv,
                               GCancellable *cancellable,
                               GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GVariantIter) iter = NULL;
  const char *app_id;
  const char *id;
  const char *path;

  context = g_option_context_new ("[APPID] - List exported files");

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR,
                                     NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    app_id = "";
  else
    app_id = argv[1];

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (session_bus == NULL)
    return FALSE;


  reply = g_dbus_connection_call_sync (session_bus,
                                       "org.freedesktop.impl.portal.Documents",
                                       "/org/freedesktop/impl/portal/documents",
                                       "org.freedesktop.impl.portal.Documents",
                                       "List",
                                       g_variant_new ("(s)", app_id),
                                       G_VARIANT_TYPE ("(a{say})"),
                                       G_DBUS_CALL_FLAGS_NONE,
                                       30000,
                                       NULL,
                                       error);

  if (reply == NULL)
    return FALSE;

  g_variant_get (reply, "(a{say})", &iter);

  while (g_variant_iter_next (iter, "{&s^&ay}", &id, &path))
    g_print ("%s\t%s\n", id, path);

  return TRUE;
}

gboolean
flatpak_complete_document_list (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) user_dir = NULL;
  g_autoptr(FlatpakDir) system_dir = NULL;
  g_autoptr(GError) error = NULL;
  int i;

  context = g_option_context_new ("");

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* APPID */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      user_dir = flatpak_dir_get_user ();
      {
        g_auto(GStrv) refs = flatpak_dir_find_installed_refs (user_dir, NULL, NULL, NULL,
                                                              TRUE, FALSE, &error);
        if (refs == NULL)
          flatpak_completion_debug ("find local refs error: %s", error->message);
        for (i = 0; refs != NULL && refs[i] != NULL; i++)
          {
            g_auto(GStrv) parts = flatpak_decompose_ref (refs[i], NULL);
            if (parts)
              flatpak_complete_word (completion, "%s ", parts[1]);
          }
      }

      system_dir = flatpak_dir_get_user ();
      {
        g_auto(GStrv) refs = flatpak_dir_find_installed_refs (system_dir, NULL, NULL, NULL,
                                                              TRUE, FALSE, &error);
        if (refs == NULL)
          flatpak_completion_debug ("find local refs error: %s", error->message);
        for (i = 0; refs != NULL && refs[i] != NULL; i++)
          {
            g_auto(GStrv) parts = flatpak_decompose_ref (refs[i], NULL);
            if (parts)
              flatpak_complete_word (completion, "%s ", parts[1]);
          }
      }

      break;
    }

  return TRUE;
}
