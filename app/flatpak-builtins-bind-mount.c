/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2024 Red Hat, Inc
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
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <glib/gi18n.h>

#include "libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils-private.h"
#include "flatpak-dbus-generated.h"
#include "flatpak-run-private.h"
#include "flatpak-instance.h"
#include <sys/capability.h>

#include <unistd.h>
#include <sys/syscall.h>
#include <sched.h>
#include <sys/socket.h>

#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#endif
#ifndef OPEN_TREE_CLONE
#define OPEN_TREE_CLONE 1
#endif
#ifndef OPEN_TREE_CLOEXEC
#define OPEN_TREE_CLOEXEC O_CLOEXEC
#endif
#ifndef AT_RECURSIVE
#define AT_RECURSIVE 0x8000
#endif

static GOptionEntry options[] = {
  { NULL }
};

static inline int
open_tree (int           dfd,
           const char   *filename,
           unsigned int  flags)
{
  return syscall(__NR_open_tree, dfd, filename, flags);
}

static inline int
move_mount (int           from_dfd,
            const char   *from_pathname,
            int           to_dfd,
            const char   *to_pathname,
            unsigned int  flags)
{
  return syscall(__NR_move_mount, from_dfd, from_pathname, to_dfd, to_pathname, flags);
}

typedef struct _FlatpakInstanceNamespaces
{
  int user_base_fd;
  int ipc_fd;
  int net_fd;
  int pid_fd;
  int mnt_fd;
  int user_fd;
} FlatpakInstanceNamespaces;

