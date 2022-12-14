/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2014-2019 Red Hat, Inc
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

#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include "flatpak-dbus-generated.h"
#include "flatpak-session-helper.h"
#include "flatpak-utils-base-private.h"

static char *monitor_dir;
static char *p11_kit_server_socket_path;
static int p11_kit_server_pid = 0;

static GHashTable *client_pid_data_hash = NULL;
static GDBusConnection *session_bus = NULL;

static void
do_atexit (void)
{
  if (p11_kit_server_pid != 0)
    kill (p11_kit_server_pid, SIGTERM);
}

static void
handle_sigterm (int signum)
{
  struct sigaction action = { 0 };
  do_atexit ();
  action.sa_handler = SIG_DFL;
  sigaction (signum, &action, NULL);
  raise (signum);
}

typedef struct
{
  GPid     pid;
  char    *client;
  guint    child_watch;
  gboolean watch_bus;
} PidData;

static void
pid_data_free (PidData *data)
{
  g_free (data->client);
  g_free (data);
}

static gboolean
handle_request_session (FlatpakSessionHelper  *object,
                        GDBusMethodInvocation *invocation,
                        gpointer               user_data)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

  g_variant_builder_add (&builder, "{s@v}", "path",
                         g_variant_new_variant (g_variant_new_string (monitor_dir)));
  if (p11_kit_server_socket_path)
    g_variant_builder_add (&builder, "{s@v}", "pkcs11-socket",
                           g_variant_new_variant (g_variant_new_string (p11_kit_server_socket_path)));

  flatpak_session_helper_complete_request_session (object, invocation,
                                                   g_variant_builder_end (&builder));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}


static void
child_watch_died (GPid     pid,
                  gint     status,
                  gpointer user_data)
{
  PidData *pid_data = user_data;
  g_autoptr(GVariant) signal_variant = NULL;

  g_info ("Client Pid %d died", pid_data->pid);

  signal_variant = g_variant_ref_sink (g_variant_new ("(uu)", pid, status));
  g_dbus_connection_emit_signal (session_bus,
                                 pid_data->client,
                                 FLATPAK_SESSION_HELPER_PATH_DEVELOPMENT,
                                 FLATPAK_SESSION_HELPER_INTERFACE_DEVELOPMENT,
                                 "HostCommandExited",
                                 signal_variant,
                                 NULL);

  /* This frees the pid_data, so be careful */
  g_hash_table_remove (client_pid_data_hash, GUINT_TO_POINTER (pid_data->pid));
}

typedef struct
{
  int from;
  int to;
  int final;
} FdMapEntry;

typedef struct
{
  FdMapEntry *fd_map;
  int         fd_map_len;
  gboolean    set_tty;
  int         tty;
} ChildSetupData;

static void
child_setup_func (gpointer user_data)
{
  ChildSetupData *data = (ChildSetupData *) user_data;
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

  /* Second pass in case we needed an in-between fd value to avoid conflicts */
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
    {
      /* data->tty is our from fd which is closed at this point.
       * so locate the destination fd and use it for the ioctl.
       */
      for (i = 0; i < data->fd_map_len; i++)
        {
          if (fd_map[i].from == data->tty)
            {
              if (ioctl (fd_map[i].final, TIOCSCTTY, 0) == -1)
                g_info ("ioctl(%d, TIOCSCTTY, 0) failed: %s",
                        fd_map[i].final, strerror (errno));
              break;
            }
        }
    }
}


