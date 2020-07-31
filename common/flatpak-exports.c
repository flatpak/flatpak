/*
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

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <sys/personality.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <grp.h>
#include <unistd.h>
#include <gio/gunixfdlist.h>

#include <glib/gi18n-lib.h>

#include <gio/gio.h>
#include "libglnx/libglnx.h"

#include "flatpak-exports-private.h"
#include "flatpak-run-private.h"
#include "flatpak-proxy.h"
#include "flatpak-utils-base-private.h"
#include "flatpak-dir-private.h"
#include "flatpak-systemd-dbus-generated.h"
#include "flatpak-error.h"

/* We don't want to export paths pointing into these, because they are readonly
   (so we can't create mountpoints there) and don't match what's on the host anyway.
   flatpak_abs_usrmerged_dirs get the same treatment without having to be listed
   here. */
const char *dont_export_in[] = {
  "/usr", "/etc", "/app", "/dev", "/proc", NULL
};

static char *
make_relative (const char *base, const char *path)
{
  GString *s = g_string_new ("");

  while (*base != 0)
    {
      while (*base == '/')
        base++;

      if (*base != 0)
        g_string_append (s, "../");

      while (*base != '/' && *base != 0)
        base++;
    }

  while (*path == '/')
    path++;

  g_string_append (s, path);

  return g_string_free (s, FALSE);
}

#define FAKE_MODE_DIR -1 /* Ensure a dir, either on tmpfs or mapped parent */
#define FAKE_MODE_TMPFS 0
#define FAKE_MODE_SYMLINK G_MAXINT

typedef struct
{
  char *path;
  gint  mode;
} ExportedPath;

struct _FlatpakExports
{
  GHashTable           *hash;
  FlatpakFilesystemMode host_etc;
  FlatpakFilesystemMode host_os;
};

static void
exported_path_free (ExportedPath *exported_path)
{
  g_free (exported_path->path);
  g_free (exported_path);
}

FlatpakExports *
flatpak_exports_new (void)
{
  FlatpakExports *exports = g_new0 (FlatpakExports, 1);

  exports->hash = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GFreeFunc) exported_path_free);
  return exports;
}

void
flatpak_exports_free (FlatpakExports *exports)
{
  g_hash_table_destroy (exports->hash);
  g_free (exports);
}

/* Returns TRUE if the location of this export
   is not visible due to parents being exported */
static gboolean
path_parent_is_mapped (const char **keys,
                       guint        n_keys,
                       GHashTable  *hash_table,
                       const char  *path)
{
  guint i;
  gboolean is_mapped = FALSE;

  /* The keys are sorted so shorter (i.e. parents) are first */
  for (i = 0; i < n_keys; i++)
    {
      const char *mounted_path = keys[i];
      ExportedPath *ep = g_hash_table_lookup (hash_table, mounted_path);

      if (flatpak_has_path_prefix (path, mounted_path) &&
          (strcmp (path, mounted_path) != 0))
        {
          /* FAKE_MODE_DIR has same mapped value as parent */
          if (ep->mode == FAKE_MODE_DIR)
            continue;

          is_mapped = ep->mode != FAKE_MODE_TMPFS;
        }
    }

  return is_mapped;
}

static gboolean
path_is_mapped (const char **keys,
                guint        n_keys,
                GHashTable  *hash_table,
                const char  *path,
                gboolean    *is_readonly_out)
{
  guint i;
  gboolean is_mapped = FALSE;
  gboolean is_readonly = FALSE;

  /* The keys are sorted so shorter (i.e. parents) are first */
  for (i = 0; i < n_keys; i++)
    {
      const char *mounted_path = keys[i];
      ExportedPath *ep = g_hash_table_lookup (hash_table, mounted_path);

      if (flatpak_has_path_prefix (path, mounted_path))
        {
          /* FAKE_MODE_DIR has same mapped value as parent */
          if (ep->mode == FAKE_MODE_DIR)
            continue;

          if (ep->mode == FAKE_MODE_SYMLINK)
            is_mapped = strcmp (path, mounted_path) == 0;
          else
            is_mapped = ep->mode != FAKE_MODE_TMPFS;

          if (is_mapped)
            is_readonly = ep->mode == FLATPAK_FILESYSTEM_MODE_READ_ONLY;
          else
            is_readonly = FALSE;
        }
    }

  *is_readonly_out = is_readonly;
  return is_mapped;
}

