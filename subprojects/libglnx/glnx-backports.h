/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright 1998 Manish Singh
 * Copyright 1998 Tim Janik
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 * Copyright (C) 2018 Endless OS Foundation, LLC
 * Copyright 2017 Emmanuele Bassi
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * 
 * GLIB - Library of useful routines for C programming
 * Copyright (C) 1995-1997  Peter Mattis, Spencer Kimball and Josh MacDonald
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <string.h>

#include <glib-unix.h>
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

#if !GLIB_CHECK_VERSION(2, 40, 0)
#define g_info(...) g_log (G_LOG_DOMAIN, G_LOG_LEVEL_INFO, __VA_ARGS__)
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

#if !GLIB_CHECK_VERSION(2, 38, 0)
#define G_SPAWN_DEFAULT ((GSpawnFlags) 0)
#endif

#if !GLIB_CHECK_VERSION(2, 42, 0)
#define G_OPTION_FLAG_NONE ((GOptionFlags) 0)
#endif

#ifndef G_PID_FORMAT  /* added in 2.50 */
#define G_PID_FORMAT "i"
#endif

#if !GLIB_CHECK_VERSION(2, 60, 0)
#define g_strv_equal _glnx_strv_equal
gboolean _glnx_strv_equal (const gchar * const *strv1,
                           const gchar * const *strv2);
#endif

#ifndef G_DBUS_METHOD_INVOCATION_HANDLED    /* added in 2.68 */
#define G_DBUS_METHOD_INVOCATION_HANDLED TRUE
#endif

#ifndef G_DBUS_METHOD_INVOCATION_UNHANDLED  /* added in 2.68 */
#define G_DBUS_METHOD_INVOCATION_UNHANDLED FALSE
#endif

#if !GLIB_CHECK_VERSION(2, 68, 0)
static inline gpointer _glnx_memdup2 (gconstpointer mem,
                                      gsize         byte_size) G_GNUC_ALLOC_SIZE(2);
static inline gpointer
_glnx_memdup2 (gconstpointer mem,
               gsize         byte_size)
{
  gpointer new_mem;

  if (mem && byte_size != 0)
    {
      new_mem = g_malloc (byte_size);
      memcpy (new_mem, mem, byte_size);
    }
  else
    new_mem = NULL;

  return new_mem;
}
#define g_memdup2 _glnx_memdup2
#endif

#ifndef G_OPTION_ENTRY_NULL   /* added in 2.70 */
#define G_OPTION_ENTRY_NULL { NULL, 0, 0, 0, NULL, NULL, NULL }
#endif

#ifndef G_APPROX_VALUE  /* added in 2.58 */
#define G_APPROX_VALUE(a, b, epsilon) \
  (((a) > (b) ? (a) - (b) : (b) - (a)) < (epsilon))
#endif

static inline int
_glnx_steal_fd (int *fdp)
{
#if GLIB_CHECK_VERSION(2, 70, 0)
  /* Allow it to be used without deprecation warnings, even if the target
   * GLib version is older */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  return g_steal_fd (fdp);
  G_GNUC_END_IGNORE_DEPRECATIONS
#else
  int fd = *fdp;
  *fdp = -1;
  return fd;
#endif
}
#define g_steal_fd _glnx_steal_fd

#if !GLIB_CHECK_VERSION(2, 74, 0)
#define G_APPLICATION_DEFAULT_FLAGS ((GApplicationFlags) 0)
#define G_CONNECT_DEFAULT ((GConnectFlags) 0)
#define G_IO_FLAG_NONE ((GIOFlags) 0)
#define G_MARKUP_DEFAULT_FLAGS ((GMarkupParseFlags) 0)
#define G_REGEX_DEFAULT ((GRegexCompileFlags) 0)
#define G_REGEX_MATCH_DEFAULT ((GRegexMatchFlags) 0)
#define G_TEST_SUBPROCESS_DEFAULT ((GTestSubprocessFlags) 0)
#define G_TEST_TRAP_DEFAULT ((GTestTrapFlags) 0)
#define G_TLS_CERTIFICATE_NO_FLAGS ((GTlsCertificateFlags) 0)
#define G_TYPE_FLAG_NONE ((GTypeFlags) 0)
#endif

#if !GLIB_CHECK_VERSION(2, 80, 0)
#define g_closefrom _glnx_closefrom
int _glnx_closefrom (int lowfd);
#define g_fdwalk_set_cloexec _glnx_fdwalk_set_cloexec
int _glnx_fdwalk_set_cloexec (int lowfd);
#endif

G_END_DECLS
