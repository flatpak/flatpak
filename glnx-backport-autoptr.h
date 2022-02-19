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

#if !GLIB_CHECK_VERSION(2, 43, 4)

#define _GLIB_AUTOPTR_FUNC_NAME(TypeName) glib_autoptr_cleanup_##TypeName
#define _GLIB_AUTOPTR_TYPENAME(TypeName)  TypeName##_autoptr
#define _GLIB_AUTO_FUNC_NAME(TypeName)    glib_auto_cleanup_##TypeName
#define _GLIB_CLEANUP(func)               __attribute__((cleanup(func)))
#define _GLIB_DEFINE_AUTOPTR_CHAINUP(ModuleObjName, ParentName) \
  typedef ModuleObjName *_GLIB_AUTOPTR_TYPENAME(ModuleObjName);                                          \
  static inline void _GLIB_AUTOPTR_FUNC_NAME(ModuleObjName) (ModuleObjName **_ptr) {                     \
    _GLIB_AUTOPTR_FUNC_NAME(ParentName) ((ParentName **) _ptr); }                                        \


/* these macros are API */
#define G_DEFINE_AUTOPTR_CLEANUP_FUNC(TypeName, func) \
  typedef TypeName *_GLIB_AUTOPTR_TYPENAME(TypeName);                                                           \
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS                                                                              \
  static inline void _GLIB_AUTOPTR_FUNC_NAME(TypeName) (TypeName **_ptr) { if (*_ptr) (func) (*_ptr); }         \
  G_GNUC_END_IGNORE_DEPRECATIONS
#define G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(TypeName, func) \
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS                                                                              \
  static inline void _GLIB_AUTO_FUNC_NAME(TypeName) (TypeName *_ptr) { (func) (_ptr); }                         \
  G_GNUC_END_IGNORE_DEPRECATIONS
#define G_DEFINE_AUTO_CLEANUP_FREE_FUNC(TypeName, func, none) \
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS                                                                              \
  static inline void _GLIB_AUTO_FUNC_NAME(TypeName) (TypeName *_ptr) { if (*_ptr != none) (func) (*_ptr); }     \
  G_GNUC_END_IGNORE_DEPRECATIONS
#define g_autoptr(TypeName) _GLIB_CLEANUP(_GLIB_AUTOPTR_FUNC_NAME(TypeName)) _GLIB_AUTOPTR_TYPENAME(TypeName)
#define g_auto(TypeName) _GLIB_CLEANUP(_GLIB_AUTO_FUNC_NAME(TypeName)) TypeName
#define g_autofree _GLIB_CLEANUP(g_autoptr_cleanup_generic_gfree)

/**
 * g_steal_pointer:
 * @pp: a pointer to a pointer
 *
 * Sets @pp to %NULL, returning the value that was there before.
 *
 * Conceptually, this transfers the ownership of the pointer from the
 * referenced variable to the "caller" of the macro (ie: "steals" the
 * reference).
 *
 * The return value will be properly typed, according to the type of
 * @pp.
 *
 * This can be very useful when combined with g_autoptr() to prevent the
 * return value of a function from being automatically freed.  Consider
 * the following example (which only works on GCC and clang):
 *
 * |[
 * GObject *
 * create_object (void)
 * {
 *   g_autoptr(GObject) obj = g_object_new (G_TYPE_OBJECT, NULL);
 *
 *   if (early_error_case)
 *     return NULL;
 *
 *   return g_steal_pointer (&obj);
 * }
 * ]|
 *
 * It can also be used in similar ways for 'out' parameters and is
 * particularly useful for dealing with optional out parameters:
 *
 * |[
 * gboolean
 * get_object (GObject **obj_out)
 * {
 *   g_autoptr(GObject) obj = g_object_new (G_TYPE_OBJECT, NULL);
 *
 *   if (early_error_case)
 *     return FALSE;
 *
 *   if (obj_out)
 *     *obj_out = g_steal_pointer (&obj);
 *
 *   return TRUE;
 * }
 * ]|
 *
 * In the above example, the object will be automatically freed in the
 * early error case and also in the case that %NULL was given for
 * @obj_out.
 *
 * Since: 2.44
 */
static inline gpointer
(g_steal_pointer) (gpointer pp)
{
  gpointer *ptr = (gpointer *) pp;
  gpointer ref;

  ref = *ptr;
  *ptr = NULL;

  return ref;
}

/* type safety */
#define g_steal_pointer(pp) \
  (0 ? (*(pp)) : (g_steal_pointer) (pp))

#endif /* !GLIB_CHECK_VERSION(2, 43, 3) */

G_END_DECLS
