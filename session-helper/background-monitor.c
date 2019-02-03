/*
 * Copyright © 2019 Red Hat, Inc
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

#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include "flatpak-instance.h"
#include <libnotify/notify.h>
#include "background-monitor.h"
#include "app/flatpak-permission-dbus-generated.h"
#include "app/gnome-shell-introspect-dbus-generated.h"

#define PERMISSION_TABLE "background"
#define PERMISSION_ID "background"

typedef enum { UNSET, NO, YES, ASK } Permission;

static GnomeShellIntrospect *shell = NULL;

static void
init_gnome_shell (GDBusConnection *connection)
{
  g_autoptr(GError) error = NULL;

  shell = gnome_shell_introspect_proxy_new_sync (connection,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 "org.gnome.Shell",
                                                 "/org/gnome/Shell/Introspect",
                                                 NULL, &error);
  if (shell == NULL)
    g_debug ("No gnome-shell introspection: %s", error->message);
}

static GnomeShellIntrospect *
get_gnome_shell (void)
{
  return shell;
}

typedef enum { BACKGROUND, RUNNING, ACTIVE } AppState;

static GHashTable *
get_app_states (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) windows = NULL;
  g_autoptr(GHashTable) app_states = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  if (!gnome_shell_introspect_call_get_windows_sync (get_gnome_shell (), &windows, NULL, &error))
    g_debug ("Could not get window list: %s", error->message);

  if (windows)
    {
      g_autoptr(GVariantIter) iter = g_variant_iter_new (windows);
      GVariant *dict;
      while (g_variant_iter_loop (iter, "{t@a{sv}}", NULL, &dict))
        {
          const char *app_id = NULL;
          gboolean hidden = FALSE;
          gboolean focus = FALSE;
          AppState state = BACKGROUND;

          g_variant_lookup (dict, "app-id", "&s", &app_id);
          g_variant_lookup (dict, "is-hidden", "b", &hidden);
          g_variant_lookup (dict, "has-focus", "b", &focus);

          if (app_id == NULL)
            continue;

          state = GPOINTER_TO_INT (g_hash_table_lookup (app_states, app_id));

          if (!hidden)
            state = MAX (state, RUNNING);
          if (focus) 
            state = MAX (state, ACTIVE);

          g_hash_table_insert (app_states, g_strdup (app_id), GINT_TO_POINTER (state));
        } 
    }

  return g_steal_pointer (&app_states);
}

static AppState
get_app_state (const char *app_id,
               GHashTable *app_states)
{
  return (AppState)GPOINTER_TO_INT (g_hash_table_lookup (app_states, app_id));
}

static XdpDbusPermissionStore *permission_store = NULL;

static void
init_permission_store (GDBusConnection *connection)
{
  g_autoptr(GError) error = NULL;

  permission_store = xdp_dbus_permission_store_proxy_new_sync (connection,
                                                               G_DBUS_PROXY_FLAGS_NONE,
                                                               "org.freedesktop.impl.portal.PermissionStore",
                                                               "/org/freedesktop/impl/portal/PermissionStore",
                                                               NULL, &error);
  if (permission_store == NULL)
    g_warning ("No permission store: %s", error->message);
}

static XdpDbusPermissionStore *
get_permission_store (void)
{
  return permission_store;
}

static GVariant *
get_permissions (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;

  if (!xdp_dbus_permission_store_call_lookup_sync (get_permission_store (),
                                                   PERMISSION_TABLE,
                                                   PERMISSION_ID,
                                                   &out_perms,
                                                   &out_data,
                                                   NULL,
                                                   &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_debug ("No background permissions found: %s", error->message);
      return NULL;
    }

  return g_steal_pointer (&out_perms);
}

static Permission
get_permission (const char *app_id,
                GVariant   *perms)
{
  const char **permissions;

  if (!g_variant_lookup (perms, app_id, "^a&s", &permissions))
    {
      g_debug ("No permissions stored for: app %s", app_id);

      return UNSET;
    }
  else if (g_strv_length ((char **)permissions) != 1)
    {
      g_autofree char *a = g_strjoinv (" ", (char **)permissions);
      g_warning ("Wrong permission format, ignoring (%s)", a);
      return UNSET;
    }
  g_debug ("permission store: app %s -> %s", app_id, permissions[0]);

  if (strcmp (permissions[0], "yes") == 0)
    return YES;
  else if (strcmp (permissions[0], "no") == 0)
    return NO;
  else if (strcmp (permissions[0], "ask") == 0)
    return ASK;
  else
    {
      g_autofree char *a = g_strjoinv (" ", (char **)permissions);
      g_warning ("Wrong permission format, ignoring (%s)", a);
    }

  return UNSET;
}

static void
set_permission (const char *app_id,
                Permission  permission)
{
  g_autoptr(GError) error = NULL;
  const char *permissions[2];

  if (permission == ASK)
    permissions[0] = "ask";
  else if (permission == YES)
    permissions[0] = "yes";
  else if (permission == NO)
    permissions[0] = "no";
  else
    {
      g_warning ("Wrong permission format, ignoring");
      return;
    }
  permissions[1] = NULL;

  if (!xdp_dbus_permission_store_call_set_permission_sync (get_permission_store (),
                                                           PERMISSION_TABLE,
                                                           TRUE,
                                                           PERMISSION_ID,
                                                           app_id,
                                                           (const char * const*)permissions,
                                                           NULL,
                                                           &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_warning ("Error updating permission store: %s", error->message);
    }
}

static char *
flatpak_instance_get_display_name (FlatpakInstance *instance)
{
  const char *app_id = flatpak_instance_get_app (instance);
  if (app_id[0] != 0)
    {
      g_autofree char *desktop_id = NULL;
      g_autoptr(GAppInfo) info = NULL;

      desktop_id = g_strconcat (app_id, ".desktop", NULL);
      info = (GAppInfo*)g_desktop_app_info_new (desktop_id);

      if (info)
        return g_strdup (g_app_info_get_display_name (info));
    }

  return g_strdup (app_id);
}

static void
kill_instance (FlatpakInstance *instance)
{
  g_debug ("Killing app %s", flatpak_instance_get_app (instance));
  kill (flatpak_instance_get_child_pid (instance), SIGKILL);
}

static GPtrArray *notifications;
G_LOCK_DEFINE_STATIC (notifications);

static void
init_notifications (void)
{
  notifications = g_ptr_array_new_with_free_func (g_object_unref);
}

static void
remove_notification (NotifyNotification *notification)
{
  notify_notification_close (notification, NULL);

  G_LOCK (notifications);
  g_ptr_array_remove (notifications, notification);
  G_UNLOCK (notifications);
}

static void
closed_cb (NotifyNotification *notification,
           gpointer            data)
{
  g_debug ("Notification closed");

  remove_notification (notification);
}

static gboolean
same_instance (FlatpakInstance *i1,
               FlatpakInstance *i2)
{
  return strcmp (flatpak_instance_get_app (i1), flatpak_instance_get_app (i2)) == 0 &&
         flatpak_instance_get_child_pid (i1) == flatpak_instance_get_child_pid (i2);
}

static gboolean
add_notification_for_instance (FlatpakInstance    *instance,
                               NotifyNotification *notification)
{
  int i;
  gboolean found = FALSE;

  G_LOCK (notifications);
  for (i = 0; i < notifications->len && !found; i++)
    {
      NotifyNotification *n = g_ptr_array_index (notifications, i);
      FlatpakInstance *inst = (FlatpakInstance *)g_object_get_data (G_OBJECT (n), "instance");
      found = same_instance (instance, inst);
    }

  if (!found)
    {
      g_object_set_data_full (G_OBJECT (notification), "instance", g_object_ref (instance), (GDestroyNotify)g_object_unref);
      g_ptr_array_add (notifications, g_object_ref (notification));
    }

  G_UNLOCK (notifications);

  if (!found)
    {
      g_signal_connect (notification, "closed", G_CALLBACK (closed_cb), NULL);
      notify_notification_show (notification, NULL);
    }

  return !found;
}

static void
remove_outdated_notifications (GPtrArray *apps)
{
  int i, j;

  G_LOCK (notifications);
  for (i = notifications->len - 1; i >= 0; i--)
    {
      NotifyNotification *n = g_ptr_array_index (notifications, i);      
      FlatpakInstance *inst = (FlatpakInstance *)g_object_get_data (G_OBJECT (n), "instance");
      gboolean found = FALSE;
      for (j = 0; j < apps->len && !found; j++)
        {
          FlatpakInstance *app = g_ptr_array_index (apps, j);
          found = same_instance (app, inst);
        }
      if (!found)
        {
          notify_notification_close (n, NULL);
          g_ptr_array_remove (notifications, n);
        }
    }
  G_UNLOCK (notifications);
}

static void
allow_app (NotifyNotification *notification,
           char               *action,
           gpointer            user_data)
{
  FlatpakInstance *instance = user_data;

  g_debug ("Allowing app %s to run in background", flatpak_instance_get_app (instance));

  set_permission (flatpak_instance_get_app (instance), YES);

  remove_notification (notification);
}

static void
forbid_app (NotifyNotification *notification,
            char               *action,
            gpointer            user_data)
{
  FlatpakInstance *instance = user_data;

  g_debug ("Forbid app %s to run in background", flatpak_instance_get_app (instance));

  set_permission (flatpak_instance_get_app (instance), NO);
  kill_instance (instance);

  remove_notification (notification);
}

static void
ignore_app (NotifyNotification *notification,
            char               *action,
            gpointer            user_data)
{
  FlatpakInstance *instance = user_data;

  g_debug ("Let app %s run in background", flatpak_instance_get_app (instance));

  remove_notification (notification);
}

static void
stop_app (NotifyNotification *notification,
          char               *action,
          gpointer            user_data)
{
  FlatpakInstance *instance = user_data;

  g_debug ("Stop app %s", flatpak_instance_get_app (instance));

  kill_instance (instance);

  remove_notification (notification);
}

static void
send_notification (FlatpakInstance *instance,
                   Permission       permission)
{
  g_autofree char *name = flatpak_instance_get_display_name (instance);
  const char *summary;
  g_autofree char *body = NULL;
  NotifyNotification *notification;

  summary = _("Background activity");
  body = g_strdup_printf (_("%s is running in the background."), name);
  notification = notify_notification_new (summary, body, NULL);

  if (permission == UNSET)
    {
      notify_notification_add_action (notification, "allow", _("Allow"), allow_app, g_object_ref (instance), g_object_unref);
      notify_notification_add_action (notification, "forbid", _("Forbid"), forbid_app, g_object_ref (instance), g_object_unref);
    }
  else if (permission == ASK)
    {
      notify_notification_add_action (notification, "ignore", _("Ignore"), ignore_app, g_object_ref (instance), g_object_unref);
      notify_notification_add_action (notification, "stop", _("Stop"), stop_app, g_object_ref (instance), g_object_unref);
    }

  add_notification_for_instance (instance, notification);
  g_object_unref (notification);
}

static void
thread_func (GTask *task,
             gpointer source_object,
             gpointer task_data,
             GCancellable *cancellable)
{
  g_autoptr(GVariant) perms = NULL;
  g_autoptr(GHashTable) app_states = NULL;
  g_autoptr(GPtrArray) apps = NULL;
  int i;

  g_debug ("Checking background permissions");

  perms = get_permissions ();
  app_states = get_app_states ();

  apps = flatpak_instance_get_all ();

  remove_outdated_notifications (apps);

  for (i = 0; i < apps->len; i++)
    {
      FlatpakInstance *instance = g_ptr_array_index (apps, i);
      const char *app_id = flatpak_instance_get_app (instance);
      Permission permission;

      if (!flatpak_instance_is_running (instance))
        continue;

      if (get_app_state (app_id, app_states) != BACKGROUND)
        continue;

      g_debug ("App %s is running in the background", app_id);

      permission = get_permission (app_id, perms);
      if (permission == NO)
        {
          pid_t pid = flatpak_instance_get_child_pid (instance);
          g_debug ("Killing app %s (child pid %u)", app_id, pid);
          kill (pid, SIGKILL);
        }
      else if (permission == ASK || permission == UNSET)
        {
          send_notification (instance, permission);
        }
    }
}

static gboolean
enforce_background_permissions (gpointer data)
{
  g_autoptr(GTask) task = NULL;

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_run_in_thread (task, thread_func);

  return G_SOURCE_CONTINUE;
}

void
start_background_monitor (GDBusConnection *bus)
{
  init_notifications ();
  notify_init ("flatpak");
  init_permission_store (bus);
  init_gnome_shell (bus);

  g_debug ("Starting background app monitor");
  g_timeout_add_seconds (300, enforce_background_permissions, NULL);
  enforce_background_permissions (NULL);
}
