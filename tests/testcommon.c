#include "config.h"

#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <glib.h>
#include "flatpak.h"
#include "flatpak-utils-private.h"
#include "flatpak-appdata-private.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-table-printer.h"
#include "parse-datetime.h"

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
  if (!isatty (STDOUT_FILENO))
    g_assert_false (flatpak_fancy_output ()); // no tty
  else
    g_assert_true (flatpak_fancy_output ()); // a tty
  flatpak_enable_fancy_output ();
  g_assert_true (flatpak_fancy_output ());
  flatpak_disable_fancy_output ();
  g_assert_false (flatpak_fancy_output ());
}

static void
test_arches (void)
{
  const char **arches = flatpak_get_arches ();

#if defined(__i386__)
  g_assert_cmpstr (flatpak_get_arch (), ==, "i386");
  g_assert_false (g_strv_contains (arches, "x86_64"));
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
  guint        base;
  gint         min;
  gint         max;
  gint         expected;
  gboolean     should_fail;
} TestData;

const TestData test_data[] = {
  /* typical cases for unsigned */
  { "-1", 10, 0, 2, 0, TRUE  },
  { "1", 10, 0, 2, 1, FALSE },
  { "+1", 10, 0, 2, 0, TRUE  },
  { "0", 10, 0, 2, 0, FALSE },
  { "+0", 10, 0, 2, 0, TRUE  },
  { "-0", 10, 0, 2, 0, TRUE  },
  { "2", 10, 0, 2, 2, FALSE },
  { "+2", 10, 0, 2, 0, TRUE  },
  { "3", 10, 0, 2, 0, TRUE  },
  { "+3", 10, 0, 2, 0, TRUE  },

  /* min == max cases for unsigned */
  { "2", 10, 2, 2, 2, FALSE  },
  { "3", 10, 2, 2, 0, TRUE   },
  { "1", 10, 2, 2, 0, TRUE   },

  /* invalid inputs */
  { "",   10,  0,  2,  0, TRUE },
  { "a",  10,  0,  2,  0, TRUE },
  { "1a", 10,  0,  2,  0, TRUE },

  /* leading/trailing whitespace */
  { " 1", 10,  0,  2,  0, TRUE },
  { "1 ", 10,  0,  2,  0, TRUE },

  /* hexadecimal numbers */
  { "a",    16,   0, 15, 10, FALSE },
  { "0xa",  16,   0, 15,  0, TRUE  },
  { "-0xa", 16,   0, 15,  0, TRUE  },
  { "+0xa", 16,   0, 15,  0, TRUE  },
  { "- 0xa", 16,   0, 15,  0, TRUE  },
  { "+ 0xa", 16,   0, 15,  0, TRUE  },
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

typedef struct
{
  const char *a;
  const char *b;
  int         distance;
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
  const char *many_choices[] = {
    "one", "two", "three", "four", "five", "six",
    "seven", "eight", "nine", "ten", "eleven",
    NULL
  };

  g_assert_null (g_print_buffer);
  g_print_buffer = g_string_new ("");
  print_func = g_set_print_handler (my_print_func);

  flatpak_format_choices (choices, "A prompt for %d choices:", 3);

  g_assert_cmpstr (g_print_buffer->str, ==,
                   "A prompt for 3 choices:\n\n"
                   "   1) one\n"
                   "   2) two\n"
                   "   3) three\n"
                   "\n");

  g_string_truncate (g_print_buffer, 0);

  flatpak_format_choices (many_choices, "A prompt for %d choices:", 11);
  g_assert_cmpstr (g_print_buffer->str, ==,
                   "A prompt for 11 choices:\n\n"
                   "   1) one\n"
                   "   2) two\n"
                   "   3) three\n"
                   "   4) four\n"
                   "   5) five\n"
                   "   6) six\n"
                   "   7) seven\n"
                   "   8) eight\n"
                   "   9) nine\n"
                   "  10) ten\n"
                   "  11) eleven\n"
                   "\n");

  g_string_truncate (g_print_buffer, 0);

  g_set_print_handler (print_func);
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

static void
assert_numbers (int *num, ...)
{
  va_list args;
  int n;
  int i;

  g_assert_nonnull (num);

  va_start (args, num);
  for (i = 0; num[i]; i++)
    {
      n = va_arg (args, int);
      g_assert_true (n == num[i]);
    }

  n = va_arg (args, int);
  g_assert_true (n == 0);
  va_end (args);
}

static void
test_parse_numbers (void)
{
  g_autofree int *numbers = NULL;

  numbers = flatpak_parse_numbers ("", 0, 10);
  assert_numbers (numbers, 0);
  g_clear_pointer (&numbers, g_free);

  numbers = flatpak_parse_numbers ("1", 0, 10);
  assert_numbers (numbers, 1, 0);
  g_clear_pointer (&numbers, g_free);

  numbers = flatpak_parse_numbers ("1 3 2", 0, 10);
  assert_numbers (numbers, 1, 3, 2, 0);
  g_clear_pointer (&numbers, g_free);

  numbers = flatpak_parse_numbers ("1-3", 0, 10);
  assert_numbers (numbers, 1, 2, 3, 0);
  g_clear_pointer (&numbers, g_free);

  numbers = flatpak_parse_numbers ("1", 2, 4);
  g_assert_null (numbers);

  numbers = flatpak_parse_numbers ("2-6", 2, 4);
  g_assert_null (numbers);

  numbers = flatpak_parse_numbers ("1,2 2", 1, 4);
  assert_numbers (numbers, 1, 2, 0);
  g_clear_pointer (&numbers, g_free);

  numbers = flatpak_parse_numbers ("1-3,2-4", 1, 4);
  assert_numbers (numbers, 1, 2, 3, 4, 0);
  g_clear_pointer (&numbers, g_free);

  numbers = flatpak_parse_numbers ("-1", 1, 4);
  g_assert_null (numbers);
}

static void
assert_strv_equal (char **strv1,
                   char **strv2)
{
  if (strv1 == strv2)
    return;

  for (; *strv1 != NULL && *strv2 != NULL; strv1++, strv2++)
    g_assert_true (g_str_equal (*strv1, *strv2));

  g_assert_true (*strv1 == NULL && *strv2 == NULL);
}

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
  assert_strv_equal (res, bla_sorted);
  g_clear_pointer (&res, g_strfreev);

  res = flatpak_subpaths_merge (bla, NULL);
  assert_strv_equal (res, bla_sorted);
  g_clear_pointer (&res, g_strfreev);

  res = flatpak_subpaths_merge (empty, bla);
  assert_strv_equal (res, empty);
  g_clear_pointer (&res, g_strfreev);

  res = flatpak_subpaths_merge (bla, empty);
  assert_strv_equal (res, empty);
  g_clear_pointer (&res, g_strfreev);

  res = flatpak_subpaths_merge (buba, bla);
  assert_strv_equal (res, bubabla);
  g_clear_pointer (&res, g_strfreev);

  res = flatpak_subpaths_merge (bla, buba);
  assert_strv_equal (res, bubabla);
  g_clear_pointer (&res, g_strfreev);

  res = flatpak_subpaths_merge (bla, bla);
  assert_strv_equal (res, bla_sorted);
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

static void
test_parse_appdata (void)
{
  const char appdata1[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<components version=\"0.8\">\n"
    "  <component type=\"desktop\">\n"
    "    <id>org.test.Hello.desktop</id>\n"
    "    <name>Hello world test app: org.test.Hello</name>\n"
    "    <summary>Print a greeting</summary>\n"
    "    <description><p>This is a test app.</p></description>\n"
    "    <categories>\n"
    "      <category>Utility</category>\n"
    "    </categories>\n"
    "    <icon height=\"64\" width=\"64\" type=\"cached\">64x64/org.gnome.gedit.png</icon>\n"
    "    <releases>\n"
    "      <release timestamp=\"1525132800\" version=\"0.0.1\"/>\n" /* 01-05-2018 */
    "    </releases>\n"
    "    <content_rating type=\"oars-1.0\">\n"
    "      <content_attribute id=\"drugs-alcohol\">moderate</content_attribute>\n"
    "      <content_attribute id=\"language-humor\">mild</content_attribute>\n"
    "      <content_attribute id=\"violence-blood\">none</content_attribute>\n"
    "    </content_rating>\n"
    "  </component>\n"
    "</components>";
  const char appdata2[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<components version=\"0.8\">\n"
    "  <component type=\"desktop\">\n"
    "    <id>org.test.Hello.desktop</id>\n"
    "    <name>Hello world test app: org.test.Hello</name>\n"
    "    <name xml:lang=\"de\">Hallo Welt test app: org.test.Hello</name>\n"
    "    <summary>Print a greeting</summary>\n"
    "    <summary xml:lang=\"de\">Schreib mal was</summary>\n"
    "    <description><p>This is a test app.</p></description>\n"
    "    <categories>\n"
    "      <category>Utility</category>\n"
    "    </categories>\n"
    "    <icon height=\"64\" width=\"64\" type=\"cached\">64x64/org.gnome.gedit.png</icon>\n"
    "    <releases>\n"
    "      <release timestamp=\"1525132800\" version=\"0.1.0\"/>\n"
    "      <release timestamp=\"1525000800\" date=\"2018-05-02\" version=\"0.0.2\"/>\n"
    "      <release date=\"2017-05-02\" version=\"0.0.3\"/>\n"
    "      <release timestamp=\"1000000000\" version=\"0.0.1\" type=\"stable\" urgency=\"low\"/>\n"
    "    </releases>\n"
    "    <project_license>anything goes</project_license>\n"
    "    <content_rating type=\"oars-1.1\">\n"
    "    </content_rating>\n"
    "  </component>\n"
    "</components>";
  g_autoptr(GHashTable) names = NULL;
  g_autoptr(GHashTable) comments = NULL;
  g_autofree char *version = NULL;
  g_autofree char *license = NULL;
  g_autofree char *content_rating_type = NULL;
  g_autoptr(GHashTable) content_rating = NULL;
  gboolean res;
  char *name;
  char *comment;

  res = flatpak_parse_appdata (appdata1, "org.test.Hello", &names, &comments, &version, &license, &content_rating_type, &content_rating);
  g_assert_true (res);
  g_assert_cmpstr (version, ==, "0.0.1");
  g_assert_null (license);
  g_assert_nonnull (names);
  g_assert_nonnull (comments);
  g_assert_cmpint (g_hash_table_size (names), ==, 1);
  g_assert_cmpint (g_hash_table_size (comments), ==, 1);
  name = g_hash_table_lookup (names, "C");
  g_assert_cmpstr (name, ==, "Hello world test app: org.test.Hello");
  comment = g_hash_table_lookup (comments, "C");
  g_assert_cmpstr (comment, ==, "Print a greeting");
  g_assert_cmpstr (content_rating_type, ==, "oars-1.0");
  g_assert_cmpuint (g_hash_table_size (content_rating), ==, 3);
  g_assert_cmpstr (g_hash_table_lookup (content_rating, "drugs-alcohol"), ==, "moderate");
  g_assert_cmpstr (g_hash_table_lookup (content_rating, "language-humor"), ==, "mild");
  g_assert_cmpstr (g_hash_table_lookup (content_rating, "violence-blood"), ==, "none");

  g_clear_pointer (&names, g_hash_table_unref);
  g_clear_pointer (&comments, g_hash_table_unref);
  g_clear_pointer (&version, g_free);
  g_clear_pointer (&license, g_free);
  g_clear_pointer (&content_rating_type, g_free);
  g_clear_pointer (&content_rating, g_hash_table_unref);

  res = flatpak_parse_appdata (appdata2, "org.test.Hello", &names, &comments, &version, &license, &content_rating_type, &content_rating);
  g_assert_true (res);
  g_assert_cmpstr (version, ==, "0.1.0");
  g_assert_cmpstr (license, ==, "anything goes");
  g_assert_nonnull (names);
  g_assert_nonnull (comments);
  g_assert_cmpint (g_hash_table_size (names), ==, 2);
  g_assert_cmpint (g_hash_table_size (comments), ==, 2);
  name = g_hash_table_lookup (names, "C");
  g_assert_cmpstr (name, ==, "Hello world test app: org.test.Hello");
  name = g_hash_table_lookup (names, "de");
  g_assert_cmpstr (name, ==, "Hallo Welt test app: org.test.Hello");
  comment = g_hash_table_lookup (comments, "C");
  g_assert_cmpstr (comment, ==, "Print a greeting");
  comment = g_hash_table_lookup (comments, "de");
  g_assert_cmpstr (comment, ==, "Schreib mal was");
  g_assert_cmpstr (content_rating_type, ==, "oars-1.1");
  g_assert_cmpuint (g_hash_table_size (content_rating), ==, 0);
}

static void
test_name_matching (void)
{
  gboolean res;

  /* examples from 8f428fd7683765dd706da06e9f376d3732ce5c0c */

  res = flatpak_name_matches_one_wildcard_prefix ("org.sparkleshare.SparkleShare.Invites",
                                                  (const char *[]){"org.sparkleshare.SparkleShare.*", NULL},
                                                  FALSE);
  g_assert_true (res);

  res = flatpak_name_matches_one_wildcard_prefix ("org.sparkleshare.SparkleShare-symbolic",
                                                  (const char *[]){"org.sparkleshare.SparkleShare.*", NULL},
                                                  FALSE);
  g_assert_true (res);

  res = flatpak_name_matches_one_wildcard_prefix ("org.libreoffice.LibreOffice",
                                                  (const char *[]){"org.libreoffice.LibreOffice.*", NULL},
                                                  FALSE);
  g_assert_true (res);

  res = flatpak_name_matches_one_wildcard_prefix ("org.libreoffice.LibreOffice-impress",
                                                  (const char *[]){"org.libreoffice.LibreOffice.*", NULL},
                                                  FALSE);
  g_assert_true (res);

  res = flatpak_name_matches_one_wildcard_prefix ("org.libreoffice.LibreOffice-writer",
                                                  (const char *[]){"org.libreoffice.LibreOffice.*", NULL},
                                                  FALSE);
  g_assert_true (res);

  res = flatpak_name_matches_one_wildcard_prefix ("org.libreoffice.LibreOffice-calc",
                                                  (const char *[]){"org.libreoffice.LibreOffice.*", NULL},
                                                  FALSE);
  g_assert_true (res);

  res = flatpak_name_matches_one_wildcard_prefix ("com.github.bajoja.indicator-kdeconnect",
                                                  (const char *[]){"com.github.bajoja.indicator-kdeconnect.*", NULL},
                                                  FALSE);
  g_assert_true (res);

  res = flatpak_name_matches_one_wildcard_prefix ("com.github.bajoja.indicator-kdeconnect.settings",
                                                  (const char *[]){"com.github.bajoja.indicator-kdeconnect.*", NULL},
                                                  FALSE);
  g_assert_true (res);

  res = flatpak_name_matches_one_wildcard_prefix ("com.github.bajoja.indicator-kdeconnect.tablettrusted",
                                                  (const char *[]){"com.github.bajoja.indicator-kdeconnect.*", NULL},
                                                  FALSE);
  g_assert_true (res);

  res = flatpak_name_matches_one_wildcard_prefix ("org.gnome.Characters.BackgroundService",
                                                  (const char *[]){"org.gnome.Characters.*", NULL},
                                                  TRUE);
  g_assert_true (res);

  res = flatpak_name_matches_one_wildcard_prefix ("org.example.Example.Tracker1.Miner.Applications",
                                                  (const char *[]){"org.example.Example.*", NULL},
                                                  TRUE);
  g_assert_true (res);
}

static void
test_looks_like_branch (void)
{
  g_assert_false (looks_like_branch ("abc/d"));
  g_assert_false (looks_like_branch ("ab.c.d"));
  g_assert_true (looks_like_branch ("master"));
  g_assert_true (looks_like_branch ("stable"));
  g_assert_true (looks_like_branch ("3.30"));
}

static void
test_columns (void)
{
  Column columns[] = {
    { "column1", "col1", "col1",       0, 0, 1, 1 },
    { "install", "install", "install", 0, 0, 0, 1 },
    { "helper", "helper", "helper",    0, 0, 1, 0 },
    { "column2", "col2", "col2",       0, 0, 0, 0 },
    { NULL, }
  };
  Column *cols;
  g_autofree char *help = NULL;
  g_autoptr(GError) error = NULL;
  const char *args[3];

  help = column_help (columns);
  g_assert_cmpstr (help, ==,
                   "Available columns:\n"
                   "  column1     col1\n"
                   "  install     install\n"
                   "  helper      helper\n"
                   "  column2     col2\n"
                   "  all         Show all columns\n"
                   "  help        Show available columns\n"
                   "\n"
                   "Append :s[tart], :m[iddle], :e[nd] or :f[ull] to change ellipsization\n");

  cols = handle_column_args (columns, FALSE, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (cols[0].name, ==, "column1");
  g_assert_cmpstr (cols[1].name, ==, "install");
  g_assert_null (cols[2].name);
  g_free (cols);

  cols = handle_column_args (columns, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (cols[0].name, ==, "column1");
  g_assert_cmpstr (cols[1].name, ==, "install");
  g_assert_cmpstr (cols[2].name, ==, "helper");
  g_assert_null (cols[3].name);
  g_free (cols);

  args[0] = "all";
  args[1] = NULL;
  cols = handle_column_args (columns, FALSE, args, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (cols[0].name, ==, "column1");
  g_assert_cmpstr (cols[1].name, ==, "install");
  g_assert_cmpstr (cols[2].name, ==, "helper");
  g_assert_null (cols[3].name);
  g_free (cols);

  args[0] = "column1,column2";
  args[1] = "helper";
  args[2] = NULL;
  cols = handle_column_args (columns, FALSE, args, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (cols[0].name, ==, "column1");
  g_assert_cmpstr (cols[1].name, ==, "column2");
  g_assert_cmpstr (cols[2].name, ==, "helper");
  g_assert_null (cols[3].name);
  g_free (cols);

  args[0] = "column";
  args[1] = NULL;
  cols = handle_column_args (columns, FALSE, args, &error);
  g_assert_null (cols);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  args[0] = "app";
  args[1] = NULL;
  cols = handle_column_args (columns, FALSE, args, &error);
  g_assert_null (cols);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);
}

typedef struct
{
  const char          *in;
  int                  len;
  FlatpakEllipsizeMode mode;
  const char          *out;
} EllipsizeData;

static EllipsizeData ellipsize[] = {
  { "abcdefghijklmnopqrstuvwxyz", 10, FLATPAK_ELLIPSIZE_MODE_NONE,   "abcdefghijklmnopqrstuvwxyz" },
  { "abcdefghijklmnopqrstuvwxyz", 10, FLATPAK_ELLIPSIZE_MODE_END,    "abcdefghi…" },
  { "abcdefghijklmnopqrstuvwxyz", 10, FLATPAK_ELLIPSIZE_MODE_MIDDLE, "abcde…wxyz" },
  { "abcdefghijklmnopqrstuvwxyz", 10, FLATPAK_ELLIPSIZE_MODE_START,  "…rstuvwxyz" },
  { "ģ☢ab", 3, FLATPAK_ELLIPSIZE_MODE_START,  "…ab" },
  { "ģ☢ab", 3, FLATPAK_ELLIPSIZE_MODE_MIDDLE, "ģ…b" },
  { "ģ☢ab", 3, FLATPAK_ELLIPSIZE_MODE_END,    "ģ☢…" }
};

static void
test_string_ellipsize (void)
{
  gsize idx;

  for (idx = 0; idx < G_N_ELEMENTS (ellipsize); idx++)
    {
      EllipsizeData *data = &ellipsize[idx];
      g_autofree char *ret = NULL;

      ret = ellipsize_string_full (data->in, data->len, data->mode);
      g_assert_cmpstr (ret, ==, data->out);
    }
}

static void
test_table (void)
{
  GPrintFunc print_func;
  FlatpakTablePrinter *printer;

  g_assert_null (g_print_buffer);
  g_print_buffer = g_string_new ("");
  print_func = g_set_print_handler (my_print_func);
  flatpak_enable_fancy_output ();

  printer = flatpak_table_printer_new ();

  flatpak_table_printer_set_column_title (printer, 0, "Column1");
  flatpak_table_printer_set_column_title (printer, 1, "Column2");

  flatpak_table_printer_add_column (printer, "text1");
  flatpak_table_printer_add_column (printer, "text2");
  flatpak_table_printer_finish_row (printer);

  flatpak_table_printer_add_column (printer, "text3");
  flatpak_table_printer_add_column (printer, "text4");
  flatpak_table_printer_finish_row (printer);

  flatpak_table_printer_print (printer);
  g_assert_cmpstr (g_print_buffer->str, ==,
                   FLATPAK_ANSI_BOLD_ON
                   "Column1 Column2" FLATPAK_ANSI_BOLD_OFF "\n"
                   "text1   text2\n"
                   "text3   text4\n");
  g_string_truncate (g_print_buffer, 0);

  flatpak_table_printer_set_cell (printer, 0, 0, "newtext1");
  flatpak_table_printer_set_decimal_cell (printer, 0, 1, "0.123");
  flatpak_table_printer_set_decimal_cell (printer, 1, 1, "123.0");
  flatpak_table_printer_print (printer);
  g_assert_cmpstr (g_print_buffer->str, ==,
                   FLATPAK_ANSI_BOLD_ON
                   "Column1  Column2" FLATPAK_ANSI_BOLD_OFF "\n"
                   "newtext1   0.123\n"
                   "text3    123.0\n");
  g_string_truncate (g_print_buffer, 0);

  flatpak_table_printer_free (printer);

  flatpak_disable_fancy_output ();
  g_set_print_handler (print_func);
  g_string_free (g_print_buffer, TRUE);
  g_print_buffer = NULL;
}

static void
test_table_expand (void)
{
  GPrintFunc print_func;
  FlatpakTablePrinter *printer;
  int rows, cols;

  g_assert_null (g_print_buffer);
  g_print_buffer = g_string_new ("");
  print_func = g_set_print_handler (my_print_func);
  flatpak_enable_fancy_output ();

  printer = flatpak_table_printer_new ();

  flatpak_table_printer_set_column_title (printer, 0, "Column1");
  flatpak_table_printer_set_column_title (printer, 1, "Column2");
  flatpak_table_printer_set_column_title (printer, 2, "Column3");

  flatpak_table_printer_add_column (printer, "text1");
  flatpak_table_printer_add_column (printer, "text2");
  flatpak_table_printer_add_column (printer, "text3");
  flatpak_table_printer_finish_row (printer);
  flatpak_table_printer_add_span (printer, "012345678901234567890234567890123456789");
  flatpak_table_printer_finish_row (printer);

  flatpak_table_printer_set_column_expand (printer, 0, TRUE);

  flatpak_table_printer_print_full (printer, 0, 40, &rows, &cols);

  g_assert_cmpint (rows, ==, 3);
  g_assert_cmpint (cols, ==, 34);
  g_assert_cmpstr (g_print_buffer->str, ==,
                   FLATPAK_ANSI_BOLD_ON
                   "Column1            Column2 Column3" FLATPAK_ANSI_BOLD_OFF "\n"
                   "text1              text2   text3" "\n"
                   "012345678901234567890234567890123456789");
  g_string_truncate (g_print_buffer, 0);

  flatpak_table_printer_set_column_expand (printer, 2, TRUE);

  flatpak_table_printer_print_full (printer, 0, 40, &rows, &cols);

  g_assert_cmpint (rows, ==, 3);
  g_assert_cmpint (cols, ==, 34);
  g_assert_cmpstr (g_print_buffer->str, ==,
                   FLATPAK_ANSI_BOLD_ON
                   "Column1       Column2 Column3" FLATPAK_ANSI_BOLD_OFF "\n"
                   "text1         text2   text3" "\n"
                   "012345678901234567890234567890123456789");
  g_string_truncate (g_print_buffer, 0);

  flatpak_table_printer_free (printer);

  flatpak_disable_fancy_output ();
  g_set_print_handler (print_func);
  g_string_free (g_print_buffer, TRUE);
  g_print_buffer = NULL;
}

static void
test_table_shrink (void)
{
  GPrintFunc print_func;
  FlatpakTablePrinter *printer;
  int rows, cols;

  g_assert_null (g_print_buffer);
  g_print_buffer = g_string_new ("");
  print_func = g_set_print_handler (my_print_func);
  flatpak_enable_fancy_output ();

  printer = flatpak_table_printer_new ();

  flatpak_table_printer_set_column_title (printer, 0, "Column1");
  flatpak_table_printer_set_column_title (printer, 1, "Column2");
  flatpak_table_printer_set_column_title (printer, 2, "Column3");

  flatpak_table_printer_add_column (printer, "a very long text");
  flatpak_table_printer_add_column (printer, "text2");
  flatpak_table_printer_add_column (printer, "long text too");
  flatpak_table_printer_finish_row (printer);

  flatpak_table_printer_add_column (printer, "short");
  flatpak_table_printer_add_column (printer, "short");
  flatpak_table_printer_add_column (printer, "short");
  flatpak_table_printer_finish_row (printer);

  flatpak_table_printer_add_span (printer, "0123456789012345678902345");
  flatpak_table_printer_finish_row (printer);

  flatpak_table_printer_set_column_ellipsize (printer, 0, FLATPAK_ELLIPSIZE_MODE_END);

  flatpak_table_printer_print_full (printer, 0, 25, &rows, &cols);

  g_assert_cmpint (rows, ==, 4);
  g_assert_cmpint (cols, ==, 25);
  g_assert_cmpstr (g_print_buffer->str, ==,
                   FLATPAK_ANSI_BOLD_ON
                   "Co… Column2 Column3" FLATPAK_ANSI_BOLD_OFF "\n"
                   "a … text2   long text too" "\n"
                   "sh… short   short" "\n"
                   "0123456789012345678902345");
  g_string_truncate (g_print_buffer, 0);

  flatpak_table_printer_set_column_ellipsize (printer, 2, FLATPAK_ELLIPSIZE_MODE_MIDDLE);

  flatpak_table_printer_print_full (printer, 0, 25, &rows, &cols);

  g_assert_cmpint (rows, ==, 4);
  g_assert_cmpint (cols, ==, 25);
  g_assert_cmpstr (g_print_buffer->str, ==,
                   FLATPAK_ANSI_BOLD_ON
                   "Column1  Column2 Column3" FLATPAK_ANSI_BOLD_OFF "\n"
                   "a very … text2   long…too" "\n"
                   "short    short   short" "\n"
                                                                                                                         "0123456789012345678902345");
  g_string_truncate (g_print_buffer, 0);

  flatpak_table_printer_free (printer);

  flatpak_disable_fancy_output ();
  g_set_print_handler (print_func);
  g_string_free (g_print_buffer, TRUE);
  g_print_buffer = NULL;
}

static void
test_table_shrink_more (void)
{
  GPrintFunc print_func;
  FlatpakTablePrinter *printer;
  int rows, cols;

  g_assert_null (g_print_buffer);
  g_print_buffer = g_string_new ("");
  print_func = g_set_print_handler (my_print_func);
  flatpak_enable_fancy_output ();

  printer = flatpak_table_printer_new ();

  flatpak_table_printer_set_column_title (printer, 0, "Column1");
  flatpak_table_printer_set_column_title (printer, 1, "Column2");
  flatpak_table_printer_set_column_title (printer, 2, "Column3");

  flatpak_table_printer_add_column (printer, "a very long text");
  flatpak_table_printer_add_column (printer, "midsize text");
  flatpak_table_printer_add_column (printer, "another very long text");
  flatpak_table_printer_finish_row (printer);

  flatpak_table_printer_set_column_ellipsize (printer, 1, FLATPAK_ELLIPSIZE_MODE_END);

  flatpak_table_printer_print_full (printer, 0, 25, &rows, &cols);

  g_assert_cmpint (rows, ==, 4);
  g_assert_cmpint (cols, ==, 40);
  g_assert_cmpstr (g_print_buffer->str, ==,
                   FLATPAK_ANSI_BOLD_ON
                   "Column1          … Column3" FLATPAK_ANSI_BOLD_OFF "\n"
                   "a very long text … another very long text");

  flatpak_table_printer_free (printer);

  flatpak_disable_fancy_output ();
  g_set_print_handler (print_func);
  g_string_free (g_print_buffer, TRUE);
  g_print_buffer = NULL;
}

static void
test_parse_datetime (void)
{
  struct timespec ts;
  struct timespec now;
  gboolean ret;
  g_autoptr(GDateTime) dt = NULL;
  GTimeVal tv;

  clock_gettime (CLOCK_REALTIME, &now);
  ret = parse_datetime (&ts, "NOW", NULL);
  g_assert (ret);

  g_assert_true (ts.tv_sec == now.tv_sec); // close enough

  ret = parse_datetime (&ts, "2018-10-29 00:19:07 +0000", NULL);
  dt = g_date_time_new_utc (2018, 10, 29, 0, 19, 7);
  g_date_time_to_timeval (dt, &tv);

  g_assert_true (tv.tv_sec == ts.tv_sec &&
                 tv.tv_usec == ts.tv_nsec / 1000);

  ret = parse_datetime (&ts, "nonsense", NULL);
  g_assert_false (ret);
}

/* Test various syntax errors */
static void
test_filter_parser (void)
{
  struct {
    char *filter;
    guint expected_error;
  } filters[] = {
    {
     "foobar",
     FLATPAK_ERROR_INVALID_DATA
    },
    {
     "foobar *",
     FLATPAK_ERROR_INVALID_DATA
    },
    {
     "deny",
     FLATPAK_ERROR_INVALID_DATA
    },
    {
     "deny 23+123",
     FLATPAK_ERROR_INVALID_DATA
    },
    {
     "deny *\n"
     "allow",
     FLATPAK_ERROR_INVALID_DATA
    },
    {
     "deny *\n"
     "allow org.foo.bar extra\n",
     FLATPAK_ERROR_INVALID_DATA
    }
  };
  gboolean ret;
  int i;

  for (i = 0; i < G_N_ELEMENTS(filters); i++)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GRegex) allow_refs = NULL;
      g_autoptr(GRegex) deny_refs = NULL;

      ret = flatpak_parse_filters (filters[i].filter, &allow_refs, &deny_refs, &error);
      g_assert_error (error, FLATPAK_ERROR, filters[i].expected_error);
      g_assert (ret == FALSE);
      g_assert (allow_refs == NULL);
      g_assert (deny_refs == NULL);
    }
}

static void
test_filter (void)
{
  GError *error = NULL;
  g_autoptr(GRegex) allow_refs = NULL;
  g_autoptr(GRegex) deny_refs = NULL;
  gboolean ret;
  int i;
  char *filter =
    " # This is a comment\n"
    "\tallow\t org.foo.*#comment\n"
    "  deny   org.*   # Comment\n"
    "  deny   com.*   # Comment\n"
    " # another comment\n"
    "allow com.foo.bar\n"
    "allow app/com.bar.foo*/*/stable\n"
    "allow app/com.armed.foo*/arm\n"
    "allow runtime/com.gazonk\n"
    "allow runtime/com.gazonk.*\t#comment*a*"; /* Note: lack of last newline to test */
  struct {
    char *ref;
    gboolean expected_result;
  } filter_refs[] = {
     /* General denies (org/com)*/
     { "app/org.filter.this/x86_64/stable", FALSE },
     { "app/com.filter.this/arm/stable", FALSE },
     /* But net. not denied */
     { "app/net.dont.filter.this/x86_64/stable", TRUE },
     { "runtime/net.dont.filter.this/x86_64/1.0", TRUE },

     /* Special allow overrides */

     /* allow com.foo.bar */
     { "app/com.foo.bar/x86_64/stable", TRUE },
     { "app/com.foo.bar/arm/foo", TRUE },
     { "runtime/com.foo.bar/x86_64/1.0", TRUE },

     /* allow app/com.bar.foo* / * /stable */
     { "app/com.bar.foo/x86_64/stable", TRUE },
     { "app/com.bar.foo/arm/stable", TRUE },
     { "app/com.bar.foobar/x86_64/stable", TRUE },
     { "app/com.bar.foobar/arm/stable", TRUE },
     { "app/com.bar.foo.bar/x86_64/stable", TRUE },
     { "app/com.bar.foo.bar/arm/stable", TRUE },
     { "app/com.bar.foo/x86_64/unstable", FALSE },
     { "app/com.bar.foobar/x86_64/unstable", FALSE },
     { "runtime/com.bar.foo/x86_64/stable", FALSE },

     /* allow app/com.armed.foo* /arm */
     { "app/com.armed.foo/arm/stable", TRUE },
     { "app/com.armed.foo/arm/unstable", TRUE },
     { "app/com.armed.foo/x86_64/stable", FALSE },
     { "app/com.armed.foo/x86_64/unstable", FALSE },
     { "app/com.armed.foobar/arm/stable", TRUE },
     { "app/com.armed.foobar/arm/unstable", TRUE },
     { "app/com.armed.foobar/x86_64/stable", FALSE },
     { "app/com.armed.foobar/x86_64/unstable", FALSE },
     { "runtime/com.armed.foo/arm/stable", FALSE },
     { "runtime/com.armed.foobar/arm/stable", FALSE },
     { "runtime/com.armed.foo/x86_64/stable", FALSE },
     { "runtime/com.armed.foobar/x86_64/stable", FALSE },

     /* allow runtime/com.gazonk */
     /* allow runtime/com.gazonk.* */
     { "runtime/com.gazonk/x86_64/1.0", TRUE },
     { "runtime/com.gazonk.Locale/x86_64/1.0", TRUE },
     { "runtime/com.gazonked/x86_64/1.0", FALSE },
     { "runtime/com.gazonk/arm/1.0", TRUE },
     { "runtime/com.gazonk.Locale/arm/1.0", TRUE },
     { "app/com.gazonk/x86_64/stable", FALSE },
     { "app/com.gazonk.Locale/x86_64/stable", FALSE },

  };

  ret = flatpak_parse_filters (filter, &allow_refs, &deny_refs, &error);
  g_assert_no_error (error);
  g_assert (ret == TRUE);

  g_assert (allow_refs != NULL);
  g_assert (deny_refs != NULL);

  for (i = 0; i < G_N_ELEMENTS(filter_refs); i++)
    g_assert_cmpint (flatpak_filters_allow_ref (allow_refs, deny_refs, filter_refs[i].ref), ==, filter_refs[i].expected_result);
}

static void
test_dconf_app_id (void)
{
  struct {
    const char *app_id;
    const char *path;
  } tests[] = {
    { "org.gnome.Builder", "/org/gnome/Builder/" },
    { "org.gnome.builder", "/org/gnome/builder/" },
    { "org.gnome.builder-2", "/org/gnome/builder-2/" },
  };
  int i;

  for (i = 0; i < G_N_ELEMENTS (tests); i++)
    {
      g_autofree char *path = NULL;

      path = flatpak_dconf_path_for_app_id (tests[i].app_id);
      g_assert_cmpstr (path, ==, tests[i].path);
    }
}

static void
test_dconf_paths (void)
{
  struct {
    const char *path1;
    const char *path2;
    gboolean result;
  } tests[] = {
    { "/org/gnome/Builder/", "/org/gnome/builder/", 1 },
    { "/org/gnome/Builder-2/", "/org/gnome/Builder_2/", 1 },
    { "/org/gnome/Builder/", "/org/gnome/Builder", 0 },
    { "/org/gnome/Builder/", "/org/gnome/Buildex/", 0 },
  };
  int i;

  for (i = 0; i < G_N_ELEMENTS (tests); i++)
    {
      gboolean result;

      result = flatpak_dconf_path_is_similar (tests[i].path1, tests[i].path2);
      g_assert_cmpint (result, ==, tests[i].result);
    }
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
  g_test_add_func ("/common/parse-numbers", test_parse_numbers);
  g_test_add_func ("/common/subpaths-merge", test_subpaths_merge);
  g_test_add_func ("/common/lang-from-locale", test_lang_from_locale);
  g_test_add_func ("/common/appdata", test_parse_appdata);
  g_test_add_func ("/common/name-matching", test_name_matching);
  g_test_add_func ("/common/filter_parser", test_filter_parser);
  g_test_add_func ("/common/filter", test_filter);
  g_test_add_func ("/common/dconf-app-id", test_dconf_app_id);
  g_test_add_func ("/common/dconf-paths", test_dconf_paths);

  g_test_add_func ("/app/looks-like-branch", test_looks_like_branch);
  g_test_add_func ("/app/columns", test_columns);
  g_test_add_func ("/app/string-ellipsize", test_string_ellipsize);
  g_test_add_func ("/app/table", test_table);
  g_test_add_func ("/app/table-expand", test_table_expand);
  g_test_add_func ("/app/table-shrink", test_table_shrink);
  g_test_add_func ("/app/table-shrink-more", test_table_shrink_more);
  g_test_add_func ("/app/parse-datetime", test_parse_datetime);

  res = g_test_run ();

  return res;
}
