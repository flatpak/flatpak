/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012,2015 Colin Walters <walters@verbum.org>.
 * SPDX-License-Identifier: LGPL-2.0-or-later
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

#pragma once

#include <gio/gio.h>
#include <errno.h>

#include "glnx-backports.h"

G_BEGIN_DECLS

/**
 * glnx_unref_object:
 *
 * Call g_object_unref() on a variable location when it goes out of
 * scope.  Note that unlike g_object_unref(), the variable may be
 * %NULL.
 */
#define glnx_unref_object __attribute__ ((cleanup(glnx_local_obj_unref)))
static inline void
glnx_local_obj_unref (void *v)
{
  GObject *o = *(GObject **)v;
  if (o)
    g_object_unref (o);
}
#define glnx_unref_object __attribute__ ((cleanup(glnx_local_obj_unref)))

/* Backwards-compat with older libglnx */
#define glnx_steal_fd g_steal_fd

/**
 * glnx_close_fd:
 * @fdp: Pointer to fd
 *
 * Effectively `close (g_steal_fd (&fd))`.  Also
 * asserts that `close()` did not raise `EBADF` - encountering
 * that error is usually a critical bug in the program.
 */
static inline void
glnx_close_fd (int *fdp)
{
  int errsv;

  g_assert (fdp);

  int fd = g_steal_fd (fdp);
  if (fd >= 0)
    {
      errsv = errno;
      if (close (fd) < 0)
        g_assert (errno != EBADF);
      errno = errsv;
    }
}

/**
 * glnx_fd_close:
 *
 * Deprecated in favor of `glnx_autofd`.
 */
#define glnx_fd_close __attribute__((cleanup(glnx_close_fd)))
/**
 * glnx_autofd:
 *
 * Call close() on a variable location when it goes out of scope.
 */
#define glnx_autofd __attribute__((cleanup(glnx_close_fd)))

G_END_DECLS
