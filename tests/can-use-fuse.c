/*
 * Copyright 2019-2021 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "can-use-fuse.h"

#include <unistd.h>

#include <glib/gstdio.h>

#include "libglnx.h"

#ifndef FUSE_USE_VERSION
#error config.h needs to define FUSE_USE_VERSION
#endif

#if FUSE_USE_VERSION >= 31
#include <fuse.h>
#else
#include <fuse_lowlevel.h>
#endif

gchar *cannot_use_fuse = NULL;

/*
 * If we cannot use FUSE, set cannot_use_fuse and return %FALSE.
 */
gboolean
check_fuse (void)
{
  g_autofree gchar *fusermount = NULL;
  g_autofree gchar *path = NULL;
  char *argv[] = { "flatpak-fuse-test", NULL };
  struct fuse_args args = FUSE_ARGS_INIT (G_N_ELEMENTS (argv) - 1, argv);
  g_autoptr(GError) error = NULL;
#if FUSE_USE_VERSION >= 31
  struct fuse *fuse = NULL;
  const struct fuse_operations ops = { NULL };
#else
  struct fuse_chan *chan = NULL;
#endif

  if (cannot_use_fuse != NULL)
    return FALSE;

  if (access ("/dev/fuse", W_OK) != 0)
    {
      cannot_use_fuse = g_strdup_printf ("access /dev/fuse: %s",
                                         g_strerror (errno));
      return FALSE;
    }

  fusermount = g_find_program_in_path ("fusermount");

  if (fusermount == NULL)
    {
      cannot_use_fuse = g_strdup ("fusermount not found in PATH");
      return FALSE;
    }

  if (!g_file_test (fusermount, G_FILE_TEST_IS_EXECUTABLE))
    {
      cannot_use_fuse = g_strdup_printf ("%s not executable", fusermount);
      return FALSE;
    }

  if (!g_file_test ("/etc/mtab", G_FILE_TEST_EXISTS))
    {
      cannot_use_fuse = g_strdup ("fusermount won't work without /etc/mtab");
      return FALSE;
    }

  path = g_dir_make_tmp ("flatpak-test.XXXXXX", &error);
  g_assert_no_error (error);

#if FUSE_USE_VERSION >= 31
  fuse = fuse_new (&args, &ops, sizeof (ops), NULL);

  if (fuse == NULL)
    {
      fuse_opt_free_args (&args);
      cannot_use_fuse = g_strdup_printf ("fuse_new: %s",
                                         g_strerror (errno));
      return FALSE;
    }

  if (fuse_mount (fuse, path) != 0)
    {
      fuse_destroy (fuse);
      fuse_opt_free_args (&args);
      cannot_use_fuse = g_strdup_printf ("fuse_mount: %s",
                                         g_strerror (errno));
      return FALSE;
    }
#else
  chan = fuse_mount (path, &args);

  if (chan == NULL)
    {
      fuse_opt_free_args (&args);
      cannot_use_fuse = g_strdup_printf ("fuse_mount: %s",
                                         g_strerror (errno));
      return FALSE;
    }
#endif

  g_test_message ("Successfully set up test FUSE fs on %s", path);

#if FUSE_USE_VERSION >= 31
  fuse_unmount (fuse);
  fuse_destroy (fuse);
#else
  fuse_unmount (path, chan);
#endif

  if (g_rmdir (path) != 0)
    g_error ("rmdir %s: %s", path, g_strerror (errno));

  fuse_opt_free_args (&args);

  return TRUE;
}

gboolean
check_fuse_or_skip_test (void)
{
  if (!check_fuse ())
    {
      g_assert (cannot_use_fuse != NULL);
      g_test_skip (cannot_use_fuse);
      return FALSE;
    }

  return TRUE;
}
