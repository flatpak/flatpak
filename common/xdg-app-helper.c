/* xdg-app-helper
 * Copyright (C) 2014 Alexander Larsson
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
 */

#include "config.h"

#include <assert.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
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
#include <sys/utsname.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

#ifdef ENABLE_SECCOMP
#include <seccomp.h>
#endif

#if 0
#define __debug__(x) printf x
#else
#define __debug__(x)
#endif

#define N_ELEMENTS(arr)		(sizeof (arr) / sizeof ((arr)[0]))

#define TRUE 1
#define FALSE 0
typedef int bool;

#define READ_END 0
#define WRITE_END 1

/* Globals to avoid having to use getuid(), since the uid/gid changes during runtime */
static uid_t uid;
static gid_t gid;
static bool is_privileged;

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

static void
die_oom (void)
{
  die ("Out of memory");
}

static void *
xmalloc (size_t size)
{
  void *res = malloc (size);
  if (res == NULL)
    die_oom ();
  return res;
}

static void *
xrealloc (void *ptr, size_t size)
{
  void *res = realloc (ptr, size);
  if (size != 0 && res == NULL)
    die_oom ();
  return res;
}

static char *
xstrdup (const char *str)
{
  char *res;

  assert (str != NULL);

  res = strdup (str);
  if (res == NULL)
    die_oom ();

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

static char *
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

static char *
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

static char*
strdup_printf (const char *format,
               ...)
{
  char *buffer = NULL;
  va_list args;

  va_start (args, format);
  vasprintf (&buffer, format, args);
  va_end (args);

  if (buffer == NULL)
    die_oom ();

  return buffer;
}

static const char *
get_relative_path (const char *path)
{
  while (*path == '/')
    path++;
  return path;
}

#ifndef HAVE_FDWALK
static int
fdwalk (int (*cb)(void *data, int fd), void *data)
{
  int open_max;
  int fd;
  int res = 0;
  DIR *d;

  if ((d = opendir ("/proc/self/fd")))
    {
      struct dirent *de;

      while ((de = readdir (d)))
        {
          long l;
          char *e = NULL;

          if (de->d_name[0] == '.')
            continue;

          errno = 0;
          l = strtol (de->d_name, &e, 10);
          if (errno != 0 || !e || *e)
            continue;

          fd = (int) l;

          if ((long) fd != l)
            continue;

          if (fd == dirfd (d))
            continue;

          if ((res = cb (data, fd)) != 0)
            break;
        }

      closedir (d);
      return res;
  }

  open_max = sysconf (_SC_OPEN_MAX);

  for (fd = 0; fd < open_max; fd++)
    if ((res = cb (data, fd)) != 0)
      break;

  return res;
}
#endif


static inline int raw_clone(unsigned long flags, void *child_stack) {
#if defined(__s390__) || defined(__CRIS__)
        /* On s390 and cris the order of the first and second arguments
         * of the raw clone() system call is reversed. */
        return (int) syscall(__NR_clone, child_stack, flags);
#else
        return (int) syscall(__NR_clone, flags, child_stack);
#endif
}

static void
setup_seccomp (bool devel)
{
#ifdef ENABLE_SECCOMP
  scmp_filter_ctx seccomp;
  /**** BEGIN NOTE ON CODE SHARING
   *
   * There are today a number of different Linux container
   * implementations.  That will likely continue for long into the
   * future.  But we can still try to share code, and it's important
   * to do so because it affects what library and application writers
   * can do, and we should support code portability between different
   * container tools.
   *
   * This syscall blacklist is copied from xdg-app, which was in turn
   * clearly influenced by the Sandstorm.io blacklist.
   *
   * If you make any changes here, I suggest sending the changes along
   * to other sandbox maintainers.  Using the libseccomp list is also
   * an appropriate venue:
   * https://groups.google.com/forum/#!topic/libseccomp
   *
   * A non-exhaustive list of links to container tooling that might
   * want to share this blacklist:
   *
   *  https://github.com/sandstorm-io/sandstorm
   *    in src/sandstorm/supervisor.c++
   *  http://cgit.freedesktop.org/xdg-app/xdg-app/
   *    in lib/xdg-app-helper.c
   *  https://git.gnome.org/browse/linux-user-chroot
   *    in src/setup-seccomp.c
   *
   **** END NOTE ON CODE SHARING
   */
  struct {
    int scall;
    struct scmp_arg_cmp *arg;
  } syscall_blacklist[] = {
    /* Block dmesg */
    {SCMP_SYS(syslog)},
    /* Useless old syscall */
    {SCMP_SYS(uselib)},
    /* Don't allow you to switch to bsd emulation or whatnot */
    {SCMP_SYS(personality)},
    /* Don't allow disabling accounting */
    {SCMP_SYS(acct)},
    /* 16-bit code is unnecessary in the sandbox, and modify_ldt is a
       historic source of interesting information leaks. */
    {SCMP_SYS(modify_ldt)},
    /* Don't allow reading current quota use */
    {SCMP_SYS(quotactl)},

    /* Scary VM/NUMA ops */
    {SCMP_SYS(move_pages)},
    {SCMP_SYS(mbind)},
    {SCMP_SYS(get_mempolicy)},
    {SCMP_SYS(set_mempolicy)},
    {SCMP_SYS(migrate_pages)},

    /* Don't allow subnamespace setups: */
    {SCMP_SYS(unshare)},
    {SCMP_SYS(mount)},
    {SCMP_SYS(pivot_root)},
    {SCMP_SYS(clone), &SCMP_A0(SCMP_CMP_MASKED_EQ, CLONE_NEWUSER, CLONE_NEWUSER)},
  };

  struct {
    int scall;
    struct scmp_arg_cmp *arg;
  } syscall_nondevel_blacklist[] = {
    /* Profiling operations; we expect these to be done by tools from outside
     * the sandbox.  In particular perf has been the source of many CVEs.
     */
    {SCMP_SYS(perf_event_open)},
    {SCMP_SYS(ptrace)}
  };
  /* Blacklist all but unix, inet, inet6 and netlink */
  int socket_family_blacklist[] = {
    AF_AX25,
    AF_IPX,
    AF_APPLETALK,
    AF_NETROM,
    AF_BRIDGE,
    AF_ATMPVC,
    AF_X25,
    AF_ROSE,
    AF_DECnet,
    AF_NETBEUI,
    AF_SECURITY,
    AF_KEY,
    AF_NETLINK + 1, /* Last gets CMP_GE, so order is important */
  };
  int i, r;
  struct utsname uts;

  seccomp = seccomp_init(SCMP_ACT_ALLOW);
  if (!seccomp)
    return die_oom ();

  /* Add in all possible secondary archs we are aware of that
   * this kernel might support. */
#if defined(__i386__) || defined(__x86_64__)
  r = seccomp_arch_add (seccomp, SCMP_ARCH_X86);
  if (r < 0 && r != -EEXIST)
    die_with_error ("Failed to add x86 architecture to seccomp filter");

  r = seccomp_arch_add (seccomp, SCMP_ARCH_X86_64);
  if (r < 0 && r != -EEXIST)
    die_with_error ("Failed to add x86_64 architecture to seccomp filter");

  r = seccomp_arch_add (seccomp, SCMP_ARCH_X32);
  if (r < 0 && r != -EEXIST)
    die_with_error ("Failed to add x32 architecture to seccomp filter");
#endif

  /* TODO: Should we filter the kernel keyring syscalls in some way?
   * We do want them to be used by desktop apps, but they could also perhaps
   * leak system stuff or secrets from other apps.
   */

  for (i = 0; i < N_ELEMENTS (syscall_blacklist); i++)
    {
      int scall = syscall_blacklist[i].scall;
      if (syscall_blacklist[i].arg)
        r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO(EPERM), scall, 1, *syscall_blacklist[i].arg);
      else
        r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO(EPERM), scall, 0);
      if (r < 0 && r == -EFAULT /* unknown syscall */)
        die_with_error ("Failed to block syscall %d", scall);
    }

  if (!devel)
    {
      for (i = 0; i < N_ELEMENTS (syscall_nondevel_blacklist); i++)
        {
          int scall = syscall_nondevel_blacklist[i].scall;
          if (syscall_nondevel_blacklist[i].arg)
            r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO(EPERM), scall, 1, *syscall_nondevel_blacklist[i].arg);
          else
            r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO(EPERM), scall, 0);
          if (r < 0 && r == -EFAULT /* unknown syscall */)
            die_with_error ("Failed to block syscall %d", scall);
        }
    }

  /* Socket filtering doesn't work on x86 */
  if (uname (&uts) == 0 && strcmp (uts.machine, "i686") != 0)
    {
      for (i = 0; i < N_ELEMENTS (socket_family_blacklist); i++)
	{
	  int family = socket_family_blacklist[i];
	  if (i == N_ELEMENTS (socket_family_blacklist) - 1)
	    r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1, SCMP_A0(SCMP_CMP_GE, family));
	  else
	    r = seccomp_rule_add (seccomp, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1, SCMP_A0(SCMP_CMP_EQ, family));
	  if (r < 0)
	    die_with_error ("Failed to block socket family %d", family);
	}
    }

  r = seccomp_load (seccomp);
  if (r < 0)
    die_with_error ("Failed to install seccomp audit filter: ");

  seccomp_release (seccomp);
