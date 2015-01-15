/* xdg-app-helper
 * Copyright (C) 2014 Alexander Larsson
 *
 * This probram is free software; you can redistribute it and/or
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
 */

#define _GNU_SOURCE /* Required for CLONE_NEWNS */
#include <assert.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/loop.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#if 0
#define __debug__(x) printf x
#else
#define __debug__(x)
#endif

#define N_ELEMENTS(arr)		(sizeof (arr) / sizeof ((arr)[0]))

#define READ_END 0
#define WRITE_END 1

static void
die_with_error (const char *format, ...)
{
  va_list args;
  int errsv;

  errsv = errno;

  va_start (args, format);
  vfprintf (stderr, format, args);
  va_end (args);

  fprintf (stderr, ": %s\n", strerror (errsv));

  exit (1);
}

static void
die (const char *format, ...)
{
  va_list args;

  va_start (args, format);
  vfprintf (stderr, format, args);
  va_end (args);

  fprintf (stderr, "\n");

  exit (1);
}

static void *
xmalloc (size_t size)
{
  void *res = malloc (size);
  if (res == NULL)
    die ("oom");
  return res;
}

static char *
xstrdup (const char *str)
{
  char *res;

  assert (str != NULL);

  res = strdup (str);
  if (res == NULL)
    die ("oom");

  return res;
}

static void
xsetenv (const char *name, const char *value, int overwrite)
{
  if (setenv (name, value, overwrite))
    die ("setenv failed");
}

static void
xunsetenv (const char *name)
{
  if (unsetenv(name))
    die ("unsetenv failed");
}

char *
strconcat (const char *s1,
           const char *s2)
{
  size_t len = 0;
  char *res;

  if (s1)
    len += strlen (s1);
  if (s2)
    len += strlen (s2);

  res = xmalloc (len + 1);
  *res = 0;
  if (s1)
    strcat (res, s1);
  if (s2)
    strcat (res, s2);

  return res;
}

char *
strconcat_len (const char *s1,
               const char *s2,
               size_t s2_len)
{
  size_t len = 0;
  char *res;

  if (s1)
    len += strlen (s1);
  if (s2)
    len += s2_len;

  res = xmalloc (len + 1);
  *res = 0;
  if (s1)
    strcat (res, s1);
  if (s2)
    strncat (res, s2, s2_len);

  return res;
}

char*
strdup_printf (const char *format,
               ...)
{
  char *buffer = NULL;
  va_list args;

  va_start (args, format);
  vasprintf (&buffer, format, args);
  va_end (args);

  if (buffer == NULL)
    die ("oom");

  return buffer;
}

void
usage (char **argv)
{
  fprintf (stderr, "usage: %s [-n] [-i] [-p <pulsaudio socket>] [-x X11 socket] [-w] [-W] [-a <path to app>] [-v <path to var>] <path to runtime> <command..>\n", argv[0]);
  exit (1);
}

static int
pivot_root (const char * new_root, const char * put_old)
{
#ifdef __NR_pivot_root
  return syscall(__NR_pivot_root, new_root, put_old);
#else
  errno = ENOSYS;
  return -1;
#endif
}

typedef enum {
  FILE_TYPE_REGULAR,
  FILE_TYPE_DIR,
  FILE_TYPE_SYMLINK,
  FILE_TYPE_SYSTEM_SYMLINK,
  FILE_TYPE_BIND,
  FILE_TYPE_BIND_RO,
  FILE_TYPE_MOUNT,
  FILE_TYPE_DEVICE,
  FILE_TYPE_SHM,
} file_type_t;

typedef enum {
  FILE_FLAGS_NONE = 0,
  FILE_FLAGS_USER_OWNED = 1 << 0,
  FILE_FLAGS_NON_FATAL = 1 << 1,
  FILE_FLAGS_IF_LAST_FAILED = 1 << 2,
  FILE_FLAGS_DEVICES = 1 << 3,
} file_flags_t;

typedef struct {
    file_type_t type;
    const char *name;
    mode_t mode;
    const char *data;
    file_flags_t flags;
} create_table_t;

typedef struct {
    const char *what;
    const char *where;
    const char *type;
    const char *options;
    unsigned long flags;
} mount_table_t;

int
ascii_isdigit (char c)
{
  return c >= '0' && c <= '9';
}

