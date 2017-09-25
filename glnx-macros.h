/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Colin Walters <walters@verbum.org>
 * With original source from systemd:
 * Copyright 2010 Lennart Poettering
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

#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* All of these are for C only. */
#ifndef __GI_SCANNER__

/* Taken from https://github.com/systemd/systemd/src/basic/string-util.h
 * at revision v228-666-gcf6c8c4
 */
#define glnx_strjoina(a, ...)                                           \
        ({                                                              \
                const char *_appendees_[] = { a, __VA_ARGS__ };         \
                char *_d_, *_p_;                                        \
                size_t _len_ = 0;                                       \
                unsigned _i_;                                           \
                for (_i_ = 0; _i_ < G_N_ELEMENTS(_appendees_) && _appendees_[_i_]; _i_++) \
                        _len_ += strlen(_appendees_[_i_]);              \
                _p_ = _d_ = alloca(_len_ + 1);                          \
                for (_i_ = 0; _i_ < G_N_ELEMENTS(_appendees_) && _appendees_[_i_]; _i_++) \
                        _p_ = stpcpy(_p_, _appendees_[_i_]);            \
                *_p_ = 0;                                               \
                _d_;                                                    \
        })

#ifndef G_IN_SET

/* Infrastructure for `G_IN_SET`; this code is copied from
 * systemd's macro.h - please treat that version as canonical
 * and submit patches first to systemd.
 */
#define _G_INSET_CASE_F(X) case X:
#define _G_INSET_CASE_F_1(CASE, X) _G_INSET_CASE_F(X)
#define _G_INSET_CASE_F_2(CASE, X, ...)  CASE(X) _G_INSET_CASE_F_1(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_3(CASE, X, ...)  CASE(X) _G_INSET_CASE_F_2(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_4(CASE, X, ...)  CASE(X) _G_INSET_CASE_F_3(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_5(CASE, X, ...)  CASE(X) _G_INSET_CASE_F_4(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_6(CASE, X, ...)  CASE(X) _G_INSET_CASE_F_5(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_7(CASE, X, ...)  CASE(X) _G_INSET_CASE_F_6(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_8(CASE, X, ...)  CASE(X) _G_INSET_CASE_F_7(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_9(CASE, X, ...)  CASE(X) _G_INSET_CASE_F_8(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_10(CASE, X, ...) CASE(X) _G_INSET_CASE_F_9(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_11(CASE, X, ...) CASE(X) _G_INSET_CASE_F_10(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_12(CASE, X, ...) CASE(X) _G_INSET_CASE_F_11(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_13(CASE, X, ...) CASE(X) _G_INSET_CASE_F_12(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_14(CASE, X, ...) CASE(X) _G_INSET_CASE_F_13(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_15(CASE, X, ...) CASE(X) _G_INSET_CASE_F_14(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_16(CASE, X, ...) CASE(X) _G_INSET_CASE_F_15(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_17(CASE, X, ...) CASE(X) _G_INSET_CASE_F_16(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_18(CASE, X, ...) CASE(X) _G_INSET_CASE_F_17(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_19(CASE, X, ...) CASE(X) _G_INSET_CASE_F_18(CASE, __VA_ARGS__)
#define _G_INSET_CASE_F_20(CASE, X, ...) CASE(X) _G_INSET_CASE_F_19(CASE, __VA_ARGS__)

#define _G_INSET_GET_CASE_F(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,_17,_18,_19,_20,NAME,...) NAME
#define _G_INSET_FOR_EACH_MAKE_CASE(...) \
  _G_INSET_GET_CASE_F(__VA_ARGS__,_G_INSET_CASE_F_20,_G_INSET_CASE_F_19,_G_INSET_CASE_F_18,_G_INSET_CASE_F_17,_G_INSET_CASE_F_16,_G_INSET_CASE_F_15,_G_INSET_CASE_F_14,_G_INSET_CASE_F_13,_G_INSET_CASE_F_12,_G_INSET_CASE_F_11, \
                               _G_INSET_CASE_F_10,_G_INSET_CASE_F_9,_G_INSET_CASE_F_8,_G_INSET_CASE_F_7,_G_INSET_CASE_F_6,_G_INSET_CASE_F_5,_G_INSET_CASE_F_4,_G_INSET_CASE_F_3,_G_INSET_CASE_F_2,_G_INSET_CASE_F_1) \
                   (_G_INSET_CASE_F,__VA_ARGS__)

/* Note: claiming the name here even though it isn't upstream yet
 * https://bugzilla.gnome.org/show_bug.cgi?id=783751
 */
/**
 * G_IN_SET:
 * @x: Integer (or smaller) sized value
 * @...: Elements to compare
 *
 * It's quite common to test whether or not `char` values or Unix @errno (among) others
 * are members of a small set.  Normally one has to choose to either use `if (x == val || x == otherval ...)`
 * or a `switch` statement.  This macro is useful to reduce duplication in the first case,
 * where one can write simply `if (G_IN_SET (x, val, otherval))`, and avoid the verbosity
 * that the `switch` statement requires.
 */