static gboolean
handle_host_command (FlatpakDevelopment    *object,
                     GDBusMethodInvocation *invocation,
                     GUnixFDList           *fd_list,
                     const gchar           *arg_cwd_path,
                     const gchar *const    *arg_argv,
                     GVariant              *arg_fds,
                     GVariant              *arg_envs,
                     guint                  flags)
{
  g_autoptr(GError) error = NULL;
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

  if (arg_argv == NULL || *arg_argv == NULL || *arg_argv[0] == 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "No command given");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!g_variant_is_of_type (arg_fds, G_VARIANT_TYPE ("a{uh}")) ||
      !g_variant_is_of_type (arg_envs, G_VARIANT_TYPE ("a{ss}")) ||
      (flags & ~(FLATPAK_HOST_COMMAND_FLAGS_CLEAR_ENV |
                 FLATPAK_HOST_COMMAND_FLAGS_WATCH_BUS)) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Unexpected argument");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_info ("Running host command %s", arg_argv[0]);

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
                                 (char **) arg_argv,
                                 env,
                                 G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
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
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  pid_data = g_new0 (PidData, 1);
  pid_data->pid = pid;
  pid_data->client = g_strdup (g_dbus_method_invocation_get_sender (invocation));
  pid_data->watch_bus = (flags & FLATPAK_HOST_COMMAND_FLAGS_WATCH_BUS) != 0;
  pid_data->child_watch = g_child_watch_add_full (G_PRIORITY_DEFAULT,
                                                  pid,
                                                  child_watch_died,
                                                  pid_data,
                                                  NULL);

  g_info ("Client Pid is %d", pid_data->pid);

  g_hash_table_replace (client_pid_data_hash, GUINT_TO_POINTER (pid_data->pid),
                        pid_data);


  flatpak_development_complete_host_command (object, invocation, NULL,
                                             pid_data->pid);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_host_command_signal (FlatpakDevelopment    *object,
                            GDBusMethodInvocation *invocation,
                            guint                  arg_pid,
                            guint                  arg_signal,
                            gboolean               to_process_group)
{
  PidData *pid_data = NULL;

  pid_data = g_hash_table_lookup (client_pid_data_hash, GUINT_TO_POINTER (arg_pid));
  if (pid_data == NULL ||
      strcmp (pid_data->client, g_dbus_method_invocation_get_sender (invocation)) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_UNIX_PROCESS_ID_UNKNOWN,
                                             "No such pid");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_info ("Sending signal %d to client pid %d", arg_signal, arg_pid);

  if (to_process_group)
    killpg (pid_data->pid, arg_signal);
  else
    kill (pid_data->pid, arg_signal);

  flatpak_development_complete_host_command_signal (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
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

  g_variant_get (parameters, "(&s&s&s)", &name, &from, &to);

  if (name[0] == ':' &&
      strcmp (name, from) == 0 &&
      strcmp (to, "") == 0)
    {
      GHashTableIter iter;
      PidData *pid_data = NULL;
      gpointer value = NULL;
      GList *list = NULL, *l;

      g_hash_table_iter_init (&iter, client_pid_data_hash);
      while (g_hash_table_iter_next (&iter, NULL, &value))
        {
          pid_data = value;

          if (pid_data->watch_bus && g_str_equal (pid_data->client, name))
            list = g_list_prepend (list, pid_data);
        }

      for (l = list; l; l = l->next)
        {
          pid_data = l->data;
          killpg (pid_data->pid, SIGINT);
        }

      g_list_free (list);
    }
}

#define DBUS_NAME_DBUS "org.freedesktop.DBus"
#define DBUS_INTERFACE_DBUS DBUS_NAME_DBUS
#define DBUS_PATH_DBUS "/org/freedesktop/DBus"

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  FlatpakSessionHelper *helper;
  FlatpakDevelopment *devel;
  GError *error = NULL;

  g_dbus_connection_signal_subscribe (connection,
                                      DBUS_NAME_DBUS,
                                      DBUS_INTERFACE_DBUS,
                                      "NameOwnerChanged",
                                      DBUS_PATH_DBUS,
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      name_owner_changed,
                                      NULL, NULL);

  helper = flatpak_session_helper_skeleton_new ();

  flatpak_session_helper_set_version (FLATPAK_SESSION_HELPER (helper), 1);

  g_signal_connect (helper, "handle-request-session", G_CALLBACK (handle_request_session), NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (helper),
                                         connection,
                                         FLATPAK_SESSION_HELPER_PATH,
                                         &error))
    {
      g_warning ("error: %s", error->message);
      g_error_free (error);
    }

  devel = flatpak_development_skeleton_new ();
  flatpak_development_set_version (FLATPAK_DEVELOPMENT (devel), 1);
  g_signal_connect (devel, "handle-host-command", G_CALLBACK (handle_host_command), NULL);
  g_signal_connect (devel, "handle-host-command-signal", G_CALLBACK (handle_host_command_signal), NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (devel),
                                         connection,
                                         FLATPAK_SESSION_HELPER_PATH_DEVELOPMENT,
                                         &error))
    {
      g_warning ("error: %s", error->message);
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

/*
 * In the case that the monitored file is a symlink, we set up a separate
 * GFileMonitor for the real target of the link so that we don't miss updates
 * to the linked file contents. This is critical in the case of resolv.conf
 * which on stateless systems is often a symlink to a dyamically-generated
 * or updated file in /run.
 */
typedef struct
{
  const gchar  *source;
  char         *real;
  GFileMonitor *monitor_source;
  GFileMonitor *monitor_real;
} MonitorData;

static void
monitor_data_free (MonitorData *data)
{
  free (data->real);
  g_signal_handlers_disconnect_by_data (data->monitor_source, data);
  g_object_unref (data->monitor_source);
  if (data->monitor_real)
    {
      g_signal_handlers_disconnect_by_data (data->monitor_real, data);
      g_object_unref (data->monitor_real);
    }
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MonitorData, monitor_data_free)

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

static void file_changed (GFileMonitor     *monitor,
                          GFile            *file,
                          GFile            *other_file,
                          GFileMonitorEvent event_type,
                          MonitorData      *data);

static void
update_real_monitor (MonitorData *data)
{
  char *real = realpath (data->source, NULL);

  if (real == NULL)
    {
      g_info ("unable to get real path to monitor host file %s: %s", data->source,
              g_strerror (errno));
      return;
    }

  /* source path matches real path, second monitor is not required, but an old
   * one may still exist. set to NULL and compare to what we have. */
  if (!g_strcmp0 (data->source, real))
    {
      free (real);
      real = NULL;
    }

  /* no more work needed if the monitor we have matches the additional monitor
     we need (including NULL == NULL) */
  if (!g_strcmp0 (data->real, real))
    {
      free (real);
      return;
    }

  /* otherwise we're not monitoring the right thing and need to remove
     any old monitor and make a new one if needed */
  free (data->real);
  data->real = real;

  if (data->monitor_real)
    {
      g_signal_handlers_disconnect_by_data (data->monitor_real, data);
      g_clear_object (&(data->monitor_real));
    }

  if (!real)
    return;

  g_autoptr(GFile) r = g_file_new_for_path (real);
  g_autoptr(GError) err = NULL;
  data->monitor_real = g_file_monitor_file (r, G_FILE_MONITOR_NONE, NULL, &err);
  if (!data->monitor_real)
    {
      g_info ("failed to monitor host file %s (real path of %s): %s",
               real, data->source, err->message);
      return;
    }

  g_signal_connect (data->monitor_real, "changed", G_CALLBACK (file_changed), data);
}

static void
file_monitor_do (MonitorData *data)
{
  update_real_monitor (data);
  copy_file (data->source, monitor_dir);

  if (strcmp (data->source, "/etc/localtime") == 0)
    {
      /* We can't update the /etc/localtime symlink at runtime, nor can we make it a of the
       * correct form "../usr/share/zoneinfo/$timezone". So, instead we use the old debian
       * /etc/timezone file for telling the sandbox the timezone. */
      char *dest = g_build_filename (monitor_dir, "timezone", NULL);
      g_autofree char *raw_timezone = flatpak_get_timezone ();
      g_autofree char *timezone_content = g_strdup_printf ("%s\n", raw_timezone);

      g_file_set_contents (dest, timezone_content, -1, NULL);
    }
}

static void
file_changed (GFileMonitor     *monitor,
              GFile            *file,
              GFile            *other_file,
              GFileMonitorEvent event_type,
              MonitorData      *data)
{
  if (event_type != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
    return;

  file_monitor_do (data);
}

static MonitorData *
setup_file_monitor (const char *source)
{
  g_autoptr(GFile) s = g_file_new_for_path (source);
  g_autoptr(GError) err = NULL;
  GFileMonitor *monitor = NULL;
  MonitorData *data = NULL;

  data = g_new0 (MonitorData, 1);
  data->source = source;

  monitor = g_file_monitor_file (s, G_FILE_MONITOR_NONE, NULL, &err);
  if (monitor)
    {
      data->monitor_source = monitor;
      g_signal_connect (monitor, "changed", G_CALLBACK (file_changed), data);
    }
  else
    {
      g_info ("failed to monitor host file %s: %s", source, err->message);
    }

  file_monitor_do (data);

  return data;
}

static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
{
  /* Make this look like normal console output */
  if (log_level & (G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO))
    g_printerr ("F: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

static void
start_p11_kit_server (const char *flatpak_dir)
{
  g_autofree char *socket_basename = g_strdup_printf ("pkcs11-flatpak-%d", getpid ());
  g_autofree char *socket_path = g_build_filename (flatpak_dir, socket_basename, NULL);
  g_autofree char *p11_kit_stdout = NULL;
  gint exit_status;
  g_autoptr(GError) local_error = NULL;
  g_auto(GStrv) stdout_lines = NULL;
  int i;
  char *p11_argv[] = {
    "p11-kit", "server",
    /* We explicitly request --sh here, because we then fail on earlier versions that doesn't support
     * this flag. This is good, because those earlier versions did not properly daemonize and caused
     * the spawn_sync to hang forever, waiting for the pipe to close.
     */
    "--sh",
    "-n", socket_path,
    "--provider",  "p11-kit-trust.so",
    "pkcs11:model=p11-kit-trust?write-protected=yes",
    NULL
  };

  g_info ("starting p11-kit server");

  if (!g_spawn_sync (NULL,
                     p11_argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL,
                     &p11_kit_stdout, NULL,
                     &exit_status, &local_error))
    {
      g_warning ("Unable to start p11-kit server: %s", local_error->message);
      return;
    }

  if (!g_spawn_check_exit_status (exit_status, &local_error))
    {
      g_warning ("Unable to start p11-kit server: %s", local_error->message);
      return;
    }

  stdout_lines = g_strsplit (p11_kit_stdout, "\n", 0);
  /* Output is something like:
     P11_KIT_SERVER_ADDRESS=unix:path=/run/user/1000/p11-kit/pkcs11-2603742; export P11_KIT_SERVER_ADDRESS;
     P11_KIT_SERVER_PID=2603743; export P11_KIT_SERVER_PID;
   */
  for (i = 0; stdout_lines[i] != NULL; i++)
    {
      char *line = stdout_lines[i];

      if (g_str_has_prefix (line, "P11_KIT_SERVER_PID="))
        {
          char *pid = line + strlen ("P11_KIT_SERVER_PID=");
          char *p = pid;
          while (g_ascii_isdigit (*p))
            p++;

          *p = 0;
          p11_kit_server_pid = atol (pid);
        }
    }

  if (p11_kit_server_pid != 0)
    {
      g_info ("Using p11-kit socket path %s, pid %d", socket_path, p11_kit_server_pid);
      p11_kit_server_socket_path = g_steal_pointer (&socket_path);
    }
  else
    g_info ("Not using p11-kit due to older version");
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
  g_autofree char *flatpak_dir = NULL;
  g_autoptr(GError) error = NULL;
  const GOptionEntry options[] = {
    { "replace", 'r', 0, G_OPTION_ARG_NONE, &replace,  "Replace old daemon.", NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,  "Enable debug output.", NULL },
    { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, "Show program version.", NULL},
    { NULL }
  };
  g_autoptr(MonitorData) m_resolv_conf = NULL,
                         m_host_conf = NULL,
                         m_hosts = NULL,
                         m_gai_conf = NULL,
                         m_localtime = NULL;
  struct sigaction action;

  atexit (do_atexit);

  memset (&action, 0, sizeof (struct sigaction));
  action.sa_handler = handle_sigterm;
  sigaction (SIGTERM, &action, NULL);
  sigaction (SIGHUP, &action, NULL);
  sigaction (SIGINT, &action, NULL);

  setlocale (LC_ALL, "");

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_prgname (argv[0]);

  g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, message_handler, NULL);

  context = g_option_context_new ("");

  replace = FALSE;
  verbose = FALSE;
  show_version = FALSE;

  g_option_context_set_summary (context, "Flatpak session helper");
  g_option_context_add_main_entries (context, options, GETTEXT_PACKAGE);

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("%s: %s", g_get_application_name (), error->message);
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
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO, message_handler, NULL);

  client_pid_data_hash = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify) pid_data_free);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_printerr ("Can't find bus: %s\n", error->message);
      return 1;
    }

  flatpak_dir = g_build_filename (g_get_user_runtime_dir (), ".flatpak-helper", NULL);
  if (g_mkdir_with_parents (flatpak_dir, 0700) != 0)
    {
      g_print ("Can't create %s\n", monitor_dir);
      exit (1);
    }

  if (g_find_program_in_path ("p11-kit"))
    start_p11_kit_server (flatpak_dir);
  else
    g_info ("p11-kit not found");

  monitor_dir = g_build_filename (flatpak_dir, "monitor", NULL);
  if (g_mkdir_with_parents (monitor_dir, 0755) != 0)
    {
      g_print ("Can't create %s\n", monitor_dir);
      exit (1);
    }

  m_resolv_conf = setup_file_monitor ("/etc/resolv.conf");
  m_host_conf   = setup_file_monitor ("/etc/host.conf");
  m_hosts       = setup_file_monitor ("/etc/hosts");
  m_gai_conf    = setup_file_monitor ("/etc/gai.conf");
  m_localtime   = setup_file_monitor ("/etc/localtime");

  flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
  if (replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             FLATPAK_SESSION_HELPER_BUS_NAME,
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