static const create_table_t create[] = {
  { FILE_TYPE_DIR, ".oldroot", 0755 },
  { FILE_TYPE_DIR, "usr", 0755 },
  { FILE_TYPE_DIR, "tmp", 01777 },
  { FILE_TYPE_DIR, "self", 0755},
  { FILE_TYPE_DIR, "run", 0755},
  { FILE_TYPE_DIR, "run/dbus", 0755},
  { FILE_TYPE_DIR, "run/user", 0755},
  { FILE_TYPE_DIR, "run/user/%1$d", 0700, NULL, FILE_FLAGS_USER_OWNED },
  { FILE_TYPE_DIR, "run/user/%1$d/pulse", 0700, NULL, FILE_FLAGS_USER_OWNED },
  { FILE_TYPE_REGULAR, "run/user/%1$d/pulse/native", 0700, NULL, FILE_FLAGS_USER_OWNED },
  { FILE_TYPE_DIR, "var", 0755},
  { FILE_TYPE_SYMLINK, "var/tmp", 0755, "/tmp"},
  { FILE_TYPE_SYMLINK, "var/run", 0755, "/run"},
  { FILE_TYPE_SYSTEM_SYMLINK, "lib32", 0755, NULL},
  { FILE_TYPE_SYSTEM_SYMLINK, "lib64", 0755, NULL},
  { FILE_TYPE_SYSTEM_SYMLINK, "lib", 0755, "usr/lib"},
  { FILE_TYPE_SYSTEM_SYMLINK, "bin", 0755, "usr/bin" },
  { FILE_TYPE_SYSTEM_SYMLINK, "sbin", 0755, "usr/sbin"},
  { FILE_TYPE_SYSTEM_SYMLINK, "etc", 0755, "usr/etc"},
  { FILE_TYPE_DIR, "tmp/.X11-unix", 0755 },
  { FILE_TYPE_REGULAR, "tmp/.X11-unix/X99", 0755 },
  { FILE_TYPE_DIR, "proc", 0755},
  { FILE_TYPE_MOUNT, "proc"},
  { FILE_TYPE_BIND_RO, "proc/sys", 0755, "proc/sys"},
  { FILE_TYPE_DIR, "sys", 0755},
  { FILE_TYPE_MOUNT, "sys"},
  { FILE_TYPE_DIR, "dev", 0755},
  { FILE_TYPE_MOUNT, "dev"},
  { FILE_TYPE_DIR, "dev/pts", 0755},
  { FILE_TYPE_MOUNT, "dev/pts"},
  { FILE_TYPE_DIR, "dev/shm", 0755},
  { FILE_TYPE_SHM, "dev/shm"},
  { FILE_TYPE_DEVICE, "dev/null", S_IFCHR|0666, "/dev/null"},
  { FILE_TYPE_DEVICE, "dev/zero", S_IFCHR|0666, "/dev/zero"},
  { FILE_TYPE_DEVICE, "dev/full", S_IFCHR|0666, "/dev/full"},
  { FILE_TYPE_DEVICE, "dev/random", S_IFCHR|0666, "/dev/random"},
  { FILE_TYPE_DEVICE, "dev/urandom", S_IFCHR|0666, "/dev/urandom"},
  { FILE_TYPE_DEVICE, "dev/tty", S_IFCHR|0666, "/dev/tty"},
  { FILE_TYPE_DIR, "dev/dri", 0755},
  { FILE_TYPE_BIND, "dev/dri", 0755, "/dev/dri", FILE_FLAGS_NON_FATAL|FILE_FLAGS_DEVICES},
};

static const create_table_t create_post[] = {
  { FILE_TYPE_BIND, "usr/etc/machine-id", 0444, "/etc/machine-id", FILE_FLAGS_NON_FATAL},
  { FILE_TYPE_BIND, "usr/etc/machine-id", 0444, "/var/lib/dbus/machine-id", FILE_FLAGS_NON_FATAL | FILE_FLAGS_IF_LAST_FAILED},
};