static gint
compare_eps (const ExportedPath *a,
             const ExportedPath *b)
{
  return g_strcmp0 (a->path, b->path);
}

/* This differs from g_file_test (path, G_FILE_TEST_IS_DIR) which
   returns true if the path is a symlink to a dir */
static gboolean
path_is_dir (const char *path)
{
  struct stat s;

  if (lstat (path, &s) != 0)
    return FALSE;

  return S_ISDIR (s.st_mode);
}

static gboolean
path_is_symlink (const char *path)
{
  struct stat s;

  if (lstat (path, &s) != 0)
    return FALSE;

  return S_ISLNK (s.st_mode);
}

/*
 * @name: A file or directory below /etc
 * @test: How we test whether it is suitable
 *
 * The paths in /etc that are required if we want to make use of the
 * host /usr (and /lib, and so on).
 */
typedef struct
{
  const char *name;
  GFileTest test;
} LibsNeedEtc;

static const LibsNeedEtc libs_need_etc[] =
{
  /* glibc */
  { "ld.so.cache", G_FILE_TEST_IS_REGULAR },
  /* Used for executables and a few libraries on e.g. Debian */
  { "alternatives", G_FILE_TEST_IS_DIR }
};

void
flatpak_exports_append_bwrap_args (FlatpakExports *exports,
                                   FlatpakBwrap   *bwrap)
{
  guint n_keys;
  g_autofree const char **keys = (const char **) g_hash_table_get_keys_as_array (exports->hash, &n_keys);
  g_autoptr(GList) eps = NULL;
  GList *l;

  eps = g_hash_table_get_values (exports->hash);
  eps = g_list_sort (eps, (GCompareFunc) compare_eps);

  g_qsort_with_data (keys, n_keys, sizeof (char *), (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);

  for (l = eps; l != NULL; l = l->next)
    {
      ExportedPath *ep = l->data;
      const char *path = ep->path;

      if (ep->mode == FAKE_MODE_SYMLINK)
        {
          if (!path_parent_is_mapped (keys, n_keys, exports->hash, path))
            {
              g_autofree char *resolved = flatpak_resolve_link (path, NULL);
              if (resolved)
                {
                  g_autofree char *parent = g_path_get_dirname (path);
                  g_autofree char *relative = make_relative (parent, resolved);
                  flatpak_bwrap_add_args (bwrap, "--symlink", relative, path,  NULL);
                }
            }
        }
      else if (ep->mode == FAKE_MODE_TMPFS)
        {
          /* Mount a tmpfs to hide the subdirectory, but only if there
             is a pre-existing dir we can mount the path on. */
          if (path_is_dir (path))
            {
              if (!path_parent_is_mapped (keys, n_keys, exports->hash, path))
                /* If the parent is not mapped, it will be a tmpfs, no need to mount another one */
                flatpak_bwrap_add_args (bwrap, "--dir", path, NULL);
              else
                flatpak_bwrap_add_args (bwrap, "--tmpfs", path, NULL);
            }
        }
      else if (ep->mode == FAKE_MODE_DIR)
        {
          if (path_is_dir (path))
            flatpak_bwrap_add_args (bwrap, "--dir", path, NULL);
        }
      else
        {
          flatpak_bwrap_add_args (bwrap,
                                  (ep->mode == FLATPAK_FILESYSTEM_MODE_READ_ONLY) ? "--ro-bind" : "--bind",
                                  path, path, NULL);
        }
    }

  if (exports->host_os != 0)
    {
      const char *os_bind_mode = "--bind";
      int i;

      if (exports->host_os == FLATPAK_FILESYSTEM_MODE_READ_ONLY)
        os_bind_mode = "--ro-bind";

      if (g_file_test ("/usr", G_FILE_TEST_IS_DIR))
        flatpak_bwrap_add_args (bwrap,
                                os_bind_mode, "/usr", "/run/host/usr", NULL);

      for (i = 0; flatpak_abs_usrmerged_dirs[i] != NULL; i++)
        {
          const char *subdir = flatpak_abs_usrmerged_dirs[i];
          g_autofree char *target = NULL;
          g_autofree char *run_host_subdir = NULL;

          g_assert (subdir[0] == '/');
          /* e.g. /run/host/lib32 */
          run_host_subdir = g_strconcat ("/run/host", subdir, NULL);
          target = glnx_readlinkat_malloc (-1, subdir, NULL, NULL);

          if (target != NULL &&
              g_str_has_prefix (target, "usr/"))
            {
              /* e.g. /lib32 is a relative symlink to usr/lib32, or
               * on Arch Linux, /lib64 is a relative symlink to usr/lib;
               * keep it relative */
              flatpak_bwrap_add_args (bwrap,
                                      "--symlink", target, run_host_subdir,
                                      NULL);
            }
          else if (target != NULL &&
                   g_str_has_prefix (target, "/usr/"))
            {
              /* e.g. /lib32 is an absolute symlink to /usr/lib32; make
               * it a relative symlink to usr/lib32 instead by skipping
               * the '/' */
              flatpak_bwrap_add_args (bwrap,
                                      "--symlink", target + 1, run_host_subdir,
                                      NULL);
            }
          else if (g_file_test (subdir, G_FILE_TEST_IS_DIR))
            {
              /* e.g. /lib32 is a symlink to /opt/compat/ia32/lib,
               * or is a plain directory because the host OS has not
               * undergone the /usr merge; bind-mount the directory instead */
              flatpak_bwrap_add_args (bwrap,
                                      os_bind_mode, subdir, run_host_subdir,
                                      NULL);
            }
        }

      if (exports->host_etc == 0)
        {
          guint i;

          /* We are exposing the host /usr (and friends) but not the
           * host /etc. Additionally expose just enough of /etc to make
           * things that want to read /usr work as expected.
           *
           * (If exports->host_etc is nonzero, we'll do this as part of
           * /etc instead.) */

          for (i = 0; i < G_N_ELEMENTS (libs_need_etc); i++)
            {
              const LibsNeedEtc *item = &libs_need_etc[i];
              g_autofree gchar *host_path = g_strconcat ("/etc/", item->name, NULL);

              if (g_file_test (host_path, item->test))
                {
                  g_autofree gchar *run_host_path = g_strconcat ("/run/host/etc/", item->name, NULL);

                  flatpak_bwrap_add_args (bwrap,
                                          os_bind_mode, host_path, run_host_path,
                                          NULL);
                }
            }
        }
    }

  if (exports->host_etc != 0)
    {
      const char *etc_bind_mode = "--bind";

      if (exports->host_etc == FLATPAK_FILESYSTEM_MODE_READ_ONLY)
        etc_bind_mode = "--ro-bind";

      if (g_file_test ("/etc", G_FILE_TEST_IS_DIR))
        flatpak_bwrap_add_args (bwrap,
                                etc_bind_mode, "/etc", "/run/host/etc", NULL);
    }
}

/* Returns 0 if not visible */
FlatpakFilesystemMode
flatpak_exports_path_get_mode (FlatpakExports *exports,
                               const char     *path)
{
  guint n_keys;
  g_autofree const char **keys = (const char **) g_hash_table_get_keys_as_array (exports->hash, &n_keys);
  g_autofree char *canonical = NULL;
  gboolean is_readonly = FALSE;
  g_auto(GStrv) parts = NULL;
  int i;
  g_autoptr(GString) path_builder = g_string_new ("");
  struct stat st;

  g_qsort_with_data (keys, n_keys, sizeof (char *), (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);

  path = canonical = flatpak_canonicalize_filename (path);

  parts = g_strsplit (path + 1, "/", -1);

  /* A path is visible in the sandbox if no parent
   * path element that is mapped in the sandbox is
   * a symlink, and the final element is mapped.
   * If any parent is a symlink we resolve that and
   * continue with that instead.
   */
  for (i = 0; parts[i] != NULL; i++)
    {
      g_string_append (path_builder, "/");
      g_string_append (path_builder, parts[i]);

      if (path_is_mapped (keys, n_keys, exports->hash, path_builder->str, &is_readonly))
        {
          if (lstat (path_builder->str, &st) != 0)
            {
              if (errno == ENOENT && parts[i + 1] == NULL && !is_readonly)
                {
                  /* Last element was mapped but isn't there, this is
                   * OK (used for the save case) if we the parent is
                   * mapped and writable, as the app can then create
                   * the file here.
                   */
                  break;
                }

              return 0;
            }

          if (S_ISLNK (st.st_mode))
            {
              g_autofree char *resolved = flatpak_resolve_link (path_builder->str, NULL);
              g_autoptr(GString) path2_builder = NULL;
              int j;

              if (resolved == NULL)
                return 0;

              path2_builder = g_string_new (resolved);

              for (j = i + 1; parts[j] != NULL; j++)
                {
                  g_string_append (path2_builder, "/");
                  g_string_append (path2_builder, parts[j]);
                }

              return flatpak_exports_path_get_mode (exports, path2_builder->str);
            }
        }
      else if (parts[i + 1] == NULL)
        return 0; /* Last part was not mapped */
    }

  if (is_readonly)
    return FLATPAK_FILESYSTEM_MODE_READ_ONLY;

  return FLATPAK_FILESYSTEM_MODE_READ_WRITE;
}

gboolean
flatpak_exports_path_is_visible (FlatpakExports *exports,
                                 const char     *path)
{
  return flatpak_exports_path_get_mode (exports, path) > 0;
}

static gboolean
never_export_as_symlink (const char *path)
{
  /* Don't export /tmp as a symlink even if it is on the host, because
     that will fail with the pre-existing directory we created for /tmp,
     and anyway, it being a symlink is not useful in the sandbox */
  if (strcmp (path, "/tmp") == 0)
    return TRUE;

  return FALSE;
}

static void
do_export_path (FlatpakExports *exports,
                const char     *path,
                gint            mode)
{
  ExportedPath *old_ep = g_hash_table_lookup (exports->hash, path);
  ExportedPath *ep;

  ep = g_new0 (ExportedPath, 1);
  ep->path = g_strdup (path);

  if (old_ep != NULL)
    ep->mode = MAX (old_ep->mode, mode);
  else
    ep->mode = mode;

  g_hash_table_replace (exports->hash, ep->path, ep);
}

/* AUTOFS mounts are tricky, as using them as a source in a bind mount
 * causes the mount to trigger, which can take a long time (or forever)
 * waiting for a device or network mount. We try to open the directory
 * but time out after a while, ignoring the mount. Unfortunately we
 * have to mess with forks and stuff to be able to handle the timeout.
 */
static gboolean
check_if_autofs_works (const char *path)
{
  int selfpipe[2];
  struct timeval timeout;
  pid_t pid;
  fd_set rfds;
  int res;
  int wstatus;

  if (pipe2 (selfpipe, O_CLOEXEC) == -1)
    return FALSE;

  fcntl (selfpipe[0], F_SETFL, fcntl (selfpipe[0], F_GETFL) | O_NONBLOCK);
  fcntl (selfpipe[1], F_SETFL, fcntl (selfpipe[1], F_GETFL) | O_NONBLOCK);

  pid = fork ();
  if (pid == -1)
    {
      close (selfpipe[0]);
      close (selfpipe[1]);
      return FALSE;
    }

  if (pid == 0)
    {
      /* Note: open, close and _exit are signal-async-safe, so it is ok to call in the child after fork */

      close (selfpipe[0]); /* Close unused read end */
      int dir_fd = open (path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_DIRECTORY);
      _exit (dir_fd == -1 ? 1 : 0);
    }

  /* Parent */
  close (selfpipe[1]);  /* Close unused write end */

  /* 200 msec timeout*/
  timeout.tv_sec = 0;
  timeout.tv_usec = 200 * 1000;

  FD_ZERO (&rfds);
  FD_SET (selfpipe[0], &rfds);
  res = select (selfpipe[0] + 1, &rfds, NULL, NULL, &timeout);

  close (selfpipe[0]);

  if (res == -1 /* Error */ || res == 0) /* Timeout */
    {
      /* Kill, but then waitpid to avoid zombie */
      kill (pid, SIGKILL);
    }

  if (waitpid (pid, &wstatus, 0) != pid)
    return FALSE;

  if (res == -1 /* Error */ || res == 0) /* Timeout */
    return FALSE;

  if (!WIFEXITED (wstatus) || WEXITSTATUS (wstatus) != 0)
    return FALSE;

  return TRUE;
}

/* We use level to avoid infinite recursion */
static gboolean
_exports_path_expose (FlatpakExports *exports,
                      int             mode,
                      const char     *path,
                      int             level)
{
  g_autofree char *canonical = NULL;
  struct stat st;
  struct statfs stfs;
  char *slash;
  int i;
  glnx_autofd int o_path_fd = -1;

  if (level > 40) /* 40 is the current kernel ELOOP check */
    {
      g_debug ("Expose too deep, bail");
      return FALSE;
    }

  if (!g_path_is_absolute (path))
    {
      g_debug ("Not exposing relative path %s", path);
      return FALSE;
    }

  /* Check if it exists at all */
  o_path_fd = open (path, O_PATH | O_NOFOLLOW | O_CLOEXEC);
  if (o_path_fd == -1)
    return FALSE;

  if (fstat (o_path_fd, &st) != 0)
    return FALSE;

  /* Don't expose weird things */
  if (!(S_ISDIR (st.st_mode) ||
        S_ISREG (st.st_mode) ||
        S_ISLNK (st.st_mode) ||
        S_ISSOCK (st.st_mode)))
    return FALSE;

  /* O_PATH + fstatfs is the magic that we need to statfs without automounting the target */
  if (fstatfs (o_path_fd, &stfs) != 0)
    return FALSE;

  if (stfs.f_type == AUTOFS_SUPER_MAGIC)
    {
      if (!check_if_autofs_works (path))
        {
          g_debug ("ignoring blocking autofs path %s", path);
          return FALSE;
        }
    }

  path = canonical = flatpak_canonicalize_filename (path);

  for (i = 0; dont_export_in[i] != NULL; i++)
    {
      /* Don't expose files in non-mounted dirs like /app or /usr, as
         they are not the same as on the host, and we generally can't
         create the parents for them anyway */
      if (flatpak_has_path_prefix (path, dont_export_in[i]))
        {
          g_debug ("skipping export for path %s", path);
          return FALSE;
        }
    }

  for (i = 0; flatpak_abs_usrmerged_dirs[i] != NULL; i++)
    {
      /* Same as /usr, but for the directories that get merged into /usr */
      if (flatpak_has_path_prefix (path, flatpak_abs_usrmerged_dirs[i]))
        {
          g_debug ("skipping export for path %s", path);
          return FALSE;
        }
    }

  /* Handle any symlinks prior to the target itself. This includes path itself,
     because we expose the target of the symlink. */
  slash = canonical;
  do
    {
      slash = strchr (slash + 1, '/');
      if (slash)
        *slash = 0;

      if (path_is_symlink (path) && !never_export_as_symlink (path))
        {
          g_autofree char *resolved = flatpak_resolve_link (path, NULL);
          g_autofree char *new_target = NULL;

          if (resolved)
            {
              if (slash)
                new_target = g_build_filename (resolved, slash + 1, NULL);
              else
                new_target = g_strdup (resolved);

              if (_exports_path_expose (exports, mode, new_target, level + 1))
                {
                  do_export_path (exports, path, FAKE_MODE_SYMLINK);
                  return TRUE;
                }
            }

          return FALSE;
        }
      if (slash)
        *slash = '/';
    }
  while (slash != NULL);

  do_export_path (exports, path, mode);
  return TRUE;
}

void
flatpak_exports_add_path_expose (FlatpakExports       *exports,
                                 FlatpakFilesystemMode mode,
                                 const char           *path)
{
  _exports_path_expose (exports, mode, path, 0);
}

void
flatpak_exports_add_path_tmpfs (FlatpakExports *exports,
                                const char     *path)
{
  _exports_path_expose (exports, FAKE_MODE_TMPFS, path, 0);
}

void
flatpak_exports_add_path_expose_or_hide (FlatpakExports       *exports,
                                         FlatpakFilesystemMode mode,
                                         const char           *path)
{
  if (mode == 0)
    flatpak_exports_add_path_tmpfs (exports, path);
  else
    flatpak_exports_add_path_expose (exports, mode, path);
}

void
flatpak_exports_add_path_dir (FlatpakExports *exports,
                              const char     *path)
{
  _exports_path_expose (exports, FAKE_MODE_DIR, path, 0);
}

void
flatpak_exports_add_host_etc_expose (FlatpakExports       *exports,
                                     FlatpakFilesystemMode mode)
{
  exports->host_etc = mode;
}

void
flatpak_exports_add_host_os_expose (FlatpakExports       *exports,
                                    FlatpakFilesystemMode mode)
{
  exports->host_os = mode;
}
