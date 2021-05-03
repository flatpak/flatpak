/*
 * Copyright © 2014 Red Hat, Inc
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
 *       Alexander Larsson <alexl@redhat.com>
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

/* Flags accepted by org.freedesktop.portal.Documents.AddFull */
enum {
  XDP_ADD_FLAGS_REUSE_EXISTING             =  (1 << 0),
  XDP_ADD_FLAGS_PERSISTENT                 =  (1 << 1),
  XDP_ADD_FLAGS_AS_NEEDED_BY_APP           =  (1 << 2),
  XDP_ADD_FLAGS_DIRECTORY                  =  (1 << 3),
};

static gboolean opt_unique = FALSE;
static gboolean opt_transient = FALSE;
static gboolean opt_noexist = FALSE;
static gboolean opt_allow_read = TRUE;
static gboolean opt_forbid_read = FALSE;
static gboolean opt_allow_write = FALSE;
static gboolean opt_forbid_write = FALSE;
static gboolean opt_allow_delete = FALSE;
static gboolean opt_forbid_delete = FALSE;
static gboolean opt_allow_grant_permissions = FALSE;
static gboolean opt_forbid_grant_permissions = FALSE;
static char **opt_apps = NULL;

static GOptionEntry options[] = {
  { "unique", 'u', 0, G_OPTION_ARG_NONE, &opt_unique, N_("Create a unique document reference"), NULL },
  { "transient", 't', 0, G_OPTION_ARG_NONE, &opt_transient, N_("Make the document transient for the current session"), NULL },
  { "noexist", 'n', 0, G_OPTION_ARG_NONE, &opt_noexist, N_("Don't require the file to exist already"), NULL },
  { "allow-read", 'r', 0, G_OPTION_ARG_NONE, &opt_allow_read, N_("Give the app read permissions"), NULL },
  { "allow-write", 'w', 0, G_OPTION_ARG_NONE, &opt_allow_write, N_("Give the app write permissions"), NULL },
  { "allow-delete", 'd', 0, G_OPTION_ARG_NONE, &opt_allow_delete, N_("Give the app delete permissions"), NULL },
  { "allow-grant-permission", 'g', 0, G_OPTION_ARG_NONE, &opt_allow_grant_permissions, N_("Give the app permissions to grant further permissions"), NULL },
  { "forbid-read", 0, 0, G_OPTION_ARG_NONE, &opt_forbid_read, N_("Revoke read permissions of the app"), NULL },
  { "forbid-write", 0, 0, G_OPTION_ARG_NONE, &opt_forbid_write, N_("Revoke write permissions of the app"), NULL },
  { "forbid-delete", 0, 0, G_OPTION_ARG_NONE, &opt_forbid_delete, N_("Revoke delete permissions of the app"), NULL },
  { "forbid-grant-permission", 0, 0, G_OPTION_ARG_NONE, &opt_forbid_grant_permissions, N_("Revoke the permission to grant further permissions"), NULL },
  { "app", 'a', 0, G_OPTION_ARG_STRING_ARRAY, &opt_apps, N_("Add permissions for this app"), N_("APPID") },
  { NULL }
};

