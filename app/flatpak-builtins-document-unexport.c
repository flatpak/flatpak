/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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

static gboolean opt_doc_id;

static GOptionEntry options[] = {
  { "doc-id", 0, 0, G_OPTION_ARG_NONE, &opt_doc_id, N_("Specify the document ID"), NULL },
  { NULL }
};

gboolean
flatpak_builtin_document_unexport (int argc, char **argv,
                                   GCancellable *cancellable,
                                   GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  XdpDbusDocuments *documents;
  const char *file;
  g_autofree char *doc_id = NULL;

  context = g_option_context_new (_("FILE - Unexport a file to apps"));
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

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (session_bus == NULL)
    return FALSE;

  documents = xdp_dbus_documents_proxy_new_sync (session_bus, 0,
                                                 "org.freedesktop.portal.Documents",
                                                 "/org/freedesktop/portal/documents",
                                                 NULL, error);
  if (documents == NULL)
    return FALSE;

  if (opt_doc_id)
    doc_id = g_strdup (file);
  else if (!xdp_dbus_documents_call_lookup_sync (documents, file, &doc_id, NULL, error))
    return FALSE;

  if (strcmp (doc_id, "") == 0)
    {
      g_print (_("Not exported\n"));
      return TRUE;
    }

  if (!xdp_dbus_documents_call_delete_sync (documents, doc_id, NULL, error))
    return FALSE;

  return TRUE;
}

static gboolean
_complete_document_ids (FlatpakCompletion  *completion,
                        GError            **error)
{
  g_autoptr(GDBusConnection) session_bus = NULL;
  XdpDbusDocuments *documents;
  g_autoptr(GVariant) apps = NULL;
  g_autoptr(GVariantIter) iter = NULL;
  const char *id;
  const char *origin;

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (session_bus == NULL)
    return FALSE;

  documents = xdp_dbus_documents_proxy_new_sync (session_bus, 0,
                                                 "org.freedesktop.portal.Documents",
                                                 "/org/freedesktop/portal/documents",
                                                 NULL, error);
  if (documents == NULL)
    return FALSE;

  if (!xdp_dbus_documents_call_list_sync (documents, "", &apps, NULL, error))
    return FALSE;

  iter = g_variant_iter_new (apps);
  while (g_variant_iter_next (iter, "{&s^&ay}", &id, &origin))
    flatpak_complete_word (completion, "%s ", id);

  return TRUE;
}

gboolean
flatpak_complete_document_unexport (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GError) error = NULL;

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

      if (!opt_doc_id)
        flatpak_complete_file (completion, "__FLATPAK_FILE");
      else if (!_complete_document_ids (completion, &error))
        flatpak_completion_debug ("complete document ids error: %s", error->message);

      break;
    }

  return TRUE;
}
