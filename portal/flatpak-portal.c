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

/* NOTE: This code was copied mostly as-is from xdg-desktop-portal */

#include <locale.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <glib/gi18n-lib.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <gio/gdesktopappinfo.h>
#include "flatpak-portal-dbus.h"
#include "flatpak-portal.h"
#include "flatpak-dir-private.h"
#include "flatpak-utils-private.h"
#include "flatpak-transaction.h"
#include "flatpak-installation-private.h"
#include "flatpak-instance-private.h"
#include "flatpak-portal-app-info.h"
#include "flatpak-portal-error.h"
#include "flatpak-utils-base-private.h"
#include "portal-impl.h"
#include "flatpak-permission-dbus.h"

/* GLib 2.47.92 was the first release to define these in gdbus-codegen */
#if !GLIB_CHECK_VERSION (2, 47, 92)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PortalFlatpakProxy, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PortalFlatpakSkeleton, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PortalFlatpakUpdateMonitorProxy, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PortalFlatpakUpdateMonitorSkeleton, g_object_unref)
#endif

#define IDLE_TIMEOUT_SECS 10 * 60

/* Should be roughly 2 seconds */
#define CHILD_STATUS_CHECK_ATTEMPTS 20

static GHashTable *client_pid_data_hash = NULL;
static GDBusConnection *session_bus = NULL;
static GNetworkMonitor *network_monitor = NULL;
static gboolean no_idle_exit = FALSE;
static guint name_owner_id = 0;
static GMainLoop *main_loop;
static PortalFlatpak *portal;
static gboolean opt_verbose;
static int opt_poll_timeout;
static gboolean opt_poll_when_metered;
static FlatpakSpawnSupportFlags supports = 0;

G_LOCK_DEFINE (update_monitors); /* This protects the three variables below */
static GHashTable *update_monitors;
static guint update_monitors_timeout = 0;
static gboolean update_monitors_timeout_running_thread = FALSE;

/* Poll all update monitors twice an hour */
#define DEFAULT_UPDATE_POLL_TIMEOUT_SEC (30 * 60)

#define PERMISSION_TABLE "flatpak"
#define PERMISSION_ID "updates"

/* Instance IDs are 32-bit unsigned integers */
#define INSTANCE_ID_BUFFER_SIZE 16

typedef enum { UNSET, ASK, YES, NO } Permission;
typedef enum {
  PROGRESS_STATUS_RUNNING = 0,
  PROGRESS_STATUS_EMPTY   = 1,
  PROGRESS_STATUS_DONE    = 2,
  PROGRESS_STATUS_ERROR   = 3
} UpdateStatus;
static XdpDbusPermissionStore *permission_store;

typedef struct {
  GMutex lock; /* This protects the closed, running and installed state */
  gboolean closed;
  gboolean running; /* While this is set, don't close the monitor */
  gboolean installing;

  char *sender;
  char *obj_path;
  GCancellable *cancellable;

  /* Static data */
  char *name;
  char *arch;
  char *branch;
  char *commit;
  char *app_path;

  /* Last reported values, starting at the instance commit */
  char *reported_local_commit;
  char *reported_remote_commit;
} UpdateMonitorData;

static gboolean           check_all_for_updates_cb (void                       *data);
static gboolean           has_update_monitors      (void);
static UpdateMonitorData *update_monitor_get_data  (PortalFlatpakUpdateMonitor *monitor);
static gboolean           handle_close             (PortalFlatpakUpdateMonitor *monitor,
                                                    GDBusMethodInvocation      *invocation);
static gboolean           handle_update            (PortalFlatpakUpdateMonitor *monitor,
                                                    GDBusMethodInvocation      *invocation,
                                                    const char                 *arg_window,
                                                    GVariant                   *arg_options);

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
  if (name_owner_id &&
      g_hash_table_size (client_pid_data_hash) == 0 &&
      !has_update_monitors ())
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
  GPid     pid;
  char    *client;
  guint    child_watch;
  gboolean watch_bus;
  gboolean expose_or_share_pids;
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
                                 FLATPAK_PORTAL_PATH,
                                 FLATPAK_PORTAL_INTERFACE,
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
  gsize       fd_map_len;
  int         instance_id_fd;
  gboolean    set_tty;
  int         tty;
  int         env_fd;
} ChildSetupData;

typedef struct
{
  guint pid;
  gchar buffer[INSTANCE_ID_BUFFER_SIZE];
} InstanceIdReadData;

typedef struct
{
  FlatpakInstance *instance;
  guint            pid;
  guint            attempt;
} BwrapinfoWatcherData;

static void
bwrapinfo_watcher_data_free (BwrapinfoWatcherData* data)
{
  g_object_unref (data->instance);
  g_free (data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BwrapinfoWatcherData, bwrapinfo_watcher_data_free)

static int
get_child_pid_relative_to_parent_sandbox (int      pid,
                                          GError **error)
{
  g_autofree char *status_file_path = NULL;
  g_autoptr(GFile) status_file = NULL;
  g_autoptr(GFileInputStream) input_stream = NULL;
  g_autoptr(GDataInputStream) data_stream = NULL;
  int relative_pid = 0;

  status_file_path = g_strdup_printf ("/proc/%u/status", pid);
  status_file = g_file_new_for_path (status_file_path);

  input_stream = g_file_read (status_file, NULL, error);
  if (input_stream == NULL)
    return 0;

  data_stream = g_data_input_stream_new (G_INPUT_STREAM (input_stream));

  while (TRUE)
    {
      g_autofree char *line = g_data_input_stream_read_line_utf8 (data_stream, NULL, NULL, error);
      if (line == NULL)
        break;

      g_strchug (line);

      if (g_str_has_prefix (line, "NSpid:"))
        {
          g_auto(GStrv) fields = NULL;
          guint nfields = 0;
          char *endptr = NULL;

          fields = g_strsplit (line, "\t", -1);
          nfields = g_strv_length (fields);
          if (nfields < 3)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "NSpid line has too few fields: %s", line);
              return 0;
            }

          /* The second to last PID namespace is the one that spawned this process */
          relative_pid = strtol (fields[nfields - 2], &endptr, 10);
          if (*endptr)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Invalid parent-relative PID in NSpid line: %s", line);
              return 0;
            }

          return relative_pid;
        }
    }

  if (*error == NULL)
    /* EOF was reached while reading the file */
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "NSpid not found");

  return 0;
}

static int
check_child_pid_status (void *user_data)
{
  /* Stores a sequence of the time interval to use until the child PID is checked again.
     In general from testing, bwrapinfo is never ready before 25ms have passed at minimum,
     thus 25ms is the first interval, doubling until a max interval of 100ms is reached.

     In addition, if the program is not available after 100ms for an extended period of time,
     the timeout is further increased to a full second. */
  static gint timeouts[] = {25, 50, 100};

  g_autoptr(GVariant) signal_variant = NULL;
  g_autoptr(BwrapinfoWatcherData) data = user_data;
  PidData *pid_data;
  guint pid;
  int child_pid;
  int relative_child_pid = 0;

  pid = data->pid;

  pid_data = g_hash_table_lookup (client_pid_data_hash, GUINT_TO_POINTER (pid));

  /* Process likely already exited if pid_data == NULL, so don't send the
     signal to avoid an awkward out-of-order SpawnExited -> SpawnStarted. */
  if (pid_data == NULL)
    {
      g_warning ("%u already exited, skipping SpawnStarted", pid);
      return G_SOURCE_REMOVE;
    }

  child_pid = flatpak_instance_get_child_pid (data->instance);
  if (child_pid == 0)
    {
      gint timeout;
      gboolean readd_timer = FALSE;

      if (data->attempt >= CHILD_STATUS_CHECK_ATTEMPTS)
        /* If too many attempts, use a 1 second timeout */
        timeout = 1000;
      else
        timeout = timeouts[MIN (data->attempt, G_N_ELEMENTS (timeouts) - 1)];

      g_debug ("Failed to read child PID, trying again in %d ms", timeout);

      /* The timer source only needs to be re-added if the timeout has changed,
          which won't happen while staying on the 100 or 1000ms timeouts.

          This test must happen *before* the attempt counter is incremented, since the
          attempt counter represents the *current* timeout. */
      readd_timer = data->attempt <= G_N_ELEMENTS (timeouts) || data->attempt == CHILD_STATUS_CHECK_ATTEMPTS;
      data->attempt++;

      /* Make sure the data isn't destroyed */
      data = NULL;

      if (readd_timer)
        {
          g_timeout_add (timeout, check_child_pid_status, user_data);
          return G_SOURCE_REMOVE;
        }

      return G_SOURCE_CONTINUE;
    }

  /* Only send the child PID if it's exposed */
  if (pid_data->expose_or_share_pids)
    {
      g_autoptr(GError) error = NULL;
      relative_child_pid = get_child_pid_relative_to_parent_sandbox (child_pid, &error);
      if (relative_child_pid == 0)
        g_warning ("Failed to find relative PID for %d: %s", child_pid, error->message);
    }

  g_debug ("Emitting SpawnStarted(%u, %d)", pid, relative_child_pid);

  signal_variant = g_variant_ref_sink (g_variant_new ("(uu)", pid, relative_child_pid));
  g_dbus_connection_emit_signal (session_bus,
                                 pid_data->client,
                                 FLATPAK_PORTAL_PATH,
                                 FLATPAK_PORTAL_INTERFACE,
                                 "SpawnStarted",
                                 signal_variant,
                                 NULL);

  return G_SOURCE_REMOVE;
}

