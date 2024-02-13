/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2014-2018 Red Hat, Inc
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
#include <sys/personality.h>
#include <grp.h>
#include <unistd.h>
#include <gio/gunixfdlist.h>

#include <glib/gi18n-lib.h>

#include <gio/gio.h>
#include "libglnx.h"

#include "flatpak-bwrap-private.h"
#include "flatpak-utils-private.h"
#include "flatpak-utils-base-private.h"

static void
clear_fd (gpointer data)
{
  int *fd_p = data;

  if (fd_p != NULL && *fd_p != -1)
    close (*fd_p);
}

char *flatpak_bwrap_empty_env[] = { NULL };

FlatpakBwrap *
flatpak_bwrap_new (char **env)
{
  FlatpakBwrap *bwrap = g_new0 (FlatpakBwrap, 1);

  bwrap->argv = g_ptr_array_new_with_free_func (g_free);
  bwrap->noinherit_fds = g_array_new (FALSE, TRUE, sizeof (int));
  g_array_set_clear_func (bwrap->noinherit_fds, clear_fd);
  bwrap->fds = g_array_new (FALSE, TRUE, sizeof (int));
  g_array_set_clear_func (bwrap->fds, clear_fd);

  if (env)
    bwrap->envp = g_strdupv (env);
  else
    bwrap->envp = g_get_environ ();

  bwrap->sync_fds[0] = -1;
  bwrap->sync_fds[1] = -1;

  return bwrap;
}

void
flatpak_bwrap_free (FlatpakBwrap *bwrap)
{
  g_ptr_array_unref (bwrap->argv);
  g_array_unref (bwrap->noinherit_fds);
  g_array_unref (bwrap->fds);
  g_strfreev (bwrap->envp);
  g_clear_pointer (&bwrap->runtime_dir_members, g_ptr_array_unref);
  g_free (bwrap);
}

gboolean
flatpak_bwrap_is_empty (FlatpakBwrap *bwrap)
{
  return bwrap->argv->len == 0;
}

void
flatpak_bwrap_set_env (FlatpakBwrap *bwrap,
                       const char   *variable,
                       const char   *value,
                       gboolean      overwrite)
{
  bwrap->envp = g_environ_setenv (bwrap->envp, variable, value, overwrite);
}

void
flatpak_bwrap_unset_env (FlatpakBwrap *bwrap,
                         const char   *variable)
{
  bwrap->envp = g_environ_unsetenv (bwrap->envp, variable);
}

void
flatpak_bwrap_add_arg (FlatpakBwrap *bwrap, const char *arg)
{
  g_ptr_array_add (bwrap->argv, g_strdup (arg));
}

/*
 * flatpak_bwrap_take_arg:
 * @arg: (transfer full): Take ownership of this argument
 *
 * Add @arg to @bwrap's argv, taking ownership of the pointer.
 */
void
flatpak_bwrap_take_arg (FlatpakBwrap *bwrap, char *arg)
{
  g_ptr_array_add (bwrap->argv, arg);
}

void
flatpak_bwrap_finish (FlatpakBwrap *bwrap)
{
  g_ptr_array_add (bwrap->argv, NULL);
}

void
flatpak_bwrap_add_noinherit_fd (FlatpakBwrap *bwrap,
                                int           fd)
{
  g_array_append_val (bwrap->noinherit_fds, fd);
}

void
flatpak_bwrap_add_fd (FlatpakBwrap *bwrap,
                      int           fd)
{
  g_array_append_val (bwrap->fds, fd);
}

void
flatpak_bwrap_add_arg_printf (FlatpakBwrap *bwrap, const char *format, ...)
{
  va_list args;

  va_start (args, format);
  g_ptr_array_add (bwrap->argv, g_strdup_vprintf (format, args));
  va_end (args);
}
void
flatpak_bwrap_add_args (FlatpakBwrap *bwrap, ...)
{
  va_list args;
  const gchar *arg;

  va_start (args, bwrap);
  while ((arg = va_arg (args, const gchar *)))
    flatpak_bwrap_add_arg (bwrap, arg);
  va_end (args);
}

void
flatpak_bwrap_append_argsv (FlatpakBwrap *bwrap,
                            char        **args,
                            int           len)
{
  int i;

  if (len < 0)
    len = g_strv_length (args);

  for (i = 0; i < len; i++)
    g_ptr_array_add (bwrap->argv, g_strdup (args[i]));
}