#endif
}

static void
usage (char **argv)
{
  fprintf (stderr, "usage: %s [OPTIONS...] RUNTIMEPATH COMMAND [ARGS...]\n\n", argv[0]);

  fprintf (stderr,
           "	-a		 Specify path for application (mounted at /app)\n"
           "	-b DEST[=SOURCE] Bind extra source path read-only into DEST\n"
           "	-B DEST[=SOURCE] Bind extra source path into DEST\n"
           "	-M DEST[=SOURCE] Bind extra source path into DEST and remove original\n"
           "	-c               Enable developer mode (allows strace and perf)\n"
           "	-d SOCKETPATH	 Use SOCKETPATH as dbus session bus\n"
           "	-D SOCKETPATH	 Use SOCKETPATH as dbus system bus\n"
           "	-e		 Make /app/exports writable\n"
           "	-E		 Make /etc a pure symlink to /usr/etc\n"
           "	-F		 Mount the host filesystems\n"
           "	-f		 Mount the host filesystems read-only\n"
           "	-g               Allow use of direct rendering graphics\n"
           "	-H		 Mount the users home directory\n"
           "	-h		 Mount the users home directory read-only\n"
           "	-i		 Share IPC namespace with session\n"
           "	-I APPID	 Set app id (used to find app data)\n"
           "	-l		 Lock .ref files in all mounts\n"
           "	-m PATH		 Set path to xdg-app-session-helper output\n"
           "	-n		 Share network namespace with session\n"
           "	-p SOCKETPATH	 Use SOCKETPATH as pulseaudio connection\n"
           "	-P PATH	         Chdir into PATH before running\n"
           "	-r               Bind mount /etc/resolv.conf\n"
           "	-s		 Share Shm namespace with session\n"
           "	-S FD            Pass fd into app to detect when it dies\n"
           "	-v PATH		 Mount PATH as /var\n"
           "	-w		 Make /app writable\n"
           "	-W		 Make /usr writable\n"
           "	-x SOCKETPATH	 Use SOCKETPATH as X display\n"
           "	-y SOCKETPATH	 Use SOCKETPATH as Wayland display\n"
           );
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
  FILE_TYPE_ETC_PASSWD,
  FILE_TYPE_ETC_GROUP,
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

static bool create_etc_symlink = FALSE;
static bool create_etc_dir = TRUE;
static bool create_monitor_links = FALSE;
static bool bind_resolv_conf = FALSE;
static bool allow_dri = FALSE;

static const create_table_t create[] = {
  { FILE_TYPE_DIR, ".oldroot", 0755 },
  { FILE_TYPE_DIR, "usr", 0755 },
  { FILE_TYPE_DIR, "tmp", 01777 },
  { FILE_TYPE_DIR, "app", 0755},
  { FILE_TYPE_DIR, "run", 0755},
  { FILE_TYPE_DIR, "run/host", 0755},
  { FILE_TYPE_DIR, "run/dbus", 0755},
  { FILE_TYPE_DIR, "run/media", 0755},
  { FILE_TYPE_DIR, "run/user", 0755},
  { FILE_TYPE_DIR, "run/user/%1$d", 0700, NULL},
  { FILE_TYPE_DIR, "run/user/%1$d/pulse", 0700, NULL},
  { FILE_TYPE_DIR, "run/user/%1$d/dconf", 0700, NULL},
  { FILE_TYPE_DIR, "run/user/%1$d/xdg-app-monitor", 0700, NULL},
  { FILE_TYPE_REGULAR, "run/user/%1$d/pulse/native", 0700, NULL},
  { FILE_TYPE_DIR, "var", 0755},
  { FILE_TYPE_SYMLINK, "var/tmp", 0755, "/tmp"},
  { FILE_TYPE_SYMLINK, "var/run", 0755, "/run"},
  { FILE_TYPE_SYSTEM_SYMLINK, "lib32", 0755, "usr/lib32"},
  { FILE_TYPE_SYSTEM_SYMLINK, "lib64", 0755, "usr/lib64"},
  { FILE_TYPE_SYSTEM_SYMLINK, "lib", 0755, "usr/lib"},
  { FILE_TYPE_SYSTEM_SYMLINK, "bin", 0755, "usr/bin" },
  { FILE_TYPE_SYSTEM_SYMLINK, "sbin", 0755, "usr/sbin"},
  { FILE_TYPE_SYMLINK, "etc", 0755, "usr/etc", 0, &create_etc_symlink},
  { FILE_TYPE_DIR, "etc", 0755, NULL, 0, &create_etc_dir},
  { FILE_TYPE_ETC_PASSWD, "etc/passwd", 0755, NULL, 0, &create_etc_dir},
  { FILE_TYPE_ETC_GROUP, "etc/group", 0755, NULL, 0, &create_etc_dir},
  { FILE_TYPE_REGULAR, "etc/resolv.conf", 0755, NULL, 0, &bind_resolv_conf},
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
  { FILE_TYPE_DIR, "sys/block", 0755},
  { FILE_TYPE_BIND, "sys/block", 0755, "/sys/block"},
  { FILE_TYPE_DIR, "sys/bus", 0755},
  { FILE_TYPE_BIND, "sys/bus", 0755, "/sys/bus"},
  { FILE_TYPE_DIR, "sys/class", 0755},
  { FILE_TYPE_BIND, "sys/class", 0755, "/sys/class"},
  { FILE_TYPE_DIR, "sys/dev", 0755},
  { FILE_TYPE_BIND, "sys/dev", 0755, "/sys/dev"},
  { FILE_TYPE_DIR, "sys/devices", 0755},
  { FILE_TYPE_BIND, "sys/devices", 0755, "/sys/devices"},
  { FILE_TYPE_DIR, "dev", 0755},
  { FILE_TYPE_DIR, "dev/pts", 0755},
  { FILE_TYPE_MOUNT, "dev/pts"},
  { FILE_TYPE_SYMLINK, "dev/ptmx", 0666, "pts/ptmx"},
  { FILE_TYPE_DIR, "dev/shm", 0755},
  { FILE_TYPE_SHM, "dev/shm"},
  { FILE_TYPE_DEVICE, "dev/null", 0666},
  { FILE_TYPE_DEVICE, "dev/zero", 0666},
  { FILE_TYPE_DEVICE, "dev/full", 0666},
  { FILE_TYPE_DEVICE, "dev/random", 0666},
  { FILE_TYPE_DEVICE, "dev/urandom", 0666},
  { FILE_TYPE_DEVICE, "dev/tty", 0666},
  { FILE_TYPE_DIR, "dev/dri", 0755},
  { FILE_TYPE_BIND_RO, "dev/dri", 0755, "/dev/dri", FILE_FLAGS_NON_FATAL|FILE_FLAGS_DEVICES, &allow_dri},
  { FILE_TYPE_DEVICE, "dev/nvidiactl", 0666, NULL, FILE_FLAGS_NON_FATAL, &allow_dri},
  { FILE_TYPE_DEVICE, "dev/nvidia0", 0666, NULL, FILE_FLAGS_NON_FATAL, &allow_dri},
};

/* warning: Don't create any actual files here, as we could potentially
   write over bind mounts to the system */
static const create_table_t create_post[] = {
  { FILE_TYPE_BIND_RO, "etc/machine-id", 0444, "/etc/machine-id", FILE_FLAGS_NON_FATAL},
  { FILE_TYPE_BIND_RO, "etc/machine-id", 0444, "/var/lib/dbus/machine-id", FILE_FLAGS_NON_FATAL | FILE_FLAGS_IF_LAST_FAILED},
  { FILE_TYPE_BIND_RO, "etc/resolv.conf", 0444, "/etc/resolv.conf", 0, &bind_resolv_conf},
};

static const mount_table_t mount_table[] = {
  { "proc",      "proc",     "proc",  NULL,        MS_NOSUID|MS_NOEXEC|MS_NODEV           },
  { "devpts",    "dev/pts",  "devpts","newinstance,ptmxmode=0666,mode=620", MS_NOSUID|MS_NOEXEC },
  { "tmpfs",     "dev/shm",  "tmpfs", "mode=1777", MS_NOSUID|MS_NODEV|MS_STRICTATIME      },
};

const char *dont_mount_in_root[] = {
  ".", "..", "lib", "lib32", "lib64", "bin", "sbin", "usr", "boot", "root",
  "tmp", "etc", "app", "run", "proc", "sys", "dev", "var"
};

typedef enum {
  BIND_READONLY = (1<<0),
  BIND_PRIVATE = (1<<1),
  BIND_DEVICES = (1<<2),
  BIND_RECURSIVE = (1<<3),
} bind_option_t;

typedef struct {
  char *src;
  char *dest;
  bool readonly;
  bool move;
} ExtraFile;

ExtraFile *extra_files = NULL;
int n_extra_files = 0;

static void
add_extra_file (char *src, char *dest, bool readonly, bool move)
{
  int i = n_extra_files;
  n_extra_files++;
  extra_files = xrealloc (extra_files, n_extra_files * sizeof (ExtraFile));
  extra_files[i].src = src;
  extra_files[i].dest = dest;
  extra_files[i].readonly = readonly;
  extra_files[i].move = move;
}

static int n_lock_dirs = 0;
static const char **lock_dirs = NULL;

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
  int i = n_lock_dirs;

  n_lock_dirs++;
  lock_dirs = xrealloc (lock_dirs, n_lock_dirs * sizeof (char *));
  lock_dirs[i] = dir;
}

