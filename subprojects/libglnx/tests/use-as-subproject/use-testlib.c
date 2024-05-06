/*
 * Copyright 2022 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <libglnx.h>
#include <libglnx-testlib.h>

int
main (void)
{
  _GLNX_TEST_DECLARE_ERROR (local_error, error);

  glnx_throw (error, "Whatever");
  g_clear_error (&local_error);
  return 0;
}
