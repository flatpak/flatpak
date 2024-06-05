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
test_info (void)
{
  g_info ("hello, world");
  g_info ("answer=%d", 42);
}

static void
test_inset (void)
{
  g_assert (G_IN_SET (7, 7));
  g_assert (G_IN_SET (7, 42, 7));
  g_assert (G_IN_SET (7, 7,42,3,9));
  g_assert (G_IN_SET (42, 7,42,3,9));
  g_assert (G_IN_SET (3, 7,42,3,9));
  g_assert (G_IN_SET (9, 7,42,3,9));
  g_assert (!G_IN_SET (8, 7,42,3,9));
  g_assert (!G_IN_SET (-1, 7,42,3,9));
  g_assert (G_IN_SET ('x', 'a', 'x', 'c'));
  g_assert (!G_IN_SET ('y', 'a', 'x', 'c'));
}

static void
test_hash_table_foreach (void)
{
  /* use var names all different from the macro metavars to ensure proper
   * substitution */
  g_autoptr(GHashTable) table = g_hash_table_new (g_str_hash, g_str_equal);
  const char *keys[] = {"key1", "key2"};
  const char *vals[] = {"val1", "val2"};
  g_hash_table_insert (table, (gpointer)keys[0], (gpointer)vals[0]);
  g_hash_table_insert (table, (gpointer)keys[1], (gpointer)vals[1]);

  guint i = 0;
  GLNX_HASH_TABLE_FOREACH_IT (table, it, const char*, key, const char*, val)
    {
      g_assert_cmpstr (key, ==, keys[i]);
      g_assert_cmpstr (val, ==, vals[i]);
      i++;
    }
  g_assert_cmpuint (i, ==, 2);

  i = 0;
  GLNX_HASH_TABLE_FOREACH_IT (table, it, const char*, key, const char*, val)
    {
      g_hash_table_iter_remove (&it);
      break;
    }
  g_assert_cmpuint (g_hash_table_size (table), ==, 1);

  g_hash_table_insert (table, (gpointer)keys[1], (gpointer)vals[1]);
  g_assert_cmpuint (g_hash_table_size (table), ==, 1);

  g_hash_table_insert (table, (gpointer)keys[0], (gpointer)vals[0]);
  g_assert_cmpuint (g_hash_table_size (table), ==, 2);

  i = 0;
  GLNX_HASH_TABLE_FOREACH_KV (table, const char*, key, const char*, val)
    {
      g_assert_cmpstr (key, ==, keys[i]);
      g_assert_cmpstr (val, ==, vals[i]);
      i++;
    }
  g_assert_cmpuint (i, ==, 2);

  i = 0;
  GLNX_HASH_TABLE_FOREACH (table, const char*, key)
    {
      g_assert_cmpstr (key, ==, keys[i]);
      i++;
    }
  g_assert_cmpuint (i, ==, 2);

  i = 0;
  GLNX_HASH_TABLE_FOREACH_V (table, const char*, val)
    {
      g_assert_cmpstr (val, ==, vals[i]);
      i++;
    }
  g_assert_cmpuint (i, ==, 2);
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/info", test_info);
  g_test_add_func ("/inset", test_inset);
  g_test_add_func ("/hash_table_foreach", test_hash_table_foreach);
  return g_test_run();
}
