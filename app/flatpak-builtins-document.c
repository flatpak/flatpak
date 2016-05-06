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

#include "flatpak-builtins.h"
#include "flatpak-utils.h"
#include "flatpak-run.h"

gboolean opt_unique = FALSE;
gboolean opt_allow_write = FALSE;
gboolean opt_allow_delete = FALSE;
gboolean opt_transient = FALSE;
gboolean opt_noexist = FALSE;
gboolean opt_allow_grant_permissions = FALSE;
char **opt_apps = NULL;

static GOptionEntry options[] = {
  { "unique", 'u', 0, G_OPTION_ARG_NONE, &opt_unique, "Create a unique document reference", NULL },
  { "transient", 't', 0, G_OPTION_ARG_NONE, &opt_transient, "Make the document transient for the current session", NULL },
  { "noexist", 'n', 0, G_OPTION_ARG_NONE, &opt_noexist, "Don't require the file to exist already", NULL },
  { "allow-write", 'w', 0, G_OPTION_ARG_NONE, &opt_allow_write, "Give the app write permissions", NULL },
  { "allow-delete", 'd', 0, G_OPTION_ARG_NONE, &opt_allow_delete, "Give the app permissions to delete the document id", NULL },
  { "allow-grant-permission", 'd', 0, G_OPTION_ARG_NONE, &opt_allow_grant_permissions, "Give the app permissions to grant furthern permissions", NULL },
  { "app", 'a', 0, G_OPTION_ARG_STRING_ARRAY, &opt_apps, "Add permissions for this app", NULL },
  { NULL }
};

gboolean
flatpak_builtin_export_file (int argc, char **argv,
                             GCancellable *cancellable,
                             GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  g_autoptr(GPtrArray) permissions = NULL;
  const char *file;
  g_autofree char *mountpoint = NULL;
  g_autofree char *basename = NULL;
  g_autofree char *dirname = NULL;
  g_autofree char *doc_path = NULL;
  XdpDbusDocuments *documents;
  int fd, fd_id;
  int i;
  GUnixFDList *fd_list = NULL;
  const char *doc_id;

  context = g_option_context_new ("FILE - Export a file to apps");

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR,
                                     NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, "FILE must be specified", error);

  file = argv[1];
  dirname = g_path_get_dirname (file);
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

  if (opt_noexist)
    fd = open (dirname, O_PATH | O_CLOEXEC);
  else
    fd = open (file, O_PATH | O_CLOEXEC);

  if (fd == -1)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  fd_list = g_unix_fd_list_new ();
  fd_id = g_unix_fd_list_append (fd_list, fd, error);
  close (fd);

  if (opt_noexist)
    {
      reply = g_dbus_connection_call_with_unix_fd_list_sync (session_bus,
                                                             "org.freedesktop.portal.Documents",
                                                             "/org/freedesktop/portal/documents",
                                                             "org.freedesktop.portal.Documents",
                                                             "AddNamed",
                                                             g_variant_new ("(h^aybb)", fd_id, basename, !opt_unique, !opt_transient),
                                                             G_VARIANT_TYPE ("(s)"),
                                                             G_DBUS_CALL_FLAGS_NONE,
                                                             30000,
                                                             fd_list, NULL,
                                                             NULL,
                                                             error);
    }
  else
    {
      reply = g_dbus_connection_call_with_unix_fd_list_sync (session_bus,
                                                             "org.freedesktop.portal.Documents",
                                                             "/org/freedesktop/portal/documents",
                                                             "org.freedesktop.portal.Documents",
                                                             "Add",
                                                             g_variant_new ("(hbb)", fd_id, !opt_unique, !opt_transient),
                                                             G_VARIANT_TYPE ("(s)"),
                                                             G_DBUS_CALL_FLAGS_NONE,
                                                             30000,
                                                             fd_list, NULL,
                                                             NULL,
                                                             error);
    }
  g_object_unref (fd_list);

  if (reply == NULL)
    return FALSE;

  g_variant_get (reply, "(&s)", &doc_id);

  permissions = g_ptr_array_new ();

  g_ptr_array_add (permissions, "read");
  if (opt_allow_write)
    g_ptr_array_add (permissions, "write");
  if (opt_allow_delete)
    g_ptr_array_add (permissions, "delete");
  if (opt_allow_grant_permissions)
    g_ptr_array_add (permissions, "grant-permissions");
  g_ptr_array_add (permissions, NULL);

  for (i = 0; opt_apps != NULL && opt_apps[i] != NULL; i++)
    {
      if (!xdp_dbus_documents_call_grant_permissions_sync (documents,
                                                           doc_id,
                                                           opt_apps[i],
                                                           (const char **) permissions->pdata,
                                                           NULL,
                                                           error))
        return FALSE;

    }

  doc_path = g_build_filename (mountpoint, doc_id, basename, NULL);
  g_print ("%s\n", doc_path);

  return TRUE;
}