#define G_IN_SET(x, ...)                          \
        ({                                      \
                gboolean _g_inset_found = FALSE;            \
                /* If the build breaks in the line below, you need to extend the case macros */ \
                static G_GNUC_UNUSED char _static_assert__macros_need_to_be_extended[20 - sizeof((int[]){__VA_ARGS__})/sizeof(int)]; \
                switch(x) {                     \
                _G_INSET_FOR_EACH_MAKE_CASE(__VA_ARGS__) \
                        _g_inset_found = TRUE;          \
                        break;                  \
                default:                        \
                        break;                  \
                }                               \
                _g_inset_found;                 \
        })

#endif /* ifndef G_IN_SET */

#define _GLNX_CONCAT(a, b)  a##b
#define _GLNX_CONCAT_INDIRECT(a, b) _GLNX_CONCAT(a, b)
#define _GLNX_MAKE_ANONYMOUS(a) _GLNX_CONCAT_INDIRECT(a, __COUNTER__)

#define _GLNX_HASH_TABLE_FOREACH_IMPL_KV(guard, ht, it, kt, k, vt, v)          \
    gboolean guard = TRUE;                                                     \
    G_STATIC_ASSERT (sizeof (kt) == sizeof (void*));                           \
    G_STATIC_ASSERT (sizeof (vt) == sizeof (void*));                           \
    for (GHashTableIter it;                                                    \
         guard && ({ g_hash_table_iter_init (&it, ht), TRUE; });               \
         guard = FALSE)                                                        \
            for (kt k; guard; guard = FALSE)                                   \
                for (vt v; g_hash_table_iter_next (&it, (gpointer)&k, (gpointer)&v);)


/* Cleaner method to iterate over a GHashTable. I.e. rather than
 *
 *   gpointer k, v;
 *   GHashTableIter it;
 *   g_hash_table_iter_init (&it, table);
 *   while (g_hash_table_iter_next (&it, &k, &v))
 *     {
 *       const char *str = k;
 *       GPtrArray *arr = v;
 *       ...
 *     }
 *
 * you can simply do
 *
 *   GLNX_HASH_TABLE_FOREACH_IT (table, it, const char*, str, GPtrArray*, arr)
 *     {
 *       ...
 *     }
 *
 * All variables are scoped within the loop. You may use the `it` variable as
 * usual, e.g. to remove an element using g_hash_table_iter_remove(&it). There
 * are shorter variants for the more common cases where you do not need access
 * to the iterator or to keys/values:
 *
 *   GLNX_HASH_TABLE_FOREACH (table, const char*, str) { ... }
 *   GLNX_HASH_TABLE_FOREACH_V (table, MyData*, data) { ... }
 *   GLNX_HASH_TABLE_FOREACH_KV (table, const char*, str, MyData*, data) { ... }
 *
 */
#define GLNX_HASH_TABLE_FOREACH_IT(ht, it, kt, k, vt, v) \
    _GLNX_HASH_TABLE_FOREACH_IMPL_KV( \
         _GLNX_MAKE_ANONYMOUS(_glnx_ht_iter_guard_), ht, it, kt, k, vt, v)

/* Variant of GLNX_HASH_TABLE_FOREACH without having to specify an iterator. An
 * anonymous iterator will be created. */
#define GLNX_HASH_TABLE_FOREACH_KV(ht, kt, k, vt, v) \
    _GLNX_HASH_TABLE_FOREACH_IMPL_KV( \
         _GLNX_MAKE_ANONYMOUS(_glnx_ht_iter_guard_), ht, \
         _GLNX_MAKE_ANONYMOUS(_glnx_ht_iter_it_), kt, k, vt, v)

/* Variant of GLNX_HASH_TABLE_FOREACH_KV which omits unpacking keys. */
#define GLNX_HASH_TABLE_FOREACH_V(ht, vt, v) \
    _GLNX_HASH_TABLE_FOREACH_IMPL_KV( \
         _GLNX_MAKE_ANONYMOUS(_glnx_ht_iter_guard_), ht, \
         _GLNX_MAKE_ANONYMOUS(_glnx_ht_iter_it_), \
         gpointer, _GLNX_MAKE_ANONYMOUS(_glnx_ht_iter_v_), \
         vt, v)

/* Variant of GLNX_HASH_TABLE_FOREACH_KV which omits unpacking vals. */
#define GLNX_HASH_TABLE_FOREACH(ht, kt, k) \
    _GLNX_HASH_TABLE_FOREACH_IMPL_KV( \
         _GLNX_MAKE_ANONYMOUS(_glnx_ht_iter_guard_), ht, \
         _GLNX_MAKE_ANONYMOUS(_glnx_ht_iter_it_), kt, k, \
         gpointer, _GLNX_MAKE_ANONYMOUS(_glnx_ht_iter_v_))

#endif /* GI_SCANNER */

G_END_DECLS
