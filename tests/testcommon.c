#include "config.h"

#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <glib.h>
#include "flatpak.h"
#include "flatpak-utils-private.h"

static void
test_has_path_prefix (void)
{
  g_assert_true (flatpak_has_path_prefix ("/a/prefix/foo/bar", "/a/prefix"));
  g_assert_true (flatpak_has_path_prefix ("/a///prefix/foo/bar", "/a/prefix"));
  g_assert_true (flatpak_has_path_prefix ("/a/prefix/foo/bar", "/a/prefix/"));
  g_assert_true (flatpak_has_path_prefix ("/a/prefix/foo/bar", "/a/prefix//"));
  g_assert_true (flatpak_has_path_prefix ("/a/prefix/foo/bar", ""));
  g_assert_false (flatpak_has_path_prefix ("/a/prefixfoo/bar", "/a/prefix"));
}

static void
test_path_match_prefix (void)
{
  g_assert_cmpstr (flatpak_path_match_prefix ("/?/pre*", "/a/prefix/x"), ==, "/x");
  g_assert_cmpstr (flatpak_path_match_prefix ("/a/prefix/*", "/a/prefix/"), ==, "");
  g_assert_null (flatpak_path_match_prefix ("/?/pre?", "/a/prefix/x"));
}

static void
test_fancy_output (void)
{
  g_assert_false (flatpak_fancy_output ()); /* always false in tests */
}

static void
test_arches (void)
{
  const char **arches = flatpak_get_arches ();

#if defined(__i386__)
  g_assert_cmpstr (flatpak_get_arch (), ==, "i386");
  g_assert_true (g_strv_contains (arches, "x86_64"));
  g_assert_true (g_strv_contains (arches, "i386"));
#elif defined(__x86_64__)
  g_assert_cmpstr (flatpak_get_arch (), ==, "x86_64");
  g_assert_true (g_strv_contains (arches, "x86_64"));
  g_assert_true (g_strv_contains (arches, "i386"));
  g_assert_true (flatpak_is_linux32_arch ("i386"));
  g_assert_false (flatpak_is_linux32_arch ("x86_64"));
#else
  g_assert_true (g_strv_contains (arches, flatpak_get_arch ()));
#endif
}

static void
test_extension_matches (void)
{
  g_assert_true (flatpak_extension_matches_reason ("org.foo.bar", "", TRUE));
  g_assert_false (flatpak_extension_matches_reason ("org.foo.nosuchdriver", "active-gl-driver", TRUE));
  g_assert_false (flatpak_extension_matches_reason ("org.foo.nosuchtheme", "active-gtk-theme", TRUE));
  g_assert_false (flatpak_extension_matches_reason ("org.foo.nosuchtheme", "have-intel-gpu", TRUE));
  g_assert_false (flatpak_extension_matches_reason ("org.foo.nonono", "on-xdg-desktop-nosuchdesktop", TRUE));
  g_assert_false (flatpak_extension_matches_reason ("org.foo.nonono", "active-gl-driver;active-gtk-theme", TRUE));
}

static void
test_valid_name (void)
{
  g_assert_false (flatpak_is_valid_name ("", NULL));
  g_assert_false (flatpak_is_valid_name ("org", NULL));
  g_assert_false (flatpak_is_valid_name ("org.", NULL));
  g_assert_false (flatpak_is_valid_name ("org..", NULL));
  g_assert_false (flatpak_is_valid_name ("org..test", NULL));
  g_assert_false (flatpak_is_valid_name ("org.flatpak", NULL));
  g_assert_false (flatpak_is_valid_name ("org.1flatpak.test", NULL));
  g_assert_false (flatpak_is_valid_name ("org.flat-pak.test", NULL));
  g_assert_false (flatpak_is_valid_name ("org.-flatpak.test", NULL));
  g_assert_false (flatpak_is_valid_name ("org.flat,pak.test", NULL));

  g_assert_true (flatpak_is_valid_name ("org.flatpak.test", NULL));
  g_assert_true (flatpak_is_valid_name ("org.FlatPak.TEST", NULL));
  g_assert_true (flatpak_is_valid_name ("org0.f1atpak.test", NULL));
  g_assert_true (flatpak_is_valid_name ("org.flatpak.-test", NULL));
  g_assert_true (flatpak_is_valid_name ("org.flatpak._test", NULL));
  g_assert_true (flatpak_is_valid_name ("org.flat_pak__.te--st", NULL));
}

typedef struct
{
  const gchar *str;
  guint base;
  gint min;
  gint max;
  gint expected;
  gboolean should_fail;
} TestData;

