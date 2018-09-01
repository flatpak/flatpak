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
#include "flatpak-document-dbus-generated.h"

#include <gio/gunixfdlist.h>

#include "flatpak-builtins.h"
#include "flatpak-utils-private.h"
#include "flatpak-run-private.h"

static GOptionEntry options[] = {
  { NULL }
};

gboolean
flatpak_builtin_document_forward (int argc, char **argv,
                                  GCancellable *cancellable,
                                  GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  XdpDbusDocuments *documents;
  const char *app;
  char **files;
  int n_files;
  int fd;
  int i;
  g_autoptr(GUnixFDList) fd_list = NULL;
  GVariantBuilder fds;
  g_auto(GStrv) doc_ids = NULL;
  g_autoptr(GVariant) extra_out = NULL;
  const char *mountpoint;
  const char * const permissions[] = { "read", "write", NULL };

  context = g_option_context_new (_("APP FILE… - Make files available to an app"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR,
                                     NULL, cancellable, error))
    return FALSE;

  if (argc < 3)
    return usage_error (context, _("APP and FILE must be specified"), error);

  app = argv[1];
  files = argv + 2;
  n_files = argc - 2;

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (session_bus == NULL)
    return FALSE;

  documents = xdp_dbus_documents_proxy_new_sync (session_bus, 0,
                                                 "org.freedesktop.portal.Documents",
                                                 "/org/freedesktop/portal/documents",
                                                 NULL, error);
  if (documents == NULL)
    return FALSE;

  g_variant_builder_init (&fds, G_VARIANT_TYPE ("ah"));
  fd_list = g_unix_fd_list_new ();
  for (i = 0; i < n_files; i++)
    {
      int idx;

      fd = open (files[i], O_PATH | O_CLOEXEC);

      if (fd == -1)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }

      idx = g_unix_fd_list_append (fd_list, fd, error);
      if (idx == -1)
        return FALSE;

      g_variant_builder_add (&fds, "h", idx);
      close (fd);
    }
  
  if (!xdp_dbus_documents_call_add_full_sync (documents,
                                              g_variant_new ("ah", &fds),
                                              5,
                                              app,
                                              permissions,
                                              fd_list,
                                              &doc_ids,
                                              &extra_out,
                                              NULL,
                                              NULL,
                                              error))
    return FALSE;

  g_variant_lookup (extra_out, "mountpoint", "^&ay", &mountpoint);

  for (i = 0; i < n_files; i++)
    {
      if (doc_ids[i][0] != '\0')
        {
          g_autofree char *basename = NULL;

          basename = g_path_get_basename (files[i]);
          g_print ("%s/%s/%s\n", mountpoint, doc_ids[i], basename);
        }
      else
        g_print ("%s\n", files[i]);
    }

  return TRUE;
}

gboolean
flatpak_complete_document_forward (FlatpakCompletion *completion)
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

      flatpak_complete_partial_ref (completion, FLATPAK_KINDS_APP, FALSE, flatpak_dir_get_user (), NULL);
      flatpak_complete_partial_ref (completion, FLATPAK_KINDS_APP, FALSE, flatpak_dir_get_system_default (), NULL);

      break;

    default:
      flatpak_complete_file (completion, "__FLATPAK_FILE");
      break;
    }

  return TRUE;
}
