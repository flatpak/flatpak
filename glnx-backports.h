/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 * 
 * GLIB - Library of useful routines for C programming
 * Copyright (C) 1995-1997  Peter Mattis, Spencer Kimball and Josh MacDonald
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

G_BEGIN_DECLS

#if !GLIB_CHECK_VERSION(2, 34, 0)
#define g_clear_pointer(pp, destroy) \
  G_STMT_START {                                                               \
    G_STATIC_ASSERT (sizeof *(pp) == sizeof (gpointer));                       \
    /* Only one access, please; work around type aliasing */                   \
    union { char *in; gpointer *out; } _pp;                                    \
    gpointer _p;                                                               \
    /* This assignment is needed to avoid a gcc warning */                     \
    GDestroyNotify _destroy = (GDestroyNotify) (destroy);                      \
                                                                               \
    _pp.in = (char *) (pp);                                                    \
    _p = *_pp.out;                                                             \
    if (_p)                                                                    \
      {                                                                        \
        *_pp.out = NULL;                                                       \
        _destroy (_p);                                                         \
      }                                                                        \
  } G_STMT_END
#endif

#if !GLIB_CHECK_VERSION(2, 44, 0)

#define g_strv_contains glnx_strv_contains
gboolean              glnx_strv_contains  (const gchar * const *strv,
                                           const gchar         *str);

#define g_set_object(object_ptr, new_object) \
 (/* Check types match. */ \
  0 ? *(object_ptr) = (new_object), FALSE : \
  glnx_set_object ((GObject **) (object_ptr), (GObject *) (new_object)) \
 )
gboolean              glnx_set_object  (GObject **object_ptr,
                                        GObject  *new_object);

#endif /* !GLIB_CHECK_VERSION(2, 44, 0) */

#ifndef g_assert_nonnull
#define g_assert_nonnull(x) g_assert (x != NULL)
#endif

#ifndef g_assert_null
#define g_assert_null(x) g_assert (x == NULL)
#endif

#if !GLIB_CHECK_VERSION (2, 38, 0)
#define g_test_skip(s) g_test_message ("SKIP: %s", s)
#endif

G_END_DECLS
