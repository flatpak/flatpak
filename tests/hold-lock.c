/*
 * Copyright © 2019-2021 Collabora Ltd.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <fcntl.h>
#include <fcntl.h>
#include <sysexits.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libglnx/libglnx.h"

static GArray *global_locks = NULL;
static gboolean opt_wait = FALSE;
static gboolean opt_write = FALSE;

static gboolean
opt_fd_cb (const char *name,
           const char *value,
           gpointer data,
           GError **error)
{
  char *endptr;
  gint64 i64 = g_ascii_strtoll (value, &endptr, 10);
  int fd;
  int fd_flags;

  g_return_val_if_fail (global_locks != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);

  if (i64 < 0 || i64 > G_MAXINT || endptr == value || *endptr != '\0')
    {
      g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                   "Integer out of range or invalid: %s", value);
      return FALSE;
    }

  fd = (int) i64;

  fd_flags = fcntl (fd, F_GETFD);

  if (fd_flags < 0)
    return glnx_throw_errno_prefix (error, "Unable to receive --fd %d", fd);

  if ((fd_flags & FD_CLOEXEC) == 0
      && fcntl (fd, F_SETFD, fd_flags | FD_CLOEXEC) != 0)
    return glnx_throw_errno_prefix (error,
                                    "Unable to configure --fd %d for "
                                    "close-on-exec",
                                    fd);

  g_array_append_val (global_locks, fd);
  return TRUE;
}

static gboolean
opt_lock_file_cb (const char *name,
                  const char *value,
                  gpointer data,
                  GError **error)
{
  int open_flags = O_CLOEXEC | O_CREAT | O_NOCTTY | O_RDWR;
  int fd;
  int cmd;
  struct flock l =
  {
    .l_type = F_RDLCK,
    .l_whence = SEEK_SET,
    .l_start = 0,
    .l_len = 0,
  };

  g_return_val_if_fail (global_locks != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);

  fd = TEMP_FAILURE_RETRY (open (value, open_flags, 0644));

  if (fd < 0)
    return glnx_throw_errno_prefix (error, "open %s", value);

  if (opt_write)
    l.l_type = F_WRLCK;

  if (opt_wait)
    cmd = F_SETLKW;
  else
    cmd = F_SETLK;

  if (TEMP_FAILURE_RETRY (fcntl (fd, cmd, &l)) < 0)
    {
      if (errno == EACCES || errno == EAGAIN)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                     "Unable to lock %s: file is busy", value);
      else
        glnx_throw_errno_prefix (error, "lock %s", value);

      close (fd);
      return FALSE;
    }

  g_array_append_val (global_locks, fd);
  return TRUE;
}

static GOptionEntry options[] =
{
  { "fd", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK, opt_fd_cb,
    "Take a file descriptor, already locked if desired, and keep it "
    "open. May be repeated.",
    NULL },

  { "wait", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_wait,
    "Wait for each subsequent lock file.",
    NULL },
  { "no-wait", '\0',
    G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &opt_wait,
    "Exit unsuccessfully if a lock-file is busy [default].",
    NULL },

  { "write", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &opt_write,
    "Lock each subsequent lock file for write access.",
    NULL },
  { "no-write", '\0',
    G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &opt_write,
    "Lock each subsequent lock file for read-only access [default].",
    NULL },

  { "lock-file", '\0',
    G_OPTION_FLAG_NONE, G_OPTION_ARG_CALLBACK, opt_lock_file_cb,
    "Open the given file and lock it, affected by options appearing "
    "earlier on the command-line. May be repeated.",
    NULL },

  { NULL }
};

int
main (int argc,
      char *argv[])
{
  g_autoptr(GArray) locks = NULL;
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  int ret = EX_USAGE;

  locks = g_array_new (FALSE, FALSE, sizeof (int));
  global_locks = locks;

  context = g_option_context_new (NULL);
  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_BUSY))
        ret = EX_TEMPFAIL;
      else if (local_error->domain == G_OPTION_ERROR)
        ret = EX_USAGE;
      else
        ret = EX_UNAVAILABLE;

      goto out;
    }

  ret = EX_UNAVAILABLE;

  /* Self-destruct when parent exits */
  if (prctl (PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0) != 0)
    {
      glnx_throw_errno_prefix (error,
                               "Unable to set parent death signal");
      goto out;
    }

  /* Signal to caller that we are ready */
  fclose (stdout);

  while (TRUE)
    pause ();

  g_assert_not_reached ();

out:
  global_locks = NULL;

  if (local_error != NULL)
    g_warning ("%s", local_error->message);

  return ret;
}
