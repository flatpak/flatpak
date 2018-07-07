/*
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

#include <glib/gi18n.h>

#include <gio/gio.h>
#include "libglnx/libglnx.h"

#include "flatpak-bwrap-private.h"
#include "flatpak-utils-private.h"

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

  return bwrap;
}

void
flatpak_bwrap_free (FlatpakBwrap *bwrap)
{
  g_ptr_array_unref (bwrap->argv);
  g_array_unref (bwrap->noinherit_fds);
  g_array_unref (bwrap->fds);
  g_strfreev (bwrap->envp);
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

  flatpak_bwrap_add_args_data_fd (bwrap, "--bind-data", glnx_steal_fd (&args_tmpf.fd), path);
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

gboolean
flatpak_bwrap_bundle_args (FlatpakBwrap *bwrap,
                           int           start,
                           int           end,
                           gboolean      one_arg,
                           GError      **error)
{
  gchar *data;
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
  *data = 0;
  ptr = data;
  for (i = start; i < end; i++)
    ptr = g_stpcpy (ptr, bwrap->argv->pdata[i]) + 1;

  if (!flatpak_buffer_to_sealed_memfd_or_tmpfile (&args_tmpf, "bwrap-args", data, data_len, error))
    return FALSE;

  fd = glnx_steal_fd (&args_tmpf.fd);

  {
    g_autofree char *commandline = flatpak_quote_argv ((const char **) bwrap->argv->pdata + start, end - start);
    flatpak_debug2 ("bwrap --args %d = %s", fd, commandline);
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

/* Unset FD_CLOEXEC on the array of fds passed in @user_data */
void
flatpak_bwrap_child_setup_cb (gpointer user_data)
{
  GArray *fd_array = user_data;
  int i;

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
