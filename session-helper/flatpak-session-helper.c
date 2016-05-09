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
#include <gio/gio.h>
#include "flatpak-dbus.h"
#include "flatpak-utils.h"

static char *monitor_dir;

static gboolean
handle_request_monitor (FlatpakSessionHelper   *object,
                        GDBusMethodInvocation *invocation,
                        gpointer               user_data)
{
  flatpak_session_helper_complete_request_monitor (object, invocation,
                                                   monitor_dir);

  return TRUE;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  FlatpakSessionHelper *helper;
  GError *error = NULL;

  helper = flatpak_session_helper_skeleton_new ();

  g_signal_connect (helper, "handle-request-monitor", G_CALLBACK (handle_request_monitor), NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (helper),
                                         connection,
                                         "/org/freedesktop/Flatpak/SessionHelper",
                                         &error))
    {
      g_warning ("error: %s\n", error->message);
      g_error_free (error);
    }
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  exit (1);
}

static void
copy_file (const char *source,
           const char *target_dir)
{
  char *basename = g_path_get_basename (source);
  char *dest = g_build_filename (target_dir, basename, NULL);
  gchar *contents = NULL;
  gsize len;

  if (g_file_get_contents (source, &contents, &len, NULL))
    g_file_set_contents (dest, contents, len, NULL);

  g_free (basename);
  g_free (dest);
  g_free (contents);
}

static void
file_changed (GFileMonitor     *monitor,
              GFile            *file,
              GFile            *other_file,
              GFileMonitorEvent event_type,
              char             *source)
{
  if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
      event_type == G_FILE_MONITOR_EVENT_CREATED)
    copy_file (source, monitor_dir);
}

static void
setup_file_monitor (const char *source)
{
  GFile *s = g_file_new_for_path (source);
  GFileMonitor *monitor;

  copy_file (source, monitor_dir);

  monitor = g_file_monitor_file (s, G_FILE_MONITOR_NONE, NULL, NULL);
  if (monitor)
    g_signal_connect (monitor, "changed", G_CALLBACK (file_changed), (char *) source);
}

int
main (int    argc,
      char **argv)
{
  guint owner_id;
  GMainLoop *loop;

  setlocale (LC_ALL, "");

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_prgname (argv[0]);

  flatpak_migrate_from_xdg_app ();

  monitor_dir = g_build_filename (g_get_user_runtime_dir (), "flatpak-monitor", NULL);
  if (g_mkdir_with_parents (monitor_dir, 0755) != 0)
    {
      g_print ("Can't create %s\n", monitor_dir);
      exit (1);
    }

  setup_file_monitor ("/etc/resolv.conf");
  setup_file_monitor ("/etc/localtime");

  owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             "org.freedesktop.Flatpak",
                             G_BUS_NAME_OWNER_FLAGS_NONE,
                             on_bus_acquired,
                             on_name_acquired,
                             on_name_lost,
                             NULL,
                             NULL);

  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  g_bus_unown_name (owner_id);

  return 0;
}