const TestData test_data[] = {
  /* typical cases for unsigned */
  { "-1",10, 0, 2, 0, TRUE  },
  { "1", 10, 0, 2, 1, FALSE },
  { "+1",10, 0, 2, 0, TRUE  },
  { "0", 10, 0, 2, 0, FALSE },
  { "+0",10, 0, 2, 0, TRUE  },
  { "-0",10, 0, 2, 0, TRUE  },
  { "2", 10, 0, 2, 2, FALSE },
  { "+2",10, 0, 2, 0, TRUE  },
  { "3", 10, 0, 2, 0, TRUE  },
  { "+3",10, 0, 2, 0, TRUE  },

  /* min == max cases for unsigned */
  { "2",10, 2, 2, 2, FALSE  },
  { "3",10, 2, 2, 0, TRUE   },
  { "1",10, 2, 2, 0, TRUE   },

  /* invalid inputs */
  { "",   10,  0,  2,  0, TRUE },
  { "a",  10,  0,  2,  0, TRUE },
  { "1a", 10,  0,  2,  0, TRUE },

  /* leading/trailing whitespace */
  { " 1",10,  0,  2,  0, TRUE },
  { "1 ",10,  0,  2,  0, TRUE },

  /* hexadecimal numbers */
  { "a",    16,   0, 15, 10, FALSE },
  { "0xa",  16,   0, 15,  0, TRUE  },
  { "-0xa", 16,   0, 15,  0, TRUE  },
  { "+0xa", 16,   0, 15,  0, TRUE  },
  { "- 0xa",16,   0, 15,  0, TRUE  },
  { "+ 0xa",16,   0, 15,  0, TRUE  },
};

static void
test_string_to_unsigned (void)
{
  gsize idx;

  for (idx = 0; idx < G_N_ELEMENTS (test_data); ++idx)
    {
      GError *error = NULL;
      const TestData *data = &test_data[idx];
      gboolean result;
      gint value;
      guint64 value64 = 0;
      result = flatpak_utils_ascii_string_to_unsigned (data->str,
                                                       data->base,
                                                       data->min,
                                                       data->max,
                                                       &value64,
                                                       &error);
      value = value64;
      g_assert_cmpint (value, ==, value64);

      if (data->should_fail)
        {
          g_assert_false (result);
          g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
          g_clear_error (&error);
        }
      else
        {
          g_assert_true (result);
          g_assert_no_error (error);
          g_assert_cmpint (value, ==, data->expected);
        }
    }
}

typedef struct {
  const char *a;
  const char *b;
  int distance;
} Levenshtein;

static Levenshtein levenshtein_tests[] = {
  { "", "", 0 },
  { "abcdef", "abcdef", 0 },
  { "kitten", "sitting", 3 },
  { "Saturday", "Sunday", 3 }
};

static void
test_levenshtein (void)
{
  gsize idx;

  for (idx = 0; idx < G_N_ELEMENTS (levenshtein_tests); idx++)
    {
      Levenshtein *data = &levenshtein_tests[idx];

      g_assert_cmpint (flatpak_levenshtein_distance (data->a, data->b), ==, data->distance);
      g_assert_cmpint (flatpak_levenshtein_distance (data->b, data->a), ==, data->distance);
    }
}

static GString *g_print_buffer;

static void
my_print_func (const char *string)
{
  g_string_append (g_print_buffer, string);
}

static void
test_format_choices (void)
{
  GPrintFunc print_func;
  const char *choices[] = { "one", "two", "three", NULL };

  g_assert_null (g_print_buffer);
  g_print_buffer = g_string_new ("");
  print_func = g_set_print_handler (my_print_func);

  flatpak_format_choices (choices, "A prompt for %d choices:", 3);

  g_assert_cmpstr (g_print_buffer->str, ==,
                   "A prompt for 3 choices:\n\n"
                   "  1) one\n"
                   "  2) two\n"
                   "  3) three\n"
                   "\n");

  g_set_print_handler (print_func);
  g_string_free (g_print_buffer, TRUE);
  g_print_buffer = NULL;
}

static void
test_yes_no_prompt (void)
{
  GPrintFunc print_func;
  gboolean ret;

  g_assert_null (g_print_buffer);
  g_print_buffer = g_string_new ("");
  print_func = g_set_print_handler (my_print_func);

  /* not a tty, so flatpak_yes_no_prompt will auto-answer 'n' */
  ret = flatpak_yes_no_prompt (TRUE, "Prompt %d ?", 1);
  g_assert_false (ret);
  g_assert_cmpstr (g_print_buffer->str, ==, "Prompt 1 ? [Y/n]: n\n");
  g_string_truncate (g_print_buffer, 0);

  ret = flatpak_yes_no_prompt (FALSE, "Prompt %d ?", 2);
  g_assert_false (ret);
  g_assert_cmpstr (g_print_buffer->str, ==, "Prompt 2 ? [y/n]: n\n");

  g_set_print_handler (print_func);
  g_string_free (g_print_buffer, TRUE);
  g_print_buffer = NULL;
}

