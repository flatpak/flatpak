/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012,2015 Colin Walters <walters@verbum.org>.
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

G_BEGIN_DECLS

#define GLNX_DEFINE_CLEANUP_FUNCTION(Type, name, func) \
  static inline void name (void *v) \
  { \
    func (*(Type*)v); \
  }

#define GLNX_DEFINE_CLEANUP_FUNCTION0(Type, name, func) \
  static inline void name (void *v) \
  { \
    if (*(Type*)v) \
      func (*(Type*)v); \
  }

/**
 * glnx_free:
 *
 * Call g_free() on a variable location when it goes out of scope.
 */
#define glnx_free __attribute__ ((cleanup(glnx_local_free)))
#ifdef GLNX_GSYSTEM_COMPAT
#define gs_free __attribute__ ((cleanup(glnx_local_free)))
#endif
GLNX_DEFINE_CLEANUP_FUNCTION(void*, glnx_local_free, g_free)

/**
 * glnx_unref_object:
 *
 * Call g_object_unref() on a variable location when it goes out of
 * scope.  Note that unlike g_object_unref(), the variable may be
 * %NULL.
 */
#define glnx_unref_object __attribute__ ((cleanup(glnx_local_obj_unref)))
#ifdef GLNX_GSYSTEM_COMPAT
#define gs_unref_object __attribute__ ((cleanup(glnx_local_obj_unref)))
#endif
GLNX_DEFINE_CLEANUP_FUNCTION0(GObject*, glnx_local_obj_unref, g_object_unref)

/**
 * glnx_unref_variant:
 *
 * Call g_variant_unref() on a variable location when it goes out of
 * scope.  Note that unlike g_variant_unref(), the variable may be
 * %NULL.
 */
#define glnx_unref_variant __attribute__ ((cleanup(glnx_local_variant_unref)))
#ifdef GLNX_GSYSTEM_COMPAT
#define gs_unref_variant __attribute__ ((cleanup(glnx_local_variant_unref)))
#endif
GLNX_DEFINE_CLEANUP_FUNCTION0(GVariant*, glnx_local_variant_unref, g_variant_unref)

/**
 * glnx_free_variant_iter:
 *
 * Call g_variant_iter_free() on a variable location when it goes out of
 * scope.
 */
#define glnx_free_variant_iter __attribute__ ((cleanup(glnx_local_variant_iter_free)))
GLNX_DEFINE_CLEANUP_FUNCTION0(GVariantIter*, glnx_local_variant_iter_free, g_variant_iter_free)

/**
 * glnx_free_variant_builder:
 *
 * Call g_variant_builder_unref() on a variable location when it goes out of
 * scope.
 */
#define glnx_unref_variant_builder __attribute__ ((cleanup(glnx_local_variant_builder_unref)))
GLNX_DEFINE_CLEANUP_FUNCTION0(GVariantBuilder*, glnx_local_variant_builder_unref, g_variant_builder_unref)

/**
 * glnx_unref_array:
 *
 * Call g_array_unref() on a variable location when it goes out of
 * scope.  Note that unlike g_array_unref(), the variable may be
 * %NULL.

 */
#define glnx_unref_array __attribute__ ((cleanup(glnx_local_array_unref)))
GLNX_DEFINE_CLEANUP_FUNCTION0(GArray*, glnx_local_array_unref, g_array_unref)

/**
 * glnx_unref_ptrarray:
 *
 * Call g_ptr_array_unref() on a variable location when it goes out of
 * scope.  Note that unlike g_ptr_array_unref(), the variable may be
 * %NULL.

 */
#define glnx_unref_ptrarray __attribute__ ((cleanup(glnx_local_ptrarray_unref)))
#ifdef GLNX_GSYSTEM_COMPAT
#define gs_unref_ptrarray __attribute__ ((cleanup(glnx_local_ptrarray_unref)))
#endif
GLNX_DEFINE_CLEANUP_FUNCTION0(GPtrArray*, glnx_local_ptrarray_unref, g_ptr_array_unref)

