/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014,2015 Colin Walters <walters@verbum.org>.
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

#include <glnx-backport-autocleanups.h>
#include <errno.h>

G_BEGIN_DECLS

/* Set @error with G_IO_ERROR/G_IO_ERROR_FAILED.
 *
 * This function returns %FALSE so it can be used conveniently in a single
 * statement:
 *
 * ```
 *   if (strcmp (foo, "somevalue") != 0)
 *     return glnx_throw (error, "key must be somevalue, not '%s'", foo);
 * ```
 */
static inline gboolean G_GNUC_PRINTF (2,3)
glnx_throw (GError **error, const char *fmt, ...)
{
  if (error == NULL)
    return FALSE;

  va_list args;
  va_start (args, fmt);
  GError *new = g_error_new_valist (G_IO_ERROR, G_IO_ERROR_FAILED, fmt, args);
  va_end (args);
  g_propagate_error (error, g_steal_pointer (&new));
  return FALSE;
}

/* Like `glnx_throw ()`, but returns %NULL. */
#define glnx_null_throw(error, args...) \
  ({glnx_throw (error, args); NULL;})

/* Implementation detail of glnx_throw_prefix() */
void glnx_real_set_prefix_error_va (GError     *error,
                                    const char *format,
                                    va_list     args) G_GNUC_PRINTF (2,0);

/* Prepend to @error's message by `$prefix: ` where `$prefix` is computed via
 * printf @fmt. Returns %FALSE so it can be used conveniently in a single
 * statement:
 *
 * ```
 *   if (!function_that_fails (s, error))
 *     return glnx_throw_prefix (error, "while handling '%s'", s);
 * ```
 * */
static inline gboolean G_GNUC_PRINTF (2,3)
glnx_prefix_error (GError **error, const char *fmt, ...)
{
  if (error == NULL)
    return FALSE;

  va_list args;
  va_start (args, fmt);
  glnx_real_set_prefix_error_va (*error, fmt, args);
  va_end (args);
  return FALSE;
}

/* Like `glnx_prefix_error ()`, but returns %NULL. */
#define glnx_prefix_error_null(error, args...) \
  ({glnx_prefix_error (error, args); NULL;})

/* Set @error using the value of `g_strerror (errno)`.
 *
 * This function returns %FALSE so it can be used conveniently in a single
 * statement:
 *
 * ```
 *   if (unlinkat (fd, somepathname) < 0)
 *     return glnx_throw_errno (error);
 * ```
 */
static inline gboolean
glnx_throw_errno (GError **error)
{
  /* Save the value of errno, in case one of the
   * intermediate function calls happens to set it.
   */
  int errsv = errno;
  g_set_error_literal (error, G_IO_ERROR,
                       g_io_error_from_errno (errsv),
                       g_strerror (errsv));
  /* We also restore the value of errno, since that's
   * what was done in a long-ago libgsystem commit
   * https://git.gnome.org/browse/libgsystem/commit/?id=ed106741f7a0596dc8b960b31fdae671d31d666d
   * but I certainly can't remember now why I did that.
   */
  errno = errsv;
  return FALSE;
}

/* Like glnx_throw_errno(), but yields a NULL pointer. */
#define glnx_null_throw_errno(error) \
  ({glnx_throw_errno (error); NULL;})

/* Implementation detail of glnx_throw_errno_prefix() */
void glnx_real_set_prefix_error_from_errno_va (GError     **error,
                                               gint         errsv,
                                               const char  *format,
                                               va_list      args) G_GNUC_PRINTF (3,0);

/* Set @error using the value of `$prefix: g_strerror (errno)` where `$prefix`
 * is computed via printf @fmt.
 *
 * This function returns %FALSE so it can be used conveniently in a single
 * statement:
 *
 * ```
 *   return glnx_throw_errno_prefix (error, "unlinking %s", pathname);
 * ```
 */
static inline gboolean G_GNUC_PRINTF (2,3)
glnx_throw_errno_prefix (GError **error, const char *fmt, ...)
{
  int errsv = errno;
  va_list args;
  va_start (args, fmt);
  glnx_real_set_prefix_error_from_errno_va (error, errsv, fmt, args);
  va_end (args);
  /* See comment above about preserving errno */
  errno = errsv;
  return FALSE;
}

/* Like glnx_throw_errno_prefix(), but yields a NULL pointer. */
#define glnx_null_throw_errno_prefix(error, args...) \
  ({glnx_throw_errno_prefix (error, args); NULL;})

/* BEGIN LEGACY APIS */

#define glnx_set_error_from_errno(error)                \
  do {                                                  \
    glnx_throw_errno (error);                           \
  } while (0);

#define glnx_set_prefix_error_from_errno(error, format, args...)  \
  do {                                                            \
    glnx_throw_errno_prefix (error, format, args);                \
  } while (0);

G_END_DECLS