static void
instance_id_read_finish (GObject      *source,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  g_autoptr(GInputStream) stream = NULL;
  g_autofree InstanceIdReadData *data = NULL;
  g_autoptr(FlatpakInstance) instance = NULL;
  g_autoptr(GError) error = NULL;
  BwrapinfoWatcherData *watcher_data = NULL;
  gssize bytes_read;

  stream = G_INPUT_STREAM (source);
  data = (InstanceIdReadData *) user_data;

  bytes_read = g_input_stream_read_finish (stream, res, &error);
  if (bytes_read <= 0)
    {
      /* 0 means EOF, so the process could never have been started. */
      if (bytes_read == -1)
        g_warning ("Failed to read instance id: %s", error->message);

      return;
    }

  data->buffer[bytes_read] = 0;

  instance = flatpak_instance_new_for_id (data->buffer);

  watcher_data = g_new0 (BwrapinfoWatcherData, 1);
  watcher_data->instance = g_steal_pointer (&instance);
  watcher_data->pid = data->pid;

  check_child_pid_status (watcher_data);
}

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
  gsize i;

  flatpak_close_fds_workaround (3);

  if (data->instance_id_fd != -1)
    drop_cloexec (data->instance_id_fd);

  if (data->env_fd != -1)
    drop_cloexec (data->env_fd);

  /* Unblock all signals */
  sigemptyset (&set);
  if (pthread_sigmask (SIG_SETMASK, &set, NULL) == -1)
    {
      g_warning ("Failed to unblock signals when starting child");
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

      /* Ensure we inherit the final fd value */
      drop_cloexec (fd_map[i].final);
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
                 GError    **error)
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
filesystem_arg (const char *path,
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

  if (readonly)
    g_string_append (s, ":ro");

  return g_string_free (g_steal_pointer (&s), FALSE);
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

static char *
bubblewrap_remap_path (const char *path)
{
  if (g_str_has_prefix (path, "/newroot/"))
    path = path + strlen ("/newroot");
  return g_strdup (path);
}

static char *
verify_proc_self_fd (const char *proc_path,
                     GError **error)
{
  char path_buffer[PATH_MAX + 1];
  ssize_t symlink_size;

  symlink_size = readlink (proc_path, path_buffer, PATH_MAX);
  if (symlink_size < 0)
    return glnx_null_throw_errno_prefix (error, "readlink");

  path_buffer[symlink_size] = 0;

  /* All normal paths start with /, but some weird things
     don't, such as socket:[27345] or anon_inode:[eventfd].
     We don't support any of these */
  if (path_buffer[0] != '/')
    return glnx_null_throw (error, "%s resolves to non-absolute path %s",
                            proc_path, path_buffer);

  /* File descriptors to actually deleted files have " (deleted)"
     appended to them. This also happens to some fake fd types
     like shmem which are "/<name> (deleted)". All such
     files are considered invalid. Unfortunatelly this also
     matches files with filenames that actually end in " (deleted)",
     but there is not much to do about this. */
  if (g_str_has_suffix (path_buffer, " (deleted)"))
    return glnx_null_throw (error, "%s resolves to deleted path %s",
                            proc_path, path_buffer);

  /* remap from sandbox to host if needed */
  return bubblewrap_remap_path (path_buffer);
}

static char *
get_path_for_fd (int fd,
                 gboolean *writable_out,
                 GError **error)
{
  g_autofree char *proc_path = NULL;
  int fd_flags;
  struct stat st_buf;
  struct stat real_st_buf;
  g_autofree char *path = NULL;
  gboolean writable = FALSE;
  int read_access_mode;

  /* Must be able to get fd flags */
  fd_flags = fcntl (fd, F_GETFL);
  if (fd_flags == -1)
    return glnx_null_throw_errno_prefix (error, "fcntl F_GETFL");

  /* Must be O_PATH */
  if ((fd_flags & O_PATH) != O_PATH)
    return glnx_null_throw (error, "not opened with O_PATH");

  /* We don't want to allow exposing symlinks, because if they are
   * under the callers control they could be changed between now and
   * starting the child allowing it to point anywhere, so enforce NOFOLLOW.
   * and verify that stat is not a link.
   */
  if ((fd_flags & O_NOFOLLOW) != O_NOFOLLOW)
    return glnx_null_throw (error, "not opened with O_NOFOLLOW");

  /* Must be able to fstat */
  if (fstat (fd, &st_buf) < 0)
    return glnx_null_throw_errno_prefix (error, "fstat");

  /* As per above, no symlinks */
  if (S_ISLNK (st_buf.st_mode))
    return glnx_null_throw (error, "is a symbolic link");

  proc_path = g_strdup_printf ("/proc/self/fd/%d", fd);

  /* Must be able to read valid path from /proc/self/fd */
  /* This is an absolute and (at least at open time) symlink-expanded path */
  path = verify_proc_self_fd (proc_path, error);
  if (path == NULL)
    return NULL;

  /* Verify that this is the same file as the app opened */
  if (stat (path, &real_st_buf) < 0 ||
      st_buf.st_dev != real_st_buf.st_dev ||
      st_buf.st_ino != real_st_buf.st_ino)
    {
      /* Different files on the inside and the outside, reject the request */
      return glnx_null_throw (error,
                              "different file inside and outside sandbox");
    }

  read_access_mode = R_OK;
  if (S_ISDIR (st_buf.st_mode))
    read_access_mode |= X_OK;

  /* Must be able to access the path via the sandbox supplied O_PATH fd,
     which applies the sandbox side mount options (like readonly). */
  if (access (proc_path, read_access_mode) != 0)
    return glnx_null_throw (error, "not %s in sandbox",
                            read_access_mode & X_OK ? "accessible" : "readable");

  if (access (proc_path, W_OK) == 0)
    writable = TRUE;

  if (writable_out != NULL)
    *writable_out = writable;

  return g_steal_pointer (&path);
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
  InstanceIdReadData *instance_id_read_data = NULL;
  gsize i, j, n_fds, n_envs;
  const gint *fds = NULL;
  gint fds_len = 0;
  g_autoptr(GArray) fd_map = NULL;
  g_auto(GStrv) env = NULL;
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
  g_auto(GStrv) sockets = NULL;
  g_auto(GStrv) devices = NULL;
  g_auto(GStrv) unset_env = NULL;
  g_auto(GStrv) sandbox_expose = NULL;
  g_auto(GStrv) sandbox_expose_ro = NULL;
  g_autoptr(GVariant) sandbox_expose_fd = NULL;
  g_autoptr(GVariant) sandbox_expose_fd_ro = NULL;
  g_autoptr(GVariant) app_fd = NULL;
  g_autoptr(GVariant) usr_fd = NULL;
  g_autoptr(GOutputStream) instance_id_out_stream = NULL;
  guint sandbox_flags = 0;
  gboolean sandboxed;
  gboolean expose_pids;
  gboolean share_pids;
  gboolean notify_start;
  gboolean devel;
  gboolean empty_app;
  g_autoptr(GString) env_string = g_string_new ("");
  glnx_autofd int env_fd = -1;

  child_setup_data.instance_id_fd = -1;
  child_setup_data.env_fd = -1;

  if (fd_list != NULL)
    fds = g_unix_fd_list_peek_fds (fd_list, &fds_len);

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
                                             FLATPAK_PORTAL_INTERFACE ".Spawn only works in a flatpak");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (*arg_cwd_path == 0)
    arg_cwd_path = NULL;

  if (arg_argv == NULL || *arg_argv == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "No command given");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if ((arg_flags & ~FLATPAK_SPAWN_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", arg_flags & ~FLATPAK_SPAWN_FLAGS_ALL);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  runtime_ref = g_key_file_get_string (app_info,
                                       FLATPAK_METADATA_GROUP_APPLICATION,
                                       FLATPAK_METADATA_KEY_RUNTIME, NULL);
  if (runtime_ref == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "No runtime found");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
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
  sockets = g_key_file_get_string_list (app_info, FLATPAK_METADATA_GROUP_CONTEXT,
                                       FLATPAK_METADATA_KEY_SOCKETS, NULL, NULL);
  devices = g_key_file_get_string_list (app_info, FLATPAK_METADATA_GROUP_CONTEXT,
                                        FLATPAK_METADATA_KEY_DEVICES, NULL, NULL);

  devel = g_key_file_get_boolean (app_info, FLATPAK_METADATA_GROUP_INSTANCE,
                                  FLATPAK_METADATA_KEY_DEVEL, NULL);

  g_variant_lookup (arg_options, "sandbox-expose", "^as", &sandbox_expose);
  g_variant_lookup (arg_options, "sandbox-expose-ro", "^as", &sandbox_expose_ro);
  g_variant_lookup (arg_options, "sandbox-flags", "u", &sandbox_flags);
  sandbox_expose_fd = g_variant_lookup_value (arg_options, "sandbox-expose-fd", G_VARIANT_TYPE ("ah"));
  sandbox_expose_fd_ro = g_variant_lookup_value (arg_options, "sandbox-expose-fd-ro", G_VARIANT_TYPE ("ah"));
  g_variant_lookup (arg_options, "unset-env", "^as", &unset_env);
  app_fd = g_variant_lookup_value (arg_options, "app-fd", G_VARIANT_TYPE_HANDLE);
  usr_fd = g_variant_lookup_value (arg_options, "usr-fd", G_VARIANT_TYPE_HANDLE);

  if ((sandbox_flags & ~FLATPAK_SPAWN_SANDBOX_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported sandbox flags enabled: 0x%x", arg_flags & ~FLATPAK_SPAWN_SANDBOX_FLAGS_ALL);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (instance_path == NULL &&
      ((sandbox_expose != NULL && sandbox_expose[0] != NULL) ||
       (sandbox_expose_ro != NULL && sandbox_expose_ro[0] != NULL)))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid sandbox expose, caller has no instance path");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  for (i = 0; sandbox_expose != NULL && sandbox_expose[i] != NULL; i++)
    {
      const char *expose = sandbox_expose[i];

      g_debug ("exposing %s", expose);
      if (!is_valid_expose (expose, &error))
        {
          g_dbus_method_invocation_return_gerror (invocation, error);
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }
    }

  for (i = 0; sandbox_expose_ro != NULL && sandbox_expose_ro[i] != NULL; i++)
    {
      const char *expose = sandbox_expose_ro[i];
      g_debug ("exposing %s", expose);
      if (!is_valid_expose (expose, &error))
        {
          g_dbus_method_invocation_return_gerror (invocation, error);
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }
    }

  g_debug ("Running spawn command %s", arg_argv[0]);

  n_fds = 0;
  if (fds != NULL)
    n_fds = g_variant_n_children (arg_fds);

  fd_map = g_array_sized_new (FALSE, FALSE, sizeof (FdMapEntry), n_fds);

  max_fd = -1;
  for (i = 0; i < n_fds; i++)
    {
      FdMapEntry fd_map_entry;
      gint32 handle, dest_fd;
      int handle_fd;

      g_variant_get_child (arg_fds, i, "{uh}", &dest_fd, &handle);

      if (handle >= fds_len || handle < 0)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "No file descriptor for handle %d",
                                                 handle);
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      handle_fd = fds[handle];

      fd_map_entry.to = dest_fd;
      fd_map_entry.from = handle_fd;
      fd_map_entry.final = fd_map_entry.to;
      g_array_append_val (fd_map, fd_map_entry);

      /* If stdin/out/err is a tty we try to set it as the controlling
         tty for the app, this way we can use this to run in a terminal. */
      if ((dest_fd == 0 || dest_fd == 1 || dest_fd == 2) &&
          !child_setup_data.set_tty &&
          isatty (handle_fd))
        {
          child_setup_data.set_tty = TRUE;
          child_setup_data.tty = handle_fd;
        }

      max_fd = MAX (max_fd, fd_map_entry.to);
      max_fd = MAX (max_fd, fd_map_entry.from);
    }

  /* TODO: Ideally we should let `flatpak run` inherit the portal's
   * environment, in case e.g. a LD_LIBRARY_PATH is needed to be able
   * to run `flatpak run`, but tell it to start from a blank environment
   * when running the Flatpak app; but this isn't currently possible, so
   * for now we preserve existing behaviour. */
  if (arg_flags & FLATPAK_SPAWN_FLAGS_CLEAR_ENV)
    {
      char *empty[] = { NULL };
      env = g_strdupv (empty);
    }
  else
    env = g_get_environ ();

  g_ptr_array_add (flatpak_argv, g_strdup (FLATPAK_BINDIR "/flatpak"));
  g_ptr_array_add (flatpak_argv, g_strdup ("run"));

  sandboxed = (arg_flags & FLATPAK_SPAWN_FLAGS_SANDBOX) != 0;

  if (sandboxed)
    {
      g_ptr_array_add (flatpak_argv, g_strdup ("--sandbox"));

      if (sandbox_flags & FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_DISPLAY)
        {
          if (sockets != NULL && g_strv_contains ((const char * const *) sockets, "wayland"))
            g_ptr_array_add (flatpak_argv, g_strdup ("--socket=wayland"));
          if (sockets != NULL && g_strv_contains ((const char * const *) sockets, "fallback-x11"))
            g_ptr_array_add (flatpak_argv, g_strdup ("--socket=fallback-x11"));
          if (sockets != NULL && g_strv_contains ((const char * const *) sockets, "x11"))
            g_ptr_array_add (flatpak_argv, g_strdup ("--socket=x11"));
          if (shares != NULL && g_strv_contains ((const char * const *) shares, "ipc") &&
              sockets != NULL && (g_strv_contains ((const char * const *) sockets, "fallback-x11") ||
                                  g_strv_contains ((const char * const *) sockets, "x11")))
            g_ptr_array_add (flatpak_argv, g_strdup ("--share=ipc"));
        }
      if (sandbox_flags & FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_SOUND)
        {
          if (sockets != NULL && g_strv_contains ((const char * const *) sockets, "pulseaudio"))
            g_ptr_array_add (flatpak_argv, g_strdup ("--socket=pulseaudio"));
        }
      if (sandbox_flags & FLATPAK_SPAWN_SANDBOX_FLAGS_SHARE_GPU)
        {
          if (devices != NULL &&
              (g_strv_contains ((const char * const *) devices, "dri") ||
               g_strv_contains ((const char * const *) devices, "all")))
            g_ptr_array_add (flatpak_argv, g_strdup ("--device=dri"));
        }
      if (sandbox_flags & FLATPAK_SPAWN_SANDBOX_FLAGS_ALLOW_DBUS)
        g_ptr_array_add (flatpak_argv, g_strdup ("--session-bus"));
      if (sandbox_flags & FLATPAK_SPAWN_SANDBOX_FLAGS_ALLOW_A11Y)
        g_ptr_array_add (flatpak_argv, g_strdup ("--a11y-bus"));
    }
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

  /* Let the environment variables given by the caller override the ones
   * from extra_args. Don't add them to @env, because they are controlled
   * by our caller, which might be trying to use them to inject code into
   * flatpak(1); add them to the environment block instead.
   *
   * We don't use --env= here, so that if the values are something that
   * should not be exposed to other uids, they can remain confidential. */
  n_envs = g_variant_n_children (arg_envs);
  for (i = 0; i < n_envs; i++)
    {
      const char *var = NULL;
      const char *val = NULL;
      g_variant_get_child (arg_envs, i, "{&s&s}", &var, &val);

      if (var[0] == '\0')
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "Environment variable cannot have empty name");
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      if (strchr (var, '=') != NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "Environment variable name cannot contain '='");
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      g_string_append (env_string, var);
      g_string_append_c (env_string, '=');
      g_string_append (env_string, val);
      g_string_append_c (env_string, '\0');
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

      env_fd = glnx_steal_fd (&env_tmpf.fd);
      child_setup_data.env_fd = env_fd;
      g_ptr_array_add (flatpak_argv,
                       g_strdup_printf ("--env-fd=%d", env_fd));
    }

  for (i = 0; unset_env != NULL && unset_env[i] != NULL; i++)
    {
      const char *var = unset_env[i];

      if (var[0] == '\0')
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "Environment variable cannot have empty name");
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      if (strchr (var, '=') != NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "Environment variable name cannot contain '='");
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      g_ptr_array_add (flatpak_argv,
                       g_strdup_printf ("--unset-env=%s", var));
    }

  expose_pids = (arg_flags & FLATPAK_SPAWN_FLAGS_EXPOSE_PIDS) != 0;
  share_pids = (arg_flags & FLATPAK_SPAWN_FLAGS_SHARE_PIDS) != 0;

  if (expose_pids || share_pids)
    {
      g_autofree char *instance_id = NULL;
      int sender_pid1 = 0;

      if (!(supports & FLATPAK_SPAWN_SUPPORT_FLAGS_EXPOSE_PIDS))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_NOT_SUPPORTED,
                                                 "Expose pids not supported with setuid bwrap");
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      instance_id = g_key_file_get_string (app_info,
                                           FLATPAK_METADATA_GROUP_INSTANCE,
                                           FLATPAK_METADATA_KEY_INSTANCE_ID, NULL);

      if (instance_id)
        {
          g_autoptr(FlatpakInstance) instance = flatpak_instance_new_for_id (instance_id);
          sender_pid1 = flatpak_instance_get_child_pid (instance);
        }

      if (sender_pid1 == 0)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "Could not find requesting pid");
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      g_ptr_array_add (flatpak_argv, g_strdup_printf ("--parent-pid=%d", sender_pid1));

      if (share_pids)
        g_ptr_array_add (flatpak_argv, g_strdup ("--parent-share-pids"));
      else
        g_ptr_array_add (flatpak_argv, g_strdup ("--parent-expose-pids"));
    }

  notify_start = (arg_flags & FLATPAK_SPAWN_FLAGS_NOTIFY_START) != 0;
  if (notify_start)
    {
      int pipe_fds[2];
      if (pipe (pipe_fds) == -1)
        {
          int errsv = errno;
          g_dbus_method_invocation_return_error (invocation, G_IO_ERROR,
                                                 g_io_error_from_errno (errsv),
                                                 "Failed to create instance ID pipe: %s",
                                                 g_strerror (errsv));
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      GInputStream *in_stream = G_INPUT_STREAM (g_unix_input_stream_new (pipe_fds[0], TRUE));
      /* This is saved to ensure the portal's end gets closed after the exec. */
      instance_id_out_stream = G_OUTPUT_STREAM (g_unix_output_stream_new (pipe_fds[1], TRUE));

      instance_id_read_data = g_new0 (InstanceIdReadData, 1);

      g_input_stream_read_async (in_stream, instance_id_read_data->buffer,
                                 INSTANCE_ID_BUFFER_SIZE - 1, G_PRIORITY_DEFAULT, NULL,
                                 instance_id_read_finish, instance_id_read_data);

      g_ptr_array_add (flatpak_argv, g_strdup_printf ("--instance-id-fd=%d", pipe_fds[1]));
      child_setup_data.instance_id_fd = pipe_fds[1];
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

  if (sandbox_expose_fd != NULL)
    {
      gsize len = g_variant_n_children (sandbox_expose_fd);
      for (i = 0; i < len; i++)
        {
          gint32 handle;
          g_variant_get_child (sandbox_expose_fd, i, "h", &handle);
          if (handle >= 0 && handle < fds_len)
            {
              int handle_fd = fds[handle];
              g_autofree char *path = NULL;
              gboolean writable = FALSE;

              path = get_path_for_fd (handle_fd, &writable, &error);

              if (path)
                {
                  g_ptr_array_add (flatpak_argv, filesystem_arg (path, !writable));
                }
              else
                {
                  g_debug ("unable to get path for sandbox-exposed fd %d, ignoring: %s",
                           handle_fd, error->message);
                  g_clear_error (&error);
                }
            }
          else
            {
              g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                     G_DBUS_ERROR_INVALID_ARGS,
                                                     "No file descriptor for handle %d",
                                                     handle);
              return G_DBUS_METHOD_INVOCATION_HANDLED;
            }
        }
    }

  if (sandbox_expose_fd_ro != NULL)
    {
      gsize len = g_variant_n_children (sandbox_expose_fd_ro);
      for (i = 0; i < len; i++)
        {
          gint32 handle;
          g_variant_get_child (sandbox_expose_fd_ro, i, "h", &handle);
          if (handle >= 0 && handle < fds_len)
            {
              int handle_fd = fds[handle];
              g_autofree char *path = NULL;
              gboolean writable = FALSE;

              path = get_path_for_fd (handle_fd, &writable, &error);

              if (path)
                {
                  g_ptr_array_add (flatpak_argv, filesystem_arg (path, TRUE));
                }
              else
                {
                  g_debug ("unable to get path for sandbox-exposed fd %d, ignoring: %s",
                           handle_fd, error->message);
                  g_clear_error (&error);
                }
            }
          else
            {
              g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                     G_DBUS_ERROR_INVALID_ARGS,
                                                     "No file descriptor for handle %d",
                                                     handle);
              return G_DBUS_METHOD_INVOCATION_HANDLED;
            }
        }
    }

  empty_app = (arg_flags & FLATPAK_SPAWN_FLAGS_EMPTY_APP) != 0;

  if (app_fd != NULL)
    {
      gint32 handle = g_variant_get_handle (app_fd);
      g_autofree char *path = NULL;

      if (empty_app)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "app-fd and EMPTY_APP cannot both be used");
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      if (handle >= fds_len || handle < 0)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "No file descriptor for handle %d",
                                                 handle);
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      g_assert (fds != NULL);   /* otherwise fds_len would be 0 */
      path = get_path_for_fd (fds[handle], NULL, &error);

      if (path == NULL)
        {
          g_prefix_error (&error, "Unable to convert /app fd %d into path: ",
                          fds[handle]);
          g_dbus_method_invocation_return_gerror (invocation, error);
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      g_debug ("Using %s as /app instead of app", path);
      g_ptr_array_add (flatpak_argv, g_strdup_printf ("--app-path=%s", path));
    }
  else if (empty_app)
    {
      g_ptr_array_add (flatpak_argv, g_strdup ("--app-path="));
    }

  if (usr_fd != NULL)
    {
      gint32 handle = g_variant_get_handle (usr_fd);
      g_autofree char *path = NULL;

      if (handle >= fds_len || handle < 0)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                 G_DBUS_ERROR_INVALID_ARGS,
                                                 "No file descriptor for handle %d",
                                                 handle);
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      g_assert (fds != NULL);   /* otherwise fds_len would be 0 */
      path = get_path_for_fd (fds[handle], NULL, &error);

      if (path == NULL)
        {
          g_prefix_error (&error, "Unable to convert /usr fd %d into path: ",
                          fds[handle]);
          g_dbus_method_invocation_return_gerror (invocation, error);
          return G_DBUS_METHOD_INVOCATION_HANDLED;
        }

      g_debug ("Using %s as /usr instead of runtime", path);
      g_ptr_array_add (flatpak_argv, g_strdup_printf ("--usr-path=%s", path));
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

      for (i = 0; flatpak_argv->pdata[i] != NULL; i++)
        {
          if (i > 0)
            g_string_append (cmd, " ");
          g_string_append (cmd, flatpak_argv->pdata[i]);
        }

      g_debug ("Starting: %s\n", cmd->str);
    }

  /* We make a second pass over the fds to find if any "to" fd index
     overlaps an already in use fd (i.e. one in the "from" category
     that are allocated randomly). If a fd overlaps "to" fd then its
     a caller issue and not our fault, so we ignore that. */
  for (i = 0; i < fd_map->len; i++)
    {
      int to_fd = g_array_index (fd_map, FdMapEntry, i).to;
      gboolean conflict = FALSE;

      /* At this point we're fine with using "from" values for this
         value (because we handle to==from in the code), or values
         that are before "i" in the fd_map (because those will be
         closed at this point when dup:ing). However, we can't
         reuse a fd that is in "from" for j > i. */
      for (j = i + 1; j < fd_map->len; j++)
        {
          int from_fd = g_array_index(fd_map, FdMapEntry, j).from;
          if (from_fd == to_fd)
            {
              conflict = TRUE;
              break;
            }
        }

      if (conflict)
        g_array_index (fd_map, FdMapEntry, i).to = ++max_fd;
    }

  child_setup_data.fd_map = &g_array_index (fd_map, FdMapEntry, 0);
  child_setup_data.fd_map_len = fd_map->len;

  /* We use LEAVE_DESCRIPTORS_OPEN to work around dead-lock, see flatpak_close_fds_workaround */
  if (!g_spawn_async_with_pipes (NULL,
                                 (char **) flatpak_argv->pdata,
                                 env,
                                 G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
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

  if (instance_id_read_data)
    instance_id_read_data->pid = pid;

  pid_data = g_new0 (PidData, 1);
  pid_data->pid = pid;
  pid_data->client = g_strdup (g_dbus_method_invocation_get_sender (invocation));
  pid_data->watch_bus = (arg_flags & FLATPAK_SPAWN_FLAGS_WATCH_BUS) != 0;
  pid_data->expose_or_share_pids = (expose_pids || share_pids);
  pid_data->child_watch = g_child_watch_add_full (G_PRIORITY_DEFAULT,
                                                  pid,
                                                  child_watch_died,
                                                  pid_data,
                                                  NULL);

  g_debug ("Client Pid is %d", pid_data->pid);

  g_hash_table_replace (client_pid_data_hash, GUINT_TO_POINTER (pid_data->pid),
                        pid_data);

  portal_flatpak_complete_spawn (object, invocation, NULL, pid);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
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
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_debug ("Sending signal %d to client pid %d", arg_signal, arg_pid);

  if (arg_to_process_group)
    killpg (pid_data->pid, arg_signal);
  else
    kill (pid_data->pid, arg_signal);

  portal_flatpak_complete_spawn_signal (portal, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
authorize_method_handler (GDBusInterfaceSkeleton *interface,
                          GDBusMethodInvocation  *invocation,
                          gpointer                user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autofree char *app_id = NULL;
  const char *required_sender;

  /* Ensure we don't idle exit */
  schedule_idle_callback ();

  required_sender = g_object_get_data (G_OBJECT (interface), "required-sender");

  if (required_sender)
    {
      const char *sender = g_dbus_method_invocation_get_sender (invocation);
      if (g_strcmp0 (required_sender, sender) != 0)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                                                 "Client not allowed to access object");
          return FALSE;
        }
    }

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
register_update_monitor (PortalFlatpakUpdateMonitor *monitor,
                         const char                 *obj_path)
{
  G_LOCK (update_monitors);

  g_hash_table_insert (update_monitors, g_strdup (obj_path), g_object_ref (monitor));

  /* Trigger update timeout if needed */
  if (update_monitors_timeout == 0 && !update_monitors_timeout_running_thread)
    update_monitors_timeout = g_timeout_add_seconds (opt_poll_timeout, check_all_for_updates_cb, NULL);

  G_UNLOCK (update_monitors);
}

static void
unregister_update_monitor (const char *obj_path)
{
  G_LOCK (update_monitors);
  g_hash_table_remove (update_monitors, obj_path);
  G_UNLOCK (update_monitors);
}

static gboolean
has_update_monitors (void)
{
  gboolean res;
  G_LOCK (update_monitors);
  res = g_hash_table_size (update_monitors) > 0;
  G_UNLOCK (update_monitors);
  return res;
}

static GList *
update_monitors_get_all (const char *optional_sender)
{
  GList *list = NULL;

  G_LOCK (update_monitors);
  if (update_monitors)
    {
      GLNX_HASH_TABLE_FOREACH_V (update_monitors, PortalFlatpakUpdateMonitor *, monitor)
        {
          UpdateMonitorData *data = update_monitor_get_data (monitor);

          if (optional_sender == NULL ||
              strcmp (data->sender, optional_sender) == 0)
            list = g_list_prepend (list, g_object_ref (monitor));
        }
    }
  G_UNLOCK (update_monitors);

  return list;
}

static void
update_monitor_data_free (gpointer data)
{
  UpdateMonitorData *m = data;

  g_mutex_clear (&m->lock);

  g_free (m->sender);
  g_free (m->obj_path);
  g_object_unref (m->cancellable);

  g_free (m->name);
  g_free (m->arch);
  g_free (m->branch);
  g_free (m->commit);
  g_free (m->app_path);

  g_free (m->reported_local_commit);
  g_free (m->reported_remote_commit);

  g_free (m);
}

static UpdateMonitorData *
update_monitor_get_data (PortalFlatpakUpdateMonitor *monitor)
{
  return (UpdateMonitorData *)g_object_get_data (G_OBJECT (monitor), "update-monitor-data");
}

static PortalFlatpakUpdateMonitor *
create_update_monitor (GDBusMethodInvocation *invocation,
                       const char            *obj_path,
                       GError               **error)
{
  PortalFlatpakUpdateMonitor *monitor;
  UpdateMonitorData *m;
  g_autoptr(GKeyFile) app_info = NULL;
  g_autofree char *name = NULL;

  app_info = flatpak_invocation_lookup_app_info (invocation, NULL, error);
  if (app_info == NULL)
    return NULL;

  name = g_key_file_get_string (app_info,
                                FLATPAK_METADATA_GROUP_APPLICATION,
                                "name", NULL);
  if (name == NULL || *name == 0)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
                   "Updates only supported by flatpak apps");
      return NULL;
    }

  m = g_new0 (UpdateMonitorData, 1);

  g_mutex_init (&m->lock);
  m->obj_path = g_strdup (obj_path);
  m->sender = g_strdup (g_dbus_method_invocation_get_sender (invocation));
  m->cancellable = g_cancellable_new ();

  m->name = g_steal_pointer (&name);
  m->arch = g_key_file_get_string (app_info,
                                   FLATPAK_METADATA_GROUP_INSTANCE,
                                   "arch", NULL);
  m->branch = g_key_file_get_string (app_info,
                                     FLATPAK_METADATA_GROUP_INSTANCE,
                                     "branch", NULL);
  m->commit = g_key_file_get_string (app_info,
                                     FLATPAK_METADATA_GROUP_INSTANCE,
                                     "app-commit", NULL);
  m->app_path = g_key_file_get_string (app_info,
                                       FLATPAK_METADATA_GROUP_INSTANCE,
                                       "app-path", NULL);

  m->reported_local_commit = g_strdup (m->commit);
  m->reported_remote_commit = g_strdup (m->commit);

  monitor = portal_flatpak_update_monitor_skeleton_new ();

  g_object_set_data_full (G_OBJECT (monitor), "update-monitor-data", m, update_monitor_data_free);
  g_object_set_data_full (G_OBJECT (monitor), "required-sender", g_strdup (m->sender), g_free);

  g_debug ("created UpdateMonitor for %s/%s at %s", m->name, m->branch, obj_path);

  return monitor;
}