/* We need to lock the dirs in pid1 because otherwise the
   locks are not held by the right process and will not live
   for the full duration of the sandbox. */
static void
lock_all_dirs (void)
{
  int i;
  for (i = 0; i < n_lock_dirs; i++)
    lock_dir (lock_dirs[i]);
}

static char *
load_file (const char *path)
{
  int fd;
  char *data;
  ssize_t data_read;
  ssize_t data_len;
  ssize_t res;

  fd = open (path, O_CLOEXEC | O_RDONLY);
  if (fd == -1)
    return NULL;

  data_read = 0;
  data_len = 4080;
  data = xmalloc (data_len);

  do
    {
      if (data_len >= data_read + 1)
        {
          data_len *= 2;
          data = xrealloc (data, data_len);
        }

      do
        res = read (fd, data + data_read, data_len - data_read - 1);
      while (res < 0 && errno == EINTR);

      if (res < 0)
        {
          int errsv = errno;
          free (data);
          errno = errsv;
          return NULL;
        }

      data_read += res;
    }
  while (res > 0);

  data[data_read] = 0;

  close (fd);

  return data;
}

static char *
skip_line (char *line)
{
  while (*line != 0 && *line != '\n')
    line++;

  if (*line == '\n')
    line++;

  return line;
}

static char *
skip_token (char *line, bool eat_whitespace)
{
  while (*line != ' ' && *line != '\n')
    line++;

  if (eat_whitespace && *line == ' ')
    line++;

  return line;
}

static bool
str_has_prefix (const char *str,
                const char *prefix)
{
  return strncmp (str, prefix, strlen (prefix)) == 0;
}

static char *
unescape_string (const char *escaped, ssize_t len)
{
  char *unescaped, *res;
  const char *end;

  if (len < 0)
    len = strlen (escaped);
  end = escaped + len;

  unescaped = res = xmalloc (len + 1);
  while (escaped < end)
    {
      if (*escaped == '\\')
	{
	  *unescaped++ =
	    ((escaped[1] - '0')  << 6) |
	    ((escaped[2] - '0')  << 3) |
	    ((escaped[3] - '0')  << 0);
	  escaped += 4;
	}
      else
	*unescaped++ = *escaped++;
    }
  *unescaped = 0;
  return res;
}

