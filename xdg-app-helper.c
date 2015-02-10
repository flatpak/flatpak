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
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/capability.h>
#include <sys/prctl.h>
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
strconcat3 (const char *s1,
	    const char *s2,
	    const char *s3)
{
  size_t len = 0;
  char *res;

  if (s1)
    len += strlen (s1);
  if (s2)
    len += strlen (s2);
  if (s3)
    len += strlen (s3);

  res = xmalloc (len + 1);
  *res = 0;
  if (s1)
    strcat (res, s1);
  if (s2)
    strcat (res, s2);
  if (s3)
    strcat (res, s3);

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

static inline int raw_clone(unsigned long flags, void *child_stack) {
#if defined(__s390__) || defined(__CRIS__)
        /* On s390 and cris the order of the first and second arguments
         * of the raw clone() system call is reversed. */
        return (int) syscall(__NR_clone, child_stack, flags);
#else
        return (int) syscall(__NR_clone, flags, child_stack);
#endif
}

void
usage (char **argv)
{
  fprintf (stderr, "usage: %s [-n] [-i] [-p <pulsaudio socket>] [-x X11 socket] [-y Wayland socket] [-w] [-W] [-E] [-l] [-m <path to monitor dir>] [-a <path to app>] [-v <path to var>] [-b <target-dir>=<src-dir>] <path to runtime> <command..>\n", argv[0]);
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
  FILE_TYPE_REMOUNT,
  FILE_TYPE_DEVICE,
  FILE_TYPE_SHM,
} file_type_t;

typedef enum {
  FILE_FLAGS_NONE = 0,
  FILE_FLAGS_NON_FATAL = 1 << 0,
  FILE_FLAGS_IF_LAST_FAILED = 1 << 1,
  FILE_FLAGS_DEVICES = 1 << 2,
} file_flags_t;

typedef struct {
  file_type_t type;
  const char *name;
  mode_t mode;
  const char *data;
  file_flags_t flags;
  int *option;
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

static int create_etc_symlink = 0;
static int create_etc_dir = 1;
static int create_monitor_links = 0;

static const create_table_t create[] = {
  { FILE_TYPE_DIR, ".oldroot", 0755 },
  { FILE_TYPE_DIR, "usr", 0755 },
  { FILE_TYPE_DIR, "tmp", 01777 },
  { FILE_TYPE_DIR, "self", 0755},
  { FILE_TYPE_DIR, "run", 0755},
  { FILE_TYPE_DIR, "run/dbus", 0755},
  { FILE_TYPE_DIR, "run/user", 0755},
  { FILE_TYPE_DIR, "run/user/%1$d", 0700, NULL},
  { FILE_TYPE_DIR, "run/user/%1$d/pulse", 0700, NULL},
  { FILE_TYPE_DIR, "run/user/%1$d/dconf", 0700, NULL},
  { FILE_TYPE_DIR, "run/user/%1$d/xdg-app-monitor", 0700, NULL},
  { FILE_TYPE_REGULAR, "run/user/%1$d/pulse/native", 0700, NULL},
  { FILE_TYPE_DIR, "var", 0755},
  { FILE_TYPE_SYMLINK, "var/tmp", 0755, "/tmp"},
  { FILE_TYPE_SYMLINK, "var/run", 0755, "/run"},
  { FILE_TYPE_SYSTEM_SYMLINK, "lib32", 0755, NULL},
  { FILE_TYPE_SYSTEM_SYMLINK, "lib64", 0755, NULL},
  { FILE_TYPE_SYSTEM_SYMLINK, "lib", 0755, "usr/lib"},
  { FILE_TYPE_SYSTEM_SYMLINK, "bin", 0755, "usr/bin" },
  { FILE_TYPE_SYSTEM_SYMLINK, "sbin", 0755, "usr/sbin"},
  { FILE_TYPE_SYMLINK, "etc", 0755, "usr/etc", 0, &create_etc_symlink},
  { FILE_TYPE_DIR, "etc", 0755, NULL, 0, &create_etc_dir},
  { FILE_TYPE_REGULAR, "etc/passwd", 0755, NULL, 0, &create_etc_dir},
  { FILE_TYPE_REGULAR, "etc/group", 0755, NULL, 0, &create_etc_dir},
  { FILE_TYPE_SYMLINK, "etc/resolv.conf", 0755, "/run/user/%1$d/xdg-app-monitor/resolv.conf", 0, &create_monitor_links},
  { FILE_TYPE_REGULAR, "etc/machine-id", 0755, NULL, 0, &create_etc_dir},
  { FILE_TYPE_DIR, "tmp/.X11-unix", 0755 },
  { FILE_TYPE_REGULAR, "tmp/.X11-unix/X99", 0755 },
  { FILE_TYPE_DIR, "proc", 0755},
  { FILE_TYPE_MOUNT, "proc"},
  { FILE_TYPE_BIND_RO, "proc/sys", 0755, "proc/sys"},
  { FILE_TYPE_BIND_RO, "proc/sysrq-trigger", 0755, "proc/sysrq-trigger"},
  { FILE_TYPE_BIND_RO, "proc/irq", 0755, "proc/irq"},
  { FILE_TYPE_BIND_RO, "proc/bus", 0755, "proc/bus"},
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
  { FILE_TYPE_BIND_RO, "dev/dri", 0755, "/dev/dri", FILE_FLAGS_NON_FATAL|FILE_FLAGS_DEVICES},
  { FILE_TYPE_REMOUNT, "dev", MS_RDONLY|MS_NOSUID|MS_NOEXEC},
};

/* warning: Don't create any actual files here, as we could potentially
   write over bind mounts to the system */
static const create_table_t create_post[] = {
  { FILE_TYPE_BIND_RO, "etc/passwd", 0444, "/etc/passwd", 0},
  { FILE_TYPE_BIND_RO, "etc/group", 0444, "/etc/group", 0},
  { FILE_TYPE_BIND_RO, "etc/machine-id", 0444, "/etc/machine-id", FILE_FLAGS_NON_FATAL},
  { FILE_TYPE_BIND_RO, "etc/machine-id", 0444, "/var/lib/dbus/machine-id", FILE_FLAGS_NON_FATAL | FILE_FLAGS_IF_LAST_FAILED},
};

static const mount_table_t mount_table[] = {
  { "proc",      "proc",     "proc",  NULL,        MS_NOSUID|MS_NOEXEC|MS_NODEV           },
  { "sysfs",     "sys",      "sysfs", NULL,        MS_RDONLY|MS_NOSUID|MS_NOEXEC|MS_NODEV },
  { "tmpfs",     "dev",      "tmpfs", "mode=755",  MS_NOSUID|MS_STRICTATIME               },
  { "devpts",    "dev/pts",  "devpts","newinstance,ptmxmode=0666,mode=620,gid=5", MS_NOSUID|MS_NOEXEC },
  { "tmpfs",     "dev/shm",  "tmpfs", "mode=1777", MS_NOSUID|MS_NODEV|MS_STRICTATIME      },
};

const char *dont_mount_in_root[] = {
  ".", "..", "lib", "lib32", "lib64", "bin", "sbin", "usr", "boot", "root",
  "tmp", "etc", "self", "run", "proc", "sys", "dev", "var"
};

typedef enum {
  BIND_READONLY = (1<<0),
  BIND_PRIVATE = (1<<1),
  BIND_DEVICES = (1<<2),
  BIND_RECURSIVE = (1<<3),
} bind_option_t;

#define MAX_EXTRA_DIRS 32
#define MAX_LOCK_DIRS (MAX_EXTRA_DIRS+2)

static int n_lock_dirs = 0;
static const char *lock_dirs[MAX_LOCK_DIRS];

static void
lock_dir (const char *dir)
{
  char *file = strconcat3 ("/", dir, "/.ref");
  struct flock lock = {0};
  int fd;

  fd = open (file, O_RDONLY | O_CLOEXEC);
  free (file);
  if (fd != -1)
    {
      lock.l_type = F_RDLCK;
      lock.l_whence = SEEK_SET;
      lock.l_start = 0;
      lock.l_len = 0;

      if (fcntl(fd, F_SETLK, &lock) < 0)
	{
	  printf ("lock failed\n");
	  close (fd);
	}
    }
}

static void
add_lock_dir (const char *dir)
{
  /* We need to lock the dirs in pid1 because otherwise the
     locks are not held by the right process and will not live
     for the full duration of the sandbox. */
  if (n_lock_dirs < MAX_LOCK_DIRS)
    lock_dirs[n_lock_dirs++] = dir;
}

static void
lock_all_dirs (void)
{
  int i;
  for (i = 0; i < n_lock_dirs; i++)
    lock_dir (lock_dirs[i]);
}

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
                    int         mode)
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
      char *name;
      char *data = NULL;
      mode_t mode = create[i].mode;
      file_flags_t flags = create[i].flags;
      int *option = create[i].option;
      struct stat st;
      int k;
      int found;
      int res;

      if ((flags & FILE_FLAGS_IF_LAST_FAILED) &&
          !last_failed)
        continue;

      if (option && !*option)
	continue;

      name = strdup_printf (create[i].name, getuid());
      if (create[i].data)
	data = strdup_printf (create[i].data, getuid());

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

        case FILE_TYPE_REMOUNT:
          if (mount ("none", name,
                     NULL, MS_MGC_VAL|MS_REMOUNT|mode, NULL) != 0)
            die_with_error ("Unable to remount %s\n", name);

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

      free (name);
      free (data);
    }
}

