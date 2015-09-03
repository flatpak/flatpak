/*
 * Copyright © 2014 Red Hat, Inc
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
 *       Alexander Larsson <alexl@redhat.com>
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

#include <gio/gunixfdlist.h>

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"
#include "xdg-app-run.h"

static GOptionEntry options[] = {
  { NULL }
};

gboolean
xdg_app_builtin_export_file (int argc, char **argv,
                             GCancellable *cancellable,
                             GError **error)
{
  gboolean ret = FALSE;
  GOptionContext *context;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  const char *file;
  g_autofree char  *mountpoint = NULL;
  XdpDbusDocuments *documents;
  int fd, fd_id;
  GUnixFDList *fd_list = NULL;
  const char *doc_id;

  context = g_option_context_new ("FILE - Export a file to apps");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv,
                                     XDG_APP_BUILTIN_FLAG_NO_DIR,
                                     NULL, cancellable, error))
    goto out;

  if (argc < 2)
    {
      usage_error (context, "FILE must be specified", error);
      goto out;
    }

  file = argv[1];

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (session_bus == NULL)
    goto out;

  documents = xdp_dbus_documents_proxy_new_sync (session_bus, 0,
                                                 "org.freedesktop.portal.Documents",
                                                 "/org/freedesktop/portal/documents",
                                                 NULL, error);
  if (documents == NULL)
    goto out;

  if (!xdp_dbus_documents_call_get_mount_point_sync (documents, &mountpoint,
                                                     NULL, error))
    goto out;

  fd = open (file, O_PATH | O_CLOEXEC);
  if (fd == -1)
    goto out;

  fd_list = g_unix_fd_list_new ();
  fd_id = g_unix_fd_list_append (fd_list, fd, error);
  close (fd);

  reply = g_dbus_connection_call_with_unix_fd_list_sync (session_bus,
                                                         "org.freedesktop.portal.Documents",
                                                         "/org/freedesktop/portal/documents",
                                                         "org.freedesktop.portal.Documents",
                                                         "Add",
                                                         g_variant_new ("(h)", fd_id),
                                                         G_VARIANT_TYPE ("(s)"),
                                                         G_DBUS_CALL_FLAGS_NONE,
                                                         30000,
                                                         fd_list, NULL,
                                                         NULL,
                                                         error);
  g_object_unref (fd_list);

  if (reply == NULL)
    goto out;

  g_variant_get (reply, "(s)", &doc_id);

  g_print ("%s/%s\n", mountpoint, doc_id);

  ret = TRUE;

 out:

  if (context)
    g_option_context_free (context);

  return ret;
}