static const mount_table_t mount_table[] = {
  { "proc",      "proc",     "proc",  NULL,        MS_NOSUID|MS_NOEXEC|MS_NODEV           },
  { "sysfs",     "sys",      "sysfs", NULL,        MS_RDONLY|MS_NOSUID|MS_NOEXEC|MS_NODEV },
  { "tmpfs",     "dev",      "tmpfs", "mode=755",  MS_NOSUID|MS_STRICTATIME               },
  { "devpts",    "dev/pts",  "devpts","newinstance,ptmxmode=0666,mode=620,gid=5", MS_NOSUID|MS_NOEXEC },
  { "tmpfs",     "dev/shm",  "tmpfs", "mode=1777", MS_NOSUID|MS_NODEV|MS_STRICTATIME      },
};

const char *dont_mount_in_root[] = {
  ".", "..", "lib", "lib32", "lib64", "bin", "sbin", "usr", "boot",
  "tmp", "etc", "self", "run", "proc", "sys", "dev", "var"
};

typedef enum {
  BIND_READONLY = (1<<0),
  BIND_PRIVATE = (1<<1),
  BIND_DEVICES = (1<<2),
  BIND_RECURSIVE = (1<<3),
} bind_option_t;

static int
bind_mount (const char *src, const char *dest, bind_option_t options)
{
  int readonly = (options & BIND_READONLY) != 0;
  int private = (options & BIND_PRIVATE) != 0;
  int devices = (options & BIND_DEVICES) != 0;
  int recursive = (options & BIND_RECURSIVE) != 0;

  if (mount (src, dest, NULL, MS_MGC_VAL|MS_BIND|(recursive?MS_REC:0), NULL) != 0)
    return 1;

  if (private)
    {
      if (mount ("none", dest,
                 NULL, MS_REC|MS_PRIVATE, NULL) != 0)
        return 2;
    }

  if (mount ("none", dest,
             NULL, MS_MGC_VAL|MS_BIND|MS_REMOUNT|(devices?0:MS_NODEV)|MS_NOSUID|(readonly?MS_RDONLY:0), NULL) != 0)
    return 3;

  return 0;
}

static int
mkdir_with_parents (const char *pathname,
                    int         mode,
                    int         uid)
{
  char *fn, *p;
  struct stat buf;

  if (pathname == NULL || *pathname == '\0')
    {
      errno = EINVAL;
      return 1;
    }

  fn = xstrdup (pathname);

  p = fn;
  while (*p == '/')
    p++;

  do
    {
      while (*p && *p != '/')
        p++;

      if (!*p)
        p = NULL;
      else
        *p = '\0';

      if (stat (fn, &buf) !=  0)
        {
          if (mkdir (fn, mode) == -1 && errno != EEXIST)
            {
              int errsave = errno;
              free (fn);
              errno = errsave;
              return -1;
            }
          if (chown (fn, uid, -1))
            return -1;
        }
      else if (!S_ISDIR (buf.st_mode))
        {
          free (fn);
          errno = ENOTDIR;
          return -1;
        }

      if (p)
        {
          *p++ = '/';
          while (*p && *p == '/')
            p++;
        }
    }
  while (p);

  free (fn);

  return 0;
}


static int
create_file (const char *path, mode_t mode, const char *content)
{
  int fd;

  fd = creat (path, mode);
  if (fd == -1)
    return -1;

  if (content)
    {
      ssize_t len = strlen (content);
      ssize_t res;

      while (len > 0)
        {
          res = write (fd, content, len);
          if (res < 0 && errno == EINTR)
            continue;
          if (res <= 0)
            {
              close (fd);
              return -1;
            }
          len -= res;
          content += res;
        }
    }

  close (fd);

  return 0;
}

