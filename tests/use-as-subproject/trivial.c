/*
 * Copyright 2022 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <glib.h>

int
main (void)
{
  GError *error = NULL;

  g_clear_error (&error);
  return 0;
}
