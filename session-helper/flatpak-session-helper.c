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
#include "flatpak-db.h"
#include "flatpak-dbus.h"
#include "flatpak-utils.h"

#ifdef ENABLE_SELINUX
#include <selinux/selinux.h>

#define TABLE_NAME "selinux"
#define ROW_ID "labels"

static FlatpakDb *db = NULL;

G_LOCK_DEFINE (db);

static GHashTable *mcs_table;

static void
init_mcs_table (void)
{
  mcs_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

static gboolean
add_mcs (const char *mcs)
{
  if (g_hash_table_contains (mcs_table, mcs))
    return FALSE;

  g_hash_table_add (mcs_table, g_strdup (mcs));
  return TRUE;
}

static gboolean
delete_mcs (const char *mcs)
{
  if (!g_hash_table_contains (mcs_table, mcs))
    return FALSE;

  g_hash_table_remove (mcs_table, mcs);
  return TRUE;
}

char *
unique_mcs (int range)
{
  char *mcs;

  while (1)
    {
      int c1, c2;

      c1 = g_random_int_range (0, range);
      c2 = g_random_int_range (0, range);

      if (c1 == c2)
        continue;
      else if (c1 > c2)
        {
          int t = c1;
          c1 = c2;
          c2 = t;
        }

      mcs = g_strdup_printf ("s0:c%d,c%d", c1, c2);
      if (add_mcs (mcs))
        break;
      g_free (mcs);
    }

  return mcs;
}

void
get_lxc_contexts (const char **exec_context,
                  const char **file_context)
{
  static char *lxc_exec_context;
  static char *lxc_file_context;
  g_autofree char *lxc_path = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *buffer = NULL;
  gsize length;
  g_auto(GStrv) lines = NULL;
  int i;
  g_autofree char *mcs = NULL;

  if (lxc_exec_context)
    goto out;

  lxc_exec_context = "";
  lxc_file_context = "";

  if (!is_selinux_enabled ())
    {
      g_warning ("SELinux not enabled");
      *exec_context = lxc_exec_context;
      *file_context = lxc_file_context;
      goto out;
    }

  lxc_path = g_build_filename (selinux_policy_root(), "contexts/lxc_contexts", NULL);
  if (!g_file_get_contents (lxc_path, &buffer, &length, &error))
    {
      g_warning ("Failed to read lxc contexts: %s", error->message);
      goto out;
    }

  lines = g_strsplit (buffer, "\n", 0);
  for (i = 0; lines[i]; i++)
    {
      char *line = g_strstrip (lines[i]);
      g_auto(GStrv) parts = NULL;
      char *key, *value;

      if (line[0] == '\0' || line[0] == ';' || line[0] == '#')
        continue;

      parts = g_strsplit (line, "=", 2);

      key = g_strstrip (parts[0]);
      value = g_strstrip (parts[1]);
      if (value[0] == '"')
        {
          value[0] = ' ';
          value[strlen (value) - 1] = ' ';
          value = g_strstrip (value);
        }

      if (strcmp (key, "process") == 0)
        lxc_exec_context = g_strdup (value);

      if (strcmp (key, "file") == 0)
        lxc_file_context = g_strdup (value);
    }

out:
  *exec_context = (const char *)lxc_exec_context;
  *file_context = (const char *)lxc_file_context;
}

static void
get_unique_labels (char **exec_label,
                   char **file_label)
{
  const char *exec_context = NULL;
  const char *file_context = NULL;
  char *current_context;
  g_auto(GStrv) current_parts = NULL;
  g_auto(GStrv) exec_parts = NULL;
  g_auto(GStrv) file_parts = NULL;
  g_autofree char *mcs = NULL;

  get_lxc_contexts (&exec_context, &file_context);

  if (exec_context[0] == '\0')
    {
      *exec_label = g_strdup ("");
      *file_label = g_strdup ("");
      return;
    }

  exec_parts = g_strsplit (exec_context, ":", 0);
  file_parts = g_strsplit (file_context, ":", 0);

  getcon (&current_context);
  current_parts = g_strsplit (current_context, ":", 0);
  freecon (current_context);

  mcs = unique_mcs (1024);

  *exec_label = g_strdup_printf ("%s:%s:%s:%s",
                                 current_parts[0],
                                 current_parts[1],
                                 exec_parts[2],
                                 mcs);

  *file_label = g_strdup_printf ("%s:%s:%s:%s",
                                 file_parts[0],
                                 file_parts[1],
                                 file_parts[2],
                                 mcs);
}

static gboolean
remove_exec_label (const char *label)
{
  g_autofree char **parts = NULL;
  parts = g_strsplit (label, ":", 4);
  return delete_mcs (parts[3]);
}

static gboolean
add_exec_label (const char *label)
{
  g_autofree char **parts = NULL;
  parts = g_strsplit (label, ":", 4);
  return add_mcs (parts[3]);
}

static void
load_existing_mcs_labels (void)
{
  g_autoptr(FlatpakDbEntry) entry = NULL;
  g_autofree char *exec_label = NULL;
  g_autofree char **apps = NULL;
  int i;

  init_mcs_table ();

  AUTOLOCK (db);

  entry = flatpak_db_lookup (db, ROW_ID);
  if (entry == NULL)
    return;

  apps = (char **)flatpak_db_entry_list_apps (entry);
  for (i = 0; apps[i]; i++)
    {
      g_autofree char **perms = NULL;
      perms = (char **)flatpak_db_entry_list_permissions (entry, apps[i]);
g_print ("found existing label for %s: %s\n", apps[i], perms[0]);
      if (perms[0] != NULL && perms[1] != NULL)
        add_exec_label (perms[0]);
    }
}

static gboolean
handle_request_selinux_label (FlatpakSessionHelper *object,
                              GDBusMethodInvocation *invocation,
                              const char *arg_app_id)
{
  g_autoptr(FlatpakDbEntry) entry = NULL;
  g_autofree char *exec_label = NULL;
  g_autofree char *file_label = NULL;
  gboolean persist = FALSE;
  g_autofree char **perms = NULL;

  AUTOLOCK (db);

  entry = flatpak_db_lookup (db, ROW_ID);
  if (entry == NULL)
    {
      entry = flatpak_db_entry_new (NULL);
      persist = TRUE;
    }

  perms = (char **)flatpak_db_entry_list_permissions (entry, arg_app_id);
  if (perms[0] != NULL && perms[1] != NULL)
    {
      exec_label = g_strdup (perms[0]);
      file_label = g_strdup (perms[1]);
    }
  else
    {
      const char *nperms[3];

      get_unique_labels (&exec_label, &file_label);

      nperms[0] = exec_label;
      nperms[1] = file_label;
      nperms[2] = NULL;
      entry = flatpak_db_entry_set_app_permissions (entry, arg_app_id, nperms);
      flatpak_db_set_entry (db, ROW_ID, entry);
      persist = TRUE;
    }

  g_print ("sending exec label for %s: %s\n", arg_app_id, exec_label);
  flatpak_session_helper_complete_request_selinux_label (object, invocation,
                                                         exec_label,
                                                         file_label);

  if (persist)
    {
      g_autoptr(GError) error = NULL;
g_print ("storing db\n");
      flatpak_db_update (db);
      if (!flatpak_db_save_content (db, &error))
        g_warning ("Failed to save selinux label db: %s", error->message);
    }

  return TRUE;
}

static gboolean
handle_release_selinux_label (FlatpakSessionHelper *object,
                              GDBusMethodInvocation *invocation,
                              const char *arg_app_id)
{
  g_autoptr(FlatpakDbEntry) entry = NULL;
  gboolean persist = FALSE;
  g_autofree char **perms = NULL;

  AUTOLOCK (db);

  entry = flatpak_db_lookup (db, ROW_ID);
  if (entry == NULL)
    goto out;

  perms = (char **)flatpak_db_entry_list_permissions (entry, arg_app_id);
  if (perms[0] != NULL && perms[1] != NULL)
    {
      remove_exec_label (perms[0]);
      entry = flatpak_db_entry_set_app_permissions (entry, arg_app_id, NULL);
      flatpak_db_set_entry (db, ROW_ID, entry);
      persist = TRUE;
    }

out:
  flatpak_session_helper_complete_release_selinux_label (object, invocation);

  if (persist)
    {
      g_autoptr(GError) error = NULL;
      flatpak_db_update (db);
      if (!flatpak_db_save_content (db, &error))
        g_warning ("Failed to save selinux label db: %s", error->message);
    }

  return TRUE;
}

#else /* !ENABLE_SELINUX */

static gboolean
handle_request_selinux_label (FlatpakSessionHelper *object,
                              GDBusMethodInvocation *invocation,
                              const char *arg_app_id)
{
  g_dbus_method_invocation_return_error (invocation,
                                         G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
                                         "Flatpak was built without SELinux support");
  return TRUE;
}

static gboolean
handle_release_selinux_label (FlatpakSessionHelper *object,
                              GDBusMethodInvocation *invocation,
                              const char *arg_app_id)
{
  g_dbus_method_invocation_return_error (invocation,
                                         G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
                                         "Flatpak was built without SELinux support");
  return TRUE;
}

#endif


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

  g_signal_connect (helper, "handle-request-selinux-label", G_CALLBACK (handle_request_selinux_label), NULL);
  g_signal_connect (helper, "handle-release-selinux-label", G_CALLBACK (handle_release_selinux_label), NULL);

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
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

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

#ifdef ENABLE_SELINUX
  path = g_build_filename (g_get_user_data_dir (), "flatpak/db", TABLE_NAME, NULL);
  db = flatpak_db_new (path, FALSE, &error);
  if (db == NULL)
    {
      g_printerr ("Failed to load db: %s", error->message);
      exit (2);
    }
  load_existing_mcs_labels ();
#endif

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