static char *
get_mountinfo (const char *mountpoint)
{
  char *line_mountpoint, *line_mountpoint_end;
  char *mountinfo;
  char *free_me = NULL;
  char *line, *line_start;
  char *res = NULL;
  int i;

  if (mountpoint[0] != '/')
    {
      char *cwd = getcwd(NULL, 0);
      if (cwd == NULL)
        die_oom ();

      mountpoint = free_me = strconcat3 (cwd, "/", mountpoint);
      free (cwd);
    }

  mountinfo = load_file ("/proc/self/mountinfo");
  if (mountinfo == NULL)
    return NULL;

  line = mountinfo;

  while (*line != 0)
    {
      char *unescaped;

      line_start = line;
      for (i = 0; i < 4; i++)
        line = skip_token (line, TRUE);
      line_mountpoint = line;
      line = skip_token (line, FALSE);
      line_mountpoint_end = line;
      line = skip_line (line);

      unescaped = unescape_string (line_mountpoint, line_mountpoint_end - line_mountpoint);
      if (strcmp (mountpoint, unescaped) == 0)
        {
	  free (unescaped);
          res = line_start;
          line[-1] = 0;
          break;
        }
      free (unescaped);
    }

  if (free_me)
    free (free_me);
  free (mountinfo);

  if (res)
    return xstrdup (res);
  return NULL;
}

static unsigned long
get_mountflags (const char *mountpoint)
{
  char *line, *token, *end_token;
  int i;
  unsigned long flags = 0;
  static const struct  { int flag; char *name; } flags_data[] = {
    { 0, "rw" },
    { MS_RDONLY, "ro" },
    { MS_NOSUID, "nosuid" },
    { MS_NODEV, "nodev" },
    { MS_NOEXEC, "noexec" },
    { MS_NOATIME, "noatime" },
    { MS_NODIRATIME, "nodiratime" },
    { MS_RELATIME, "relatime" },
    { 0, NULL }
  };

  line = get_mountinfo (mountpoint);
  if (line == NULL)
    return 0;

  token = line;
  for (i = 0; i < 5; i++)
    token = skip_token (token, TRUE);

  end_token = skip_token (token, FALSE);
  *end_token = 0;

  do {
    end_token = strchr (token, ',');
    if (end_token != NULL)
      *end_token = 0;

    for (i = 0; flags_data[i].name != NULL; i++)
      {
        if (strcmp (token, flags_data[i].name) == 0)
          flags |= flags_data[i].flag;
      }

    if (end_token)
      token = end_token + 1;
    else
      token = NULL;
  } while (token != NULL);

  free (line);

  return flags;
}


static char **
get_submounts (const char *parent_mount)
{
  char *mountpoint, *mountpoint_end;
  char **submounts;
  int i, n_submounts, submounts_size;
  char *mountinfo;
  char *line;

  mountinfo = load_file ("/proc/self/mountinfo");
  if (mountinfo == NULL)
    return NULL;

  submounts_size = 8;
  n_submounts = 0;
  submounts = xmalloc (sizeof (char *) * submounts_size);

  line = mountinfo;

  while (*line != 0)
    {
      char *unescaped;
      for (i = 0; i < 4; i++)
        line = skip_token (line, TRUE);
      mountpoint = line;
      line = skip_token (line, FALSE);
      mountpoint_end = line;
      line = skip_line (line);
      *mountpoint_end = 0;

      unescaped = unescape_string (mountpoint, -1);

      if (*unescaped == '/' &&
          str_has_prefix (unescaped + 1, parent_mount) &&
          *(unescaped + 1 + strlen (parent_mount)) == '/')
        {
          if (n_submounts + 1 >= submounts_size)
            {
              submounts_size *= 2;
              submounts = xrealloc (submounts, sizeof (char *) * submounts_size);
            }
          submounts[n_submounts++] = xstrdup (unescaped + 1);
        }
      free (unescaped);
    }

  submounts[n_submounts] = NULL;

  free (mountinfo);

  return submounts;
}

static int
bind_mount (const char *src, const char *dest, bind_option_t options)
{
  bool readonly = (options & BIND_READONLY) != 0;
  bool private = (options & BIND_PRIVATE) != 0;
  bool devices = (options & BIND_DEVICES) != 0;
  bool recursive = (options & BIND_RECURSIVE) != 0;
  unsigned long current_flags;
  char **submounts;
  int i;

  if (mount (src, dest, NULL, MS_MGC_VAL|MS_BIND|(recursive?MS_REC:0), NULL) != 0)
    return 1;

  if (private)
    {
      if (mount ("none", dest,
                 NULL, MS_REC|MS_PRIVATE, NULL) != 0)
        return 2;
    }

  current_flags = get_mountflags (dest);

  if (mount ("none", dest,
             NULL, MS_MGC_VAL|MS_BIND|MS_REMOUNT|current_flags|(devices?0:MS_NODEV)|MS_NOSUID|(readonly?MS_RDONLY:0), NULL) != 0)
    return 3;

  /* We need to work around the fact that a bind mount does not apply the flags, so we need to manually
   * apply the flags to all submounts in the recursive case.
   * Note: This does not apply the flags to mounts which are later propagated into this namespace.
   */
  if (recursive)
    {
      submounts = get_submounts (dest);
      if (submounts == NULL)
        return 4;

      for (i = 0; submounts[i] != NULL; i++)
        {
          current_flags = get_mountflags (submounts[i]);
          if (mount ("none", submounts[i],
                     NULL, MS_MGC_VAL|MS_BIND|MS_REMOUNT|current_flags|(devices?0:MS_NODEV)|MS_NOSUID|(readonly?MS_RDONLY:0), NULL) != 0)
            return 5;
          free (submounts[i]);
        }

      free (submounts);
    }

  return 0;
}

static bool
stat_is_dir (const char *pathname)
{
 struct stat buf;

 if (stat (pathname, &buf) !=  0)
   return FALSE;

 return S_ISDIR (buf.st_mode);
}

static int
mkdir_with_parents (const char *pathname,
                    int         mode,
                    bool        create_last)
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

      if (!create_last && p == NULL)
        break;

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

static bool
write_to_file (int fd, const char *content, ssize_t len)
{
  ssize_t res;

  while (len > 0)
    {
      res = write (fd, content, len);
      if (res < 0 && errno == EINTR)
	continue;
      if (res <= 0)
	return FALSE;
      len -= res;
      content += res;
    }

  return TRUE;
}

#define BUFSIZE	8192
static bool
copy_file_data (int     sfd,
                int     dfd)
{
  char buffer[BUFSIZE];
  ssize_t bytes_read;

  while (TRUE)
    {
      bytes_read = read (sfd, buffer, BUFSIZE);
      if (bytes_read == -1)
        {
          if (errno == EINTR)
            continue;

          return FALSE;
        }

      if (bytes_read == 0)
        break;

      if (!write_to_file (dfd, buffer, bytes_read))
        return FALSE;
    }

  return TRUE;
}

static bool
copy_file (const char *src_path, const char *dst_path, mode_t mode)
{
  int sfd, dfd;
  bool res;
  int errsv;

  sfd = open (src_path, O_CLOEXEC | O_RDONLY);
  if (sfd == -1)
    return FALSE;

  dfd = creat (dst_path, mode);
  if (dfd == -1)
    {
      close (sfd);
      return FALSE;
    }

  res = copy_file_data (sfd, dfd);

  errsv = errno;
  close (sfd);
  close (dfd);
  errno = errsv;

  return res;
}