static void
update_monitor_do_close (PortalFlatpakUpdateMonitor *monitor)
{
  UpdateMonitorData *m = update_monitor_get_data (monitor);

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (monitor));
  unregister_update_monitor (m->obj_path);
}

/* Always called in worker thread */
static void
update_monitor_close (PortalFlatpakUpdateMonitor *monitor)
{
  UpdateMonitorData *m = update_monitor_get_data (monitor);
  gboolean do_close;

  g_mutex_lock (&m->lock);
  /* Close at most once, but not if running, if running it will be closed when that is done */
  do_close = !m->closed && !m->running;
  m->closed = TRUE;
  g_mutex_unlock (&m->lock);

  /* Always cancel though, so we can exit any running code early */
  g_cancellable_cancel (m->cancellable);

  if (do_close)
    update_monitor_do_close (monitor);
}

static GDBusConnection *
update_monitor_get_connection (PortalFlatpakUpdateMonitor *monitor)
{
  return g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (monitor));
}

static GHashTable *installation_cache = NULL;

static void
clear_installation_cache (void)
{
  if (installation_cache != NULL)
    g_hash_table_remove_all (installation_cache);
}

/* Caching lookup of Installation for a path */
static FlatpakInstallation *
lookup_installation_for_path (GFile *path, GError **error)
{
  FlatpakInstallation *installation;

  if (installation_cache == NULL)
    installation_cache = g_hash_table_new_full (g_file_hash, (GEqualFunc)g_file_equal, g_object_unref, g_object_unref);

  installation = g_hash_table_lookup (installation_cache, path);
  if (installation == NULL)
    {
      g_autoptr(FlatpakDir) dir = NULL;

      dir = flatpak_dir_get_by_path (path);
      installation = flatpak_installation_new_for_dir (dir, NULL, error);
      if (installation == NULL)
        return NULL;

      flatpak_installation_set_no_interaction (installation, TRUE);

      g_hash_table_insert (installation_cache, g_object_ref (path), installation);
    }

  return g_object_ref (installation);
}

