/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright 1995-1997 Peter Mattis, Spencer Kimball and Josh MacDonald
 * Copyright 2015 Colin Walters <walters@verbum.org>
 * Copyright 2014 Dan Winship
 * Copyright 2015 Colin Walters
 * Copyright 2017 Emmanuele Bassi
 * Copyright 2018-2019 Endless OS Foundation LLC
 * Copyright 2020 Niels De Graef
 * Copyright 2021-2022 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include <gio/gio.h>

#include "glnx-backports.h"

G_BEGIN_DECLS

#ifndef g_assert_true       /* added in 2.38 */
#define g_assert_true(x) g_assert ((x))
#endif

#ifndef g_assert_false      /* added in 2.38 */
#define g_assert_false(x) g_assert (!(x))
#endif

#ifndef g_assert_nonnull    /* added in 2.40 */
#define g_assert_nonnull(x) g_assert (x != NULL)
#endif

#ifndef g_assert_null       /* added in 2.40 */
#define g_assert_null(x) g_assert (x == NULL)
#endif

#if !GLIB_CHECK_VERSION (2, 38, 0)
/* Not exactly equivalent, but close enough */
#define g_test_skip(s) g_test_message ("SKIP: %s", s)
#endif

#if !GLIB_CHECK_VERSION (2, 58, 0)
/* Before 2.58, g_test_incomplete() didn't set the exit status correctly */
#define g_test_incomplete(s) _glnx_test_incomplete_printf ("%s", s)
#endif

