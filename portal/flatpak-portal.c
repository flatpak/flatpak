/*
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

#include <locale.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include "flatpak-portal-dbus.h"
#include "flatpak-portal.h"
#include "flatpak-portal-app-info.h"
#include "flatpak-portal-error.h"
#include "flatpak-utils-memfd-private.h"

/* Syntactic sugar added in newer GLib, which makes the error paths more
 * clearly correct */
#ifndef G_DBUS_METHOD_INVOCATION_HANDLED
# define G_DBUS_METHOD_INVOCATION_HANDLED TRUE
# define G_DBUS_METHOD_INVOCATION_UNHANDLED FALSE
#endif

#define IDLE_TIMEOUT_SECS 10 * 60

static GHashTable *client_pid_data_hash = NULL;
static GDBusConnection *session_bus = NULL;
static gboolean no_idle_exit = FALSE;
static guint name_owner_id = 0;
static GMainLoop *main_loop;
static PortalFlatpak *portal;
static gboolean opt_verbose;

static void
skeleton_died_cb (gpointer data)
{
  g_debug ("skeleton finalized, exiting");
  g_main_loop_quit (main_loop);
}

static gboolean
unref_skeleton_in_timeout_cb (gpointer user_data)
{
  static gboolean unreffed = FALSE;

  g_debug ("unreffing portal main ref");
  if (!unreffed)
    {
      g_object_unref (portal);
      unreffed = TRUE;
    }

  return G_SOURCE_REMOVE;
}

static void
unref_skeleton_in_timeout (void)
{
  if (name_owner_id)
    g_bus_unown_name (name_owner_id);
  name_owner_id = 0;

  /* After we've lost the name or idled we drop the main ref on the helper
     so that we'll exit when it drops to zero. However, if there are
     outstanding calls these will keep the refcount up during the
     execution of them. We do the unref on a timeout to make sure
     we're completely draining the queue of (stale) requests. */
  g_timeout_add (500, unref_skeleton_in_timeout_cb, NULL);
}

static guint idle_timeout_id = 0;

static gboolean
idle_timeout_cb (gpointer user_data)
{
  if (name_owner_id && g_hash_table_size (client_pid_data_hash) == 0)
    {
      g_debug ("Idle - unowning name");
      unref_skeleton_in_timeout ();
    }

  idle_timeout_id = 0;
  return G_SOURCE_REMOVE;
}

G_LOCK_DEFINE_STATIC (idle);
static void
schedule_idle_callback (void)
{
  G_LOCK (idle);

  if (!no_idle_exit)
    {
      if (idle_timeout_id != 0)
        g_source_remove (idle_timeout_id);

      idle_timeout_id = g_timeout_add_seconds (IDLE_TIMEOUT_SECS, idle_timeout_cb, NULL);
    }

  G_UNLOCK (idle);
}

typedef struct
{
  GPid  pid;
  char *client;
  guint child_watch;
  gboolean watch_bus;
} PidData;

static void
pid_data_free (PidData *data)
{
  g_free (data->client);
  g_free (data);
}

static void
child_watch_died (GPid     pid,
                  gint     status,
                  gpointer user_data)
{
  PidData *pid_data = user_data;

  g_autoptr(GVariant) signal_variant = NULL;

  g_debug ("Client Pid %d died", pid_data->pid);

  signal_variant = g_variant_ref_sink (g_variant_new ("(uu)", pid, status));
  g_dbus_connection_emit_signal (session_bus,
                                 pid_data->client,
                                 "/org/freedesktop/portal/Flatpak",
                                 "org.freedesktop.portal.Flatpak",
                                 "SpawnExited",
                                 signal_variant,
                                 NULL);

  /* This frees the pid_data, so be careful */
  g_hash_table_remove (client_pid_data_hash, GUINT_TO_POINTER (pid_data->pid));

  /* This might have caused us to go to idle (zero children) */
  schedule_idle_callback ();
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
  int         env_fd;
} ChildSetupData;