static GFile *
update_monitor_get_installation_path (PortalFlatpakUpdateMonitor *monitor)
{
  UpdateMonitorData *m = update_monitor_get_data (monitor);
  g_autoptr(GFile) app_path = NULL;

  app_path = g_file_new_for_path (m->app_path);

  /* The app path is always 6 level deep inside the installation dir,
   * like $dir/app/org.the.app/x86_64/stable/$commit/files, so we find
   * the installation by just going up 6 parents. */
  return g_file_resolve_relative_path (app_path, "../../../../../..");
}

static void
check_for_updates (PortalFlatpakUpdateMonitor *monitor)
{
  UpdateMonitorData *m = update_monitor_get_data (monitor);
  g_autoptr(GFile) installation_path = NULL;
  g_autoptr(FlatpakInstallation) installation = NULL;
  g_autoptr(FlatpakInstalledRef) installed_ref = NULL;
  g_autoptr(FlatpakRemoteRef) remote_ref = NULL;
  const char *origin = NULL;
  const char *local_commit = NULL;
  const char *remote_commit;
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  const char *ref;

  installation_path = update_monitor_get_installation_path (monitor);

  g_debug ("Checking for updates for %s/%s/%s in %s", m->name, m->arch, m->branch, flatpak_file_get_path_cached (installation_path));

  installation = lookup_installation_for_path (installation_path, &error);
  if (installation == NULL)
    {
      g_debug ("Unable to find installation for path %s: %s", flatpak_file_get_path_cached (installation_path), error->message);
      return;
    }

  installed_ref = flatpak_installation_get_installed_ref (installation,
                                                          FLATPAK_REF_KIND_APP,
                                                          m->name, m->arch, m->branch,
                                                          m->cancellable, &error);
  if (installed_ref == NULL)
    {
      g_debug ("getting installed ref failed: %s", error->message);
      return; /* Never report updates for uninstalled refs */
    }

  dir = flatpak_installation_get_dir (installation, NULL);
  if (dir == NULL)
    return;

  ref = flatpak_ref_format_ref_cached (FLATPAK_REF (installed_ref));
  if (flatpak_dir_ref_is_masked (dir, ref))
    return; /* Never report updates for masked refs */

  local_commit = flatpak_ref_get_commit (FLATPAK_REF (installed_ref));

  origin = flatpak_installed_ref_get_origin (installed_ref);

  remote_ref = flatpak_installation_fetch_remote_ref_sync (installation, origin,
                                                           FLATPAK_REF_KIND_APP,
                                                           m->name, m->arch, m->branch,
                                                           m->cancellable, &error);
  if (remote_ref == NULL)
    {
      /* Probably some network issue.
       * Fall back to the local_commit to at least be able to pick up already installed updates.
       */
      g_debug ("getting remote ref failed: %s", error->message);
      g_clear_error (&error);
      remote_commit = local_commit;
    }
  else
    {
      remote_commit = flatpak_ref_get_commit (FLATPAK_REF (remote_ref));
      if (remote_commit == NULL)
        {
          /* This can happen if we're offline and there is an update from an usb drive.
           * Not much we can do in terms of reporting it, but at least handle the case
           */
          g_debug ("Unknown remote commit, setting to local_commit");
          remote_commit = local_commit;
        }
    }

  if (g_strcmp0 (m->reported_local_commit, local_commit) != 0 ||
      g_strcmp0 (m->reported_remote_commit, remote_commit) != 0)
    {
      GVariantBuilder builder;
      gboolean is_closed;

      g_free (m->reported_local_commit);
      m->reported_local_commit = g_strdup (local_commit);

      g_free (m->reported_remote_commit);
      m->reported_remote_commit = g_strdup (remote_commit);

      g_debug ("Found update for %s/%s/%s, local: %s, remote: %s", m->name, m->arch, m->branch, local_commit, remote_commit);
      g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&builder, "{sv}", "running-commit", g_variant_new_string (m->commit));
      g_variant_builder_add (&builder, "{sv}", "local-commit", g_variant_new_string (local_commit));
      g_variant_builder_add (&builder, "{sv}", "remote-commit", g_variant_new_string (remote_commit));

      /* Maybe someone closed the monitor while we were checking for updates, then drop the signal.
       * There is still a minimal race between this check and the emit where a client could call close()
       * and still see the signal though. */
      g_mutex_lock (&m->lock);
      is_closed = m->closed;
      g_mutex_unlock (&m->lock);

      if (!is_closed &&
          !g_dbus_connection_emit_signal (update_monitor_get_connection (monitor),
                                          m->sender,
                                          m->obj_path,
                                          FLATPAK_PORTAL_INTERFACE_UPDATE_MONITOR,
                                          "UpdateAvailable",
                                          g_variant_new ("(a{sv})", &builder),
                                          &error))
        {
          g_warning ("Failed to emit UpdateAvailable: %s", error->message);
          g_clear_error (&error);
        }
    }
}