static void
create_files (const create_table_t *create, int n_create, int ignore_shm, int system_mode)
{
  int last_failed = 0;
  int i;

  for (i = 0; i < n_create; i++)
    {
      char *name = strdup_printf (create[i].name, getuid());
      mode_t mode = create[i].mode;
      const char *data = create[i].data;
      file_flags_t flags = create[i].flags;
      struct stat st;
      int k;
      int found;
      int res;

      if ((flags & FILE_FLAGS_IF_LAST_FAILED) &&
          !last_failed)
        continue;

      last_failed = 0;

      switch (create[i].type)
        {
        case FILE_TYPE_DIR:
          if (mkdir (name, mode) != 0)
            die_with_error ("creating dir %s", name);
          break;

        case FILE_TYPE_REGULAR:
          if (create_file (name, mode, NULL))
            die_with_error ("creating file %s", name);
          break;

        case FILE_TYPE_SYSTEM_SYMLINK:
	  if (system_mode)
	    {
	      char *in_root = strconcat ("/", name);
	      struct stat buf;

	      if (stat (in_root, &buf) ==  0)
		{
		  if (mkdir (name, mode) != 0)
		    die_with_error ("creating dir %s", name);

		  if (bind_mount (in_root, name, BIND_PRIVATE | BIND_READONLY))
		    die_with_error ("mount %s", name);
		}

	      free (in_root);

	      break;
	    }

	  if (data == NULL)
	    break;

	  /* else Fall through */

        case FILE_TYPE_SYMLINK:
          if (symlink (data, name) != 0)
            die_with_error ("creating symlink %s", name);
          break;

        case FILE_TYPE_BIND:
        case FILE_TYPE_BIND_RO:
          if ((res = bind_mount (data, name,
                                 0 |
                                 ((create[i].type == FILE_TYPE_BIND_RO) ? BIND_READONLY : 0) |
                                 ((flags & FILE_FLAGS_DEVICES) ? BIND_DEVICES : 0))))
            {
              if (res > 1 || (flags & FILE_FLAGS_NON_FATAL) == 0)
                die_with_error ("mounting bindmount %s", name);
              last_failed = 1;
            }

          break;

        case FILE_TYPE_SHM:
          if (ignore_shm)
            break;

          /* NOTE: Fall through, treat as mount */
        case FILE_TYPE_MOUNT:
          found = 0;
          for (k = 0; k < N_ELEMENTS(mount_table); k++)
            {
              if (strcmp (mount_table[k].where, name) == 0)
                {
                  if (mount(mount_table[k].what,
                            mount_table[k].where,
                            mount_table[k].type,
                            mount_table[k].flags,
                            mount_table[k].options) < 0)
                    die_with_error ("Mounting %s", name);
                  found = 1;
                }
            }

          if (!found)
            die ("Unable to find mount %s\n", name);

          break;

        case FILE_TYPE_DEVICE:
          if (stat (data, &st) < 0)
            die_with_error ("stat node %s", data);

          if (!S_ISCHR (st.st_mode) && !S_ISBLK (st.st_mode))
            die_with_error ("node %s is not a device", data);

          if (mknod (name, mode, st.st_rdev) < 0)
            die_with_error ("mknod %s", name);

          break;

        default:
          die ("Unknown create type %d\n", create[i].type);
        }

      if (flags & FILE_FLAGS_USER_OWNED)
        {
          if (chown (name, getuid(), -1))
            die_with_error ("chown to user");
        }

      free (name);
    }
}

static void
mount_extra_root_dirs (void)
{
  DIR *dir;
  struct dirent *dirent;
  int i;

  /* Bind mount most dirs in / into the new root */
  dir = opendir("/");
  if (dir != NULL)
    {
      while ((dirent = readdir(dir)))
        {
          int dont_mount = 0;
          char *path;
          struct stat st;

          for (i = 0; i < N_ELEMENTS(dont_mount_in_root); i++)
            {
              if (strcmp (dirent->d_name, dont_mount_in_root[i]) == 0)
                {
                  dont_mount = 1;
                  break;
                }
            }

          if (dont_mount)
            continue;

          path = strconcat ("/", dirent->d_name);

          if (stat (path, &st) != 0)
            {
              free (path);
              continue;
            }

          if (S_ISDIR(st.st_mode))
            {
              if (mkdir (dirent->d_name, 0755) != 0)
                die_with_error (dirent->d_name);

              if (bind_mount (path, dirent->d_name, BIND_RECURSIVE))
                die_with_error ("mount root subdir %s", dirent->d_name);
            }

          free (path);
        }
    }
}

static void
create_homedir (int do_mount)
{
  const char *home;
  const char *relative_home;

  home = getenv("HOME");
  if (home == NULL)
    return;

  relative_home = home;
  while (*relative_home == '/')
    relative_home++;

  if (mkdir_with_parents (relative_home, 0700, getuid()))
    die_with_error ("unable to create %s", relative_home);

  if (do_mount)
    {
      if (bind_mount (home, relative_home, BIND_RECURSIVE))
        die_with_error ("unable to mount %s", home);
    }
}