gboolean
flatpak_builtin_document_export (int argc, char **argv,
                                 GCancellable *cancellable,
                                 GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GDBusConnection) session_bus = NULL;
  g_autoptr(GPtrArray) permissions = NULL;
  g_autoptr(GPtrArray) revocations = NULL;
  const char *file;
  g_autofree char *mountpoint = NULL;
  g_autofree char *basename = NULL;
  g_autofree char *dirname = NULL;
  g_autofree char *doc_path = NULL;
  XdpDbusDocuments *documents;
  glnx_autofd int fd = -1;
  int i, fd_id;
  GUnixFDList *fd_list = NULL;
  const char *doc_id;
  struct stat stbuf;
  gboolean is_directory = FALSE;

  context = g_option_context_new (_("FILE - Export a file to apps"));
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

  if (fstat (fd, &stbuf) == 0 && S_ISDIR (stbuf.st_mode))
    is_directory = TRUE;

  if (is_directory)
    {
      guint portal_version = 0;
      g_autoptr(GVariant) ret = NULL;
      g_autoptr(GVariant) v = NULL;

      ret = g_dbus_connection_call_sync (session_bus,
                                         "org.freedesktop.portal.Documents",
                                         "/org/freedesktop/portal/documents",
                                         "org.freedesktop.DBus.Properties",
                                         "Get",
                                         g_variant_new ("(ss)", "org.freedesktop.portal.Documents", "version"),
                                         G_VARIANT_TYPE ("(v)"),
                                         0,
                                         G_MAXINT,
                                         NULL,
                                         NULL);
      if (ret)
        {
          g_variant_get (ret, "(v)", &v);
          g_variant_get (v, "u", &portal_version);

          if (portal_version < 4)
            return flatpak_fail (error, "Exporting directories needs version 4 of the document portal (have version %d)", portal_version);
        }
    }

  fd_list = g_unix_fd_list_new ();
  fd_id = g_unix_fd_list_append (fd_list, fd, error);
  glnx_close_fd (&fd);

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
      if (is_directory)
        {
          guint flags = XDP_ADD_FLAGS_DIRECTORY;
          const char *perms[] = {NULL};

          if (!opt_unique)
            flags |= XDP_ADD_FLAGS_REUSE_EXISTING;
          if (!opt_transient)
            flags |= XDP_ADD_FLAGS_PERSISTENT;

          /* We only use AddFull for directories so that regular adds work with old portal versions */
          reply = g_dbus_connection_call_with_unix_fd_list_sync (session_bus,
                                                                 "org.freedesktop.portal.Documents",
                                                                 "/org/freedesktop/portal/documents",
                                                                 "org.freedesktop.portal.Documents",
                                                                 "AddFull",
                                                                 g_variant_new ("(@ahus^as)",
                                                                                g_variant_new_fixed_array (G_VARIANT_TYPE_HANDLE, &fd_id, 1, sizeof (fd_id)),
                                                                                flags,"", perms),
                                                                 G_VARIANT_TYPE ("(asa{sv})"),
                                                                 G_DBUS_CALL_FLAGS_NONE,
                                                                 30000,
                                                                 fd_list, NULL,
                                                                 NULL,
                                                                 error);
        }
      else
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

  if (is_directory)
    {
      g_autofree char **doc_ids = NULL;
      g_variant_get (reply, "(^a&s@a{sv})", &doc_ids, NULL);
      doc_id = doc_ids[0];
    }
  else
    {
      g_variant_get (reply, "(&s)", &doc_id);
    }

  permissions = g_ptr_array_new ();
  if (opt_allow_read)
    g_ptr_array_add (permissions, "read");
  if (opt_allow_write)
    g_ptr_array_add (permissions, "write");
  if (opt_allow_delete)
    g_ptr_array_add (permissions, "delete");
  if (opt_allow_grant_permissions)
    g_ptr_array_add (permissions, "grant-permissions");
  g_ptr_array_add (permissions, NULL);

  revocations = g_ptr_array_new ();
  if (opt_forbid_read)
    g_ptr_array_add (revocations, "read");
  if (opt_forbid_write)
    g_ptr_array_add (revocations, "write");
  if (opt_forbid_delete)
    g_ptr_array_add (revocations, "delete");
  if (opt_forbid_grant_permissions)
    g_ptr_array_add (revocations, "grant-permissions");
  g_ptr_array_add (revocations, NULL);

  for (i = 0; opt_apps != NULL && opt_apps[i] != NULL; i++)
    {
      if (!xdp_dbus_documents_call_grant_permissions_sync (documents,
                                                           doc_id,
                                                           opt_apps[i],
                                                           (const char **) permissions->pdata,
                                                           NULL,
                                                           error))
        return FALSE;

      if (!xdp_dbus_documents_call_revoke_permissions_sync (documents,
                                                            doc_id,
                                                            opt_apps[i],
                                                            (const char **) revocations->pdata,
                                                            NULL,
                                                            error))
        return FALSE;
    }

  doc_path = g_build_filename (mountpoint, doc_id, basename, NULL);
  g_print ("%s\n", doc_path);

  return TRUE;
}

gboolean
flatpak_complete_document_export (FlatpakCompletion *completion)
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