static void
check_all_for_updates_in_thread_func (GTask *task,
                                      gpointer source_object,
                                      gpointer task_data,
                                      GCancellable *cancellable)
{
  GList *monitors, *l;

  monitors = update_monitors_get_all (NULL);

  for (l = monitors; l != NULL; l = l->next)
    {
      PortalFlatpakUpdateMonitor *monitor = l->data;
      UpdateMonitorData *m = update_monitor_get_data (monitor);
      gboolean was_closed = FALSE;

      g_mutex_lock (&m->lock);
      if (m->closed)
        was_closed = TRUE;
      else
        m->running = TRUE;
      g_mutex_unlock (&m->lock);

      if (!was_closed)
        {
          check_for_updates (monitor);

          g_mutex_lock (&m->lock);
          m->running = FALSE;
          if (m->closed) /* Was closed during running, do delayed close */
            update_monitor_do_close (monitor);
          g_mutex_unlock (&m->lock);
        }
    }

  g_list_free_full (monitors, g_object_unref);


/* We want to cache stuff between multiple monitors
   when a poll is scheduled, but there is no need to keep it
   long term to the next poll, the in-memory is just
   a waste of space then. */
  clear_installation_cache ();

  G_LOCK (update_monitors);
  update_monitors_timeout_running_thread = FALSE;

  if (g_hash_table_size (update_monitors) > 0)
    update_monitors_timeout = g_timeout_add_seconds (opt_poll_timeout, check_all_for_updates_cb, NULL);

  G_UNLOCK (update_monitors);
}