static void
drop_cloexec (int fd)
{
  fcntl (fd, F_SETFD, 0);
}

static void
child_setup_func (gpointer user_data)
{
  ChildSetupData *data = (ChildSetupData *) user_data;
  FdMapEntry *fd_map = data->fd_map;
  sigset_t set;
  int i;

  if (data->env_fd != -1)
    drop_cloexec (data->env_fd);

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
    {
      /* data->tty is our from fd which is closed at this point.
       * so locate the destination fd and use it for the ioctl.
       */
      for (i = 0; i < data->fd_map_len; i++)
        {
          if (fd_map[i].from == data->tty)
            {
              if (ioctl (fd_map[i].final, TIOCSCTTY, 0) == -1)
                g_debug ("ioctl(%d, TIOCSCTTY, 0) failed: %s",
                         fd_map[i].final, strerror (errno));
              break;
            }
        }
    }
}

static gboolean
is_valid_expose (const char *expose,
                 GError **error)
{
  /* No subdirs or absolute paths */
  if (expose[0] == '/')
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                   "Invalid sandbox expose: absolute paths not allowed");
      return FALSE;
    }
  else if (strchr (expose, '/'))
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                   "Invalid sandbox expose: subdirectories not allowed");
      return FALSE;
    }

  return TRUE;
}

static char *
filesystem_sandbox_arg (const char *path,
                        const char *sandbox,
                        gboolean    readonly)
{
  g_autoptr(GString) s = g_string_new ("--filesystem=");
  const char *p;

  for (p = path; *p != 0; p++)
    {
      if (*p == ':')
        g_string_append (s, "\\:");
      else
        g_string_append_c (s, *p);
    }

  g_string_append (s, "/sandbox/");

  for (p = sandbox; *p != 0; p++)
    {
      if (*p == ':')
        g_string_append (s, "\\:");
      else
        g_string_append_c (s, *p);
    }

  if (readonly)
    g_string_append (s, ":ro");

  return g_string_free (g_steal_pointer (&s), FALSE);
}

