/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Red Hat, Inc.
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

typedef GError _GLnxTestAutoError;
static inline void
_glnx_test_auto_error_cleanup (_GLnxTestAutoError *autoerror)
{
  g_assert_no_error (autoerror);
  /* We could add a clear call here, but no point...we'll have aborted */
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(_GLnxTestAutoError, _glnx_test_auto_error_cleanup);

#define _GLNX_TEST_DECLARE_ERROR(local_error, error)      \
  g_autoptr(_GLnxTestAutoError) local_error = NULL; \
  GError **error = &local_error
