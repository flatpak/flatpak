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
#include <signal.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include "flatpak-dbus.h"
#include "flatpak-utils.h"

static char *monitor_dir;

static guint32 next_client_pid = 0;
static GHashTable *client_pid_data_hash = NULL;
static GDBusConnection *session_bus = NULL;

typedef struct {
  GPid pid;
  guint32 client_pid;
  char *client;
  guint child_watch;
} PidData;

static void
pid_data_free (PidData *data)
{
  g_free (data->client);
  g_free (data);
}

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
child_watch_died (GPid     pid,
                  gint     status,
                  gpointer user_data)
{
  PidData *pid_data = user_data;
  guint32 client_pid = pid_data->client_pid;
  g_autoptr(GVariant) signal_variant = NULL;

  g_debug ("Client Pid %d (%d) died", pid_data->client_pid, pid);

  signal_variant = g_variant_ref_sink (g_variant_new ("(uu)", client_pid, status));
  g_dbus_connection_emit_signal (session_bus,
                                 pid_data->client,
                                 "/org/freedesktop/Flatpak/Development",
                                 "org.freedesktop.Flatpak.Development",
                                 "HostCommandExited",
                                 signal_variant,
                                 NULL);

  /* This frees the pid_data, so be careful */
  g_hash_table_remove (client_pid_data_hash, GUINT_TO_POINTER(client_pid));
}

typedef struct {
  int from;
  int to;
  int final;
} FdMapEntry;

typedef struct {
  FdMapEntry *fd_map;
  int fd_map_len;
  gboolean set_tty;
  int tty;
} ChildSetupData;

static void
child_setup_func (gpointer user_data)
{
  ChildSetupData *data = (ChildSetupData *)user_data;
  FdMapEntry *fd_map = data->fd_map;
  sigset_t set;
  int i;

  /* Unblock all signals */
  sigemptyset (&set);
  if (pthread_sigmask (SIG_SETMASK, &set, NULL) == -1)
    {
      g_error ("Failed to unblock signals when starting child");
      return;
  }

  /* Reset the handlers for all signals to their defaults. */
  for (i = 1; i < NSIG; i++)
    {
      if (i != SIGSTOP && i != SIGKILL)
        signal (i, SIG_DFL);
    }

  for (i = 0; i < data->fd_map_len; i++)
    {
      if (fd_map[i].from != fd_map[i].to)
        {
          dup2 (fd_map[i].from, fd_map[i].to);
          close (fd_map[i].from);
        }
    }

  /* Second pass in case we needed an inbetween fd value to avoid conflicts */
  for (i = 0; i < data->fd_map_len; i++)
    {
      if (fd_map[i].to != fd_map[i].final)
        {
          dup2 (fd_map[i].to, fd_map[i].final);
          close (fd_map[i].to);
        }
    }

  /* We become our own session and process group, because it never makes sense
     to share the flatpak-session-helper dbus activated process group */
  setsid ();
  setpgid (0, 0);

  if (data->set_tty)
    ioctl (data->tty, TIOCSCTTY, 0);
}