static gboolean
handle_spawn (PortalFlatpak         *object,
              GDBusMethodInvocation *invocation,
              GUnixFDList           *fd_list,
              const gchar           *arg_cwd_path,
              const gchar *const    *arg_argv,
              GVariant              *arg_fds,
              GVariant              *arg_envs,
              guint                  arg_flags,
              GVariant              *arg_options)
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
  GKeyFile *app_info;
  g_autoptr(GPtrArray) flatpak_argv = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *app_id = NULL;
  g_autofree char *branch = NULL;
  g_autofree char *arch = NULL;
  g_autofree char *app_commit = NULL;
  g_autofree char *runtime_ref = NULL;
  g_auto(GStrv) runtime_parts = NULL;
  g_autofree char *runtime_commit = NULL;
  g_autofree char *instance_path = NULL;
  g_auto(GStrv) extra_args = NULL;
  g_auto(GStrv) shares = NULL;
  g_auto(GStrv) sandbox_expose = NULL;
  g_auto(GStrv) sandbox_expose_ro = NULL;
  gboolean sandboxed;
  gboolean devel;
  g_autoptr(GString) env_string = g_string_new ("");

  child_setup_data.env_fd = -1;

  app_info = g_object_get_data (G_OBJECT (invocation), "app-info");
  g_assert (app_info != NULL);

  app_id = g_key_file_get_string (app_info,
                                  FLATPAK_METADATA_GROUP_APPLICATION,
                                  FLATPAK_METADATA_KEY_NAME, NULL);
  g_assert (app_id != NULL);

  g_debug ("spawn() called from app: '%s'", app_id);
  if (*app_id == 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "org.freedesktop.portal.Flatpak.Spawn only works in a flatpak");
      return TRUE;
    }


  if (*arg_cwd_path == 0)
    arg_cwd_path = NULL;

  if (arg_argv == NULL || *arg_argv == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "No command given");
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_SPAWN_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", arg_flags & ~FLATPAK_SPAWN_FLAGS_ALL);
      return TRUE;
    }

  runtime_ref = g_key_file_get_string (app_info,
                                       FLATPAK_METADATA_GROUP_APPLICATION,
                                       FLATPAK_METADATA_KEY_RUNTIME, NULL);
  if (runtime_ref == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "No runtime found");
      return TRUE;
    }

  runtime_parts = g_strsplit (runtime_ref, "/", -1);

  branch = g_key_file_get_string (app_info,
                                  FLATPAK_METADATA_GROUP_INSTANCE,
                                  FLATPAK_METADATA_KEY_BRANCH, NULL);
  instance_path = g_key_file_get_string (app_info,
                                         FLATPAK_METADATA_GROUP_INSTANCE,
                                         FLATPAK_METADATA_KEY_INSTANCE_PATH, NULL);
  arch = g_key_file_get_string (app_info,
                                FLATPAK_METADATA_GROUP_INSTANCE,
                                FLATPAK_METADATA_KEY_ARCH, NULL);
  extra_args = g_key_file_get_string_list (app_info,
                                           FLATPAK_METADATA_GROUP_INSTANCE,
                                           FLATPAK_METADATA_KEY_EXTRA_ARGS, NULL, NULL);
  app_commit = g_key_file_get_string (app_info,
                                      FLATPAK_METADATA_GROUP_INSTANCE,
                                      FLATPAK_METADATA_KEY_APP_COMMIT, NULL);
  runtime_commit = g_key_file_get_string (app_info,
                                          FLATPAK_METADATA_GROUP_INSTANCE,
                                          FLATPAK_METADATA_KEY_RUNTIME_COMMIT, NULL);
  shares = g_key_file_get_string_list (app_info, FLATPAK_METADATA_GROUP_CONTEXT,
                                       FLATPAK_METADATA_KEY_SHARED, NULL, NULL);

  devel = g_key_file_get_boolean (app_info, FLATPAK_METADATA_GROUP_INSTANCE,
                                  FLATPAK_METADATA_KEY_DEVEL, NULL);

  g_variant_lookup (arg_options, "sandbox-expose", "^as", &sandbox_expose);
  g_variant_lookup (arg_options, "sandbox-expose-ro", "^as", &sandbox_expose_ro);

  if (instance_path == NULL &&
      ((sandbox_expose != NULL && sandbox_expose[0] != NULL) ||
       (sandbox_expose_ro != NULL && sandbox_expose_ro[0] != NULL)))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid sandbox expose, caller has no instance path");
      return TRUE;
    }

  for (i = 0; sandbox_expose != NULL && sandbox_expose[i] != NULL; i++)
    {
      const char *expose = sandbox_expose[i];

      g_debug ("exposing %s", expose);
      if (!is_valid_expose (expose, &error))
        {
          g_dbus_method_invocation_return_gerror (invocation, error);
          return TRUE;
        }
    }

  for (i = 0; sandbox_expose_ro != NULL && sandbox_expose_ro[i] != NULL; i++)
    {
      const char *expose = sandbox_expose_ro[i];
      g_debug ("exposing %s", expose);
      if (!is_valid_expose (expose, &error))
        {
          g_dbus_method_invocation_return_gerror (invocation, error);
          return TRUE;
        }
    }

  g_debug ("Running spawn command %s", arg_argv[0]);

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

  if (arg_flags & FLATPAK_SPAWN_FLAGS_CLEAR_ENV)
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

  g_ptr_array_add (flatpak_argv, g_strdup ("flatpak"));
  g_ptr_array_add (flatpak_argv, g_strdup ("run"));

  sandboxed = (arg_flags & FLATPAK_SPAWN_FLAGS_SANDBOX) != 0;

  if (sandboxed)
    g_ptr_array_add (flatpak_argv, g_strdup ("--sandbox"));
  else
    {
      for (i = 0; extra_args != NULL && extra_args[i] != NULL; i++)
        {
          if (g_str_has_prefix (extra_args[i], "--env="))
            {
              const char *var_val = extra_args[i] + strlen ("--env=");

              if (var_val[0] == '\0' || var_val[0] == '=')
                {
                  g_warning ("Environment variable in extra-args has empty name");
                  continue;
                }

              if (strchr (var_val, '=') == NULL)
                {
                  g_warning ("Environment variable in extra-args has no value");
                  continue;
                }

              g_string_append (env_string, var_val);
              g_string_append_c (env_string, '\0');
            }
          else
            {
              g_ptr_array_add (flatpak_argv, g_strdup (extra_args[i]));
            }
        }
    }

  if (env_string->len > 0)
    {
      g_auto(GLnxTmpfile) env_tmpf  = { 0, };

      if (!flatpak_buffer_to_sealed_memfd_or_tmpfile (&env_tmpf, "environ",
                                                      env_string->str,
                                                      env_string->len, &error))
        {
          g_dbus_method_invocation_return_gerror (invocation, error);
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      child_setup_data.env_fd = glnx_steal_fd (&env_tmpf.fd);
      g_ptr_array_add (flatpak_argv,
                       g_strdup_printf ("--env-fd=%d",
                                        child_setup_data.env_fd));
    }

  if (devel)
    g_ptr_array_add (flatpak_argv, g_strdup ("--devel"));

  /* Inherit launcher network access from launcher, unless
     NO_NETWORK set. */
  if (shares != NULL && g_strv_contains ((const char * const *) shares, "network") &&
      !(arg_flags & FLATPAK_SPAWN_FLAGS_NO_NETWORK))
    g_ptr_array_add (flatpak_argv, g_strdup ("--share=network"));
  else
    g_ptr_array_add (flatpak_argv, g_strdup ("--unshare=network"));

  if (instance_path)
    {
      for (i = 0; sandbox_expose != NULL && sandbox_expose[i] != NULL; i++)
        g_ptr_array_add (flatpak_argv,
                         filesystem_sandbox_arg (instance_path, sandbox_expose[i], FALSE));
      for (i = 0; sandbox_expose_ro != NULL && sandbox_expose_ro[i] != NULL; i++)
        g_ptr_array_add (flatpak_argv,
                         filesystem_sandbox_arg (instance_path, sandbox_expose_ro[i], TRUE));
    }

  for (i = 0; sandbox_expose_ro != NULL && sandbox_expose_ro[i] != NULL; i++)
    {
      const char *expose = sandbox_expose_ro[i];
      g_debug ("exposing %s", expose);
    }

  g_ptr_array_add (flatpak_argv, g_strdup_printf ("--runtime=%s", runtime_parts[1]));
  g_ptr_array_add (flatpak_argv, g_strdup_printf ("--runtime-version=%s", runtime_parts[3]));

  if ((arg_flags & FLATPAK_SPAWN_FLAGS_LATEST_VERSION) == 0)
    {
      if (app_commit)
        g_ptr_array_add (flatpak_argv, g_strdup_printf ("--commit=%s", app_commit));
      if (runtime_commit)
        g_ptr_array_add (flatpak_argv, g_strdup_printf ("--runtime-commit=%s", runtime_commit));
    }

  if (arg_cwd_path != NULL)
    g_ptr_array_add (flatpak_argv, g_strdup_printf ("--cwd=%s", arg_cwd_path));

  if (arg_argv[0][0] != 0)
    g_ptr_array_add (flatpak_argv, g_strdup_printf ("--command=%s", arg_argv[0]));

  g_ptr_array_add (flatpak_argv, g_strdup_printf ("%s/%s/%s", app_id, arch ? arch : "", branch ? branch : ""));
  for (i = 1; arg_argv[i] != NULL; i++)
    g_ptr_array_add (flatpak_argv, g_strdup (arg_argv[i]));
  g_ptr_array_add (flatpak_argv, NULL);

  if (opt_verbose)
    {
      g_autoptr(GString) cmd = g_string_new ("");
      int i;

      for (i = 0; flatpak_argv->pdata[i] != NULL; i++)
        {
          if (i > 0)
            g_string_append (cmd, " ");
          g_string_append (cmd, flatpak_argv->pdata[i]);
        }

      g_debug ("Starting: %s\n", cmd->str);
    }

  if (!g_spawn_async_with_pipes (NULL,
                                 (char **) flatpak_argv->pdata,
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
      return TRUE;
    }

  pid_data = g_new0 (PidData, 1);
  pid_data->pid = pid;
  pid_data->client = g_strdup (g_dbus_method_invocation_get_sender (invocation));
  pid_data->watch_bus = (arg_flags & FLATPAK_SPAWN_FLAGS_WATCH_BUS) != 0;
  pid_data->child_watch = g_child_watch_add_full (G_PRIORITY_DEFAULT,
                                                  pid,
                                                  child_watch_died,
                                                  pid_data,
                                                  NULL);

  g_debug ("Client Pid is %d", pid_data->pid);

  g_hash_table_replace (client_pid_data_hash, GUINT_TO_POINTER (pid_data->pid),
                        pid_data);

  portal_flatpak_complete_spawn (object, invocation, NULL, pid);
  return TRUE;
}

static gboolean
handle_spawn_signal (PortalFlatpak         *object,
                     GDBusMethodInvocation *invocation,
                     guint                  arg_pid,
                     guint                  arg_signal,
                     gboolean               arg_to_process_group)
{
  PidData *pid_data = NULL;

  g_debug ("spawn_signal(%d %d)", arg_pid, arg_signal);

  pid_data = g_hash_table_lookup (client_pid_data_hash, GUINT_TO_POINTER (arg_pid));
  if (pid_data == NULL ||
      strcmp (pid_data->client, g_dbus_method_invocation_get_sender (invocation)) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_UNIX_PROCESS_ID_UNKNOWN,
                                             "No such pid");
      return TRUE;
    }

  g_debug ("Sending signal %d to client pid %d", arg_signal, arg_pid);

  if (arg_to_process_group)
    killpg (pid_data->pid, arg_signal);
  else
    kill (pid_data->pid, arg_signal);

  portal_flatpak_complete_spawn_signal (portal, invocation);

  return TRUE;
}

