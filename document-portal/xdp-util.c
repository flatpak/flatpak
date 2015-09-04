#include "config.h"
#include <string.h>
#include <errno.h>
#include <gio/gio.h>
#include "xdg-app-error.h"
#include "xdp-util.h"

const char **
xdg_unparse_permissions (XdpPermissionFlags permissions)
{
  GPtrArray *array;

  array = g_ptr_array_new ();

  if (permissions & XDP_PERMISSION_FLAGS_READ)
    g_ptr_array_add (array, "read");
  if (permissions & XDP_PERMISSION_FLAGS_WRITE)
    g_ptr_array_add (array, "write");
  if (permissions & XDP_PERMISSION_FLAGS_GRANT_PERMISSIONS)
    g_ptr_array_add (array, "grant-permissions");
  if (permissions & XDP_PERMISSION_FLAGS_DELETE)
    g_ptr_array_add (array, "delete");

  g_ptr_array_add (array, NULL);
  return (const char **)g_ptr_array_free (array, FALSE);
}

XdpPermissionFlags
xdp_parse_permissions (const char **permissions)
{
  XdpPermissionFlags perms;
  int i;

  perms = 0;
  for (i = 0; permissions[i]; i++)
    {
      if (strcmp (permissions[i], "read") == 0)
        perms |= XDP_PERMISSION_FLAGS_READ;
      else if (strcmp (permissions[i], "write") == 0)
        perms |= XDP_PERMISSION_FLAGS_WRITE;
      else if (strcmp (permissions[i], "grant-permissions") == 0)
        perms |= XDP_PERMISSION_FLAGS_GRANT_PERMISSIONS;
      else if (strcmp (permissions[i], "delete") == 0)
        perms |= XDP_PERMISSION_FLAGS_DELETE;
      else
        g_warning ("No such permission: %s", permissions[i]);
    }

  return perms;
}

XdpPermissionFlags
xdp_entry_get_permissions (XdgAppDbEntry *entry,
                           const char *app_id)
{
  g_autofree const char **permissions = NULL;

  if (strcmp (app_id, "") == 0)
    return XDP_PERMISSION_FLAGS_ALL;

  permissions = xdg_app_db_entry_list_permissions (entry, app_id);
  return xdp_parse_permissions (permissions);
}

gboolean
xdp_entry_has_permissions (XdgAppDbEntry *entry,
                           const char *app_id,
                           XdpPermissionFlags perms)
{
  XdpPermissionFlags current_perms;

  current_perms = xdp_entry_get_permissions (entry, app_id);

  return (current_perms & perms) == perms;
}

guint32
xdp_id_from_name (const char *name)
{
  return g_ascii_strtoull (name, NULL, 16);
}

char *
xdp_name_from_id (guint32 doc_id)
{
  return g_strdup_printf ("%x", doc_id);
}

const char *
xdp_entry_get_path (XdgAppDbEntry *entry)
{
  g_autoptr(GVariant) v = xdg_app_db_entry_get_data (entry);
  g_autoptr(GVariant) c = g_variant_get_child_value (v, 0);
  return g_variant_get_bytestring (c);
}

char *
xdp_entry_dup_basename (XdgAppDbEntry *entry)
{
  const char *path = xdp_entry_get_path (entry);

  return g_path_get_basename (path);
}

char *
xdp_entry_dup_dirname (XdgAppDbEntry *entry)
{
  const char *path = xdp_entry_get_path (entry);

  return g_path_get_dirname (path);
}

guint64
xdp_entry_get_device (XdgAppDbEntry *entry)
{
  g_autoptr(GVariant) v = xdg_app_db_entry_get_data (entry);
  g_autoptr(GVariant) c = g_variant_get_child_value (v, 1);
  return g_variant_get_uint64 (c);
}

guint64
xdp_entry_get_inode (XdgAppDbEntry *entry)
{
  g_autoptr(GVariant) v = xdg_app_db_entry_get_data (entry);
  g_autoptr(GVariant) c = g_variant_get_child_value (v, 2);
  return g_variant_get_uint64 (c);
}

guint32
xdp_entry_get_flags (XdgAppDbEntry *entry)
{
  g_autoptr(GVariant) v = xdg_app_db_entry_get_data (entry);
  g_autoptr(GVariant) c = g_variant_get_child_value (v, 3);
  return g_variant_get_uint32 (c);
}

int
xdp_entry_open_dir (XdgAppDbEntry *entry)
{
  g_autofree char *dirname = xdp_entry_dup_dirname (entry);
  struct stat st_buf;
  int fd;

  fd = open (dirname, O_CLOEXEC | O_PATH | O_DIRECTORY);
  if (fd == -1)
    return -1;

  if (fstat (fd, &st_buf) < 0)
    {
      close (fd);
      errno = ENOENT;
      return -1;
    }

  if (st_buf.st_ino != xdp_entry_get_inode (entry) ||
      st_buf.st_dev != xdp_entry_get_device (entry))
    {
      close (fd);
      errno = ENOENT;
      return -1;
    }

  return fd;
}

