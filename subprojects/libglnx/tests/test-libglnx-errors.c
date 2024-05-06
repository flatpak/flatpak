/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2017 Red Hat, Inc.
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
#include "libglnx.h"
#include <glib.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <string.h>

static void
test_error_throw (void)
{
  g_autoptr(GError) error = NULL;

  g_assert (!glnx_throw (&error, "foo: %s %d", "hello", 42));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpstr (error->message, ==, "foo: hello 42");
  g_clear_error (&error);

  gpointer dummy = glnx_null_throw (&error, "literal foo");
  g_assert (dummy == NULL);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpstr (error->message, ==, "literal foo");
  g_clear_error (&error);

  gpointer dummy2 = glnx_null_throw (&error, "foo: %s %d", "hola", 24);
  g_assert (dummy2 == NULL);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpstr (error->message, ==, "foo: hola 24");
  g_clear_error (&error);
}

static void
test_error_errno (void)
{
  g_autoptr(GError) error = NULL;
  const char noent_path[] = "/enoent-this-should-not-exist";
  int fd;

  fd = open (noent_path, O_RDONLY);
  if (fd < 0)
    {
      g_assert (!glnx_throw_errno (&error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
      g_assert (!glnx_prefix_error (&error, "myprefix"));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
      g_assert (g_str_has_prefix (error->message, "myprefix: "));
      g_clear_error (&error);
    }
  else
    g_assert_cmpint (fd, ==, -1);

  fd = open (noent_path, O_RDONLY);
  if (fd < 0)
    {
      gpointer dummy = glnx_null_throw_errno (&error);
      g_assert (dummy == NULL);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
      dummy = glnx_prefix_error_null (&error, "myprefix");
      g_assert (dummy == NULL);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
      g_assert (g_str_has_prefix (error->message, "myprefix: "));
      g_clear_error (&error);
    }
  else
    g_assert_cmpint (fd, ==, -1);

  fd = open (noent_path, O_RDONLY);
  if (fd < 0)
    {
      g_autofree char *expected_prefix = g_strdup_printf ("Failed to open %s", noent_path);
      g_assert (!glnx_throw_errno_prefix (&error, "Failed to open %s", noent_path));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
      g_assert (g_str_has_prefix (error->message, expected_prefix));
      g_clear_error (&error);
      /* And test the legacy wrapper */
      glnx_set_prefix_error_from_errno (&error, "Failed to open %s", noent_path);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
      g_assert (g_str_has_prefix (error->message, expected_prefix));
      g_clear_error (&error);
    }
  else
    g_assert_cmpint (fd, ==, -1);

  fd = open (noent_path, O_RDONLY);
  if (fd < 0)
    {
      gpointer dummy = glnx_null_throw_errno_prefix (&error, "Failed to open file");
      g_assert (dummy == NULL);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
      g_assert (g_str_has_prefix (error->message, "Failed to open file"));
      g_clear_error (&error);
    }
  else
    g_assert_cmpint (fd, ==, -1);

  fd = open (noent_path, O_RDONLY);
  if (fd < 0)
    {
      gpointer dummy = glnx_null_throw_errno_prefix (&error, "Failed to open %s", noent_path);
      g_assert (dummy == NULL);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
      g_assert (g_str_has_prefix (error->message, glnx_strjoina ("Failed to open ", noent_path)));
      g_clear_error (&error);
    }
  else
    g_assert_cmpint (fd, ==, -1);
}

static void
test_error_auto_nothrow (GError **error)
{
  GLNX_AUTO_PREFIX_ERROR("foo", error);
  /* Side effect to avoid otherwise empty function */
  g_assert_no_error (*error);
}

static void
test_error_auto_throw (GError **error)
{
  GLNX_AUTO_PREFIX_ERROR("foo", error);
  (void) glnx_throw (error, "oops");
}

static void
test_error_auto_throw_recurse (GError **error)
{
  GLNX_AUTO_PREFIX_ERROR("foo", error);

  if (TRUE)
    {
      GLNX_AUTO_PREFIX_ERROR("bar", error);
      (void) glnx_throw (error, "oops");
    }
}

static void
test_error_auto (void)
{
  g_autoptr(GError) error = NULL;
  test_error_auto_nothrow (&error);
  g_assert_no_error (error);
  test_error_auto_throw (&error);
  g_assert_nonnull (error);
  g_assert_cmpstr (error->message, ==, "foo: oops");
  g_clear_error (&error);
  test_error_auto_throw_recurse (&error);
  g_assert_nonnull (error);
  g_assert_cmpstr (error->message, ==, "foo: bar: oops");
}

int main (int argc, char **argv)
{
  int ret;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/error-throw", test_error_throw);
  g_test_add_func ("/error-errno", test_error_errno);
  g_test_add_func ("/error-auto", test_error_auto);

  ret = g_test_run();

  return ret;
}