static gboolean
authorize_method_handler (GDBusInterfaceSkeleton *interface,
                          GDBusMethodInvocation  *invocation,
                          gpointer                user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree char *app_id = NULL;

  /* Ensure we don't idle exit */
  schedule_idle_callback ();

  keyfile = flatpak_invocation_lookup_app_info (invocation, NULL, &error);
  if (keyfile == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Authorization error: %s", error->message);
      return FALSE;
    }

  app_id = g_key_file_get_string (keyfile,
                                  FLATPAK_METADATA_GROUP_APPLICATION,
                                  FLATPAK_METADATA_KEY_NAME, &error);
  if (app_id == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Authorization error: %s", error->message);
      return FALSE;
    }

  g_object_set_data_full (G_OBJECT (invocation), "app-info", g_steal_pointer (&keyfile), (GDestroyNotify) g_key_file_unref);

  return TRUE;
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
          g_debug ("%s dropped off the bus, killing %d", pid_data->client, pid_data->pid);
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
  GError *error = NULL;

  g_debug ("Bus acquired, creating skeleton");

  g_dbus_connection_set_exit_on_close (connection, FALSE);

  portal = portal_flatpak_skeleton_new ();

  g_dbus_connection_signal_subscribe (connection,
                                      DBUS_NAME_DBUS,
                                      DBUS_INTERFACE_DBUS,
                                      "NameOwnerChanged",
                                      DBUS_PATH_DBUS,
                                      NULL,
                                      G_DBUS_SIGNAL_FLAGS_NONE,
                                      name_owner_changed,
                                      NULL, NULL);

  g_object_set_data_full (G_OBJECT (portal), "track-alive", GINT_TO_POINTER (42), skeleton_died_cb);

  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (portal),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);

  portal_flatpak_set_version (PORTAL_FLATPAK (portal), 1);
  g_signal_connect (portal, "handle-spawn", G_CALLBACK (handle_spawn), NULL);
  g_signal_connect (portal, "handle-spawn-signal", G_CALLBACK (handle_spawn_signal), NULL);

  g_signal_connect (portal, "g-authorize-method", G_CALLBACK (authorize_method_handler), NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (portal),
                                         connection,
                                         "/org/freedesktop/portal/Flatpak",
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
  g_debug ("Name acquired");
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  g_debug ("Name lost");
  unref_skeleton_in_timeout ();
}