/* Runs on main thread */
static gboolean
check_all_for_updates_cb (void *data)
{
  g_autoptr(GTask) task = g_task_new (NULL, NULL, NULL, NULL);

  if (!opt_poll_when_metered &&
      g_network_monitor_get_network_metered (network_monitor))
    {
      g_debug ("Skipping update check on metered network");

      return G_SOURCE_CONTINUE;
    }

  g_debug ("Checking all update monitors");

  G_LOCK (update_monitors);
  update_monitors_timeout = 0;
  update_monitors_timeout_running_thread = TRUE;
  G_UNLOCK (update_monitors);

  g_task_run_in_thread (task, check_all_for_updates_in_thread_func);

  return G_SOURCE_REMOVE; /* This will be re-added by the thread when done */
}

/* Runs in worker thread */
static gboolean
handle_create_update_monitor (PortalFlatpak *object,
                              GDBusMethodInvocation *invocation,
                              GVariant *options)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  g_autoptr(PortalFlatpakUpdateMonitorSkeleton) monitor = NULL;
  const char *sender;
  g_autofree char *sender_escaped = NULL;
  g_autofree char *obj_path = NULL;
  g_autofree char *token = NULL;
  g_autoptr(GError) error = NULL;
  int i;

  if (!g_variant_lookup (options, "handle_token", "s", &token))
    token = g_strdup_printf ("%d", g_random_int_range (0, 1000));

  sender = g_dbus_method_invocation_get_sender (invocation);
  g_debug ("handle CreateUpdateMonitor from %s", sender);

  sender_escaped = g_strdup (sender + 1);
  for (i = 0; sender_escaped[i]; i++)
    {
      if (sender_escaped[i] == '.')
        sender_escaped[i] = '_';
    }

  obj_path = g_strdup_printf ("%s/update_monitor/%s/%s",
                              FLATPAK_PORTAL_PATH,
                              sender_escaped,
                              token);

  monitor = (PortalFlatpakUpdateMonitorSkeleton *) create_update_monitor (invocation, obj_path, &error);
  if (monitor == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_signal_connect (monitor, "handle-close", G_CALLBACK (handle_close), NULL);
  g_signal_connect (monitor, "handle-update", G_CALLBACK (handle_update), NULL);
  g_signal_connect (monitor, "g-authorize-method", G_CALLBACK (authorize_method_handler), NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (monitor),
                                         connection,
                                         obj_path,
                                         &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  register_update_monitor ((PortalFlatpakUpdateMonitor*)monitor, obj_path);

  portal_flatpak_complete_create_update_monitor (portal, invocation, obj_path);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

/* Runs in worker thread */
static gboolean
handle_close (PortalFlatpakUpdateMonitor *monitor,
              GDBusMethodInvocation *invocation)
{
  update_monitor_close (monitor);

  g_debug ("handle UpdateMonitor.Close");

  portal_flatpak_update_monitor_complete_close (monitor, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
deep_free_object_list (gpointer data)
{
  g_list_free_full ((GList *)data, g_object_unref);
}

static void
close_update_monitors_in_thread_func (GTask *task,
                                      gpointer source_object,
                                      gpointer task_data,
                                      GCancellable *cancellable)
{
  GList *list = task_data;
  GList *l;

  for (l = list; l; l = l->next)
    {
      PortalFlatpakUpdateMonitor *monitor = l->data;
      UpdateMonitorData *m = update_monitor_get_data (monitor);

      g_debug ("closing monitor %s", m->obj_path);
      update_monitor_close (monitor);
    }
}

static void
close_update_monitors_for_sender (const char *sender)
{
  GList *list = update_monitors_get_all (sender);

  if (list)
    {
      g_autoptr(GTask) task = g_task_new (NULL, NULL, NULL, NULL);
      g_task_set_task_data (task, list, deep_free_object_list);

      g_debug ("%s dropped off the bus, closing monitors", sender);
      g_task_run_in_thread (task, close_update_monitors_in_thread_func);
    }
}

static guint32
get_update_permission (const char *app_id)
{
  g_autoptr(GVariant) out_perms = NULL;
  g_autoptr(GVariant) out_data = NULL;
  g_autoptr(GError) error = NULL;
  guint32 ret = UNSET;

  if (permission_store == NULL)
    {
      g_debug ("No portals installed, assume no permissions");
      return NO;
    }

  if (!xdp_dbus_permission_store_call_lookup_sync (permission_store,
                                                   PERMISSION_TABLE,
                                                   PERMISSION_ID,
                                                   &out_perms,
                                                   &out_data,
                                                   NULL,
                                                   &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_debug ("No updates permissions found: %s", error->message);
      g_clear_error (&error);
    }

  if (out_perms != NULL)
    {
      const char **perms;

      if (g_variant_lookup (out_perms, app_id, "^a&s", &perms))
        {
          if (strcmp (perms[0], "ask") == 0)
            ret = ASK;
          else if (strcmp (perms[0], "yes") == 0)
            ret = YES;
          else
            ret = NO;
        }
    }

  g_debug ("Updates permissions for %s: %d", app_id, ret);

  return ret;
}

static void
set_update_permission (const char *app_id,
                       Permission permission)
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

  if (!xdp_dbus_permission_store_call_set_permission_sync (permission_store,
                                                           PERMISSION_TABLE,
                                                           TRUE,
                                                           PERMISSION_ID,
                                                           app_id,
                                                           (const char * const*)permissions,
                                                           NULL,
                                                           &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_info ("Error updating permission store: %s", error->message);
    }
}

static char *
get_app_display_name (const char *app_id)
{
  g_autofree char *id = NULL;
  g_autoptr(GDesktopAppInfo) info = NULL;
  const char *name = NULL;

  id = g_strconcat (app_id, ".desktop", NULL);
  info = g_desktop_app_info_new (id);
  if (info)
    {
      name = g_app_info_get_display_name (G_APP_INFO (info));
      if (name)
        return g_strdup (name);
    }

  return g_strdup (app_id);
}

static gboolean
request_update_permissions_sync (PortalFlatpakUpdateMonitor *monitor,
                                 const char *app_id,
                                 const char *window,
                                 GError **error)
{
  Permission permission;

  permission = get_update_permission (app_id);
  if (permission == UNSET || permission == ASK)
    {
      guint access_response = 2;
      PortalImplementation *access_impl;
      GVariantBuilder access_opt_builder;
      g_autofree char *app_name = NULL;
      g_autofree char *title = NULL;
      g_autoptr(GVariant) ret = NULL;

      access_impl = find_portal_implementation ("org.freedesktop.impl.portal.Access");
      if (access_impl == NULL)
        {
          g_warning ("No Access portal implementation found");
          g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, _("No portal support found"));
          return FALSE;
        }

      g_variant_builder_init (&access_opt_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "deny_label", g_variant_new_string (_("Deny")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "grant_label", g_variant_new_string (_("Update")));
      g_variant_builder_add (&access_opt_builder, "{sv}",
                             "icon", g_variant_new_string ("package-x-generic-symbolic"));

      app_name = get_app_display_name (app_id);
      title = g_strdup_printf (_("Update %s?"), app_name);

      ret = g_dbus_connection_call_sync (update_monitor_get_connection (monitor),
                                         access_impl->dbus_name,
                                         "/org/freedesktop/portal/desktop",
                                         "org.freedesktop.impl.portal.Access",
                                         "AccessDialog",
                                         g_variant_new ("(osssssa{sv})",
                                                        "/request/path",
                                                        app_id,
                                                        window,
                                                        title,
                                                        _("The application wants to update itself."),
                                                        _("Update access can be changed any time from the privacy settings."),
                                                        &access_opt_builder),
                                         G_VARIANT_TYPE ("(ua{sv})"),
                                         G_DBUS_CALL_FLAGS_NONE,
                                         G_MAXINT,
                                         NULL,
                                         error);
      if (ret == NULL)
        {
          g_dbus_error_strip_remote_error (*error);
          g_warning ("Failed to show access dialog: %s", (*error)->message);
          return FALSE;
        }

      g_variant_get (ret, "(ua{sv})", &access_response, NULL);

      if (permission == UNSET)
        set_update_permission (app_id, (access_response == 0) ? YES : NO);

      permission = (access_response == 0) ? YES : NO;
    }

  if (permission == NO)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                   _("Application update not allowed"));
      return FALSE;
    }

  return TRUE;
}

static void
emit_progress (PortalFlatpakUpdateMonitor *monitor,
               int op,
               int n_ops,
               int progress,
               int status,
               const char *error_name,
               const char *error_message)
{
  UpdateMonitorData *m = update_monitor_get_data (monitor);
  GDBusConnection *connection;
  GVariantBuilder builder;
  g_autoptr(GError) error = NULL;

  g_debug ("%d/%d ops, progress %d, status: %d", op, n_ops, progress, status);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  if (n_ops > 0)
    {
      g_variant_builder_add (&builder, "{sv}", "op", g_variant_new_uint32 (op));
      g_variant_builder_add (&builder, "{sv}", "n_ops", g_variant_new_uint32 (n_ops));
      g_variant_builder_add (&builder, "{sv}", "progress", g_variant_new_uint32 (progress));
    }

  g_variant_builder_add (&builder, "{sv}", "status", g_variant_new_uint32 (status));

  if (error_name)
    {
      g_variant_builder_add (&builder, "{sv}", "error", g_variant_new_string (error_name));
      g_variant_builder_add (&builder, "{sv}", "error_message", g_variant_new_string (error_message));
    }

  connection = update_monitor_get_connection (monitor);
  if (!g_dbus_connection_emit_signal (connection,
                                      m->sender,
                                      m->obj_path,
                                      FLATPAK_PORTAL_INTERFACE_UPDATE_MONITOR,
                                      "Progress",
                                      g_variant_new ("(a{sv})", &builder),
                                      &error))
    {
      g_warning ("Failed to emit ::progress: %s", error->message);
    }
}

static char *
get_progress_error (const GError *update_error)
{
  g_autofree gchar *name = NULL;

  name = g_dbus_error_encode_gerror (update_error);

  /* Don't return weird dbus wrapped things from the portal */
  if (g_str_has_prefix (name, "org.gtk.GDBus.UnmappedGError.Quark"))
    return g_strdup ("org.freedesktop.DBus.Error.Failed");
  return g_steal_pointer (&name);
}

static void
emit_progress_error (PortalFlatpakUpdateMonitor *monitor,
                     GError *update_error)
{
  g_autofree gchar *error_name = get_progress_error (update_error);

  emit_progress (monitor, 0, 0, 0,
                 PROGRESS_STATUS_ERROR,
                 error_name, update_error->message);
}

static void
send_variant (GVariant *v, GOutputStream *out)
{
  g_autoptr(GError) error = NULL;
  const guchar *data;
  gsize size;
  guint32 size32;

  data = g_variant_get_data (v);
  size = g_variant_get_size (v);
  size32 = size;

  if (!g_output_stream_write_all (out, &size32, 4, NULL, NULL, &error) ||
      !g_output_stream_write_all (out, data, size, NULL, NULL, &error))
    {
      g_warning ("sending to parent failed: %s", error->message);
      exit (1); // This will exit the child process and cause the parent to report an error
    }
}

static void
send_progress (GOutputStream *out,
               int op,
               int n_ops,
               int progress,
               int status,
               const GError *update_error)
{
  g_autoptr(GVariant) v = NULL;
  g_autofree gchar *error_name = NULL;

  if (update_error)
    error_name = get_progress_error (update_error);

  v = g_variant_ref_sink (g_variant_new ("(uuuuss)",
                                         op, n_ops, progress, status,
                                         error_name ? error_name : "",
                                         update_error ? update_error->message : ""));
  send_variant (v, out);
}

typedef struct {
  GOutputStream *out;
  int n_ops;
  int op;
  int progress;
  gboolean saw_first_operation;
} TransactionData;

static gboolean
transaction_ready (FlatpakTransaction *transaction,
                   TransactionData *d)
{
  GList *ops = flatpak_transaction_get_operations (transaction);
  int status;
  GList *l;

  d->n_ops = g_list_length (ops);
  d->op = 0;
  d->progress = 0;

  for (l = ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      const char *ref = flatpak_transaction_operation_get_ref (op);
      FlatpakTransactionOperationType type = flatpak_transaction_operation_get_operation_type (op);

      /* Actual app updates need to not increase premission requirements */
      if (type == FLATPAK_TRANSACTION_OPERATION_UPDATE && g_str_has_prefix (ref, "app/"))
        {
          GKeyFile *new_metadata = flatpak_transaction_operation_get_metadata (op);
          GKeyFile *old_metadata = flatpak_transaction_operation_get_old_metadata (op);
          g_autoptr(FlatpakContext) new_context = flatpak_context_new ();
          g_autoptr(FlatpakContext) old_context = flatpak_context_new ();

          if (!flatpak_context_load_metadata (new_context, new_metadata, NULL) ||
              !flatpak_context_load_metadata (old_context, old_metadata, NULL) ||
              flatpak_context_adds_permissions (old_context, new_context))
            {
              g_autoptr(GError) error = NULL;
              g_set_error (&error, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
                           _("Self update not supported, new version requires new permissions"));
              send_progress (d->out,
                             d->op,  d->n_ops, d->progress,
                             PROGRESS_STATUS_ERROR,
                             error);
              return FALSE;
            }
        }
    }

  if (flatpak_transaction_is_empty (transaction))
    status = PROGRESS_STATUS_EMPTY;
  else
    status = PROGRESS_STATUS_RUNNING;

  send_progress (d->out,
                 d->op,  d->n_ops,
                 d->progress, status,
                 NULL);

  if (status == PROGRESS_STATUS_EMPTY)
    return FALSE; /* This will cause us to return an ABORTED error */

  return TRUE;
}

static void
transaction_progress_changed (FlatpakTransactionProgress *progress,
                              TransactionData *d)
{
  /* Only report 100 when really done */
  d->progress = MIN (flatpak_transaction_progress_get_progress (progress), 99);

  send_progress (d->out,
                 d->op,  d->n_ops,
                 d->progress, PROGRESS_STATUS_RUNNING,
                 NULL);
}

static void
transaction_new_operation (FlatpakTransaction *transaction,
                           FlatpakTransactionOperation *op,
                           FlatpakTransactionProgress *progress,
                           TransactionData *d)
{
  d->progress = 0;
  if (d->saw_first_operation)
    d->op++;
  else
    d->saw_first_operation = TRUE;

  send_progress (d->out,
                 d->op,  d->n_ops,
                 d->progress, PROGRESS_STATUS_RUNNING,
                 NULL);

  g_signal_connect (progress, "changed", G_CALLBACK (transaction_progress_changed), d);
}

static gboolean
transaction_operation_error  (FlatpakTransaction            *transaction,
                              FlatpakTransactionOperation   *operation,
                              const GError                  *error,
                              FlatpakTransactionErrorDetails detail,
                              TransactionData *d)
{
  gboolean non_fatal = (detail & FLATPAK_TRANSACTION_ERROR_DETAILS_NON_FATAL) != 0;

  if (non_fatal)
    return TRUE;  /* Continue */

  send_progress (d->out,
                 d->op,  d->n_ops, d->progress,
                 PROGRESS_STATUS_ERROR,
                 error);

  return FALSE; /* This will cause us to return an ABORTED error */
}


static void
transaction_operation_done (FlatpakTransaction *transaction,
                            FlatpakTransactionOperation *op,
                            const char *commit,
                            FlatpakTransactionResult result,
                            TransactionData *d)
{
  d->progress = 100;

  send_progress (d->out,
                 d->op,  d->n_ops,
                 d->progress, PROGRESS_STATUS_RUNNING,
                 NULL);
}

static void
update_child_setup_func (gpointer user_data)
{
  int *socket = user_data;

  dup2 (*socket, 3);
  flatpak_close_fds_workaround (4);
}

/* This is the meat of the update process, its run out of process (via
   spawn) to avoid running lots of complicated code in the portal
   process and possibly long-term leaks in a long-running process. */
static int
do_update_child_process (const char *installation_path, const char *ref, int socket_fd)
{
  g_autoptr(GOutputStream) out = g_unix_output_stream_new (socket_fd, TRUE);
  g_autoptr(FlatpakInstallation) installation = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(GFile) f = g_file_new_for_path (installation_path);
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  TransactionData transaction_data = { NULL };

  dir = flatpak_dir_get_by_path (f);

  if (!flatpak_dir_maybe_ensure_repo (dir, NULL, &error))
    {
      send_progress (out, 0, 0, 0,
                     PROGRESS_STATUS_ERROR, error);
      return 0;
    }

  installation = flatpak_installation_new_for_dir (dir, NULL, &error);
  if (installation)
    transaction = flatpak_transaction_new_for_installation (installation, NULL, &error);
  if (transaction == NULL)
    {
      send_progress (out, 0, 0, 0,
                     PROGRESS_STATUS_ERROR, error);
      return 0;
    }

  flatpak_transaction_add_default_dependency_sources (transaction);

  if (!flatpak_transaction_add_update (transaction, ref, NULL, NULL, &error))
    {
      send_progress (out, 0, 0, 0,
                     PROGRESS_STATUS_ERROR, error);
      return 0;
    }

  transaction_data.out = out;

  g_signal_connect (transaction, "ready", G_CALLBACK (transaction_ready), &transaction_data);
  g_signal_connect (transaction, "new-operation", G_CALLBACK (transaction_new_operation), &transaction_data);
  g_signal_connect (transaction, "operation-done", G_CALLBACK (transaction_operation_done), &transaction_data);
  g_signal_connect (transaction, "operation-error", G_CALLBACK (transaction_operation_error), &transaction_data);

  if (!flatpak_transaction_run (transaction, NULL, &error))
    {
      if (!g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED)) /* If aborted we already sent error */
        send_progress (out, transaction_data.op, transaction_data.n_ops, transaction_data.progress,
                       PROGRESS_STATUS_ERROR, error);
      return 0;
    }

  send_progress (out, transaction_data.op, transaction_data.n_ops, transaction_data.progress,
                 PROGRESS_STATUS_DONE, error);

  return 0;
}

static GVariant *
read_variant (GInputStream *in,
              GCancellable *cancellable,
              GError **error)
{
  guint32 size;
  guchar *data;
  gsize bytes_read;

  if (!g_input_stream_read_all (in, &size, 4, &bytes_read, cancellable, error))
    return NULL;

  if (bytes_read != 4)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                   _("Update ended unexpectedly"));
      return NULL;
    }

  data = g_try_malloc (size);
  if (data == NULL)
    {
      flatpak_fail (error, "Out of memory");
      return NULL;
    }

  if (!g_input_stream_read_all (in, data, size, &bytes_read, cancellable, error))
    return NULL;

  if (bytes_read != size)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                   _("Update ended unexpectedly"));
      return NULL;
    }

  return g_variant_ref_sink (g_variant_new_from_data (G_VARIANT_TYPE("(uuuuss)"),
                                                      data, size, FALSE, g_free, data));
}