static void *
add_rta (struct nlmsghdr *header, int type, size_t size)
{
  struct rtattr *rta;
  size_t rta_size = RTA_LENGTH(size);

  rta = (struct rtattr*)((char *)header + NLMSG_ALIGN(header->nlmsg_len));
  rta->rta_type = type;
  rta->rta_len = rta_size;

  header->nlmsg_len = NLMSG_ALIGN(header->nlmsg_len) + rta_size;

  return RTA_DATA(rta);
}

static int
rtnl_send_request (int rtnl_fd, struct nlmsghdr *header)
{
  struct sockaddr_nl dst_addr = { AF_NETLINK, 0 };
  ssize_t sent;

  sent = sendto (rtnl_fd, (void *)header, header->nlmsg_len, 0,
                 (struct sockaddr *)&dst_addr, sizeof (dst_addr));
  if (sent < 0)
    return 1;

  return 0;
}

static int
rtnl_read_reply (int rtnl_fd, int seq_nr)
{
  char buffer[1024];
  ssize_t received;
  struct nlmsghdr *rheader;

  while (1)
    {
      received = recv (rtnl_fd, buffer, sizeof(buffer), 0);
      if (received < 0)
        return 1;

      rheader = (struct nlmsghdr *)buffer;
      while (received >= NLMSG_HDRLEN)
        {
          if (rheader->nlmsg_seq != seq_nr)
            return 1;
          if (rheader->nlmsg_pid != getpid ())
            return 1;
          if (rheader->nlmsg_type == NLMSG_ERROR)
            {
              uint32_t *err = NLMSG_DATA(rheader);
              if (*err == 0)
                return 0;

              return 1;
            }
          if (rheader->nlmsg_type == NLMSG_DONE)
            return 0;

          rheader = NLMSG_NEXT(rheader, received);
        }
    }
}

static int
rtnl_do_request (int rtnl_fd, struct nlmsghdr *header)
{
  if (!rtnl_send_request (rtnl_fd, header))
    return 1;

  if (!rtnl_read_reply (rtnl_fd, header->nlmsg_seq))
    return 1;

  return 0;
}

static struct nlmsghdr *
rtnl_setup_request (char *buffer, int type, int flags, size_t size)
{
  struct nlmsghdr *header;
  size_t len = NLMSG_LENGTH (size);
  static uint32_t counter = 0;

  memset (buffer, 0, len);

  header = (struct nlmsghdr *)buffer;
  header->nlmsg_len = len;
  header->nlmsg_type = type;
  header->nlmsg_flags = flags | NLM_F_REQUEST;
  header->nlmsg_seq = counter++;
  header->nlmsg_pid = getpid ();

  return (struct nlmsghdr *)header;
}

static int
loopback_setup (void)
{
  int r, if_loopback;
  int rtnl_fd = -1;
  char buffer[1024];
  struct sockaddr_nl src_addr = { AF_NETLINK, 0 };
  struct nlmsghdr *header;
  struct ifaddrmsg *addmsg;
  struct ifinfomsg *infomsg;
  struct in_addr *ip_addr;
  int res = 1;

  src_addr.nl_pid = getpid ();

  if_loopback = (int) if_nametoindex ("lo");
  if (if_loopback <= 0)
    goto error;

  rtnl_fd = socket (PF_NETLINK, SOCK_RAW|SOCK_CLOEXEC, NETLINK_ROUTE);
  if (rtnl_fd < 0)
    goto error;

  r = bind (rtnl_fd, (struct sockaddr *)&src_addr, sizeof (src_addr));
  if (r < 0)
    goto error;

  header = rtnl_setup_request (buffer, RTM_NEWADDR,
                               NLM_F_CREATE|NLM_F_EXCL|NLM_F_ACK,
                               sizeof (struct ifaddrmsg));
  addmsg = NLMSG_DATA(header);

  addmsg->ifa_family = AF_INET;
  addmsg->ifa_prefixlen = 8;
  addmsg->ifa_flags = IFA_F_PERMANENT;
  addmsg->ifa_scope = RT_SCOPE_HOST;
  addmsg->ifa_index = if_loopback;

  ip_addr = add_rta (header, IFA_LOCAL, sizeof (*ip_addr));
  ip_addr->s_addr = htonl(INADDR_LOOPBACK);

  ip_addr = add_rta (header, IFA_ADDRESS, sizeof (*ip_addr));
  ip_addr->s_addr = htonl(INADDR_LOOPBACK);

  assert (header->nlmsg_len < sizeof (buffer));

  if (rtnl_do_request (rtnl_fd, header))
    goto error;

  header = rtnl_setup_request (buffer, RTM_NEWLINK,
                               NLM_F_ACK,
                               sizeof (struct ifinfomsg));
  infomsg = NLMSG_DATA(header);

  infomsg->ifi_family = AF_UNSPEC;
  infomsg->ifi_type = 0;
  infomsg->ifi_index = if_loopback;
  infomsg->ifi_flags = IFF_UP;
  infomsg->ifi_change = IFF_UP;

  assert (header->nlmsg_len < sizeof (buffer));

  if (rtnl_do_request (rtnl_fd, header))
    goto error;

  res = 0;

 error:
  if (rtnl_fd != -1)
    close (rtnl_fd);

  return res;
}