static void
binary_file_changed_cb (GFileMonitor     *file_monitor,
                        GFile            *file,
                        GFile            *other_file,
                        GFileMonitorEvent event_type,
                        gpointer          data)
{
  static gboolean got_it = FALSE;

  if (!got_it)
    {
      g_debug ("binary file changed");
      unref_skeleton_in_timeout ();
    }

  got_it = TRUE;
}


static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    g_printerr ("F: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

int
main (int    argc,
      char **argv)
{
  gchar exe_path[PATH_MAX + 1];
  ssize_t exe_path_len;
  gboolean replace;
  gboolean show_version;
  GOptionContext *context;
  GBusNameOwnerFlags flags;

  g_autoptr(GError) error = NULL;
  const GOptionEntry options[] = {
    { "replace", 'r', 0, G_OPTION_ARG_NONE, &replace,  "Replace old daemon.", NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose,  "Enable debug output.", NULL },
    { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, "Show program version.", NULL},
    { "no-idle-exit", 0, 0, G_OPTION_ARG_NONE, &no_idle_exit,  "Don't exit when idle.", NULL },
    { NULL }
  };

  setlocale (LC_ALL, "");

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_prgname (argv[0]);

  g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, message_handler, NULL);

  context = g_option_context_new ("");

  replace = FALSE;
  opt_verbose = FALSE;
  show_version = FALSE;

  g_option_context_set_summary (context, "Flatpak portal");
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

  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  client_pid_data_hash = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify) pid_data_free);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_printerr ("Can't find bus: %s\n", error->message);
      return 1;
    }

  exe_path_len = readlink ("/proc/self/exe", exe_path, sizeof (exe_path) - 1);
  if (exe_path_len > 0 && (size_t) exe_path_len < sizeof (exe_path))
    {
      exe_path[exe_path_len] = 0;
      GFileMonitor *monitor;
      g_autoptr(GFile) exe = NULL;
      g_autoptr(GError) local_error = NULL;

      exe = g_file_new_for_path (exe_path);
      monitor =  g_file_monitor_file (exe,
                                      G_FILE_MONITOR_NONE,
                                      NULL,
                                      &local_error);
      if (monitor == NULL)
        g_warning ("Failed to set watch on %s: %s", exe_path, error->message);
      else
        g_signal_connect (monitor, "changed",
                          G_CALLBACK (binary_file_changed_cb), NULL);
    }

  flatpak_connection_track_name_owners (session_bus);

  flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
  if (replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  name_owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                  "org.freedesktop.portal.Flatpak",
                                  flags,
                                  on_bus_acquired,
                                  on_name_acquired,
                                  on_name_lost,
                                  NULL,
                                  NULL);

  /* Ensure we don't idle exit */
  schedule_idle_callback ();

  main_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (main_loop);

  return 0;
}