void
flatpak_bwrap_append_args (FlatpakBwrap *bwrap,
                           GPtrArray    *other_array)
{
  flatpak_bwrap_append_argsv (bwrap,
                              (char **) other_array->pdata,
                              other_array->len);
}

static int *
flatpak_bwrap_steal_fds (FlatpakBwrap *bwrap,
                         gsize        *len_out)
{
  gsize len = bwrap->fds->len;
  int *res = (int *) g_array_free (bwrap->fds, FALSE);

  bwrap->fds = g_array_new (FALSE, TRUE, sizeof (int));
  *len_out = len;
  return res;
}

void
flatpak_bwrap_append_bwrap (FlatpakBwrap *bwrap,
                            FlatpakBwrap *other)
{
  g_autofree int *fds = NULL;
  gsize n_fds, i;

  fds = flatpak_bwrap_steal_fds (other, &n_fds);
  for (i = 0; i < n_fds; i++)
    flatpak_bwrap_add_fd (bwrap, fds[i]);

  flatpak_bwrap_append_argsv (bwrap,
                              (char **) other->argv->pdata,
                              other->argv->len);

  for (i = 0; other->envp[i] != NULL; i++)
    {
      char *key_val = other->envp[i];
      char *eq = strchr (key_val, '=');
      if (eq)
        {
          g_autofree char *key = g_strndup (key_val, eq - key_val);
          flatpak_bwrap_set_env (bwrap,
                                 key, eq + 1, TRUE);
        }
    }

  if (other->runtime_dir_members != NULL)
    {
      if (bwrap->runtime_dir_members == NULL)
        bwrap->runtime_dir_members = g_ptr_array_new_with_free_func (g_free);

      for (i = 0; i < other->runtime_dir_members->len; i++)
        g_ptr_array_add (bwrap->runtime_dir_members,
                         g_strdup (g_ptr_array_index (other->runtime_dir_members, i)));
    }
}

void
flatpak_bwrap_add_args_data_fd (FlatpakBwrap *bwrap,
                                const char   *op,
                                int           fd,
                                const char   *path_optional)
{
  g_autofree char *fd_str = g_strdup_printf ("%d", fd);

  flatpak_bwrap_add_fd (bwrap, fd);
  flatpak_bwrap_add_args (bwrap,
                          op, fd_str, path_optional,
                          NULL);
}


/* Given a buffer @content of size @content_size, generate a fd (memfd if available)
 * of the data.  The @name parameter is used by memfd_create() as a debugging aid;
 * it has no semantic meaning.  The bwrap command line will inject it into the target
 * container as @path.
 */
gboolean
flatpak_bwrap_add_args_data (FlatpakBwrap *bwrap,
                             const char   *name,
                             const char   *content,
                             gssize        content_size,
                             const char   *path,
                             GError      **error)
{
  g_auto(GLnxTmpfile) args_tmpf  = { 0, };

  if (!flatpak_buffer_to_sealed_memfd_or_tmpfile (&args_tmpf, name, content, content_size, error))
    return FALSE;

  flatpak_bwrap_add_args_data_fd (bwrap, "--ro-bind-data", g_steal_fd (&args_tmpf.fd), path);
  return TRUE;
}

/* This resolves the target here rather than in bwrap, because it may
 * not resolve in bwrap setup due to absolute symlinks conflicting
 * with /newroot root. For example, dest could be inside
 * ~/.var/app/XXX where XXX is an absolute symlink.  However, in the
 * usecases here the destination file often doesn't exist, so we
 * only resolve the directory part.
 */
void
flatpak_bwrap_add_bind_arg (FlatpakBwrap *bwrap,
                            const char   *type,
                            const char   *src,
                            const char   *dest)
{
  g_autofree char *dest_dirname = g_path_get_dirname (dest);
  g_autofree char *dest_dirname_real = realpath (dest_dirname, NULL);

  if (dest_dirname_real)
    {
      g_autofree char *dest_basename = g_path_get_basename (dest);
      g_autofree char *dest_real = g_build_filename (dest_dirname_real, dest_basename, NULL);
      flatpak_bwrap_add_args (bwrap, type, src, dest_real, NULL);
    }
}