static bool
write_file (const char *path, const char *content)
{
  int fd;
  bool res;
  int errsv;

  fd = open (path, O_RDWR | O_CLOEXEC, 0);
  if (fd == -1)
    return FALSE;

  res = TRUE;
  if (content)
    res = write_to_file (fd, content, strlen (content));

  errsv = errno;
  close (fd);
  errno = errsv;

  return res;
}

static bool
create_file (const char *path, mode_t mode, const char *content)
{
  int fd;
  bool res;
  int errsv;

  fd = creat (path, mode);
  if (fd == -1)
    return FALSE;

  res = TRUE;
  if (content)
    res = write_to_file (fd, content, strlen (content));

  errsv = errno;
  close (fd);
  errno = errsv;

  return res;
}

static void
create_files (const create_table_t *create, int n_create, int ignore_shm, const char *usr_path)
{
  bool last_failed = FALSE;
  int i;
  int system_mode = FALSE;

  if (strcmp (usr_path, "/usr") == 0)
    system_mode = TRUE;

  for (i = 0; i < n_create; i++)
    {
      char *name;
      char *data = NULL;
      mode_t mode = create[i].mode;
      file_flags_t flags = create[i].flags;
      int *option = create[i].option;
      unsigned long current_mount_flags;
      char *in_root;
      int k;
      bool found;
      int res;

      if ((flags & FILE_FLAGS_IF_LAST_FAILED) &&
          !last_failed)
        continue;

      if (option && !*option)
	continue;

      name = strdup_printf (create[i].name, uid);
      if (create[i].data)
	data = strdup_printf (create[i].data, uid);

      last_failed = FALSE;

      switch (create[i].type)
        {
        case FILE_TYPE_DIR:
          if (mkdir (name, mode) != 0)
            die_with_error ("creating dir %s", name);
          break;

        case FILE_TYPE_ETC_PASSWD:
          {
            char *content = NULL;
            struct passwd *p = getpwuid (uid);
            if (p)
              {
                content = strdup_printf ("%s:x:%d:%d:%s:%s:%s\n"
                                         "nfsnobody:x:65534:65534:Unmapped user:/:/sbin/nologin\n",
                                         p->pw_name,
                                         uid, gid,
                                         p->pw_gecos,
                                         p->pw_dir,
                                         p->pw_shell);

              }

            if (!create_file (name, mode, content))
              die_with_error ("creating file %s", name);

            if (content)
              free (content);
          }
          break;

        case FILE_TYPE_ETC_GROUP:
          {
            char *content = NULL;
            struct group *g = getgrgid (gid);
            struct passwd *p = getpwuid (uid);
            if (p && g)
              {
                content = strdup_printf ("%s:x:%d:%s\n"
                                         "nfsnobody:x:65534:\n",
                                         g->gr_name,
                                         gid, p->pw_name);
              }

            if (!create_file (name, mode, content))
              die_with_error ("creating file %s", name);

            if (content)
              free (content);
          }
          break;

        case FILE_TYPE_REGULAR:
          if (!create_file (name, mode, NULL))
            die_with_error ("creating file %s", name);
          break;

        case FILE_TYPE_SYSTEM_SYMLINK:
	  if (system_mode)
	    {
	      struct stat buf;
	      in_root = strconcat ("/", name);
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

          /* Only create symlink if target exists */
          if (data != NULL && str_has_prefix (data, "usr/"))
	    {
	      struct stat buf;
	      char *in_usr = strconcat3 (usr_path, "/", data + strlen("usr/"));
              int res;

              res = lstat (in_usr, &buf);
              free (in_usr);

              if (res !=  0)
                data = NULL;
            }
          else
            data = NULL;

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
                                 ((flags & FILE_FLAGS_DEVICES) ? BIND_DEVICES : 0)
				 )))
            {
              if (res > 1 || (flags & FILE_FLAGS_NON_FATAL) == 0)
                die_with_error ("mounting bindmount %s", name);
              last_failed = TRUE;
            }

          break;

        case FILE_TYPE_SHM:
          if (ignore_shm)
            break;

          /* NOTE: Fall through, treat as mount */
        case FILE_TYPE_MOUNT:
          found = FALSE;
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
                  found = TRUE;
                }
            }

          if (!found)
            die ("Unable to find mount %s\n", name);

          break;

        case FILE_TYPE_REMOUNT:
          current_mount_flags = get_mountflags (name);
          if (mount ("none", name,
                     NULL, MS_MGC_VAL|MS_REMOUNT|current_mount_flags|mode, NULL) != 0)
            die_with_error ("Unable to remount %s\n", name);

          break;

        case FILE_TYPE_DEVICE:
          if (!create_file (name, mode, NULL))
            die_with_error ("creating file %s", name);

	  in_root = strconcat ("/", name);
          if ((res = bind_mount (in_root, name,
                                 BIND_DEVICES)))
            {
              if (res > 1 || (flags & FILE_FLAGS_NON_FATAL) == 0)
                die_with_error ("binding device %s", name);
            }
	  free (in_root);

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
          char *target;
          struct stat st;

          src_path = strconcat ("etc/", dirent->d_name);
          if (lstat (src_path, &st) == 0)
            {
              free (src_path);
              continue;
            }

          dst_path = strconcat ("usr/etc/", dirent->d_name);
          if (lstat (dst_path, &st) != 0)
            {
              free (src_path);
              free (dst_path);
              continue;
            }

          /* For symlinks we copy the actual symlink value, to correctly handle
             things like /etc/localtime symlinks */
          if (S_ISLNK (st.st_mode))
            {
              ssize_t r;

              target = xmalloc (st.st_size + 1);
              r = readlink (dst_path, target, st.st_size);
              if (r == -1)
                die_with_error ("readlink %s", dst_path);
              target[r] = 0;
            }
          else
            target = strconcat ("/usr/etc/", dirent->d_name);

          if (symlink (target, src_path) != 0)
            die_with_error ("symlink %s", src_path);

          free (dst_path);
          free (src_path);
          free (target);
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
          bool dont_mount = FALSE;
          char *path;
          struct stat st;

          for (i = 0; i < N_ELEMENTS(dont_mount_in_root); i++)
            {
              if (strcmp (dirent->d_name, dont_mount_in_root[i]) == 0)
                {
                  dont_mount = TRUE;
                  break;
                }
            }

          if (dont_mount)
            continue;

          path = strconcat ("/", dirent->d_name);

          if (lstat (path, &st) != 0)
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
          else if (S_ISLNK(st.st_mode))
            {
              ssize_t r;
              char *target;

              target = xmalloc (st.st_size + 1);
              r = readlink (path, target, st.st_size);
              if (r == -1)
                die_with_error ("readlink %s", path);
              target[r] = 0;

              if (symlink (target, dirent->d_name) != 0)
                die_with_error ("symlink %s %s", target, dirent->d_name);
            }

          free (path);
        }
    }
}