static void
test_number_prompt (void)
{
  GPrintFunc print_func;
  gboolean ret;

  g_assert_null (g_print_buffer);
  g_print_buffer = g_string_new ("");
  print_func = g_set_print_handler (my_print_func);

  /* not a tty, so flatpak_number_prompt will auto-answer '0' */
  ret = flatpak_number_prompt (TRUE, 0, 8, "Prompt %d ?", 1);
  g_assert_false (ret);
  g_assert_cmpstr (g_print_buffer->str, ==, "Prompt 1 ? [0-8]: 0\n");
  g_string_truncate (g_print_buffer, 0);

  ret = flatpak_number_prompt (FALSE, 1, 3, "Prompt %d ?", 2);
  g_assert_false (ret);
  g_assert_cmpstr (g_print_buffer->str, ==, "Prompt 2 ? [1-3]: 0\n");

  g_set_print_handler (print_func);
  g_string_free (g_print_buffer, TRUE);
  g_print_buffer = NULL;
}

#if !GLIB_CHECK_VERSION(2, 60, 0)
static gboolean
g_strv_equal (char **strv1,
              char **strv2)
{
  g_return_val_if_fail (strv1 != NULL, FALSE);
  g_return_val_if_fail (strv2 != NULL, FALSE);

  if (strv1 == strv2)
    return TRUE;

  for (; *strv1 != NULL && *strv2 != NULL; strv1++, strv2++)
    {
      if (!g_str_equal (*strv1, *strv2))
        return FALSE;
    }

  return (*strv1 == NULL && *strv2 == NULL);
}
#endif

static void
test_subpaths_merge (void)
{
  char *empty[] = { NULL };
  char *buba[] = { "bu", "ba", NULL };
  char *bla[] = { "bla", "ba", NULL };
  char *bla_sorted[] = { "ba", "bla", NULL };
  char *bubabla[] = { "ba", "bla", "bu", NULL };
  g_auto(GStrv) res = NULL;

  res = flatpak_subpaths_merge (NULL, bla);
  g_assert_true (g_strv_equal (res, bla));
  g_clear_pointer (&res, g_strfreev);
  
  res = flatpak_subpaths_merge (bla, NULL);
  g_assert_true (g_strv_equal (res, bla));
  g_clear_pointer (&res, g_strfreev);
  
  res = flatpak_subpaths_merge (empty, bla);
  g_assert_true (g_strv_equal (res, empty));
  g_clear_pointer (&res, g_strfreev);
  
  res = flatpak_subpaths_merge (bla, empty);
  g_assert_true (g_strv_equal (res, empty));
  g_clear_pointer (&res, g_strfreev);
  
  res = flatpak_subpaths_merge (buba, bla);
  g_assert_true (g_strv_equal (res, bubabla));
  g_clear_pointer (&res, g_strfreev);
  
  res = flatpak_subpaths_merge (bla, buba);
  g_assert_true (g_strv_equal (res, bubabla));
  g_clear_pointer (&res, g_strfreev);

  res = flatpak_subpaths_merge (bla, bla);
  g_assert_true (g_strv_equal (res, bla_sorted));
  g_clear_pointer (&res, g_strfreev);
}

static void
test_lang_from_locale (void)
{
  g_autofree char *lang = NULL;

  lang = flatpak_get_lang_from_locale ("en_US.utf8");
  g_assert_cmpstr (lang, ==, "en");
  g_clear_pointer (&lang, g_free);

  lang = flatpak_get_lang_from_locale ("sv_FI@euro");
  g_assert_cmpstr (lang, ==, "sv");
}

int
main (int argc, char *argv[])
{
  int res;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/common/has-path-prefix", test_has_path_prefix);
  g_test_add_func ("/common/path-match-prefix", test_path_match_prefix);
  g_test_add_func ("/common/fancy-output", test_fancy_output);
  g_test_add_func ("/common/arches", test_arches);
  g_test_add_func ("/common/extension-matches", test_extension_matches);
  g_test_add_func ("/common/valid-name", test_valid_name);
  g_test_add_func ("/common/string-to-unsigned", test_string_to_unsigned);
  g_test_add_func ("/common/levenshtein", test_levenshtein);
  g_test_add_func ("/common/format-choices", test_format_choices);
  g_test_add_func ("/common/yes-no-prompt", test_yes_no_prompt);
  g_test_add_func ("/common/number-prompt", test_number_prompt);
  g_test_add_func ("/common/subpaths-merge", test_subpaths_merge);
  g_test_add_func ("/common/lang-from-locale", test_lang_from_locale);

  res = g_test_run ();

  return res;
}