int
main (int argc,
      char **argv)
{
  int res;
  mode_t old_umask;
  char *newroot;
  int pipefd[2];
  uid_t saved_euid;
  pid_t pid;
  int system_mode = 0;
  char *runtime_path = NULL;
  char *app_path = NULL;
  char *var_path = NULL;
  char *pulseaudio_socket = NULL;
  char *x11_socket = NULL;
  char *system_dbus_socket = NULL;
  char *session_dbus_socket = NULL;
  char *xdg_runtime_dir;
  char **args;
  int n_args;
  int share_shm = 0;
  int network = 0;
  int ipc = 0;
  int mount_host_fs = 0;
  int mount_home = 0;
  int writable = 0;
  int writable_app = 0;
  int writable_exports = 0;
  char old_cwd[256];

  char tmpdir[] = "/tmp/run-app.XXXXXX";

  args = &argv[1];
  n_args = argc - 1;

  while (n_args > 0 && args[0][0] == '-')
    {
      switch (args[0][1])
        {
        case 'i':
          ipc = 1;
          args += 1;
          n_args -= 1;
          break;

        case 'n':
          network = 1;
          args += 1;
          n_args -= 1;
          break;

        case 'W':
          writable = 1;
          args += 1;
          n_args -= 1;
          break;

        case 'w':
          writable_app = 1;
          args += 1;
          n_args -= 1;
          break;

        case 'e':
          writable_exports = 1;
          args += 1;
          n_args -= 1;
          break;

        case 's':
          share_shm = 1;
          args += 1;
          n_args -= 1;
          break;

        case 'f':
          mount_host_fs = 1;
          args += 1;
          n_args -= 1;
          break;

        case 'H':
          mount_home = 1;
          args += 1;
          n_args -= 1;
          break;

        case 'a':
          if (n_args < 2)
              usage (argv);

          app_path = args[1];
          args += 2;
          n_args -= 2;
          break;

        case 'p':
          if (n_args < 2)
              usage (argv);

          pulseaudio_socket = args[1];
          args += 2;
          n_args -= 2;
          break;

        case 'x':
          if (n_args < 2)
              usage (argv);

          x11_socket = args[1];
          args += 2;
          n_args -= 2;
          break;

        case 'd':
          if (n_args < 2)
              usage (argv);

          session_dbus_socket = args[1];
          args += 2;
          n_args -= 2;
          break;

        case 'D':
          if (n_args < 2)
              usage (argv);

          system_dbus_socket = args[1];
          args += 2;
          n_args -= 2;
          break;

        case 'v':
          if (n_args < 2)
              usage (argv);

          var_path = args[1];
          args += 2;
          n_args -= 2;
          break;

        default:
          usage (argv);
        }
    }

  if (n_args < 2)
    usage (argv);

  runtime_path = args[0];
  args++;
  n_args--;

  if (strcmp (runtime_path, "/usr") == 0)
    system_mode = 1;

  /* The initial code is run with a high permission euid
     (at least CAP_SYS_ADMIN), so take lots of care. */

  __debug__(("Creating temporary dir\n"));

  saved_euid = geteuid ();

  /* First switch to the real user id so we can have the
     temp directories owned by the user */

  if (seteuid (getuid ()))
    die_with_error ("seteuid to user");

  if (mkdtemp (tmpdir) == NULL)
    die_with_error ("Creating %s", tmpdir);

  newroot = strconcat (tmpdir, "/root");

  if (mkdir (newroot, 0755))
    die_with_error ("Creating new root failed");

  /* Now switch back to the root user */
  if (seteuid (saved_euid))
    die_with_error ("seteuid to privileged");

  /* We want to make the temp directory a bind mount so that
     we can ensure that it is MS_PRIVATE, so mount don't leak out
     of the namespace, and also so that pivot_root() succeeds. However
     this means if /tmp is MS_SHARED the bind-mount will be propagated
     to the parent namespace. In order to handle this we spawn a child
     in the original namespace and unmount the bind mount from that at
     the right time. */

  if (pipe (pipefd) != 0)
    die_with_error ("pipe failed");

  pid = fork();
  if (pid == -1)
    die_with_error ("fork failed");

  if (pid == 0)
    {
      char c;

      /* In child */
      close (pipefd[WRITE_END]);

      /* Don't die when the parent closes pipe */
      signal (SIGPIPE, SIG_IGN);

      /* Wait for parent */
      read (pipefd[READ_END], &c, 1);

      /* Unmount tmpdir bind mount */
      umount2 (tmpdir, MNT_DETACH);

      exit (0);
    }

  close (pipefd[READ_END]);

  __debug__(("creating new namespace\n"));
  res = unshare (CLONE_NEWNS |
                 (network ? 0 : CLONE_NEWNET) |
                 (ipc ? 0 : CLONE_NEWIPC));
  if (res != 0)
    die_with_error ("Creating new namespace failed");

  old_umask = umask (0);

  /* make it tmpdir rprivate to avoid leaking mounts */
  if (mount (tmpdir, tmpdir, NULL, MS_BIND, NULL) != 0)
    die_with_error ("Failed to make bind mount on tmpdir");
  if (mount (tmpdir, tmpdir, NULL, MS_REC|MS_PRIVATE, NULL) != 0)
    die_with_error ("Failed to make tmpdir rprivate");

  /* Create a tmpfs which we will use as / in the namespace */
  if (mount ("", newroot, "tmpfs", MS_NODEV|MS_NOEXEC|MS_NOSUID, NULL) != 0)
    die_with_error ("Failed to mount tmpfs");

  getcwd (old_cwd, sizeof (old_cwd));

  if (chdir (newroot) != 0)
      die_with_error ("chdir");

  create_files (create, N_ELEMENTS (create), share_shm, system_mode);

  if (share_shm)
    {
      if (bind_mount ("/dev/shm", "dev/shm", BIND_DEVICES))
        die_with_error ("mount /dev/shm");
    }

  if (bind_mount (runtime_path, "usr", BIND_PRIVATE | (writable?0:BIND_READONLY)))
    die_with_error ("mount usr");

  if (app_path != NULL)
    {
      if (bind_mount (app_path, "self", BIND_PRIVATE | (writable_app?0:BIND_READONLY)))
        die_with_error ("mount self");

      if (!writable_app && writable_exports)
	{
	  char *exports = strconcat (app_path, "/exports");

	  if (bind_mount (exports, "self/exports", BIND_PRIVATE))
	    die_with_error ("mount self/exports");

	  free (exports);
	}

    }

  if (var_path != NULL)
    {
      if (bind_mount (var_path, "var", BIND_PRIVATE))
        die_with_error ("mount var");
    }

  create_files (create_post, N_ELEMENTS (create_post), share_shm, system_mode);

  /* /usr now mounted private inside the namespace, tell child process to unmount the tmpfs in the parent namespace. */
  close (pipefd[WRITE_END]);

  if (bind_mount ("/etc/passwd", "etc/passwd", BIND_READONLY))
    die_with_error ("mount passwd");

  if (bind_mount ("/etc/group", "etc/group", BIND_READONLY))
    die_with_error ("mount group");

  /* Bind mount in X socket
   * This is a bit iffy, as Xlib typically uses abstract unix domain sockets
   * to connect to X, but that is not namespaced. We instead set DISPLAY=99
   * and point /tmp/.X11-unix/X99 to the right X socket. Any Xserver listening
   * to global abstract unix domain sockets are still accessible to the app
   * though...
   */
  if (x11_socket)
    {
      struct stat st;

      if (stat (x11_socket, &st) == 0 && S_ISSOCK (st.st_mode))
        {
          if (bind_mount (x11_socket, "tmp/.X11-unix/X99", 0))
            die ("can't bind X11 socket");

          xsetenv ("DISPLAY", ":99.0", 1);
        }
      else
        {
          xunsetenv ("DISPLAY");
        }
    }

  if (pulseaudio_socket != NULL)
    {
      char *pulse_path_relative = strdup_printf ("run/user/%d/pulse/native", getuid());
      char *pulse_server = strdup_printf ("unix:/run/user/%d/pulse/native", getuid());
      char *config_path_relative = strdup_printf ("run/user/%d/pulse/config", getuid());
      char *config_path_absolute = strdup_printf ("/run/user/%d/pulse/config", getuid());
      char *client_config = strdup_printf ("enable-shm=%s\n", share_shm ? "yes" : "no");

      if (create_file (config_path_relative, 0666, client_config) == 0 &&
          bind_mount (pulseaudio_socket, pulse_path_relative, BIND_READONLY) == 0)
        {
          xsetenv ("PULSE_SERVER", pulse_server, 1);
          xsetenv ("PULSE_CLIENTCONFIG", config_path_absolute, 1);
        }
      else
        {
          xunsetenv ("PULSE_SERVER");
        }

      free (pulse_path_relative);
      free (pulse_server);
      free (config_path_relative);
      free (config_path_absolute);
      free (client_config);
   }

  if (system_dbus_socket != NULL)
    {
      if (create_file ("run/dbus/system_bus_socket", 0666, NULL) == 0 &&
          bind_mount (system_dbus_socket, "run/dbus/system_bus_socket", 0) == 0)
        xsetenv ("DBUS_SYSTEM_BUS_ADDRESS",  "unix:path=/var/run/dbus/system_bus_socket", 1);
      else
        xunsetenv ("DBUS_SYSTEM_BUS_ADDRESS");
   }

  if (session_dbus_socket != NULL)
    {
      char *session_dbus_socket_path_relative = strdup_printf ("run/user/%d/bus", getuid());
      char *session_dbus_address = strdup_printf ("unix:path=/run/user/%d/bus", getuid());

      if (create_file (session_dbus_socket_path_relative, 0666, NULL) == 0 &&
          bind_mount (session_dbus_socket, session_dbus_socket_path_relative, 0) == 0)
        xsetenv ("DBUS_SESSION_BUS_ADDRESS",  session_dbus_address, 1);
      else
        xunsetenv ("DBUS_SESSION_BUS_ADDRESS");

      free (session_dbus_socket_path_relative);
      free (session_dbus_address);
   }

  if (mount_host_fs)
    mount_extra_root_dirs ();

  create_homedir (!mount_host_fs && mount_home);

  if (!network)
    loopback_setup ();

  if (pivot_root (newroot, ".oldroot"))
    die_with_error ("pivot_root");

  chdir ("/");

  /* The old root better be rprivate or we will send unmount events to the parent namespace */
  if (mount (".oldroot", ".oldroot", NULL, MS_REC|MS_PRIVATE, NULL) != 0)
    die_with_error ("Failed to make old root rprivate");

  if (umount2 (".oldroot", MNT_DETACH))
    die_with_error ("unmount oldroot");

  umask (old_umask);

  /* Now we have everything we need CAP_SYS_ADMIN for, so drop setuid */
  setuid (getuid ());

  chdir (old_cwd);

  xsetenv ("PATH", "/self/bin:/usr/bin", 1);
  xsetenv ("LD_LIBRARY_PATH", "/self/lib", 1);
  xsetenv ("XDG_CONFIG_DIRS","/self/etc/xdg:/etc/xdg", 1);
  xsetenv ("XDG_DATA_DIRS", "/self/share:/usr/share", 1);
  xdg_runtime_dir = strdup_printf ("/run/user/%d", getuid());
  xsetenv ("XDG_RUNTIME_DIR", xdg_runtime_dir, 1);
  free (xdg_runtime_dir);

  __debug__(("launch executable %s\n", args[0]));

  if (execvp (args[0], args) == -1)
    die_with_error ("execvp %s", args[0]);

  return 0;
}
