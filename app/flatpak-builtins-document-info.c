/*
 * Copyright © 2016 Red Hat, Inc
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
#include "flatpak-document-dbus-generated.h"

#include <gio/gunixfdlist.h>

#include "flatpak-builtins.h"
#include "flatpak-utils-private.h"
#include "flatpak-run-private.h"

static GOptionEntry options[] = {
  { NULL }
};

gboolean
flatpak_builtin_document_info (int argc, char **argv,
                               GCancellable *cancellable,
                               GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  const char *file;
  XdpDbusDocuments *documents;
  g_autofree char *mountpoint = NULL;
  g_autofree char *basename = NULL;
  g_autofree char *doc_id = NULL;
  g_autofree char *doc_path = NULL;
  g_autofree char *origin = NULL;
  const char *app_id;
  const char **perms;
  g_autoptr(GVariant) apps = NULL;
  g_autoptr(GVariantIter) iter = NULL;

  context = g_option_context_new (_("FILE - Get information about an exported file"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR,
                                     NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("FILE must be specified"), error);

  if (argc > 2)
    return usage_error (context, _("Too many arguments"), error);

  file = argv[1];
  basename = g_path_get_basename (file);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (session_bus == NULL)
    return FALSE;

  documents = xdp_dbus_documents_proxy_new_sync (session_bus, 0,
                                                 "org.freedesktop.portal.Documents",
                                                 "/org/freedesktop/portal/documents",
                                                 NULL, error);
  if (documents == NULL)
    return FALSE;

  if (!xdp_dbus_documents_call_get_mount_point_sync (documents, &mountpoint,
                                                     NULL, error))
    return FALSE;

  if (!xdp_dbus_documents_call_lookup_sync (documents, file, &doc_id, NULL, error))
    return FALSE;

  if (strcmp (doc_id, "") == 0)
    {
      g_print (_("Not exported\n"));
      return TRUE;
    }

  doc_path = g_build_filename (mountpoint, doc_id, basename, NULL);

  if (!xdp_dbus_documents_call_info_sync (documents, doc_id, &origin, &apps,
                                          NULL, error))
    return FALSE;

  iter = g_variant_iter_new (apps);

  g_print ("id: %s\n", doc_id);
  g_print ("path: %s\n", doc_path);
  g_print ("origin: %s\n", origin);
  if (g_variant_iter_n_children (iter) > 0)
    g_print ("permissions:\n");
  while (g_variant_iter_next (iter, "{&s^a&s}", &app_id, &perms))
    {
      int i;
      g_print ("\t%s\t", app_id);
      for (i = 0; perms[i]; i++)
        {
          if (i > 0)
            g_print (", ");
          g_print ("%s", perms[i]);
        }
      g_print ("\n");
    }

  return TRUE;
}

gboolean
flatpak_complete_document_info (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new ("");

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* FILE */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      flatpak_complete_file (completion, "__FLATPAK_FILE");
      break;
    }

  return TRUE;
}