#if !GLIB_CHECK_VERSION (2, 46, 0)
#define g_assert_cmpmem(m1, l1, m2, l2) G_STMT_START {\
                                             gconstpointer __m1 = m1, __m2 = m2; \
                                             int __l1 = l1, __l2 = l2; \
                                             if (__l1 != 0 && __m1 == NULL) \
                                               g_assertion_message (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, \
                                                                    "assertion failed (" #l1 " == 0 || " #m1 " != NULL)"); \
                                             else if (__l2 != 0 && __m2 == NULL) \
                                               g_assertion_message (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, \
                                                                    "assertion failed (" #l2 " == 0 || " #m2 " != NULL)"); \
                                             else if (__l1 != __l2) \
                                               g_assertion_message_cmpnum (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, \
                                                                           #l1 " (len(" #m1 ")) == " #l2 " (len(" #m2 "))", \
                                                                           (long double) __l1, "==", (long double) __l2, 'i'); \
                                             else if (__l1 != 0 && __m2 != NULL && memcmp (__m1, __m2, __l1) != 0) \
                                               g_assertion_message (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, \
                                                                    "assertion failed (" #m1 " == " #m2 ")"); \
                                        } G_STMT_END
#endif

#if !GLIB_CHECK_VERSION (2, 58, 0)
#define g_assert_cmpfloat_with_epsilon(n1,n2,epsilon) \
                                        G_STMT_START { \
                                             double __n1 = (n1), __n2 = (n2), __epsilon = (epsilon); \
                                             if (G_APPROX_VALUE (__n1,  __n2, __epsilon)) ; else \
                                               g_assertion_message_cmpnum (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, \
                                                 #n1 " == " #n2 " (+/- " #epsilon ")", __n1, "==", __n2, 'f'); \
                                        } G_STMT_END
#endif

#if !GLIB_CHECK_VERSION (2, 60, 0)
#define g_assert_cmpvariant(v1, v2) \
  G_STMT_START \
  { \
    GVariant *__v1 = (v1), *__v2 = (v2); \
    if (!g_variant_equal (__v1, __v2)) \
      { \
        gchar *__s1, *__s2, *__msg; \
        __s1 = g_variant_print (__v1, TRUE); \
        __s2 = g_variant_print (__v2, TRUE); \
        __msg = g_strdup_printf ("assertion failed (" #v1 " == " #v2 "): %s does not equal %s", __s1, __s2); \
        g_assertion_message (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, __msg); \
        g_free (__s1); \
        g_free (__s2); \
        g_free (__msg); \
      } \
  } \
  G_STMT_END
#endif

#if !GLIB_CHECK_VERSION (2, 62, 0)
/* Not exactly equivalent, but close enough */
#define g_test_summary(s) g_test_message ("SUMMARY: %s", s)
#endif

#if !GLIB_CHECK_VERSION (2, 66, 0)
#define g_assert_no_errno(expr)         G_STMT_START { \
                                             int __ret, __errsv; \
                                             errno = 0; \
                                             __ret = expr; \
                                             __errsv = errno; \
                                             if (__ret < 0) \
                                               { \
                                                 gchar *__msg; \
                                                 __msg = g_strdup_printf ("assertion failed (" #expr " >= 0): errno %i: %s", __errsv, g_strerror (__errsv)); \
                                                 g_assertion_message (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, __msg); \
                                                 g_free (__msg); \
                                               } \
                                        } G_STMT_END
#endif

#if !GLIB_CHECK_VERSION (2, 68, 0)
#define g_assertion_message_cmpstrv _glnx_assertion_message_cmpstrv
void _glnx_assertion_message_cmpstrv (const char         *domain,
                                      const char         *file,
                                      int                 line,
                                      const char         *func,
                                      const char         *expr,
                                      const char * const *arg1,
                                      const char * const *arg2,
                                      gsize               first_wrong_idx);
#define g_assert_cmpstrv(strv1, strv2) \
  G_STMT_START \
  { \
    const char * const *__strv1 = (const char * const *) (strv1); \
    const char * const *__strv2 = (const char * const *) (strv2); \
    if (!__strv1 || !__strv2) \
      { \
        if (__strv1) \
          { \
            g_assertion_message (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, \
                                 "assertion failed (" #strv1 " == " #strv2 "): " #strv2 " is NULL, but " #strv1 " is not"); \
          } \
        else if (__strv2) \
          { \
            g_assertion_message (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, \
                                 "assertion failed (" #strv1 " == " #strv2 "): " #strv1 " is NULL, but " #strv2 " is not"); \
          } \
      } \
    else \
      { \
        guint __l1 = g_strv_length ((char **) __strv1); \
        guint __l2 = g_strv_length ((char **) __strv2); \
        if (__l1 != __l2) \
          { \
            char *__msg; \
            __msg = g_strdup_printf ("assertion failed (" #strv1 " == " #strv2 "): length %u does not equal length %u", __l1, __l2); \
            g_assertion_message (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, __msg); \
            g_free (__msg); \
          } \
        else \
          { \
            guint __i; \
            for (__i = 0; __i < __l1; __i++) \
              { \
                if (g_strcmp0 (__strv1[__i], __strv2[__i]) != 0) \
                  { \
                    g_assertion_message_cmpstrv (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, \
                                                 #strv1 " == " #strv2, \
                                                 __strv1, __strv2, __i); \
                  } \
              } \
          } \
      } \
  } \
  G_STMT_END
#endif

#if !GLIB_CHECK_VERSION (2, 70, 0)
/* Before 2.70, diagnostic messages containing newlines were problematic */
#define g_test_message(...) _glnx_test_message_safe (__VA_ARGS__)
void _glnx_test_message_safe (const char *format, ...) G_GNUC_PRINTF (1, 2);

#define g_test_fail_printf _glnx_test_fail_printf
void _glnx_test_fail_printf (const char *format, ...) G_GNUC_PRINTF (1, 2);
#define g_test_skip_printf _glnx_test_skip_printf
void _glnx_test_skip_printf (const char *format, ...) G_GNUC_PRINTF (1, 2);
#define g_test_incomplete_printf _glnx_test_incomplete_printf
void _glnx_test_incomplete_printf (const char *format, ...) G_GNUC_PRINTF (1, 2);
#endif

#if !GLIB_CHECK_VERSION (2, 78, 0)
#define g_test_disable_crash_reporting _glnx_test_disable_crash_reporting
void _glnx_test_disable_crash_reporting (void);
#endif

G_END_DECLS