/*
 * Sort bwrap->envp. This has no practical effect, but it's easier to
 * see what is going on in a large environment block if the variables
 * are sorted.
 */
void
flatpak_bwrap_sort_envp (FlatpakBwrap *bwrap)
{
  if (bwrap->envp != NULL)
    {
      qsort (bwrap->envp, g_strv_length (bwrap->envp), sizeof (char *),
             flatpak_envp_cmp);
    }
}

/*
 * Convert bwrap->envp into a series of --setenv arguments for bwrap(1),
 * assumed to be applied to an empty environment. Reset envp to be an
 * empty environment.
 */
void
flatpak_bwrap_envp_to_args (FlatpakBwrap *bwrap)
{
  gsize i;

  for (i = 0; bwrap->envp[i] != NULL; i++)
    {
      char *key_val = bwrap->envp[i];
      char *eq = strchr (key_val, '=');

      if (eq)
        {
          flatpak_bwrap_add_arg (bwrap, "--setenv");
          flatpak_bwrap_take_arg (bwrap, g_strndup (key_val, eq - key_val));
          flatpak_bwrap_add_arg (bwrap, eq + 1);
        }
      else
        {
          g_warn_if_reached ();
        }
    }

  g_strfreev (g_steal_pointer (&bwrap->envp));
  bwrap->envp = g_strdupv (flatpak_bwrap_empty_env);
}

gboolean
flatpak_bwrap_bundle_args (FlatpakBwrap *bwrap,
                           int           start,
                           int           end,
                           gboolean      one_arg,
                           GError      **error)
{
  g_autofree gchar *data = NULL;
  gchar *ptr;
  gint i;
  gsize data_len = 0;
  int fd;
  g_auto(GLnxTmpfile) args_tmpf  = { 0, };

  if (end == -1)
    end = bwrap->argv->len;

  for (i = start; i < end; i++)
    data_len +=  strlen (bwrap->argv->pdata[i]) + 1;

  data = g_new (gchar, data_len);
  ptr = data;
  for (i = start; i < end; i++)
    ptr = g_stpcpy (ptr, bwrap->argv->pdata[i]) + 1;

  if (!flatpak_buffer_to_sealed_memfd_or_tmpfile (&args_tmpf, "bwrap-args", data, data_len, error))
    return FALSE;

  fd = g_steal_fd (&args_tmpf.fd);

  g_debug ("bwrap --args %d = ...", fd);

  for (i = start; i < end; i++)
    {
      if (flatpak_argument_needs_quoting (bwrap->argv->pdata[i]))
        {
          g_autofree char *quoted = g_shell_quote (bwrap->argv->pdata[i]);

          g_debug ("    %s", quoted);
        }
      else
        {
          g_debug ("    %s", (const char *) bwrap->argv->pdata[i]);
        }
    }

  flatpak_bwrap_add_fd (bwrap, fd);
  g_ptr_array_remove_range (bwrap->argv, start, end - start);
  if (one_arg)
    {
      g_ptr_array_insert (bwrap->argv, start, g_strdup_printf ("--args=%d", fd));
    }
  else
    {
      g_ptr_array_insert (bwrap->argv, start, g_strdup ("--args"));
      g_ptr_array_insert (bwrap->argv, start + 1, g_strdup_printf ("%d", fd));
    }

  return TRUE;
}

/*
 * Remember that we need to arrange for $XDG_RUNTIME_DIR/$name to be
 * a symlink to /run/flatpak/$name.
 */
void
flatpak_bwrap_add_runtime_dir_member (FlatpakBwrap *bwrap,
                                      const char *name)
{
  if (bwrap->runtime_dir_members == NULL)
    bwrap->runtime_dir_members = g_ptr_array_new_with_free_func (g_free);

  g_ptr_array_add (bwrap->runtime_dir_members, g_strdup (name));
}

static void
expect_symlink (const char *host_path,
                const char *target)
{
  /* This shouldn't fail in practice, so there's not much point in
   * translating the warning */
  if (symlink (target, host_path) < 0 && errno != EEXIST)
    {
      g_warning ("Unable to create symlink at %s: %s",
                 host_path, g_strerror (errno));
    }
  else
    {
      g_autoptr(GError) local_error = NULL;
      g_autofree char *got = glnx_readlinkat_malloc (AT_FDCWD,
                                                     host_path,
                                                     NULL,
                                                     &local_error);

      if (got == NULL)
        g_warning ("%s is not a symlink to \"%s\" as expected: %s",
                   host_path, target, local_error->message);
      else if (strcmp (got, target) != 0)
        g_warning ("%s is a symlink to \"%s\", not \"%s\" as expected",
                   host_path, got, target);
    }
}

