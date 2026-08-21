/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2020-2026 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <locale.h>

#include <glib.h>

#include "flatpak.h"
#include "flatpak-ref-utils-private.h"
#include "flatpak-utils-private.h"

#include "tests/testlib.h"

static void
test_valid_arch (void)
{
  static const char * const good[] =
  {
    "i386",
    "mips64el",
    "x86_64",
  };
  static const char * const bad[] =
  {
    "",
    "--overrule-some-option",
    ".",
    "..",
    "a/b",
    "x86-64",
    "../path_traversal",
    "path/../traversal",
    "path_traversal/..",
    "\xc3\xa1",   /* U+00E1 LATIN SMALL LETTER A WITH ACUTE */
    "\xff",       /* not valid UTF-8 */
    "a\xc3\xa1",
  };
  static const struct
  {
    const char *name;
    int len;
  }
  initially_good[] =
  {
    { "abc\xc3\xa1", 3 },
    { "_/", 1 },
  };

  for (size_t i = 0; i < G_N_ELEMENTS (good); i++)
    {
      g_autoptr(GError) local_error = NULL;
      gboolean ok = flatpak_is_valid_arch (good[i], -1, &local_error);

      g_test_message ("good architecture \"%s\" -> %s",
                      good[i], ok ? "OK" : local_error->message);
      g_assert_no_error (local_error);
      g_assert_true (ok);
    }

  for (size_t i = 0; i < G_N_ELEMENTS (initially_good); i++)
    {
      g_autoptr(GError) local_error = NULL;
      gboolean ok = flatpak_is_valid_arch (initially_good[i].name,
                                           initially_good[i].len,
                                           &local_error);

      g_test_message ("good architecture \"%.*s\" -> %s",
                      initially_good[i].len,
                      initially_good[i].name,
                      ok ? "OK" : local_error->message);
      g_assert_no_error (local_error);
      g_assert_true (ok);
    }

  for (size_t i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      g_autoptr(GError) local_error = NULL;
      g_autofree char *escaped = flatpak_escape_string (bad[i], FLATPAK_ESCAPE_DO_NOT_QUOTE);
      gboolean ok = flatpak_is_valid_arch (bad[i], -1, &local_error);

      g_test_message ("bad architecture \"%s\" -> %s",
                      escaped, ok ? "wrongly accepted" : local_error->message);
      g_assert_nonnull (local_error);
      g_assert_true (g_utf8_validate (local_error->message, -1, NULL));
      g_assert_false (ok);
    }

    {
      g_autoptr(GError) local_error = NULL;
      gboolean ok = flatpak_is_valid_arch (good[0], 0, &local_error);

      g_test_message ("empty architecture -> %s",
                      ok ? "wrongly accepted" : local_error->message);
      g_assert_nonnull (local_error);
      g_assert_true (g_utf8_validate (local_error->message, -1, NULL));
      g_assert_false (ok);
    }
}

