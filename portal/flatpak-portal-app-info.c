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
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <gio/gio.h>
#include "libglnx.h"
#include "flatpak-portal-app-info.h"
#include "flatpak-portal-error.h"

G_LOCK_DEFINE (app_infos);
static GHashTable *app_infos;

static void
ensure_app_infos (void)
{
  if (app_infos == NULL)
    app_infos = g_hash_table_new_full (g_str_hash, g_str_equal,
                                       NULL, (GDestroyNotify) g_key_file_unref);
}

static GKeyFile *
lookup_cached_app_info_by_sender (const char *sender)
{
  GKeyFile *keyfile = NULL;

  G_LOCK (app_infos);
  keyfile = g_hash_table_lookup (app_infos, sender);
  if (keyfile)
    g_key_file_ref (keyfile);
  G_UNLOCK (app_infos);

  return keyfile;
}

static void
invalidate_cached_app_info_by_sender (const char *sender)
{
  G_LOCK (app_infos);
  g_hash_table_remove (app_infos, sender);
  G_UNLOCK (app_infos);
}

static void
add_cached_app_info_by_sender (const char *sender, GKeyFile *keyfile)
{
  G_LOCK (app_infos);
  g_hash_table_add (app_infos, g_key_file_ref (keyfile));
  G_UNLOCK (app_infos);
}


/* Returns NULL on failure, keyfile with name "" if not sandboxed, and full app-info otherwise */
static GKeyFile *
parse_app_id_from_fileinfo (int pid)
{
  g_autofree char *root_path = NULL;
  glnx_autofd int root_fd = -1;
  glnx_autofd int info_fd = -1;
  struct stat stat_buf;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GMappedFile) mapped = NULL;
  g_autoptr(GKeyFile) metadata = NULL;

  root_path = g_strdup_printf ("/proc/%u/root", pid);
  if (!glnx_opendirat (AT_FDCWD, root_path, TRUE,
                       &root_fd,
                       &local_error))
    {
      /* Not able to open the root dir shouldn't happen. Probably the app died and
       * we're failing due to /proc/$pid not existing. In that case fail instead
         of treating this as privileged. */
      g_info ("Unable to open process root directory: %s", local_error->message);
      return NULL;
    }

  metadata = g_key_file_new ();

  info_fd = openat (root_fd, ".flatpak-info", O_RDONLY | O_CLOEXEC | O_NOCTTY);
  if (info_fd == -1)
    {
      if (errno == ENOENT)
        {
          /* No file => on the host */
          g_key_file_set_string (metadata, FLATPAK_METADATA_GROUP_APPLICATION,
                                 FLATPAK_METADATA_KEY_NAME, "");
          return g_steal_pointer (&metadata);
        }

      return NULL; /* Some weird error => failure */
    }

  if (fstat (info_fd, &stat_buf) != 0 || !S_ISREG (stat_buf.st_mode))
    return NULL; /* Some weird fd => failure */

  mapped = g_mapped_file_new_from_fd (info_fd, FALSE, &local_error);
  if (mapped == NULL)
    {
      g_warning ("Can't map .flatpak-info file: %s", local_error->message);
      return NULL;
    }

  if (!g_key_file_load_from_data (metadata,
                                  g_mapped_file_get_contents (mapped),
                                  g_mapped_file_get_length (mapped),
                                  G_KEY_FILE_NONE, &local_error))
    {
      g_warning ("Can't load .flatpak-info file: %s", local_error->message);
      return NULL;
    }

  return g_steal_pointer (&metadata);
}

GKeyFile *
flatpak_invocation_lookup_app_info (GDBusMethodInvocation *invocation,
                                    GCancellable          *cancellable,
                                    GError               **error)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(GDBusMessage) msg = NULL;
  g_autoptr(GDBusMessage) reply = NULL;
  g_autoptr(GVariantIter) iter = NULL;
  const char *key;
  GVariant *value;
  GKeyFile *keyfile;

  keyfile = lookup_cached_app_info_by_sender (sender);
  if (keyfile)
    return keyfile;

  msg = g_dbus_message_new_method_call ("org.freedesktop.DBus",
                                        "/org/freedesktop/DBus",
                                        "org.freedesktop.DBus",
                                        "GetConnectionCredentials");
  g_dbus_message_set_body (msg, g_variant_new ("(s)", sender));

  reply = g_dbus_connection_send_message_with_reply_sync (connection, msg,
                                                          G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                          30000,
                                                          NULL,
                                                          cancellable,
                                                          error);
  if (reply == NULL)
    return NULL;

  if (g_dbus_message_get_message_type (reply) == G_DBUS_MESSAGE_TYPE_METHOD_RETURN)
    {
      GVariant *body = g_dbus_message_get_body (reply);

      g_variant_get (body, "(a{sv})", &iter);
      while (g_variant_iter_loop (iter, "{&sv}", &key, &value))
        {
          if (strcmp (key, "ProcessID") == 0)
            {
              guint32 pid = g_variant_get_uint32 (value);
              g_variant_unref (value);
              keyfile = parse_app_id_from_fileinfo (pid);
              break;
            }
        }
    }

  if (keyfile == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Can't find peer app id");
      return NULL;
    }

  add_cached_app_info_by_sender (sender, keyfile);

  return keyfile;
}

static void
name_owner_changed (GDBusConnection *connection,
                    const gchar     *sender_name,
                    const gchar     *object_path,
                    const gchar     *interface_name,
                    const gchar     *signal_name,
                    GVariant        *parameters,
                    gpointer         user_data)
{
  const char *name, *from, *to;

  g_variant_get (parameters, "(sss)", &name, &from, &to);

  if (name[0] == ':' &&
      strcmp (name, from) == 0 &&
      strcmp (to, "") == 0)
    {
      invalidate_cached_app_info_by_sender (name);
    }
}

void
flatpak_connection_track_name_owners (GDBusConnection *connection)
{
  ensure_app_infos ();
  g_dbus_connection_signal_subscribe (connection,
                                      "org.freedesktop.DBus",
                                      "org.freedesktop.DBus",
                                      "NameOwnerChanged",
                                      "/org/freedesktop/DBus",
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      name_owner_changed,
                                      NULL, NULL);
}