static gboolean
mount_detached_at_path (int          detached_mnt_fd,
                        const char  *path,
                        GError     **error)
{
  int res;

  res = move_mount (detached_mnt_fd, "",
                    AT_FDCWD, path,
                    MOVE_MOUNT_F_EMPTY_PATH);
  if (res < 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  return TRUE;
}

static gboolean
join_user_and_mnt_ns (FlatpakInstanceNamespaces  *nss,
                      GError                    **error)
{
  if (setns (nss->user_base_fd, 0) < 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  if (setns (nss->mnt_fd, 0) < 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  return TRUE;
}

static gboolean
send_fd (int      socket,
         int      fd,
         int     *errno_out,
         GError **error)
{
  struct msghdr msg = { 0 };
  struct cmsghdr *cmsg;
  struct iovec io = { .iov_base = "ABC", .iov_len = 3 };
  char buf[CMSG_SPACE (sizeof (fd))];

  memset (buf, '\0', sizeof (buf));

  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = buf;
  msg.msg_controllen = sizeof (buf);

  cmsg = CMSG_FIRSTHDR (&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN (sizeof (fd));

  *((int *) CMSG_DATA (cmsg)) = fd;

  msg.msg_controllen = CMSG_SPACE (sizeof (fd));

  if (TEMP_FAILURE_RETRY (sendmsg (socket, &msg, 0)) < 0)
    {
      if (errno_out)
        *errno_out = errno;
      glnx_set_error_from_errno (error);

      return FALSE;
    }

  return TRUE;
}

static int
receive_fd (int      socket,
            GError **error)
{
  struct msghdr msg = { 0 };
  struct cmsghdr *cmsg;
  char buf[256];
  char cbuf[256];
  struct iovec io = { .iov_base = buf, .iov_len = sizeof (buf) };
  unsigned char *data;
  int fd;

  msg.msg_iov = &io;
  msg.msg_iovlen = 1;

  msg.msg_control = cbuf;
  msg.msg_controllen = sizeof (cbuf);

  if (TEMP_FAILURE_RETRY (recvmsg (socket, &msg, 0)) < 0)
    {
      glnx_set_error_from_errno (error);
      return -1;
    }

  cmsg = CMSG_FIRSTHDR (&msg);
  data = CMSG_DATA (cmsg);
  fd = *((int *) data);

  return fd;
}

typedef struct _DetachedMountTreeData
{
  const char *path;
  int socket;
} DetachedMountTreeData;

static int
get_detached_mount_tree_in_ns (void *user_data)
{
  DetachedMountTreeData *data = user_data;
  int fd_mnt;
  int err;

  fd_mnt = open_tree (AT_FDCWD,
                      g_strdup (data->path),
                      OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_RECURSIVE);
  if (fd_mnt < 0)
    return errno;

  if (!send_fd (data->socket, fd_mnt, &err, NULL))
    return err;

  return 0;
}

#define STACK_SIZE (1024 * 1024)

static int
get_detached_mount_tree (const char  *path,
                         GError     **error)
{
  int res;
  int sockets[2];
  glnx_autofd int sock_snd = -1;
  glnx_autofd int sock_rcv = -1;
  g_autofree DetachedMountTreeData *data;
  char *stack;
  pid_t pid;
  glnx_autofd int detached_mount = -1;
  siginfo_t info;

  res = socketpair (AF_UNIX, SOCK_DGRAM, 0, sockets);
  if (res < 0)
    {
      glnx_set_error_from_errno (error);
      return -1;
    }

  sock_snd = sockets[0];
  sock_rcv = sockets[1];

  data = g_new0 (DetachedMountTreeData, 1);
  data->path = path;
  data->socket = sock_snd;

  stack = g_new (char, STACK_SIZE);
  stack = stack + STACK_SIZE;

  pid = clone (get_detached_mount_tree_in_ns,
               stack,
               SIGCHLD | CLONE_NEWUSER | CLONE_NEWNS,
               data);
  if (pid < 0)
    {
      glnx_set_error_from_errno (error);
      return -1;
    }

  detached_mount = receive_fd (sock_rcv, error);
  if (detached_mount < 0)
    return -1;

  if (TEMP_FAILURE_RETRY (waitid (P_PID, pid, &info, WEXITED)) < 0)
    {
      glnx_set_error_from_errno (error);
      return -1;
    }
  g_assert (info.si_pid == pid);

  if (info.si_status < 0)
    {
      g_set_error_literal (error, G_IO_ERROR,
                           g_io_error_from_errno (info.si_status),
                           g_strerror (info.si_status));
      return -1;
    }

  return glnx_steal_fd (&detached_mount);
}

static gboolean
get_namespaces (int                         pid,
                FlatpakInstanceNamespaces  *nss,
                GError                    **error)
{
  const char *ns_name[] = { "user_base", "ipc", "net", "pid", "mnt", "user" };
  int ns_fd[G_N_ELEMENTS (ns_name)];
  g_autofree char *root_path = NULL;
  ino_t user_base_ino = 0;
  int i;

  root_path = g_strdup_printf ("/proc/%d/root", pid);

  for (i = 0; i < G_N_ELEMENTS (ns_name); i++)
    {
      g_autofree char *path = NULL;
      g_autofree char *self_path = NULL;
      struct stat path_stat, self_path_stat;

      if (strcmp (ns_name[i], "user_base") == 0)
        {
          /* We could use the NS_GET_USERNS ioctl instead of the .userns bind
           * hack, but that would require >= 4.9 kernel */
          path = g_strdup_printf ("%s/run/.userns", root_path);
          self_path = g_strdup ("/proc/self/ns/user");
        }
      else
        {
          path = g_strdup_printf ("/proc/%d/ns/%s", pid, ns_name[i]);
          self_path = g_strdup_printf ("/proc/self/ns/%s", ns_name[i]);
        }

      if (stat (path, &path_stat) != 0)
        {
          if (errno == ENOENT)
            {
              /* If for whatever reason the namespace doesn't exist, skip it */
              ns_fd[i] = -1;
              continue;
            }
          return glnx_prefix_error (error,
                                    _("Invalid %s namespace for pid %d"),
                                    ns_name[i], pid);
        }

      if (strcmp (ns_name[i], "user") == 0 && path_stat.st_ino == user_base_ino)
        {
          /* bubblewrap did not create an intermediate user namespace */
          ns_fd[i] = -1;
          continue;
        }

      if (stat (self_path, &self_path_stat) != 0)
        {
          return glnx_prefix_error (error,
                                    _("Invalid %s namespace for self"),
                                    ns_name[i]);
        }

      if (self_path_stat.st_ino == path_stat.st_ino)
        {
          /* No need to setns to the same namespace, it will only fail */
          ns_fd[i] = -1;
          continue;
        }

      if (strcmp (ns_name[i], "user_base") == 0)
        user_base_ino = path_stat.st_ino;

      ns_fd[i] = open (path, O_RDONLY);
      if (ns_fd[i] == -1)
        {
          return flatpak_fail (error,
                               _("Can't open %s namespace: %s"),
                               ns_name[i], g_strerror (errno));
        }
    }

  nss->user_base_fd = ns_fd[0];
  nss->ipc_fd = ns_fd[1];
  nss->net_fd = ns_fd[2];
  nss->pid_fd = ns_fd[3];
  nss->mnt_fd = ns_fd[4];
  nss->user_fd = ns_fd[5];

  return TRUE;
}

static int
find_pid (const char *pid_s)
{
  g_autoptr(GPtrArray) instances = NULL;
  int i;
  int pid;

  pid = atoi (pid_s);

  /* Check to see if it matches some running instance, otherwise use
     as pid if it looks as a number. */
  instances = flatpak_instance_get_all ();
  for (i = 0; i < instances->len; i++)
    {
      FlatpakInstance *instance = (FlatpakInstance *) g_ptr_array_index (instances, i);

      if (pid == flatpak_instance_get_pid (instance) ||
          g_strcmp0 (pid_s, flatpak_instance_get_app (instance)) == 0 ||
          strcmp (pid_s, flatpak_instance_get_id (instance)) == 0)
        {
          pid = flatpak_instance_get_child_pid (instance);
          break;
        }
    }

  return pid;
}

gboolean
flatpak_builtin_bind_mount (int            argc,
                            char         **argv,
                            GCancellable  *cancellable,
                            GError       **error)
{
  g_autoptr(GOptionContext) context = NULL;
  char *pid_s;
  char *src_path;
  char *dst_path;
  int pid;
  FlatpakInstanceNamespaces nss;
  int mnt_fd;

  context = g_option_context_new (_(
    "INSTANCE SRC-PATH DST-PATH - "
    "Bind mount files and folders into a running sandbox"));

  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context,
                                     options,
                                     &argc,
                                     &argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR,
                                     NULL,
                                     cancellable,
                                     error))
    return FALSE;

  if (argc < 3)
    return usage_error (context, _("INSTANCE, SRC-PATH and DST-PATH must be specified"), error);

  pid_s = argv[1];
  src_path = argv[2];
  dst_path = argv[3];

  pid = find_pid (pid_s);

  if (pid <= 0)
    {
      return flatpak_fail (error,
                           _("%s is neither a pid nor an application or instance ID"),
                           pid_s);
    }

  if (!get_namespaces (pid, &nss, error))
    {
      return glnx_prefix_error (error,
                                _("Could not get the namespaces of the instance"));
    }

  mnt_fd = get_detached_mount_tree (src_path, error);
  if (mnt_fd < 0)
    {
      return glnx_prefix_error (error,
                                _("Could not create a detached mount from SRC-PATH (%s)"),
                                src_path);
    }

  if (!join_user_and_mnt_ns (&nss, error))
    {
      return glnx_prefix_error (error,
                                _("Could not join the target user and mount namespace"));
    }

  if (!mount_detached_at_path (mnt_fd, dst_path, error))
    {
      return glnx_prefix_error (error,
                                _("Could not bind mount to the target path in the sandbox"));
    }

  exit (0);
}

gboolean
flatpak_complete_bind_mount (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) instances = NULL;
  int i;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context,
                                     options,
                                     &completion->argc,
                                     &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR,
                                     NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1:
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      instances = flatpak_instance_get_all ();
      for (i = 0; i < instances->len; i++)
        {
          FlatpakInstance *instance = (FlatpakInstance *) g_ptr_array_index (instances, i);

          const char *app_name = flatpak_instance_get_app (instance);
          if (app_name)
            flatpak_complete_word (completion, "%s ", app_name);

          flatpak_complete_word (completion, "%s ", flatpak_instance_get_id (instance));
        }
      break;

    default:
      break;
    }

  return TRUE;
}