static void
link_extra_etc_dirs ()
{
  DIR *dir;
  struct dirent *dirent;

  dir = opendir("usr/etc");
  if (dir != NULL)
    {
      while ((dirent = readdir(dir)))
        {
          char *dst_path;
          char *src_path;
          struct stat st;

          src_path = strconcat ("etc/", dirent->d_name);
          if (stat (src_path, &st) == 0)
            {
              free (src_path);
              continue;
            }

          dst_path = strconcat ("/usr/etc/", dirent->d_name);
	  if (symlink (dst_path, src_path) != 0)
	    die_with_error ("symlink %s", src_path);

	  free (dst_path);
	  free (src_path);
        }
    }
}

static void
mount_extra_root_dirs (int readonly)
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

              if (bind_mount (path, dirent->d_name, BIND_RECURSIVE | (readonly ? BIND_READONLY : 0)))
                die_with_error ("mount root subdir %s", dirent->d_name);
            }

          free (path);
        }
    }
}

static void
create_homedir (int mount_real_home)
{
  const char *home;
  const char *relative_home;
  const char *writable_home;

  home = getenv("HOME");
  if (home == NULL)
    return;

  relative_home = home;
  while (*relative_home == '/')
    relative_home++;

  if (mkdir_with_parents ("var/home", 0700))
    die_with_error ("unable to create var/home");

  if (mkdir_with_parents (relative_home, 0755))
    die_with_error ("unable to create %s", relative_home);

  if (mount_real_home)
    writable_home = home;
  else
    writable_home = "var/home";

  if (bind_mount (writable_home, relative_home, BIND_RECURSIVE))
    die_with_error ("unable to mount %s", home);
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

static void
block_sigchild (void)
{
  sigset_t mask;

  sigemptyset (&mask);
  sigaddset (&mask, SIGCHLD);

  if (sigprocmask (SIG_BLOCK, &mask, NULL) == -1)
    die_with_error ("sigprocmask");
}

/* This stays around for as long as the initial process in the app does
 * and when that exits it exits, propagating the exit status. We do this
 * by having pid1 in the sandbox detect this exit and tell the monitor
 * the exit status via a eventfd. We also track the exit of the sandbox
 * pid1 via a signalfd for SIGCHLD, and exit with an error in this case.
 * This is to catch e.g. problems during setup. */
static void
monitor_child (int event_fd)
{
  int res;
  uint64_t val;
  ssize_t s;
  int signal_fd;
  sigset_t mask;
  struct pollfd fds[2];
  struct signalfd_siginfo fdsi;

  sigemptyset (&mask);
  sigaddset (&mask, SIGCHLD);

  signal_fd = signalfd (-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
  if (signal_fd == -1)
    die_with_error ("signalfd");

  fds[0].fd = event_fd;
  fds[0].events = POLLIN;
  fds[1].fd = signal_fd;
  fds[1].events = POLLIN;

  while (1)
    {
      fds[0].revents = fds[1].revents = 0;
      res = poll (fds, 2, -1);
      if (res == -1 && errno != EINTR)
	die_with_error ("poll");

      s = read (event_fd, &val, 8);
      if (s == -1 && errno != EINTR && errno != EAGAIN)
	die_with_error ("read eventfd");
      else if (s == 8)
	exit ((int)val - 1);

      s = read (signal_fd, &fdsi, sizeof (struct signalfd_siginfo));
      if (s == -1 && errno != EINTR && errno != EAGAIN)
	die_with_error ("read signalfd");
      else if (s == sizeof(struct signalfd_siginfo))
	{
	  if (fdsi.ssi_signo != SIGCHLD)
	      die ("Read unexpected signal\n");
	  exit (1);
	}
    }
}

/* This is pid1 in the app sandbox. It is needed because we're using
 * pid namespaces, and someone has to reap zombies in it. We also detect
 * when the initial process (pid 2) dies and report its exit status to
 * the monitor so that it can return it to the original spawner.
 *
 * When there are no other processes in the sandbox the wait will return
 *  ECHILD, and we then exit pid1 to clean up the sandbox. */
static int
do_init (int event_fd, pid_t initial_pid)
{
  int initial_exit_status = 1;

  /* Grab a read on all .ref files to make it possible to detect that
     it is in use. This lock will automatically go away when this
     process dies */
  lock_all_dirs ();

  while (1)
    {
      pid_t child;
      int status;

      child = wait (&status);
      if (child == initial_pid)
	{
	  uint64_t val;

	  if (WIFEXITED (status))
	    initial_exit_status = WEXITSTATUS(status);

	  val = initial_exit_status + 1;
	  write (event_fd, &val, 8);
	}

      if (child == -1 && errno != EINTR)
	{
	  if (errno != ECHILD)
	    die_with_error ("init wait()");
	  break;
	}
    }

  return initial_exit_status;
}

#define REQUIRED_CAPS (CAP_TO_MASK(CAP_SYS_ADMIN) | CAP_TO_MASK(CAP_MKNOD))

static void
acquire_caps (void)
{
  struct __user_cap_header_struct hdr;
  struct __user_cap_data_struct data;

  if (getuid () != geteuid ())
    {
      /* Tell kernel not clear capabilities when dropping root */
      if (prctl (PR_SET_KEEPCAPS, 1, 0, 0, 0) < 0)
	die_with_error ("prctl(PR_SET_KEEPCAPS) failed");

      /* Drop root uid, but retain the required permitted caps */
      if (setuid (getuid ()) < 0)
	die_with_error ("unable to drop privs");
    }

  memset (&hdr, 0, sizeof(hdr));
  hdr.version = _LINUX_CAPABILITY_VERSION;

  /* Drop all non-require capabilities */
  data.effective = REQUIRED_CAPS;
  data.permitted = REQUIRED_CAPS;
  data.inheritable = 0;
  if (capset (&hdr, &data) < 0)
    die_with_error ("capset failed");
}

static void
drop_caps (void)
{
  struct __user_cap_header_struct hdr;
  struct __user_cap_data_struct data;

  memset (&hdr, 0, sizeof(hdr));
  hdr.version = _LINUX_CAPABILITY_VERSION;
  data.effective = 0;
  data.permitted = 0;
  data.inheritable = 0;

  if (capset (&hdr, &data) < 0)
    die_with_error ("capset failed");
}
int
main (int argc,
      char **argv)
{
  mode_t old_umask;
  char *newroot;
  int system_mode = 0;
  char *runtime_path = NULL;
  char *app_path = NULL;
  char *monitor_path = NULL;
  char *var_path = NULL;
  char *extra_dirs_src[MAX_EXTRA_DIRS];
  char *extra_dirs_dest[MAX_EXTRA_DIRS];
  int n_extra_dirs = 0;
  char *pulseaudio_socket = NULL;
  char *x11_socket = NULL;
  char *wayland_socket = NULL;
  char *system_dbus_socket = NULL;
  char *session_dbus_socket = NULL;
  char *xdg_runtime_dir;
  char *tz_val;
  char **args;
  char *tmp;
  int n_args;
  int share_shm = 0;
  int network = 0;
  int ipc = 0;
  int mount_host_fs = 0;
  int mount_host_fs_ro = 0;
  int mount_home = 0;
  int lock_files = 0;
  int writable = 0;
  int writable_app = 0;
  int writable_exports = 0;
  char old_cwd[256];
  int i;
  pid_t pid;
  int event_fd;

  /* Get the capabilities we need, drop root */
  acquire_caps ();

  /* Never gain any more privs during exec */
  if (prctl (PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
    die_with_error ("prctl(PR_SET_NO_NEW_CAPS) failed");

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

        case 'E':
          create_etc_symlink = 1;
          create_etc_dir = 0;
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

        case 'F':
          mount_host_fs = 1;
          mount_host_fs_ro = 1;
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

        case 'm':
          if (n_args < 2)
              usage (argv);

          monitor_path = args[1];
          args += 2;
          n_args -= 2;
          break;

        case 'b':
          if (n_args < 2)
              usage (argv);

	  tmp = strchr (args[1], '=');
	  if (tmp == NULL || tmp[1] == 0)
	    usage (argv);
	  *tmp = 0;
	  tmp = tmp + 1;

	  if (n_extra_dirs == MAX_EXTRA_DIRS)
	    die ("Too many extra directories");

	  if (strncmp (args[1], "/usr/", strlen ("/usr/")) != 0 &&
	      strncmp (args[1], "/self/", strlen ("/self/")) != 0)
	    die ("Extra directories must be in /usr or /self");

	  extra_dirs_dest[n_extra_dirs] = args[1] + 1;
	  extra_dirs_src[n_extra_dirs] = tmp;

	  n_extra_dirs++;

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

        case 'l':
          lock_files = 1;
          args += 1;
          n_args -= 1;
          break;

        case 'y':
          if (n_args < 2)
              usage (argv);

          wayland_socket = args[1];
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

  if (monitor_path != NULL && create_etc_dir)
    create_monitor_links = 1;

  if (n_args < 2)
    usage (argv);

  runtime_path = args[0];
  args++;
  n_args--;

  if (strcmp (runtime_path, "/usr") == 0)
    system_mode = 1;

  /* The initial code is run with high permissions
     (at least CAP_SYS_ADMIN), so take lots of care. */

  __debug__(("Creating xdg-app-root dir\n"));

  newroot = strdup_printf ("/run/user/%d/.xdg-app-root", getuid());
  if (mkdir (newroot, 0755) && errno != EEXIST)
    {
      free (newroot);
      newroot = strdup_printf ("/tmp/.xdg-app-root", getuid());
      if (mkdir (newroot, 0755) && errno != EEXIST)
	die_with_error ("Creating xdg-app-root failed");
    }

  __debug__(("creating new namespace\n"));

  event_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);

  block_sigchild (); /* Block before we clone to avoid races */

  pid = raw_clone (SIGCHLD | CLONE_NEWNS | CLONE_NEWPID |
		   (network ? 0 : CLONE_NEWNET) |
		   (ipc ? 0 : CLONE_NEWIPC),
		   NULL);
  if (pid == -1)
    die_with_error ("Creating new namespace failed");

  if (pid != 0)
    {
      /* Drop all extra caps in the monitor child process */
      drop_caps ();
      monitor_child (event_fd);
      exit (0); /* Should not be reached, but better safe... */
    }

  old_umask = umask (0);

  /* Mark everything as slave, so that we still
   * receive mounts from the real root, but don't
   * propagate mounts to the real root. */
  if (mount (NULL, "/", NULL, MS_SLAVE|MS_REC, NULL) < 0)
    die_with_error ("Failed to make / slave");

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

  if (lock_files)
    add_lock_dir ("usr");

  if (app_path != NULL)
    {
      if (bind_mount (app_path, "self", BIND_PRIVATE | (writable_app?0:BIND_READONLY)))
        die_with_error ("mount self");

      if (lock_files)
	add_lock_dir ("self");

      if (!writable_app && writable_exports)
	{
	  char *exports = strconcat (app_path, "/exports");

	  if (bind_mount (exports, "self/exports", BIND_PRIVATE))
	    die_with_error ("mount self/exports");

	  free (exports);
	}
    }

  for (i = 0; i < n_extra_dirs; i++)
    {
      if (bind_mount (extra_dirs_src[i], extra_dirs_dest[i], BIND_PRIVATE | BIND_READONLY))
	die_with_error ("mount extra dir %s", extra_dirs_src[i]);

      if (lock_files)
	add_lock_dir (extra_dirs_dest[i]);
    }

  if (var_path != NULL)
    {
      if (bind_mount (var_path, "var", BIND_PRIVATE))
        die_with_error ("mount var");
    }

  create_files (create_post, N_ELEMENTS (create_post), share_shm, system_mode);

  if (create_etc_dir)
    link_extra_etc_dirs ();

  if (monitor_path)
    {
      char *monitor_mount_path = strdup_printf ("run/user/%d/xdg-app-monitor", getuid());

      if (bind_mount (monitor_path, monitor_mount_path, BIND_READONLY))
	die ("can't bind monitor dir");

      free (monitor_mount_path);
    }

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

  /* Bind mount in the Wayland socket */
  if (wayland_socket != 0)
    {
      char *wayland_path_relative = strdup_printf ("run/user/%d/wayland-0", getuid());
      if (create_file (wayland_path_relative, 0666, NULL) ||
          bind_mount (wayland_socket, wayland_path_relative, 0))
        die ("can't bind Wayland socket %s -> %s: %s", wayland_socket, wayland_path_relative, strerror (errno));
      free (wayland_path_relative);
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

  if (mount_host_fs || mount_home)
    {
      char *dconf_run_path_relative = strdup_printf ("run/user/%d/dconf", getuid());
      char *dconf_run_path_absolute = strdup_printf ("/run/user/%d/dconf", getuid());

      bind_mount (dconf_run_path_absolute, dconf_run_path_relative, 0);
    }

  if (mount_host_fs)
    mount_extra_root_dirs (mount_host_fs_ro);

  if (!mount_host_fs)
    create_homedir (mount_home);

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

  /* Now we have everything we need CAP_SYS_ADMIN for, so drop it */
  drop_caps ();

  chdir (old_cwd);

  xsetenv ("PATH", "/self/bin:/usr/bin", 1);
  xsetenv ("LD_LIBRARY_PATH", "/self/lib", 1);
  xsetenv ("XDG_CONFIG_DIRS","/self/etc/xdg:/etc/xdg", 1);
  xsetenv ("XDG_DATA_DIRS", "/self/share:/usr/share", 1);
  xsetenv ("GI_TYPELIB_PATH", "/self/lib/girepository-1.0", 1);
  xdg_runtime_dir = strdup_printf ("/run/user/%d", getuid());
  xsetenv ("XDG_RUNTIME_DIR", xdg_runtime_dir, 1);
  free (xdg_runtime_dir);
  if (monitor_path)
    {
      tz_val = strdup_printf (":/run/user/%d/xdg-app-monitor/localtime", getuid());
      xsetenv ("TZ", tz_val, 0);
      free (tz_val);
    }

  __debug__(("forking for child\n"));

  pid = fork ();
  if (pid == -1)
    die_with_error("Can't fork for child");

  if (pid == 0)
    {
      __debug__(("launch executable %s\n", args[0]));

      if (execvp (args[0], args) == -1)
        die_with_error ("execvp %s", args[0]);
      return 0;
    }

  strncpy (argv[0], "xdg-app-init\0", strlen (argv[0]));
  return do_init (event_fd, pid);
}
