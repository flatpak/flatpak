/*
 * Copyright 2022 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <libglnx.h>

int
main (void)
{
  GError *error = NULL;

  glnx_throw (&error, "whatever");
  g_clear_error (&error);
  return 0;
}
