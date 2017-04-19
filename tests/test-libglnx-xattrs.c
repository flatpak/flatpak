/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"
#include "libglnx.h"
#include <glib.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <string.h>

#define XATTR_THREAD_RUN_TIME_USECS (5 * G_USEC_PER_SEC)

struct XattrWorker {
  int dfd;
  gboolean is_writer;
  guint n_attrs_read;
};

typedef enum {
  WRITE_RUN_MUTATE,
  WRITE_RUN_CREATE,
} WriteType;

static gboolean
set_random_xattr_value (int fd, const char *name, GError **error)
{
  const guint8 randxattrbyte = g_random_int_range (0, 256);
  const guint32 randxattrvalue_len = (g_random_int () % 256) + 1; /* Picked to be not too small or large */
  g_autofree char *randxattrvalue = g_malloc (randxattrvalue_len);

  memset (randxattrvalue, randxattrbyte, randxattrvalue_len);

  if (fsetxattr (fd, name, randxattrvalue, randxattrvalue_len, 0) < 0)
    {
      glnx_set_error_from_errno (error);
      return FALSE;
    }

  return TRUE;
}

static gboolean
add_random_xattrs (int fd, GError **error)
{
  const guint nattrs = MIN (2, g_random_int () % 16);

  for (guint i = 0; i < nattrs; i++)
    {
      guint32 randxattrname_v = g_random_int ();
      g_autofree char *randxattrname = g_strdup_printf ("user.test%u", randxattrname_v);

      if (!set_random_xattr_value (fd, randxattrname, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
do_write_run (GLnxDirFdIterator *dfd_iter, GError **error)
{
  WriteType wtype = g_random_int () % 2;

  if (wtype == WRITE_RUN_CREATE)
    {
      guint32 randname_v = g_random_int ();
      g_autofree char *randname = g_strdup_printf ("file%u", randname_v);
      glnx_fd_close int fd = -1;

    again:
      fd = openat (dfd_iter->fd, randname, O_CREAT | O_EXCL, 0644);
      if (fd < 0)
        {
          if (errno == EEXIST)
            {
              g_printerr ("Congratulations!  I suggest purchasing a lottery ticket today!\n");
              goto again;
            }
          else
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }
        }

      if (!add_random_xattrs (fd, error))
        return FALSE;
      }
  else if (wtype == WRITE_RUN_MUTATE)
    {
      while (TRUE)
        {
          g_autoptr(GVariant) current_xattrs = NULL;
          glnx_fd_close int fd = -1;

          struct dirent *dent;
          if (!glnx_dirfd_iterator_next_dent (dfd_iter, &dent, NULL, error))
            return FALSE;
          if (!dent)
            break;

          fd = openat (dfd_iter->fd, dent->d_name, O_RDONLY | O_CLOEXEC);
          if (fd < 0)
            {
              glnx_set_error_from_errno (error);
              return FALSE;
            }

          if (!glnx_fd_get_all_xattrs (fd, &current_xattrs, NULL, error))
            return FALSE;

          for (int i = 0; i < g_variant_n_children (current_xattrs); i++)
            {
              const char *name, *value;
              g_variant_get_child (current_xattrs, i, "(^&ay^&ay)", &name, &value);

              /* We don't want to potentially test/change xattrs like security.selinux
               * that were injected by the system.
               */
              if (!g_str_has_prefix (name, "user.test"))
                continue;

              if (!set_random_xattr_value (fd, name, error))
                return FALSE;
            }
        }
    }
  else
    g_assert_not_reached ();

  return TRUE;
}

static gboolean
do_read_run (GLnxDirFdIterator *dfd_iter,
             guint *out_n_read,
             GError **error)
{
  guint nattrs = 0;
  while (TRUE)
    {
      g_autoptr(GVariant) current_xattrs = NULL;
      glnx_fd_close int fd = -1;

      struct dirent *dent;
      if (!glnx_dirfd_iterator_next_dent (dfd_iter, &dent, NULL, error))
        return FALSE;
      if (!dent)
        break;

      fd = openat (dfd_iter->fd, dent->d_name, O_RDONLY | O_CLOEXEC);
      if (fd < 0)
        {
          glnx_set_error_from_errno (error);
          return FALSE;
        }

      if (!glnx_fd_get_all_xattrs (fd, &current_xattrs, NULL, error))
        return FALSE;

      /* We don't actually care about the values, just use the variable
       * to avoid compiler warnings.
       */
      nattrs += g_variant_n_children (current_xattrs);
    }

  *out_n_read = nattrs;
  return TRUE;
}

static gpointer
xattr_thread (gpointer data)
{
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  struct XattrWorker *worker = data;
  guint64 end_time = g_get_monotonic_time () + XATTR_THREAD_RUN_TIME_USECS;
  guint n_read = 0;

  while (g_get_monotonic_time () < end_time)
    {
      g_auto(GLnxDirFdIterator) dfd_iter = { 0, };

      if (!glnx_dirfd_iterator_init_at (worker->dfd, ".", TRUE, &dfd_iter, error))
        goto out;

      if (worker->is_writer)
        {
          if (!do_write_run (&dfd_iter, error))
            goto out;
        }
      else
        {
          if (!do_read_run (&dfd_iter, &n_read, error))
            goto out;
        }
    }

 out:
  g_assert_no_error (local_error);

  return GINT_TO_POINTER (n_read);
}

static void
test_xattr_races (void)
{
  /* If for some reason we're built in a VM which only has one vcpu, let's still
   * at least make the test do something.
   */
  /* FIXME - this deadlocks for me on 4.9.4-201.fc25.x86_64, whether
   * using overlayfs or xfs as source/dest.
   */
  const guint nprocs = MAX (4, g_get_num_processors ());
  struct XattrWorker wdata[nprocs];
  GThread *threads[nprocs];
  g_autoptr(GError) local_error = NULL;
  GError **error = &local_error;
  glnx_fd_close int dfd = -1;
  g_autofree char *tmpdir = g_strdup_printf ("%s/libglnx-xattrs-XXXXXX",
                                             getenv ("TMPDIR") ?: "/var/tmp");
  guint nread = 0;

  if (!glnx_mkdtempat (AT_FDCWD, tmpdir, 0700, error))
    goto out;

  if (!glnx_opendirat (AT_FDCWD, tmpdir, TRUE, &dfd, error))
    goto out;

  /* Support people building/testing on tmpfs https://github.com/flatpak/flatpak/issues/686 */
  if (fsetxattr (dfd, "user.test", "novalue", strlen ("novalue"), 0) < 0)
    {
      if (errno == EOPNOTSUPP)
        {
          g_test_skip ("no xattr support");
          return;
        }
      else
        {
          glnx_set_error_from_errno (error);
          goto out;
        }
    }

  for (guint i = 0; i < nprocs; i++)
    {
      struct XattrWorker *worker = &wdata[i];
      worker->dfd = dfd;
      worker->is_writer = i % 2 == 0;
      threads[i] = g_thread_new (NULL, xattr_thread, worker);
    }

  for (guint i = 0; i < nprocs; i++)
    {
      if (wdata[i].is_writer)
        (void) g_thread_join (threads[i]);
      else
        nread += GPOINTER_TO_UINT (g_thread_join (threads[i]));
    }

  g_print ("Read %u xattrs race free!\n", nread);

  (void) glnx_shutil_rm_rf_at (AT_FDCWD, tmpdir, NULL, NULL);

 out:
  g_assert_no_error (local_error);
}

int main (int argc, char **argv)
{
  int ret;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/xattr-races", test_xattr_races);

  ret = g_test_run();

  return ret;
}