/* We do the actual update out of process (in do_update_child_process,
   via spawn) and just proxy the feedback here */
static gboolean
handle_update_responses (PortalFlatpakUpdateMonitor *monitor,
                         int socket_fd,
                         GError **error)
{
  g_autoptr(GInputStream) in = g_unix_input_stream_new (socket_fd, FALSE); /* Closed by parent */
  UpdateMonitorData *m = update_monitor_get_data (monitor);
  guint32 status;

  do
    {
      g_autoptr(GVariant) v = NULL;
      guint32 op;
      guint32 n_ops;
      guint32 progress;
      const char *error_name;
      const char *error_message;

      v = read_variant (in, m->cancellable, error);
      if (v == NULL)
        {
          g_debug ("Reading message from child update process failed %s", (*error)->message);
          return FALSE;
        }

      g_variant_get (v, "(uuuu&s&s)",
                     &op, &n_ops, &progress, &status, &error_name, &error_message);

      emit_progress (monitor, op, n_ops, progress, status,
                     *error_name != 0 ? error_name : NULL,
                     *error_message != 0 ? error_message : NULL);
    }
  while (status == PROGRESS_STATUS_RUNNING);

  /* Don't return an received error as we emited it already, that would cause it to be emitted twice */
  return TRUE;
}

static void
handle_update_in_thread_func (GTask *task,
                              gpointer source_object,
                              gpointer task_data,
                              GCancellable *cancellable)
{
  PortalFlatpakUpdateMonitor *monitor = source_object;
  UpdateMonitorData *m = update_monitor_get_data (monitor);
  g_autoptr(GError) error = NULL;
  const char *window;

  window = (const char *)g_object_get_data (G_OBJECT (task), "window");

  if (request_update_permissions_sync (monitor, m->name, window, &error))
    {
      g_autoptr(GFile) installation_path = update_monitor_get_installation_path (monitor);
      g_autofree char *ref = flatpak_build_app_ref (m->name, m->branch, m->arch);
      const char *argv[] = { "/proc/self/exe", "flatpak-portal", "--update", flatpak_file_get_path_cached (installation_path), ref, NULL };
      int sockets[2];
      GPid pid;

      if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0)
        {
          glnx_throw_errno (&error);
        }
      else
        {
          gboolean spawn_ok;

          spawn_ok = g_spawn_async (NULL, (char **)argv, NULL,
                                    G_SPAWN_FILE_AND_ARGV_ZERO |
                                    G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                                    update_child_setup_func, &sockets[1],
                                    &pid, &error);
          close (sockets[1]); // Close remote side
          if (spawn_ok)
            {
              if (!handle_update_responses (monitor, sockets[0], &error))
                {
                  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                    kill (pid, SIGINT);
                }
            }
          close (sockets[0]); // Close local side
        }
    }

  if (error)
    emit_progress_error (monitor, error);

  g_mutex_lock (&m->lock);
  m->installing = FALSE;
  g_mutex_unlock (&m->lock);
}