static gboolean
handle_host_command (FlatpakDevelopment *object,
                     GDBusMethodInvocation *invocation,
                     const gchar *arg_cwd_path,
                     const gchar *const *arg_argv,
                     GVariant *arg_fds,
                     GVariant *arg_envs,
                     guint flags)
{
  g_autoptr(GError) error = NULL;
  GDBusMessage *message = g_dbus_method_invocation_get_message (invocation);
  GUnixFDList *fd_list = g_dbus_message_get_unix_fd_list (message);
  ChildSetupData child_setup_data = { NULL };
  GPid pid;
  PidData *pid_data;
  gsize i, j, n_fds, n_envs;
  const gint *fds;
  g_autofree FdMapEntry *fd_map = NULL;
  gchar **env;
  gint32 max_fd;

  if (*arg_cwd_path == 0)
    arg_cwd_path = NULL;

  if (*arg_argv[0] == 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "No command given");
      return TRUE;
    }


  g_debug ("Running host command %s", arg_argv[0]);

  n_fds = 0;
  fds = NULL;
  if (fd_list != NULL)
    {
      n_fds = g_variant_n_children (arg_fds);
      fds = g_unix_fd_list_peek_fds (fd_list, NULL);
    }
  fd_map = g_new0 (FdMapEntry, n_fds);

  child_setup_data.fd_map = fd_map;
  child_setup_data.fd_map_len = n_fds;

  max_fd = -1;
  for (i = 0; i < n_fds; i++)
    {
      gint32 handle, fd;
      g_variant_get_child (arg_fds, i, "{uh}", &fd, &handle);
      fd_map[i].to = fd;
      fd_map[i].from = fds[i];
      fd_map[i].final = fd_map[i].to;

      /* If stdin/out/err is a tty we try to set it as the controlling
         tty for the app, this way we can use this to run in a terminal. */
      if ((fd == 0 || fd == 1 || fd == 2) &&
          !child_setup_data.set_tty &&
          isatty (fds[i]))
        {
          child_setup_data.set_tty = TRUE;
          child_setup_data.tty = fds[i];
        }

      max_fd = MAX (max_fd, fd_map[i].to);
      max_fd = MAX (max_fd, fd_map[i].from);
    }

  /* We make a second pass over the fds to find if any "to" fd index
     overlaps an already in use fd (i.e. one in the "from" category
     that are allocated randomly). If a fd overlaps "to" fd then its
     a caller issue and not our fault, so we ignore that. */
  for (i = 0; i < n_fds; i++)
    {
      int to_fd = fd_map[i].to;
      gboolean conflict = FALSE;

      /* At this point we're fine with using "from" values for this
         value (because we handle to==from in the code), or values
         that are before "i" in the fd_map (because those will be
         closed at this point when dup:ing). However, we can't
         reuse a fd that is in "from" for j > i. */
      for (j = i + 1; j < n_fds; j++)
        {
          int from_fd = fd_map[j].from;
          if (from_fd == to_fd)
            {
              conflict = TRUE;
              break;
            }
        }

      if (conflict)
        fd_map[i].to = ++max_fd;
    }

  if (flags & FLATPAK_HOST_COMMAND_FLAGS_CLEAR_ENV)
    {
      char *empty[] = { NULL };
      env = g_strdupv (empty);
    }
  else
    env = g_get_environ ();

  n_envs = g_variant_n_children (arg_envs);
  for (i = 0; i < n_envs; i++)
    {
      const char *var = NULL;
      const char *val = NULL;
      g_variant_get_child (arg_envs, i, "{&s&s}", &var, &val);

      env = g_environ_setenv (env, var, val, TRUE);
    }

  if (!g_spawn_async_with_pipes (arg_cwd_path,
                                 (char **)arg_argv,
                                 env,
                                 G_SPAWN_SEARCH_PATH|G_SPAWN_DO_NOT_REAP_CHILD,
                                 child_setup_func, &child_setup_data,
                                 &pid,
                                 NULL,
                                 NULL,
                                 NULL,
                                 &error))
    {
      gint code = G_DBUS_ERROR_FAILED;
      if (g_error_matches (error, G_SPAWN_ERROR, G_SPAWN_ERROR_ACCES))
        code = G_DBUS_ERROR_ACCESS_DENIED;
      else if (g_error_matches (error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT))
        code = G_DBUS_ERROR_FILE_NOT_FOUND;
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, code,
                                             "Failed to start command: %s",
                                             error->message);
    }

  pid_data = g_new0 (PidData, 1);
  pid_data->pid = pid;
  pid_data->client_pid = ++next_client_pid;
  pid_data->client = g_strdup (g_dbus_method_invocation_get_sender (invocation));
  pid_data->child_watch = g_child_watch_add_full (G_PRIORITY_DEFAULT,
                                                  pid,
                                                  child_watch_died,
                                                  pid_data,
                                                  NULL);

  g_debug ("Client Pid is %d (%d)", pid_data->client_pid, pid);

  g_hash_table_replace (client_pid_data_hash, GUINT_TO_POINTER(pid_data->client_pid),
                        pid_data);


  flatpak_development_complete_host_command (object, invocation,
                                             pid_data->client_pid);
  return TRUE;
}

