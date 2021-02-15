/*
 * Copyright © 2020-2021 Collabora Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TESTLIB_H
#define TESTLIB_H

#include <glib.h>

#ifndef g_assert_no_errno
#define g_assert_no_errno(expr) \
  g_assert_cmpstr ((expr) >= 0 ? NULL : g_strerror (errno), ==, NULL)
#endif

char *assert_mkdtemp (char *tmpl);

extern char *isolated_test_dir;
void isolated_test_dir_global_setup (void);
void isolated_test_dir_global_teardown (void);

#endif