static gboolean
handle_update (PortalFlatpakUpdateMonitor *monitor,
               GDBusMethodInvocation *invocation,
               const char *arg_window,
               GVariant *arg_options)
{
  UpdateMonitorData *m = update_monitor_get_data (monitor);
  g_autoptr(GTask) task = NULL;
  gboolean already_installing = FALSE;

  g_debug ("handle UpdateMonitor.Update");

  g_mutex_lock (&m->lock);
  if (m->installing)
    already_installing = TRUE;
  else
    m->installing = TRUE;
  g_mutex_unlock (&m->lock);

  if (already_installing)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_FAILED,
                                             "Already installing");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  task = g_task_new (monitor, NULL, NULL, NULL);
  g_object_set_data_full (G_OBJECT (task), "window", g_strdup (arg_window), g_free);
  g_task_run_in_thread (task, handle_update_in_thread_func);

  portal_flatpak_update_monitor_complete_update (monitor, invocation);

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
          g_debug ("%s dropped off the bus, killing %d", pid_data->client, pid_data->pid);
          killpg (pid_data->pid, SIGINT);
        }

      g_list_free (list);

      close_update_monitors_for_sender (name);
    }
}

#define DBUS_NAME_DBUS "org.freedesktop.DBus"
#define DBUS_INTERFACE_DBUS DBUS_NAME_DBUS
#define DBUS_PATH_DBUS "/org/freedesktop/DBus"

static gboolean
supports_expose_pids (void)
{
  const char *path = g_find_program_in_path (flatpak_get_bwrap ());
  struct stat st;

  /* This is supported only if bwrap exists and is not setuid */
  return
    path != NULL &&
    stat (path, &st) == 0 &&
    (st.st_mode & S_ISUID) == 0;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  GError *error = NULL;

  g_debug ("Bus acquired, creating skeleton");

  g_dbus_connection_set_exit_on_close (connection, FALSE);

  update_monitors = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  permission_store = xdp_dbus_permission_store_proxy_new_sync (connection,
                                                               G_DBUS_PROXY_FLAGS_NONE,
                                                               "org.freedesktop.impl.portal.PermissionStore",
                                                               "/org/freedesktop/impl/portal/PermissionStore",
                                                               NULL, NULL);

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

  portal_flatpak_set_version (PORTAL_FLATPAK (portal), 6);
  portal_flatpak_set_supports (PORTAL_FLATPAK (portal), supports);

  g_signal_connect (portal, "handle-spawn", G_CALLBACK (handle_spawn), NULL);
  g_signal_connect (portal, "handle-spawn-signal", G_CALLBACK (handle_spawn_signal), NULL);
  g_signal_connect (portal, "handle-create-update-monitor", G_CALLBACK (handle_create_update_monitor), NULL);

  g_signal_connect (portal, "g-authorize-method", G_CALLBACK (authorize_method_handler), NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (portal),
                                         connection,
                                         FLATPAK_PORTAL_PATH,
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
    { "poll-timeout", 0, 0, G_OPTION_ARG_INT, &opt_poll_timeout,  "Delay in seconds between polls for updates.", NULL },
    { "poll-when-metered", 0, 0, G_OPTION_ARG_NONE, &opt_poll_when_metered, "Whether to check for updates on metered networks",  NULL },
    { NULL }
  };

  setlocale (LC_ALL, "");

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_prgname (argv[0]);

  g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, message_handler, NULL);

  if (argc >= 4 && strcmp (argv[1], "--update") == 0)
    {
      return do_update_child_process (argv[2], argv[3], 3);
    }

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

  if (opt_poll_timeout == 0)
    opt_poll_timeout = DEFAULT_UPDATE_POLL_TIMEOUT_SEC;

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

  if (supports_expose_pids ())
    supports |= FLATPAK_SPAWN_SUPPORT_FLAGS_EXPOSE_PIDS;

  flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
  if (replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  name_owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                  FLATPAK_PORTAL_BUS_NAME,
                                  flags,
                                  on_bus_acquired,
                                  on_name_acquired,
                                  on_name_lost,
                                  NULL,
                                  NULL);

  load_installed_portals (opt_verbose);

  /* Ensure we don't idle exit */
  schedule_idle_callback ();

  network_monitor = g_network_monitor_get_default ();

  main_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (main_loop);

  return 0;
}