static void
create_homedir (int mount_real_home, int mount_home_ro, const char *app_id)
{
  const char *home;
  const char *relative_home;
  const char *writable_home;
  char *app_id_dir;
  const char *relative_app_id_dir;
  struct stat st;

  home = getenv("HOME");
  if (home == NULL)
    return;

  relative_home = get_relative_path (home);

  if (mkdir_with_parents (relative_home, 0755, TRUE))
    die_with_error ("unable to create %s", relative_home);

  if (mount_real_home)
    {
      writable_home = home;
      if (bind_mount (writable_home, relative_home, BIND_RECURSIVE  | (mount_home_ro ? BIND_READONLY : 0)))
	die_with_error ("unable to mount %s", home);
    }

  if (app_id != NULL &&
      (!mount_real_home || mount_home_ro))
    {
      app_id_dir = strconcat3 (home, "/.var/app/", app_id);
      if (stat (app_id_dir, &st) == 0 && S_ISDIR (st.st_mode))
	{
	  relative_app_id_dir = get_relative_path (app_id_dir);
	  if (mkdir_with_parents (relative_app_id_dir, 0755, TRUE))
	    die_with_error ("unable to create %s", relative_app_id_dir);

	  if (bind_mount (app_id_dir, relative_app_id_dir, 0))
	    die_with_error ("unable to mount %s", home);
	}
      free (app_id_dir);
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

static void
block_sigchild (void)
{
  sigset_t mask;

  sigemptyset (&mask);
  sigaddset (&mask, SIGCHLD);

  if (sigprocmask (SIG_BLOCK, &mask, NULL) == -1)
    die_with_error ("sigprocmask");
}

static void
unblock_sigchild (void)
{
  sigset_t mask;

  sigemptyset (&mask);
  sigaddset (&mask, SIGCHLD);

  if (sigprocmask (SIG_UNBLOCK, &mask, NULL) == -1)
    die_with_error ("sigprocmask");
}

static int
close_extra_fds (void *data, int fd)
{
  int *extra_fds = (int *)data;
  int i;

  for (i = 0; extra_fds[i] != -1; i++)
    if (fd == extra_fds[i])
      return 0;

  if (fd <= 2)
    return 0;

  close (fd);
  return 0;
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
  int dont_close[] = { event_fd, -1 };

  /* Close all extra fds in the monitoring process.
     Any passed in fds have been passed on to the child anyway. */
  fdwalk (close_extra_fds, dont_close);

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

#define REQUIRED_CAPS (CAP_TO_MASK(CAP_SYS_ADMIN))

static void
acquire_caps (void)
{
  struct __user_cap_header_struct hdr;
  struct __user_cap_data_struct data;

  memset (&hdr, 0, sizeof(hdr));
  hdr.version = _LINUX_CAPABILITY_VERSION;

  if (capget (&hdr, &data)  < 0)
    die_with_error ("capget failed");

  if (((data.effective & REQUIRED_CAPS) == REQUIRED_CAPS) &&
      ((data.permitted & REQUIRED_CAPS) == REQUIRED_CAPS))
    is_privileged = TRUE;

  if (getuid () != geteuid ())
    {
      /* Tell kernel not clear capabilities when dropping root */
      if (prctl (PR_SET_KEEPCAPS, 1, 0, 0, 0) < 0)
        die_with_error ("prctl(PR_SET_KEEPCAPS) failed");

      /* Drop root uid, but retain the required permitted caps */
      if (setuid (getuid ()) < 0)
        die_with_error ("unable to drop privs");
    }

  if (is_privileged)
    {
      memset (&hdr, 0, sizeof(hdr));
      hdr.version = _LINUX_CAPABILITY_VERSION;

      /* Drop all non-require capabilities */
      data.effective = REQUIRED_CAPS;
      data.permitted = REQUIRED_CAPS;
      data.inheritable = 0;
      if (capset (&hdr, &data) < 0)
        die_with_error ("capset failed");
    }
  /* Else, we try unprivileged user namespaces */
}

static void
drop_caps (void)
{
  struct __user_cap_header_struct hdr;
  struct __user_cap_data_struct data;

  if (!is_privileged)
    return;

  memset (&hdr, 0, sizeof(hdr));
  hdr.version = _LINUX_CAPABILITY_VERSION;
  data.effective = 0;
  data.permitted = 0;
  data.inheritable = 0;

  if (capset (&hdr, &data) < 0)
    die_with_error ("capset failed");

  if (prctl (PR_SET_DUMPABLE, 1, 0, 0, 0) < 0)
    die_with_error ("prctl(PR_SET_DUMPABLE) failed");
}

static char *arg_space;
size_t arg_space_size;

static void
clean_argv (int argc,
            char **argv)
{
  int i;
  char *newargv;

  arg_space = argv[0];
  arg_space_size = argv[argc-1] - argv[0] + strlen (argv[argc-1]) + 1;
  newargv = xmalloc (arg_space_size);
  memcpy (newargv, arg_space, arg_space_size);
  for (i = 0; i < argc; i++)
    argv[i] = newargv + (argv[i] - arg_space);
}

static void
set_procname (const char *name)
{
  strncpy (arg_space, name, arg_space_size);
}

int
main (int argc,
      char **argv)
{
  mode_t old_umask;
  char *newroot;
  char *runtime_path = NULL;
  char *app_path = NULL;
  char *chdir_path = NULL;
  char *monitor_path = NULL;
  char *app_id = NULL;
  char *var_path = NULL;
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
  bool devel = FALSE;
  bool share_shm = FALSE;
  bool network = FALSE;
  bool ipc = FALSE;
  bool mount_host_fs = FALSE;
  bool mount_host_fs_ro = FALSE;
  bool mount_home = FALSE;
  bool mount_home_ro = FALSE;
  bool lock_files = FALSE;
  bool writable = FALSE;
  bool writable_app = FALSE;
  bool writable_exports = FALSE;
  int clone_flags;
  char *old_cwd = NULL;
  int c, i;
  pid_t pid;
  int event_fd;
  int sync_fd = -1;
  char *endp;
  char *uid_map, *gid_map;
  uid_t ns_uid;
  gid_t ns_gid;

  /* Get the (optional) capabilities we need, drop root */
  acquire_caps ();

  /* Never gain any more privs during exec */
  if (prctl (PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
    die_with_error ("prctl(PR_SET_NO_NEW_CAPS) failed");

  clean_argv (argc, argv);

  while ((c =  getopt (argc, argv, "+inWwceEsfFHhra:m:M:b:B:p:x:ly:d:D:v:I:gS:P:")) >= 0)
    {
      switch (c)
        {
        case 'a':
          app_path = optarg;
          break;

        case 'c':
          devel = TRUE;
          break;

        case 'M':
          /* Same, but remove source */
          goto extra_file;

        case 'B':
          /* Same, but non-readonly */
          goto extra_file;

        case 'b':
        extra_file:
          /* Format: DEST[=SOURCE] */
          tmp = strchr (optarg, '=');
          if (tmp == NULL)
            {
              /* no SOURCE, use DEST */
              tmp = optarg;
            }
          else
            {
              if (tmp[1] == 0)
                usage (argv);
              *tmp = 0;
              tmp = tmp + 1;
             }

          if (optarg[0] != '/')
            die ("Extra directories must be absolute paths");

          while (*optarg == '/')
            optarg++;

          if (*optarg == 0)
            die ("Extra directories must not be root");

          add_extra_file (tmp, optarg, c == 'b', c == 'M');
          break;

        case 'd':
          session_dbus_socket = optarg;
          break;

        case 'D':
          system_dbus_socket = optarg;
          break;

        case 'e':
          writable_exports = TRUE;
          break;

        case 'E':
          create_etc_symlink = TRUE;
          create_etc_dir = FALSE;
          break;

        case 'F':
          mount_host_fs = TRUE;
          break;

        case 'f':
          mount_host_fs = TRUE;
          mount_host_fs_ro = TRUE;
          break;

        case 'g':
          allow_dri = TRUE;
          break;

        case 'H':
          mount_home = TRUE;
          break;

        case 'h':
          mount_home = TRUE;
          mount_home_ro = TRUE;
          break;

        case 'i':
          ipc = TRUE;
          break;

        case 'I':
          app_id = optarg;
          break;

        case 'l':
          lock_files = TRUE;
          break;

        case 'm':
          monitor_path = optarg;
          break;

        case 'n':
          network = TRUE;
          break;

        case 'p':
          pulseaudio_socket = optarg;
          break;

        case 'P':
          chdir_path = optarg;
          break;

        case 'r':
          bind_resolv_conf = TRUE;
          break;

        case 's':
          share_shm = TRUE;
          break;

        case 'S':
          sync_fd = strtol (optarg, &endp, 10);
	  if (endp == optarg || *endp != 0)
	    die ("Invalid fd argument");
          break;

        case 'v':
          var_path = optarg;
          break;

        case 'w':
          writable_app = TRUE;
          break;

        case 'W':
          writable = TRUE;
          break;

        case 'x':
          x11_socket = optarg;
          break;

        case 'y':
          wayland_socket = optarg;
          break;

        default: /* '?' */
          usage (argv);
      }
    }

  args = &argv[optind];
  n_args = argc - optind;

  if (monitor_path != NULL && create_etc_dir)
    {
      create_monitor_links = TRUE;
      bind_resolv_conf = FALSE;
    }

  if (n_args < 2)
    usage (argv);

  runtime_path = args[0];
  args++;
  n_args--;

  /* The initial code is run with high permissions
     (at least CAP_SYS_ADMIN), so take lots of care. */

  __debug__(("Creating xdg-app-root dir\n"));

  uid = getuid ();
  gid = getgid ();

  newroot = strdup_printf ("/run/user/%d/.xdg-app-root", uid);
  if (mkdir (newroot, 0755) && errno != EEXIST)
    {
      free (newroot);
      newroot = "/tmp/.xdg-app-root";
      if (mkdir (newroot, 0755) && errno != EEXIST)
	die_with_error ("Creating xdg-app-root failed");
    }

  __debug__(("creating new namespace\n"));

  event_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);

  block_sigchild (); /* Block before we clone to avoid races */

  clone_flags = SIGCHLD | CLONE_NEWNS | CLONE_NEWPID;
  if (!is_privileged)
    clone_flags |= CLONE_NEWUSER;
  if (!network)
    clone_flags |= CLONE_NEWNET;
  if (!ipc)
    clone_flags |= CLONE_NEWIPC;

  pid = raw_clone (clone_flags, NULL);
  if (pid == -1)
    {
      if (!is_privileged)
        {
          if (errno == EINVAL)
            die ("Creating new namespace failed, likely because the kernel does not support user namespaces. Give the xdg-app-helper setuid root or cap_sys_admin+ep rights, or switch to a kernel with user namespace support.");
          else if (errno == EPERM)
            die ("No permissions to creating new namespace, likely because the kernel does not allow non-privileged user namespaces. On e.g. debian this can be enabled with 'sysctl kernel.unprivileged_userns_clone=1'.");
        }

      die_with_error ("Creating new namespace failed");
    }

  if (pid != 0)
    {
      if (app_id)
        set_procname (strdup_printf ("xdg-app-helper %s launcher", app_id));
      monitor_child (event_fd);
      exit (0); /* Should not be reached, but better safe... */
    }

  ns_uid = uid;
  ns_gid = gid;
  if (!is_privileged)
    {
      /* This is a bit hacky, but we need to first map the real uid/gid to
         0, otherwise we can't mount the devpts filesystem because root is
         not mapped. Later we will create another child user namespace and
         map back to the real uid */
      ns_uid = 0;
      ns_gid = 0;

      uid_map = strdup_printf ("%d %d 1\n", ns_uid, uid);
      if (!write_file ("/proc/self/uid_map", uid_map))
        die_with_error ("setting up uid map");
      free (uid_map);

      if (!write_file("/proc/self/setgroups", "deny\n"))
        die_with_error ("error writing to setgroups");

      gid_map = strdup_printf ("%d %d 1\n", ns_gid, gid);
      if (!write_file ("/proc/self/gid_map", gid_map))
        die_with_error ("setting up gid map");
      free (gid_map);
    }

  old_umask = umask (0);

  /* Mark everything as slave, so that we still
   * receive mounts from the real root, but don't
   * propagate mounts to the real root. */
  if (mount (NULL, "/", NULL, MS_SLAVE|MS_REC, NULL) < 0)
    die_with_error ("Failed to make / slave");

  /* Create a tmpfs which we will use as / in the namespace */
  if (mount ("", newroot, "tmpfs", MS_NODEV|MS_NOSUID, NULL) != 0)
    die_with_error ("Failed to mount tmpfs");

  old_cwd = get_current_dir_name ();

  if (chdir (newroot) != 0)
      die_with_error ("chdir");

  create_files (create, N_ELEMENTS (create), share_shm, runtime_path);

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
      if (bind_mount (app_path, "app", BIND_PRIVATE | (writable_app?0:BIND_READONLY)))
        die_with_error ("mount app");

      if (lock_files)
	add_lock_dir ("app");

      if (!writable_app && writable_exports)
	{
	  char *exports = strconcat (app_path, "/exports");

	  if (bind_mount (exports, "app/exports", BIND_PRIVATE))
	    die_with_error ("mount app/exports");

	  free (exports);
	}
    }

  if (var_path != NULL)
    {
      if (bind_mount (var_path, "var", BIND_PRIVATE))
        die_with_error ("mount var");
    }

  create_files (create_post, N_ELEMENTS (create_post), share_shm, runtime_path);

  if (create_etc_dir)
    link_extra_etc_dirs ();

  if (monitor_path)
    {
      char *monitor_mount_path = strdup_printf ("run/user/%d/xdg-app-monitor", uid);

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
          char *xauth_path = strdup_printf ("/run/user/%d/Xauthority", uid);
          if (bind_mount (x11_socket, "tmp/.X11-unix/X99", 0))
            die ("can't bind X11 socket");

          xsetenv ("DISPLAY", ":99.0", 1);
          xsetenv ("XAUTHORITY", xauth_path, 1);
          free (xauth_path);
        }
      else
        {
          xunsetenv ("DISPLAY");
          xunsetenv ("XAUTHORITY");
        }
    }
  else
    {
      xunsetenv ("DISPLAY");
      xunsetenv ("XAUTHORITY");
    }

  /* Bind mount in the Wayland socket */
  if (wayland_socket != 0)
    {
      char *wayland_path_relative = strdup_printf ("run/user/%d/wayland-0", uid);
      if (!create_file (wayland_path_relative, 0666, NULL) ||
          bind_mount (wayland_socket, wayland_path_relative, 0))
        die ("can't bind Wayland socket %s -> %s: %s", wayland_socket, wayland_path_relative, strerror (errno));
      free (wayland_path_relative);
    }

  if (pulseaudio_socket != NULL)
    {
      char *pulse_path_relative = strdup_printf ("run/user/%d/pulse/native", uid);
      char *pulse_server = strdup_printf ("unix:/run/user/%d/pulse/native", uid);
      char *config_path_relative = strdup_printf ("run/user/%d/pulse/config", uid);
      char *config_path_absolute = strdup_printf ("/run/user/%d/pulse/config", uid);
      char *client_config = strdup_printf ("enable-shm=%s\n", share_shm ? "yes" : "no");

      if (create_file (config_path_relative, 0666, client_config) &&
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
      if (create_file ("run/dbus/system_bus_socket", 0666, NULL) &&
          bind_mount (system_dbus_socket, "run/dbus/system_bus_socket", 0) == 0)
        xsetenv ("DBUS_SYSTEM_BUS_ADDRESS",  "unix:path=/var/run/dbus/system_bus_socket", 1);
      else
        xunsetenv ("DBUS_SYSTEM_BUS_ADDRESS");
   }

  if (session_dbus_socket != NULL)
    {
      char *session_dbus_socket_path_relative = strdup_printf ("run/user/%d/bus", uid);
      char *session_dbus_address = strdup_printf ("unix:path=/run/user/%d/bus", uid);

      if (create_file (session_dbus_socket_path_relative, 0666, NULL) &&
          bind_mount (session_dbus_socket, session_dbus_socket_path_relative, 0) == 0)
        xsetenv ("DBUS_SESSION_BUS_ADDRESS",  session_dbus_address, 1);
      else
        xunsetenv ("DBUS_SESSION_BUS_ADDRESS");

      free (session_dbus_socket_path_relative);
      free (session_dbus_address);
   }

  if (mount_host_fs)
    {
      mount_extra_root_dirs (mount_host_fs_ro);
      bind_mount ("/run/media", "run/media", BIND_RECURSIVE | (mount_host_fs_ro ? BIND_READONLY : 0));
    }

  if (!mount_host_fs || mount_host_fs_ro)
    create_homedir (mount_home, mount_home_ro, app_id);

  if (mount_host_fs || mount_home)
    {
      char *dconf_run_path = strdup_printf ("/run/user/%d/dconf", uid);

      /* If the user has homedir access, also allow dconf run dir access */
      bind_mount (dconf_run_path, get_relative_path (dconf_run_path), 0);
      free (dconf_run_path);
    }

  for (i = 0; i < n_extra_files; i++)
    {
      bool is_dir;

      is_dir = stat_is_dir (extra_files[i].src);

      if (mkdir_with_parents (extra_files[i].dest, 0755,
                              is_dir && !extra_files[i].move))
        die_with_error ("create extra dir %s", extra_files[i].dest);

      if (extra_files[i].move)
        {
          if (!copy_file (extra_files[i].src, extra_files[i].dest, 0700))
            die_with_error ("copy extra file %s", extra_files[i].dest);
          if (unlink (extra_files[i].src) != 0)
            die_with_error ("unlink moved extra file %s", extra_files[i].src);
        }
      else
        {
          if (!is_dir)
            create_file (extra_files[i].dest, 0700, NULL);

          if (bind_mount (extra_files[i].src, extra_files[i].dest, BIND_PRIVATE | (extra_files[i].readonly ? BIND_READONLY : 0)))
            die_with_error ("mount extra dir %s", extra_files[i].src);

          if (lock_files && is_dir)
            add_lock_dir (extra_files[i].dest);
        }
    }

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

  if (chdir_path)
    {
      if (chdir (chdir_path))
        die_with_error ("Can't chdir to %s", chdir_path);
      xsetenv ("PWD", chdir_path, 1);
    }
  else if (chdir (old_cwd) == 0)
    {
      xsetenv ("PWD", old_cwd, 1);
    }
  else
    {
      /* If the old cwd is not mapped, go to home */
      const char *home = getenv("HOME");
      if (home == NULL)
        home = "/";

      chdir (home);
      xsetenv ("PWD", home, 1);
    }
  free (old_cwd);

  /* We can't pass regular LD_LIBRARY_PATH, as it would affect the
     setuid helper aspect, so we use _LD_LIBRARY_PATH */
  if (getenv("_LD_LIBRARY_PATH"))
    {
      xsetenv ("LD_LIBRARY_PATH", getenv("_LD_LIBRARY_PATH"), 1);
      xunsetenv ("_LD_LIBRARY_PATH");
    }
  else
    xunsetenv ("LD_LIBRARY_PATH");

  xdg_runtime_dir = strdup_printf ("/run/user/%d", uid);
  xsetenv ("XDG_RUNTIME_DIR", xdg_runtime_dir, 1);
  free (xdg_runtime_dir);
  if (monitor_path)
    {
      tz_val = strdup_printf (":/run/user/%d/xdg-app-monitor/localtime", uid);
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

      if (ns_uid != uid || ns_gid != gid)
        {
          /* Now that devpts is mounted we can create a new userspace
             and map our uid 1:1 */

          if (unshare (CLONE_NEWUSER))
            die_with_error ("unshare user ns");

          uid_map = strdup_printf ("%d 0 1\n", uid);
          if (!write_file ("/proc/self/uid_map", uid_map))
            die_with_error ("setting up uid map");
          free (uid_map);

          gid_map = strdup_printf ("%d 0 1\n", gid);
          if (!write_file ("/proc/self/gid_map", gid_map))
            die_with_error ("setting up gid map");
          free (gid_map);
        }

      __debug__(("setting up seccomp in child\n"));
      setup_seccomp (devel);

      if (sync_fd != -1)
	close (sync_fd);

      unblock_sigchild ();

      if (execvp (args[0], args) == -1)
        die_with_error ("execvp %s", args[0]);
      return 0;
    }

  __debug__(("setting up seccomp in monitor\n"));
  setup_seccomp (devel);

  /* Close all extra fds in pid 1.
     Any passed in fds have been passed on to the child anyway. */
  {
    int dont_close[] = { event_fd, sync_fd, -1 };
    fdwalk (close_extra_fds, dont_close);
  }

  if (app_id)
    set_procname (strdup_printf ("xdg-app-helper %s monitor", app_id));
  return do_init (event_fd, pid);
}