static void
test_valid_branch (void)
{
  static const char * const good[] =
  {
    "stable",
    "private-beta",
    "public_beta",
    "1.0",
    "--whatever--",
    "x",
  };
  static const char * const bad[] =
  {
    "",
    ".",
    "..",
    "a/b",
    "../path-traversal",
    "path-traversal/..",
    "path/../traversal",
    "\xc3\xa1",   /* U+00E1 LATIN SMALL LETTER A WITH ACUTE */
    "\xff",       /* not valid UTF-8 */
    "a\xc3\xa1",
  };
  static const struct
  {
    const char *name;
    int len;
  }
  initially_good[] =
  {
    { "abc\xc3\xa1", 3 },
    { "x/", 1 },
  };

  for (size_t i = 0; i < G_N_ELEMENTS (good); i++)
    {
      g_autoptr(GError) local_error = NULL;
      gboolean ok = flatpak_is_valid_branch (good[i], -1, &local_error);

      g_test_message ("good branch \"%s\" -> %s",
                      good[i], ok ? "OK" : local_error->message);
      g_assert_no_error (local_error);
      g_assert_true (ok);
    }

  for (size_t i = 0; i < G_N_ELEMENTS (initially_good); i++)
    {
      g_autoptr(GError) local_error = NULL;
      gboolean ok = flatpak_is_valid_branch (initially_good[i].name,
                                             initially_good[i].len,
                                             &local_error);

      g_test_message ("good branch \"%.*s\" -> %s",
                      initially_good[i].len,
                      initially_good[i].name,
                      ok ? "OK" : local_error->message);
      g_assert_no_error (local_error);
      g_assert_true (ok);
    }

  for (size_t i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      g_autoptr(GError) local_error = NULL;
      g_autofree char *escaped = flatpak_escape_string (bad[i], FLATPAK_ESCAPE_DO_NOT_QUOTE);
      gboolean ok = flatpak_is_valid_branch (bad[i], -1, &local_error);

      g_test_message ("bad branch \"%s\" -> %s",
                      escaped, ok ? "wrongly accepted" : local_error->message);
      g_assert_nonnull (local_error);
      g_assert_true (g_utf8_validate (local_error->message, -1, NULL));
      g_assert_false (ok);
    }

    {
      g_autoptr(GError) local_error = NULL;
      gboolean ok = flatpak_is_valid_branch (good[0], 0, &local_error);

      g_test_message ("empty branch -> %s",
                      ok ? "wrongly accepted" : local_error->message);
      g_assert_nonnull (local_error);
      g_assert_true (g_utf8_validate (local_error->message, -1, NULL));
      g_assert_false (ok);
    }
}

#define NAME_CHAR_x16 "z12345678.abcdef"
#define NAME_CHAR_x32 NAME_CHAR_x16 NAME_CHAR_x16
#define NAME_CHAR_x64 NAME_CHAR_x32 NAME_CHAR_x32
#define NAME_CHAR_x128 NAME_CHAR_x64 NAME_CHAR_x64
#define NAME_CHAR_x256 NAME_CHAR_x128 NAME_CHAR_x128

static void
test_valid_name (void)
{
  static const char * const good[] =
  {
    "com.example.Foo",
    "a.b.c",
    "org._7_zip.Archiver",
    "org._7_zip._7-zip",    /* "-" in last element discouraged but allowed */
    "uk.co.pseudorandom.name.has.many.components",
  };
  static const char * const bad[] =
  {
    "",
    "a.b",        /* not enough elements */
    "org.7_zip.Archiver",
    "org._7-zip.Archiver",
    "\xc3\xa1",   /* U+00E1 LATIN SMALL LETTER A WITH ACUTE */
    "\xff",       /* not valid UTF-8 */
    "a\xc3\xa1",
    NAME_CHAR_x256,
  };
  static const struct
  {
    const char *name;
    int len;
  }
  initially_good[] =
  {
    { "a.b.c\xc3\xa1", 5 },
    { NAME_CHAR_x256, 255 },
  };

  for (size_t i = 0; i < G_N_ELEMENTS (good); i++)
    {
      g_autoptr(GError) local_error = NULL;
      gboolean ok = flatpak_is_valid_name (good[i], -1, &local_error);

      g_test_message ("good app name \"%s\" -> %s",
                      good[i], ok ? "OK" : local_error->message);
      g_assert_no_error (local_error);
      g_assert_true (ok);
    }

  for (size_t i = 0; i < G_N_ELEMENTS (initially_good); i++)
    {
      g_autoptr(GError) local_error = NULL;
      gboolean ok = flatpak_is_valid_name (initially_good[i].name,
                                           initially_good[i].len,
                                           &local_error);

      g_test_message ("good app name \"%.*s\" -> %s",
                      initially_good[i].len,
                      initially_good[i].name,
                      ok ? "OK" : local_error->message);
      g_assert_no_error (local_error);
      g_assert_true (ok);
    }

  for (size_t i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      g_autoptr(GError) local_error = NULL;
      g_autofree char *escaped = flatpak_escape_string (bad[i], FLATPAK_ESCAPE_DO_NOT_QUOTE);
      gboolean ok = flatpak_is_valid_name (bad[i], -1, &local_error);

      g_test_message ("bad app name \"%s\" -> %s",
                      escaped, ok ? "wrongly accepted" : local_error->message);
      g_assert_nonnull (local_error);
      g_assert_true (g_utf8_validate (local_error->message, -1, NULL));
      g_assert_false (ok);
    }

    {
      g_autoptr(GError) local_error = NULL;
      gboolean ok = flatpak_is_valid_name (good[0], 0, &local_error);

      g_test_message ("empty app name -> %s",
                      ok ? "wrongly accepted" : local_error->message);
      g_assert_nonnull (local_error);
      g_assert_true (g_utf8_validate (local_error->message, -1, NULL));
      g_assert_false (ok);
    }
}

