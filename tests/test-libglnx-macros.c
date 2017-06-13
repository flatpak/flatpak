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

#include "config.h"
#include "libglnx.h"
#include <glib.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <string.h>

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
  g_assert (!G_IN_SET ('y', 'a', 'x', 'c'))
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/inset", test_inset);
  return g_test_run();
}
