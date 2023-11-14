/*
 * Copyright 2023 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <glib.h>
#include "libglnx.h"
#include "flatpak.h"

#include "flatpak-utils-private.h"

#include "tests/testlib.h"

static const struct
{
  const char *in;
  const char *expected;
} lang_from_locale_tests[] =
{
  { "C", NULL },
  { "C.UTF-8", NULL },
  { "en.ISO-8859-15", "en" },
  { "en@cantab", "en" },
  { "en_GB", "en" },
  { "en_US.utf8", "en" },
  { "sv_FI@euro", "sv" },
};

static void
test_lang_from_locale (void)
{
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (lang_from_locale_tests); i++)
    {
      const char *in = lang_from_locale_tests[i].in;
      const char *expected = lang_from_locale_tests[i].expected;
      g_autofree char *actual = NULL;

      g_test_message ("%s", in);
      actual = flatpak_get_lang_from_locale (in);
      g_test_message ("-> %s", actual ? actual : "(null)");
      g_assert_cmpstr (actual, ==, expected);
    }
}

int
main (int argc, char *argv[])
{
  int res;

  g_test_init (&argc, &argv, NULL);
  isolated_test_dir_global_setup ();

  g_test_add_func ("/locale/lang-from-locale", test_lang_from_locale);

  res = g_test_run ();

  isolated_test_dir_global_teardown ();

  return res;
}