static gboolean
handle_host_command_signal (FlatpakDevelopment *object,
                            GDBusMethodInvocation *invocation,
                            guint arg_pid,
                            guint arg_signal,
                            gboolean to_process_group)
{
  PidData *pid_data = NULL;

  pid_data = g_hash_table_lookup (client_pid_data_hash, GUINT_TO_POINTER(arg_pid));
  if (pid_data == NULL ||
      strcmp (pid_data->client, g_dbus_method_invocation_get_sender (invocation)) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_UNIX_PROCESS_ID_UNKNOWN,
                                             "No such pid");
      return TRUE;
    }

  g_debug ("Sending signal %d to client pid %d", arg_signal, arg_pid);

  if (to_process_group)
    killpg (pid_data->pid, arg_signal);
  else
    kill (pid_data->pid, arg_signal);

  flatpak_development_complete_host_command_signal (object, invocation);

  return TRUE;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  FlatpakSessionHelper *helper;
  FlatpakDevelopment *devel;
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

  devel = flatpak_development_skeleton_new ();
  g_signal_connect (devel, "handle-host-command", G_CALLBACK (handle_host_command), NULL);
  g_signal_connect (devel, "handle-host-command-signal", G_CALLBACK (handle_host_command_signal), NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (devel),
                                         connection,
                                         "/org/freedesktop/Flatpak/Development",
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

static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    g_printerr ("XA: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

int
main (int    argc,
      char **argv)
{
  guint owner_id;
  GMainLoop *loop;
  gboolean replace;
  gboolean verbose;
  gboolean show_version;
  GOptionContext *context;
  GBusNameOwnerFlags flags;
  g_autoptr(GError) error = NULL;
  const GOptionEntry options[] = {
    { "replace", 'r', 0, G_OPTION_ARG_NONE, &replace,  "Replace old daemon.", NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,  "Enable debug output.", NULL },
    { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, "Show program version.", NULL},
    { NULL }
  };

  setlocale (LC_ALL, "");

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_prgname (argv[0]);

  g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, message_handler, NULL);

  context = g_option_context_new ("");

  g_option_context_set_summary (context, "Flatpak session helper");
  g_option_context_add_main_entries (context, options, GETTEXT_PACKAGE);

  replace = FALSE;
  verbose = FALSE;
  show_version = FALSE;

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("%s: %s", g_get_application_name(), error->message);
      g_printerr ("\n");
      g_printerr ("Try \"%s --help\" for more information.",
                  g_get_prgname ());
      g_printerr ("\n");
      g_option_context_free (context);
      return 1;
    }

  if (show_version)
    {
      g_print (PACKAGE_STRING "\n");
      return 0;
    }

  if (verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  flatpak_migrate_from_xdg_app ();

  client_pid_data_hash = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)pid_data_free);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_printerr ("Can't find bus: %s\n", error->message);
      return 1;
    }

  monitor_dir = g_build_filename (g_get_user_runtime_dir (), "flatpak-monitor", NULL);
  if (g_mkdir_with_parents (monitor_dir, 0755) != 0)
    {
      g_print ("Can't create %s\n", monitor_dir);
      exit (1);
    }

  setup_file_monitor ("/etc/resolv.conf");
  setup_file_monitor ("/etc/localtime");

  flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
  if (replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             "org.freedesktop.Flatpak",
                             flags,
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
