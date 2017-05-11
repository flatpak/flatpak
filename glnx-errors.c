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

#include "config.h"

#include <glnx-backport-autocleanups.h>
#include <glnx-errors.h>

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