/**
 * glnx_unref_hashtable:
 *
 * Call g_hash_table_unref() on a variable location when it goes out
 * of scope.  Note that unlike g_hash_table_unref(), the variable may
 * be %NULL.
 */
#define glnx_unref_hashtable __attribute__ ((cleanup(glnx_local_hashtable_unref)))
#ifdef GLNX_GSYSTEM_COMPAT
#define gs_unref_hashtable __attribute__ ((cleanup(glnx_local_hashtable_unref)))
#endif
GLNX_DEFINE_CLEANUP_FUNCTION0(GHashTable*, glnx_local_hashtable_unref, g_hash_table_unref)

/**
 * glnx_free_list:
 *
 * Call g_list_free() on a variable location when it goes out
 * of scope.
 */
#define glnx_free_list __attribute__ ((cleanup(glnx_local_free_list)))
GLNX_DEFINE_CLEANUP_FUNCTION(GList*, glnx_local_free_list, g_list_free)

/**
 * glnx_free_slist:
 *
 * Call g_slist_free() on a variable location when it goes out
 * of scope.
 */
#define glnx_free_slist __attribute__ ((cleanup(glnx_local_free_slist)))
GLNX_DEFINE_CLEANUP_FUNCTION(GSList*, glnx_local_free_slist, g_slist_free)

/**
 * glnx_free_checksum:
 *
 * Call g_checksum_free() on a variable location when it goes out
 * of scope.  Note that unlike g_checksum_free(), the variable may
 * be %NULL.
 */
#define glnx_free_checksum __attribute__ ((cleanup(glnx_local_checksum_free)))
GLNX_DEFINE_CLEANUP_FUNCTION0(GChecksum*, glnx_local_checksum_free, g_checksum_free)

/**
 * glnx_unref_bytes:
 *
 * Call g_bytes_unref() on a variable location when it goes out
 * of scope.  Note that unlike g_bytes_unref(), the variable may
 * be %NULL.
 */
#define glnx_unref_bytes __attribute__ ((cleanup(glnx_local_bytes_unref)))
GLNX_DEFINE_CLEANUP_FUNCTION0(GBytes*, glnx_local_bytes_unref, g_bytes_unref)

/**
 * glnx_strfreev:
 *
 * Call g_strfreev() on a variable location when it goes out of scope.
 */
#define glnx_strfreev __attribute__ ((cleanup(glnx_local_strfreev)))
GLNX_DEFINE_CLEANUP_FUNCTION(char**, glnx_local_strfreev, g_strfreev)

/**
 * glnx_free_error:
 *
 * Call g_error_free() on a variable location when it goes out of scope.
 */
#define glnx_free_error __attribute__ ((cleanup(glnx_local_free_error)))
GLNX_DEFINE_CLEANUP_FUNCTION0(GError*, glnx_local_free_error, g_error_free)

/**
 * glnx_unref_keyfile:
 *
 * Call g_key_file_unref() on a variable location when it goes out of scope.
 */
#define glnx_unref_keyfile __attribute__ ((cleanup(glnx_local_keyfile_unref)))
GLNX_DEFINE_CLEANUP_FUNCTION0(GKeyFile*, glnx_local_keyfile_unref, g_key_file_unref)

static inline void
glnx_cleanup_close_fdp (int *fdp)
{
  int fd, errsv;

  g_assert (fdp);
  
  fd = *fdp;
  if (fd != -1)
    {
      errsv = errno;
      (void) close (fd);
      errno = errsv;
    }
}

/**
 * glnx_fd_close:
 *
 * Call close() on a variable location when it goes out of scope.
 */
#define glnx_fd_close __attribute__((cleanup(glnx_cleanup_close_fdp)))

static inline int
glnx_steal_fd (int *fdp)
{
  int fd = *fdp;
  *fdp = -1;
  return fd;
}

G_END_DECLS