void
flatpak_bwrap_populate_runtime_dir (FlatpakBwrap *bwrap,
                                    const char *shared_xdg_runtime_dir)
{
  if (shared_xdg_runtime_dir != NULL)
    {
      g_autofree char *host_path = g_build_filename (shared_xdg_runtime_dir,
                                                     "flatpak-info", NULL);

      expect_symlink (host_path, "../../../.flatpak-info");
    }
  else
    {
      flatpak_bwrap_add_arg (bwrap, "--symlink");
      flatpak_bwrap_add_arg (bwrap, "../../../.flatpak-info");
      flatpak_bwrap_add_arg_printf (bwrap, "/run/user/%d/flatpak-info", getuid ());
    }

  if (bwrap->runtime_dir_members != NULL)
    {
      gsize i;

      for (i = 0; i < bwrap->runtime_dir_members->len; i++)
        {
          const char *member = g_ptr_array_index (bwrap->runtime_dir_members, i);
          g_autofree char *target = g_strdup_printf ("../../flatpak/%s", member);

          if (shared_xdg_runtime_dir != NULL)
            {
              g_autofree char *host_path = g_build_filename (shared_xdg_runtime_dir,
                                                             member, NULL);

              expect_symlink (host_path, target);
            }
          else
            {
              flatpak_bwrap_add_arg (bwrap, "--symlink");
              flatpak_bwrap_add_arg (bwrap, target);
              flatpak_bwrap_add_arg_printf (bwrap, "/run/user/%d/%s", getuid (), member);
            }
        }
    }
}

void
flatpak_bwrap_child_setup (GArray *fd_array,
                           gboolean close_fd_workaround)
{
  int i;

  /* There is a dead-lock in glib versions before 2.60 when it closes
   * the fds. See:  https://gitlab.gnome.org/GNOME/glib/merge_requests/490
   * This was hitting the test-suite a lot, so we work around it by using
   * the G_SPAWN_LEAVE_DESCRIPTORS_OPEN/G_SUBPROCESS_FLAGS_INHERIT_FDS flag
   * and setting CLOEXEC ourselves.
   */
  if (close_fd_workaround)
    g_fdwalk_set_cloexec (3);

  /* If no fd_array was specified, don't care. */
  if (fd_array == NULL)
    return;

  /* Otherwise, mark not - close-on-exec all the fds in the array */
  for (i = 0; i < fd_array->len; i++)
    {
      int fd = g_array_index (fd_array, int, i);

      /* We also seek all fds to the start, because this lets
         us use the same fd_array multiple times */
      if (lseek (fd, 0, SEEK_SET) < 0)
        {
          /* Ignore the error, this happens on e.g. pipe fds */
        }

      fcntl (fd, F_SETFD, 0);
    }
}

/* Unset FD_CLOEXEC on the array of fds passed in @user_data */
void
flatpak_bwrap_child_setup_cb (gpointer user_data)
{
  GArray *fd_array = user_data;

  flatpak_bwrap_child_setup (fd_array, TRUE);
}

/* Unset FD_CLOEXEC on the array of fds passed in @user_data,
 * but do not set FD_CLOEXEC on all other fds */
void
flatpak_bwrap_child_setup_inherit_fds_cb (gpointer user_data)
{
  GArray *fd_array = user_data;

  flatpak_bwrap_child_setup (fd_array, FALSE);
}

/* Add a --sync-fd argument for bwrap(1). Returns the write end of the pipe on
 * success, or -1 on error. */
int
flatpak_bwrap_add_sync_fd (FlatpakBwrap *bwrap)
{
  /* --sync-fd is only allowed once */
  if (bwrap->sync_fds[1] >= 0)
    return bwrap->sync_fds[1];

  if (pipe2 (bwrap->sync_fds, O_CLOEXEC) < 0)
    return -1;

  flatpak_bwrap_add_args_data_fd (bwrap, "--sync-fd", bwrap->sync_fds[0], NULL);
  return bwrap->sync_fds[1];
}
