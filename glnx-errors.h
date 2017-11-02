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

gboolean glnx_throw (GError **error, const char *fmt, ...) G_GNUC_PRINTF (2,3);

/* Like `glnx_throw ()`, but returns %NULL. */
#define glnx_null_throw(error, args...) \
  ({glnx_throw (error, args); NULL;})

/* Implementation detail of glnx_throw_prefix() */
void glnx_real_set_prefix_error_va (GError     *error,
                                    const char *format,
                                    va_list     args) G_GNUC_PRINTF (2,0);

gboolean glnx_prefix_error (GError **error, const char *fmt, ...) G_GNUC_PRINTF (2,3);

/* Like `glnx_prefix_error ()`, but returns %NULL. */
#define glnx_prefix_error_null(error, args...) \
  ({glnx_prefix_error (error, args); NULL;})

/**
 * GLNX_AUTO_PREFIX_ERROR:
 *
 * An autocleanup-based macro to automatically call `g_prefix_error()` (also with a colon+space `: `)
 * when it goes out of scope.  This is useful when one wants error strings built up by the callee
 * function, not all callers.
 *
 * ```
 * gboolean start_http_request (..., GError **error)
 * {
 *   GLNX_AUTO_PREFIX_ERROR ("HTTP request", error)
 *
 *   if (!libhttp_request_start (..., error))
 *     return FALSE;
 *   ...
 *   return TRUE;
 * ```
 */
typedef struct {
  const char *prefix;
  GError **error;
} GLnxAutoErrorPrefix;
static inline void
glnx_cleanup_auto_prefix_error (GLnxAutoErrorPrefix *prefix)
{
  if (prefix->error && *(prefix->error))
    g_prefix_error (prefix->error, "%s: ", prefix->prefix);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GLnxAutoErrorPrefix, glnx_cleanup_auto_prefix_error)
#define GLNX_AUTO_PREFIX_ERROR(text, error) \
  G_GNUC_UNUSED g_auto(GLnxAutoErrorPrefix) _GLNX_MAKE_ANONYMOUS(_glnxautoprefixerror_) = { text, error }

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

gboolean glnx_throw_errno_prefix (GError **error, const char *fmt, ...) G_GNUC_PRINTF (2,3);

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