int
xdp_entry_stat (XdgAppDbEntry *entry,
                struct stat *buf,
                int flags)
{
  glnx_fd_close int fd = -1;
  g_autofree char *basename = xdp_entry_dup_basename (entry);

  fd = xdp_entry_open_dir (entry);
  if (fd < 0)
    return -1;

  if (fstatat (fd, basename, buf, flags) != 0)
    return -1;

  return 0;
}

static GHashTable *app_ids;

typedef struct {
  char *name;
  char *app_id;
  gboolean exited;
  GList *pending;
} AppIdInfo;

static void
app_id_info_free (AppIdInfo *info)
{
  g_free (info->name);
  g_free (info->app_id);
  g_free (info);
}

static void
ensure_app_ids (void)
{
  if (app_ids == NULL)
    app_ids = g_hash_table_new_full (g_str_hash, g_str_equal,
                                     NULL, (GDestroyNotify)app_id_info_free);
}

static void
got_credentials_cb (GObject *source_object,
                    GAsyncResult *res,
                    gpointer user_data)
{
  AppIdInfo *info = user_data;
  g_autoptr (GDBusMessage) reply = NULL;
  g_autoptr (GError) error = NULL;
  GList *l;

  reply = g_dbus_connection_send_message_with_reply_finish (G_DBUS_CONNECTION (source_object),
                                                            res, &error);

  if (!info->exited && reply != NULL)
    {
      GVariant *body = g_dbus_message_get_body (reply);
      guint32 pid;
      g_autofree char *path = NULL;
      g_autofree char *content = NULL;

      g_variant_get (body, "(u)", &pid);

      path = g_strdup_printf ("/proc/%u/cgroup", pid);

      if (g_file_get_contents (path, &content, NULL, NULL))
        {
          gchar **lines =  g_strsplit (content, "\n", -1);
          int i;

          for (i = 0; lines[i] != NULL; i++)
            {
              if (g_str_has_prefix (lines[i], "1:name=systemd:"))
                {
                  const char *unit = lines[i] + strlen ("1:name=systemd:");
                  g_autofree char *scope = g_path_get_basename (unit);

                  if (g_str_has_prefix (scope, "xdg-app-") &&
                      g_str_has_suffix (scope, ".scope"))
                    {
                      const char *name = scope + strlen("xdg-app-");
                      char *dash = strchr (name, '-');
                      if (dash != NULL)
                        {
                          *dash = 0;
                          info->app_id = g_strdup (name);
                        }
                    }
                  else
                    info->app_id = g_strdup ("");
                }
            }
          g_strfreev (lines);
        }
    }

  for (l = info->pending; l != NULL; l = l->next)
    {
      GTask *task = l->data;

      if (info->app_id == NULL)
        g_task_return_new_error (task, XDG_APP_ERROR, XDG_APP_ERROR_FAILED,
                                 "Can't find app id");
      else
        g_task_return_pointer (task, g_strdup (info->app_id), g_free);
    }

  g_list_free_full (info->pending, g_object_unref);
  info->pending = NULL;

  if (info->app_id == NULL)
    g_hash_table_remove (app_ids, info->name);
}

void
xdp_invocation_lookup_app_id (GDBusMethodInvocation *invocation,
                              GCancellable          *cancellable,
                              GAsyncReadyCallback    callback,
                              gpointer               user_data)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(GTask) task = NULL;
  AppIdInfo *info;

  task = g_task_new (invocation, cancellable, callback, user_data);

  ensure_app_ids ();

  info = g_hash_table_lookup (app_ids, sender);

  if (info == NULL)
    {
      info = g_new0 (AppIdInfo, 1);
      info->name = g_strdup (sender);
      g_hash_table_insert (app_ids, info->name, info);
    }

  if (info->app_id)
    g_task_return_pointer (task, g_strdup (info->app_id), g_free);
  else
    {
      if (info->pending == NULL)
        {
          g_autoptr (GDBusMessage) msg = g_dbus_message_new_method_call ("org.freedesktop.DBus",
                                                                         "/org/freedesktop/DBus",
                                                                         "org.freedesktop.DBus",
                                                                         "GetConnectionUnixProcessID");
          g_dbus_message_set_body (msg, g_variant_new ("(s)", sender));

          g_dbus_connection_send_message_with_reply (connection, msg,
                                                     G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                     30000,
                                                     NULL,
                                                     cancellable,
                                                     got_credentials_cb,
                                                     info);
        }

      info->pending = g_list_prepend (info->pending, g_object_ref (task));
    }
}

char *
xdp_invocation_lookup_app_id_finish (GDBusMethodInvocation *invocation,
                                     GAsyncResult    *result,
                                     GError         **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
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

  ensure_app_ids ();

  if (name[0] == ':' &&
      strcmp (name, from) == 0 &&
      strcmp (to, "") == 0)
    {
      AppIdInfo *info = g_hash_table_lookup (app_ids, name);

      if (info != NULL)
        {
          info->exited = TRUE;
          if (info->pending == NULL)
            g_hash_table_remove (app_ids, name);
        }
    }
}

void
xdp_connection_track_name_owners (GDBusConnection *connection)
{
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
