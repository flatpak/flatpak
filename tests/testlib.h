/*
 * Copyright © 2018-2021 Collabora Ltd.
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
#include <gio/gio.h>
#include <libglnx.h>

char *assert_mkdtemp (char *tmpl);

extern char *isolated_test_dir;
void isolated_test_dir_global_setup (void);
void isolated_test_dir_global_teardown (void);

typedef struct
{
  GSubprocess *dbus_daemon;
  gchar *dbus_address;
  gchar *temp_dir;
} TestsDBusDaemon;

void tests_dbus_daemon_setup (TestsDBusDaemon *self);
void tests_dbus_daemon_teardown (TestsDBusDaemon *self);

typedef struct _TestsStdoutToStderr TestsStdoutToStderr;
TestsStdoutToStderr *tests_stdout_to_stderr_begin (void);
void tests_stdout_to_stderr_end (TestsStdoutToStderr *original);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (TestsStdoutToStderr, tests_stdout_to_stderr_end);

#define TESTS_SCOPED_STDOUT_TO_STDERR \
  G_GNUC_UNUSED g_autoptr(TestsStdoutToStderr) _tests_stdout_to_stderr = tests_stdout_to_stderr_begin ()

/*
 * assert_cmpstr_free_lhs:
 * @lhs: (transfer full): An expression returning an owned string,
 *  which will be freed
 * @op: Either `==`, `!=`, `<=`, `<`, `>` or `>=`
 * @rhs: (transfer none): An expression returning an unowned string
 *
 * Assert that @lhs has the given relationship with @rhs, then free @lhs.
 */
#define assert_cmpstr_free_lhs(lhs, op, rhs) \
  do { \
    g_autofree char *free_lhs = NULL; \
    g_assert_cmpstr ((free_lhs = (lhs)), op, (rhs)); \
  } while (0)

/*
 * assert_cmpstr_free_rhs:
 * @lhs: (transfer none): An expression returning an unowned string
 * @op: Either `==`, `!=`, `<=`, `<`, `>` or `>=`
 * @rhs: (transfer full): An expression returning an owned string,
 *  which will be freed
 *
 * Assert that @lhs has the given relationship with @rhs, then free @rhs.
 */
#define assert_cmpstr_free_rhs(lhs, op, rhs) \
  do { \
    g_autofree char *free_rhs = NULL; \
    g_assert_cmpstr ((lhs), op, (free_rhs = (rhs))); \
  } while (0)

/*
 * assert_cmpstr_free_both:
 * @lhs: (transfer full): An expression returning an owned string,
 *  which will be freed
 * @op: Either `==`, `!=`, `<=`, `<`, `>` or `>=`
 * @rhs: (transfer full): An expression returning an owned string,
 *  which will be freed
 *
 * Assert that @lhs has the given relationship with @rhs, then free both
 * strings.
 */
#define assert_cmpstr_free_both(lhs, op, rhs) \
  do { \
    g_autofree char *free_lhs = NULL; \
    g_autofree char *free_rhs = NULL; \
    g_assert_cmpstr ((free_lhs = (lhs)), op, (free_rhs = (rhs))); \
  } while (0)

#endif
