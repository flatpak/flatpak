/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright 1995-1997 Peter Mattis, Spencer Kimball and Josh MacDonald
 * Copyright 2015 Colin Walters <walters@verbum.org>
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

#ifndef g_assert_nonnull
#define g_assert_nonnull(x) g_assert (x != NULL)
#endif

#ifndef g_assert_null
#define g_assert_null(x) g_assert (x == NULL)
#endif

#if !GLIB_CHECK_VERSION (2, 38, 0)
#define g_test_skip(s) g_test_message ("SKIP: %s", s)
#endif

G_END_DECLS
