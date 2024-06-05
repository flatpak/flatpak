/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014,2015 Colin Walters <walters@verbum.org>.
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

#include "libglnx-config.h"

#include <glnx-backport-autocleanups.h>
#include <glnx-errors.h>

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
gboolean
glnx_throw (GError    **error,
            const char *fmt,
            ...)
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

void
glnx_real_set_prefix_error_va (GError     *error,
                               const char *format,
                               va_list     args)
{
  if (error == NULL)
    return;

  g_autofree char *old_msg = g_steal_pointer (&error->message);
  g_autoptr(GString) buf = g_string_new ("");
  g_string_append_vprintf (buf, format, args);
  g_string_append (buf, ": ");
  g_string_append (buf, old_msg);
  error->message = g_string_free (g_steal_pointer (&buf), FALSE);
}

/* Prepend to @error's message by `$prefix: ` where `$prefix` is computed via
 * printf @fmt. Returns %FALSE so it can be used conveniently in a single
 * statement:
 *
 * ```
 *   if (!function_that_fails (s, error))
 *     return glnx_throw_prefix (error, "while handling '%s'", s);
 * ```
 * */
gboolean
glnx_prefix_error (GError    **error,
                   const char *fmt,
                   ...)
{
  if (error == NULL)
    return FALSE;

  va_list args;
  va_start (args, fmt);
  glnx_real_set_prefix_error_va (*error, fmt, args);
  va_end (args);
  return FALSE;
}

void
glnx_real_set_prefix_error_from_errno_va (GError     **error,
                                          gint         errsv,
                                          const char  *format,
                                          va_list      args)
{
  if (!error)
    return;

  g_set_error_literal (error,
                       G_IO_ERROR,
                       g_io_error_from_errno (errsv),
                       g_strerror (errsv));
  glnx_real_set_prefix_error_va (*error, format, args);
}

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
gboolean
glnx_throw_errno_prefix (GError    **error,
                         const char *fmt,
                         ...)
{
  int errsv = errno;
  va_list args;
  va_start (args, fmt);
  glnx_real_set_prefix_error_from_errno_va (error, errsv, fmt, args);
  va_end (args);
  /* See comment in glnx_throw_errno() about preserving errno */
  errno = errsv;
  return FALSE;
}