static void
test_valid_remote_name (void)
{
  static const char * const good[] =
  {
    "7zip",
    "_abc123",
    "a",
    "a_b",
    "remote-1.0",
  };
  static const char * const bad[] =
  {
    "",
    "--overrule-some-option",
    ".",
    "..",
    "a/b",
    "../path-traversal",
    "path/../traversal",
    "path-traversal/..",
    "\xc3\xa1",   /* U+00E1 LATIN SMALL LETTER A WITH ACUTE */
    "\xff",       /* not valid UTF-8 */
    "a\xc3\xa1",
  };
  static const struct
  {
    const char *name;
    int len;
  }
  initially_good[] =
  {
    { "abc\xc3\xa1", 3 },
    { "x/", 1 },
  };

  for (size_t i = 0; i < G_N_ELEMENTS (good); i++)
    {
      g_autoptr(GError) local_error = NULL;
      gboolean ok = flatpak_is_valid_remote_name (good[i], -1, &local_error);

      g_test_message ("good remote name \"%s\" -> %s",
                      good[i], ok ? "OK" : local_error->message);
      g_assert_no_error (local_error);
      g_assert_true (ok);
    }

  for (size_t i = 0; i < G_N_ELEMENTS (initially_good); i++)
    {
      g_autoptr(GError) local_error = NULL;
      gboolean ok = flatpak_is_valid_remote_name (initially_good[i].name,
                                                  initially_good[i].len,
                                                  &local_error);

      g_test_message ("good remote name \"%.*s\" -> %s",
                      initially_good[i].len,
                      initially_good[i].name,
                      ok ? "OK" : local_error->message);
      g_assert_no_error (local_error);
      g_assert_true (ok);
    }

  for (size_t i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      g_autoptr(GError) local_error = NULL;
      g_autofree char *escaped = flatpak_escape_string (bad[i], FLATPAK_ESCAPE_DO_NOT_QUOTE);
      gboolean ok = flatpak_is_valid_remote_name (bad[i], -1, &local_error);

      g_test_message ("bad remote name \"%s\" -> %s",
                      escaped, ok ? "wrongly accepted" : local_error->message);
      g_assert_nonnull (local_error);
      g_assert_true (g_utf8_validate (local_error->message, -1, NULL));
      g_assert_false (ok);
    }

    {
      g_autoptr(GError) local_error = NULL;
      gboolean ok = flatpak_is_valid_remote_name (good[0], 0, &local_error);

      g_test_message ("empty remote name -> %s",
                      ok ? "wrongly accepted" : local_error->message);
      g_assert_nonnull (local_error);
      g_assert_true (g_utf8_validate (local_error->message, -1, NULL));
      g_assert_false (ok);
    }
}

int
main (int argc, char *argv[])
{
  setlocale (LC_ALL, "");

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/ref-utils/valid-arch", test_valid_arch);
  g_test_add_func ("/ref-utils/valid-branch", test_valid_branch);
  g_test_add_func ("/ref-utils/valid-name", test_valid_name);
  g_test_add_func ("/ref-utils/valid-remote-name", test_valid_remote_name);

  return g_test_run ();
}
