#include "config.h"

#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <glib.h>
#include "flatpak.h"
#include "flatpak-utils-private.h"
#include "flatpak-appdata-private.h"
#include "flatpak-run-private.h"
#include "flatpak-run-x11-private.h"

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
  g_assert_false (flatpak_is_valid_name ("", -1, NULL));
  g_assert_false (flatpak_is_valid_name ("org", -1, NULL));
  g_assert_false (flatpak_is_valid_name ("org.", -1, NULL));
  g_assert_false (flatpak_is_valid_name ("org..", -1, NULL));
  g_assert_false (flatpak_is_valid_name ("org..test", -1, NULL));
  g_assert_false (flatpak_is_valid_name ("org.flatpak", -1, NULL));
  g_assert_false (flatpak_is_valid_name ("org.1flatpak.test", -1, NULL));
  g_assert_false (flatpak_is_valid_name ("org.flat-pak.test", -1, NULL));
  g_assert_false (flatpak_is_valid_name ("org.-flatpak.test", -1, NULL));
  g_assert_false (flatpak_is_valid_name ("org.flat,pak.test", -1, NULL));
  g_assert_false (flatpak_is_valid_name ("org.flatpak.test", 0, NULL));
  g_assert_false (flatpak_is_valid_name ("org.flatpak.test", 3, NULL));
  g_assert_false (flatpak_is_valid_name ("org.flatpak.test", 4, NULL));

  g_assert_true (flatpak_is_valid_name ("org.flatpak.test", -1, NULL));
  g_assert_true (flatpak_is_valid_name ("org.flatpak.test", strlen("org.flatpak.test"), NULL));
  g_assert_true (flatpak_is_valid_name ("org.FlatPak.TEST", -1, NULL));
  g_assert_true (flatpak_is_valid_name ("org0.f1atpak.test", -1, NULL));
  g_assert_true (flatpak_is_valid_name ("org.flatpak.-test", -1, NULL));
  g_assert_true (flatpak_is_valid_name ("org.flatpak._test", -1, NULL));
  g_assert_true (flatpak_is_valid_name ("org.flat_pak__.te--st", -1, NULL));
}

static void
test_decompose (void)
{
  g_autoptr(FlatpakDecomposed) app_ref = NULL;
  g_autoptr(FlatpakDecomposed) runtime_ref = NULL;
  g_autoptr(FlatpakDecomposed) refspec = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *app_id = NULL;
  g_autofree char *app_arch = NULL;
  g_autofree char *app_branch = NULL;
  g_autofree char *runtime_id = NULL;
  g_autofree char *runtime_arch = NULL;
  g_autofree char *runtime_branch = NULL;
  gsize len, len2;

  g_assert_null (flatpak_decomposed_new_from_ref ("app/wrong/mips64/master", &error));
  g_assert (error != NULL);
  g_assert (error->domain == FLATPAK_ERROR);
  g_assert (error->code == FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  g_assert_null (flatpak_decomposed_new_from_ref ("app/org.the.app//master", &error));
  g_assert (error != NULL);
  g_assert (error->domain == FLATPAK_ERROR);
  g_assert (error->code == FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  g_assert_null (flatpak_decomposed_new_from_ref ("app/org.the.app/mips64/@foo", &error));
  g_assert (error != NULL);
  g_assert (error->domain == FLATPAK_ERROR);
  g_assert (error->code == FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  g_assert_null (flatpak_decomposed_new_from_ref ("wrong/org.the.wrong/mips64/master", &error));
  g_assert (error != NULL);
  g_assert (error->domain == FLATPAK_ERROR);
  g_assert (error->code == FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  g_assert_null (flatpak_decomposed_new_from_ref ("app/org.the.app/mips64/master/extra", &error));
  g_assert (error != NULL);
  g_assert (error->domain == FLATPAK_ERROR);
  g_assert (error->code == FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  g_assert_null (flatpak_decomposed_new_from_ref ("app/org.the.app/mips64", &error));
  g_assert (error != NULL);
  g_assert (error->domain == FLATPAK_ERROR);
  g_assert (error->code == FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  runtime_ref = flatpak_decomposed_new_from_ref ("runtime/org.the.runtime/mips64/master", &error);
  g_assert (runtime_ref != NULL);
  g_assert_null (error);

  g_assert_cmpstr (flatpak_decomposed_get_ref (runtime_ref), ==, "runtime/org.the.runtime/mips64/master");
  g_assert_cmpstr (flatpak_decomposed_get_refspec (runtime_ref), ==, "runtime/org.the.runtime/mips64/master");
  g_assert (flatpak_decomposed_equal (runtime_ref, runtime_ref));
  g_assert (flatpak_decomposed_hash (runtime_ref) == g_str_hash ("runtime/org.the.runtime/mips64/master"));
  g_assert (!flatpak_decomposed_is_app (runtime_ref));
  g_assert (flatpak_decomposed_is_runtime (runtime_ref));
  g_assert (flatpak_decomposed_get_kinds (runtime_ref) == FLATPAK_KINDS_RUNTIME);
  g_assert (flatpak_decomposed_get_kind (runtime_ref) == FLATPAK_REF_KIND_RUNTIME);

  g_assert_cmpstr (flatpak_decomposed_peek_id (runtime_ref, &len), ==, "org.the.runtime/mips64/master");
  g_assert (len == strlen("org.the.runtime"));
  runtime_id = flatpak_decomposed_dup_id (runtime_ref);
  g_assert_cmpstr (runtime_id, ==, "org.the.runtime");
  g_assert (flatpak_decomposed_is_id (runtime_ref, "org.the.runtime"));
  g_assert (!flatpak_decomposed_is_id (runtime_ref, "org.the.runtim"));
  g_assert (!flatpak_decomposed_is_id (runtime_ref, "org.the.runtimee"));

  g_assert_cmpstr (flatpak_decomposed_peek_arch (runtime_ref, &len), ==, "mips64/master");
  g_assert (len == strlen ("mips64"));
  runtime_arch = flatpak_decomposed_dup_arch (runtime_ref);
  g_assert_cmpstr (runtime_arch, ==, "mips64");
  g_assert (flatpak_decomposed_is_arch (runtime_ref, "mips64"));
  g_assert (!flatpak_decomposed_is_arch (runtime_ref, "mips6"));
  g_assert (!flatpak_decomposed_is_arch (runtime_ref, "mips644"));

  g_assert_cmpstr (flatpak_decomposed_peek_branch (runtime_ref, &len), ==, "master");
  g_assert (len == strlen ("master"));
  runtime_branch = flatpak_decomposed_dup_branch (runtime_ref);
  g_assert_cmpstr (runtime_branch, ==, "master");
  g_assert (flatpak_decomposed_is_branch (runtime_ref, "master"));
  g_assert (!flatpak_decomposed_is_arch (runtime_ref, "maste"));
  g_assert (!flatpak_decomposed_is_arch (runtime_ref, "masterr"));

  app_ref = flatpak_decomposed_new_from_ref ("app/org.the.app/mips64/master", &error);
  g_assert (app_ref != NULL);
  g_assert_null (error);

  g_assert_cmpstr (flatpak_decomposed_get_ref (app_ref), ==, "app/org.the.app/mips64/master");
  g_assert_cmpstr (flatpak_decomposed_get_refspec (app_ref), ==, "app/org.the.app/mips64/master");
  g_assert (flatpak_decomposed_equal (app_ref, app_ref));
  g_assert (!flatpak_decomposed_equal (app_ref, runtime_ref));
  g_assert (flatpak_decomposed_hash (app_ref) == g_str_hash ("app/org.the.app/mips64/master"));
  g_assert (flatpak_decomposed_is_app (app_ref));
  g_assert (!flatpak_decomposed_is_runtime (app_ref));
  g_assert (flatpak_decomposed_get_kinds (app_ref) == FLATPAK_KINDS_APP);
  g_assert (flatpak_decomposed_get_kind (app_ref) == FLATPAK_REF_KIND_APP);

  g_assert_cmpstr (flatpak_decomposed_peek_id (app_ref, &len), ==, "org.the.app/mips64/master");
  g_assert (len == strlen ("org.the.app"));
  app_id = flatpak_decomposed_dup_id (app_ref);
  g_assert_cmpstr (app_id, ==, "org.the.app");
  g_assert (flatpak_decomposed_is_id (app_ref, "org.the.app"));
  g_assert (!flatpak_decomposed_is_id (app_ref, "org.the.ap"));
  g_assert (!flatpak_decomposed_is_id (app_ref, "org.the.appp"));

  g_assert_cmpstr (flatpak_decomposed_peek_arch (app_ref, &len), ==, "mips64/master");
  g_assert (len == strlen ("mips64"));
  app_arch = flatpak_decomposed_dup_arch (app_ref);
  g_assert_cmpstr (app_arch, ==, "mips64");
  g_assert (flatpak_decomposed_is_arch (app_ref, "mips64"));
  g_assert (!flatpak_decomposed_is_arch (app_ref, "mips6"));
  g_assert (!flatpak_decomposed_is_arch (app_ref, "mips644"));

  g_assert_cmpstr (flatpak_decomposed_get_branch (app_ref), ==, "master");
  g_assert_cmpstr (flatpak_decomposed_peek_branch (app_ref, &len), ==, "master");
  g_assert (len == strlen ("master"));
  app_branch = flatpak_decomposed_dup_branch (app_ref);
  g_assert_cmpstr (app_branch, ==, "master");
  g_assert (flatpak_decomposed_is_branch (app_ref, "master"));
  g_assert (!flatpak_decomposed_is_arch (app_ref, "maste"));
  g_assert (!flatpak_decomposed_is_arch (app_ref, "masterr"));

  refspec = flatpak_decomposed_new_from_ref ("remote:app/org.the.app/mips64/master", &error);
  g_assert (refspec == NULL);
  g_assert (error != NULL);
  g_assert (error->domain == FLATPAK_ERROR);
  g_assert (error->code == FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  refspec = flatpak_decomposed_new_from_refspec ("remote/broken:app/org.the.app/mips64/master", &error);
  g_assert (refspec == NULL);
  g_assert (error != NULL);
  g_assert (error->domain == FLATPAK_ERROR);
  g_assert (error->code == FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  refspec = flatpak_decomposed_new_from_refspec ("remote:app/org.the.app/mips64/master", &error);
  g_assert (refspec != NULL);
  g_assert_null (error);

  g_assert_cmpstr (flatpak_decomposed_get_ref (refspec), ==, "app/org.the.app/mips64/master");
  g_assert_cmpstr (flatpak_decomposed_get_refspec (refspec), ==, "remote:app/org.the.app/mips64/master");
  g_autofree char *refspec_remote = flatpak_decomposed_dup_remote (refspec);
  g_assert_cmpstr (refspec_remote, ==, "remote");
  g_autofree char *refspec_ref = flatpak_decomposed_dup_ref (refspec);
  g_assert_cmpstr (refspec_ref, ==, "app/org.the.app/mips64/master");
  g_autofree char *refspec_refspec = flatpak_decomposed_dup_refspec (refspec);
  g_assert_cmpstr (refspec_refspec, ==, "remote:app/org.the.app/mips64/master");

  {
    FlatpakDecomposed *old = runtime_ref;
    g_autoptr(FlatpakDecomposed) new = flatpak_decomposed_new_from_decomposed (old, 0, NULL, NULL, NULL, &error);
    g_assert (new != NULL);
    g_assert_null (error);

    g_assert_cmpstr (flatpak_decomposed_get_ref (new), ==, flatpak_decomposed_get_ref (old));
    g_assert_cmpstr (flatpak_decomposed_peek_id (new, &len), ==, flatpak_decomposed_peek_id (old, &len2));
    g_assert (len == len2);
    g_assert_cmpstr (flatpak_decomposed_peek_arch (new, &len), ==, flatpak_decomposed_peek_arch (old, &len2));
    g_assert (len == len2);
    g_assert_cmpstr (flatpak_decomposed_peek_branch (new, &len), ==, flatpak_decomposed_peek_branch (old, &len2));
    g_assert (len == len2);
  }

  {
    FlatpakDecomposed *old = app_ref;
    g_autoptr(FlatpakDecomposed) new = flatpak_decomposed_new_from_decomposed (old, 0, NULL, NULL, NULL, &error);
    g_assert (new != NULL);
    g_assert_null (error);

    g_assert_cmpstr (flatpak_decomposed_get_ref (new), ==, flatpak_decomposed_get_ref (old));
    g_assert_cmpstr (flatpak_decomposed_peek_id (new, &len), ==, flatpak_decomposed_peek_id (old, &len2));
    g_assert (len == len2);
    g_assert_cmpstr (flatpak_decomposed_peek_arch (new, &len), ==, flatpak_decomposed_peek_arch (old, &len2));
    g_assert (len == len2);
    g_assert_cmpstr (flatpak_decomposed_peek_branch (new, &len), ==, flatpak_decomposed_peek_branch (old, &len2));
    g_assert (len == len2);
  }

  {
    FlatpakDecomposed *old = app_ref;
    g_autofree gchar *new_id = NULL;

    g_autoptr(FlatpakDecomposed) new = flatpak_decomposed_new_from_decomposed (old, FLATPAK_KINDS_RUNTIME, "org.new.app", NULL, NULL, &error);
    g_assert (new != NULL);
    g_assert_null (error);

    g_assert_cmpstr (flatpak_decomposed_get_ref (new), ==, "runtime/org.new.app/mips64/master");

    g_assert (flatpak_decomposed_get_kinds (new) == FLATPAK_KINDS_RUNTIME);
    new_id = flatpak_decomposed_dup_id (new);
    g_assert_cmpstr (new_id, ==, "org.new.app");

    g_assert_cmpstr (flatpak_decomposed_peek_arch (new, &len), ==, flatpak_decomposed_peek_arch (old, &len2));
    g_assert (len == len2);
    g_assert_cmpstr (flatpak_decomposed_peek_branch (new, &len), ==, flatpak_decomposed_peek_branch (old, &len2));
    g_assert (len == len2);
  }

  {
    FlatpakDecomposed *old = app_ref;
    g_autofree gchar *old_id = NULL;
    g_autofree gchar *new_id = NULL;
    g_autofree gchar *new_arch = NULL;
    g_autofree gchar *old_branch = NULL;
    g_autofree gchar *new_branch = NULL;

    g_autoptr(FlatpakDecomposed) new = flatpak_decomposed_new_from_decomposed (old, 0, NULL, "m68k", NULL, &error);
    g_assert (new != NULL);
    g_assert_null (error);

    g_assert_cmpstr (flatpak_decomposed_get_ref (new), ==, "app/org.the.app/m68k/master");

    g_assert (flatpak_decomposed_get_kinds (new) == FLATPAK_KINDS_APP);

    new_id = flatpak_decomposed_dup_id (new);
    old_id = flatpak_decomposed_dup_id (old);
    g_assert_cmpstr (new_id, ==, old_id);

    new_arch = flatpak_decomposed_dup_arch (new);
    g_assert_cmpstr (new_arch, ==, "m68k");

    new_branch = flatpak_decomposed_dup_branch (new);
    old_branch = flatpak_decomposed_dup_branch (old);
    g_assert_cmpstr (new_branch, ==, old_branch);
  }

  {
    FlatpakDecomposed *old = app_ref;
    g_autofree gchar *old_id = NULL;
    g_autofree gchar *new_id = NULL;
    g_autofree gchar *new_arch = NULL;
    g_autofree gchar *old_arch = NULL;
    g_autofree gchar *new_branch = NULL;

    g_autoptr(FlatpakDecomposed) new = flatpak_decomposed_new_from_decomposed (old, 0, NULL, NULL, "beta", &error);
    g_assert (new != NULL);
    g_assert_null (error);

    g_assert_cmpstr (flatpak_decomposed_get_ref (new), ==, "app/org.the.app/mips64/beta");

    g_assert (flatpak_decomposed_get_kinds (new) == FLATPAK_KINDS_APP);

    new_id = flatpak_decomposed_dup_id (new);
    old_id = flatpak_decomposed_dup_id (old);
    g_assert_cmpstr (new_id, ==, old_id);

    new_arch = flatpak_decomposed_dup_arch (new);
    old_arch = flatpak_decomposed_dup_arch (old);
    g_assert_cmpstr (new_arch, ==, old_arch);

    new_branch = flatpak_decomposed_dup_branch (new);
    g_assert_cmpstr (new_branch, ==, "beta");
  }

  {
    FlatpakDecomposed *old = app_ref;
    g_autofree gchar *new_id = NULL;
    g_autofree gchar *new_arch = NULL;
    g_autofree gchar *new_branch = NULL;

    g_autoptr(FlatpakDecomposed) new = flatpak_decomposed_new_from_decomposed (old, FLATPAK_KINDS_RUNTIME, "org.new.app", "m68k", "beta", &error);
    g_assert (new != NULL);
    g_assert_null (error);

    g_assert_cmpstr (flatpak_decomposed_get_ref (new), ==, "runtime/org.new.app/m68k/beta");

    g_assert (flatpak_decomposed_get_kinds (new) == FLATPAK_KINDS_RUNTIME);
    new_id = flatpak_decomposed_dup_id (new);
    g_assert_cmpstr (new_id, ==, "org.new.app");
    new_arch = flatpak_decomposed_dup_arch (new);
    g_assert_cmpstr (new_arch, ==, "m68k");
    new_branch = flatpak_decomposed_dup_branch (new);
    g_assert_cmpstr (new_branch, ==, "beta");
  }

  {
    g_autoptr(FlatpakDecomposed) pref = NULL;
    g_autofree gchar *id = NULL;
    g_autofree gchar *arch = NULL;
    g_autofree gchar *branch = NULL;

    pref = flatpak_decomposed_new_from_pref (FLATPAK_KINDS_RUNTIME, "org.the.@pp.Locale/mips64/master", &error);
    g_assert_null (pref);
    g_assert (error != NULL);
    g_assert (error->domain == FLATPAK_ERROR);
    g_assert (error->code == FLATPAK_ERROR_INVALID_REF);
    g_clear_error (&error);

    pref = flatpak_decomposed_new_from_pref (FLATPAK_KINDS_RUNTIME, "org.the.app.Locale/x86@64/master", &error);
    g_assert_null (pref);
    g_assert (error != NULL);
    g_assert (error->domain == FLATPAK_ERROR);
    g_assert (error->code == FLATPAK_ERROR_INVALID_REF);
    g_clear_error (&error);

    pref = flatpak_decomposed_new_from_pref (FLATPAK_KINDS_RUNTIME, "org.the.app.Locale//master", &error);
    g_assert_null (pref);
    g_assert (error != NULL);
    g_assert (error->domain == FLATPAK_ERROR);
    g_assert (error->code == FLATPAK_ERROR_INVALID_REF);
    g_clear_error (&error);

    pref = flatpak_decomposed_new_from_pref (FLATPAK_KINDS_RUNTIME, "org.the.app.Locale/mips64", &error);
    g_assert_null (pref);
    g_assert (error != NULL);
    g_assert (error->domain == FLATPAK_ERROR);
    g_assert (error->code == FLATPAK_ERROR_INVALID_REF);
    g_clear_error (&error);

    pref = flatpak_decomposed_new_from_pref (FLATPAK_KINDS_RUNTIME, "org.the.app.Locale/mips64/master", &error);
    if (error)
      g_print ("XXXXXXXX error: %s\n", error->message);
    g_assert_nonnull (pref);
    g_assert (error == NULL);

    g_assert_cmpstr (flatpak_decomposed_get_ref (pref), ==, "runtime/org.the.app.Locale/mips64/master");

    g_assert (flatpak_decomposed_get_kinds (pref) == FLATPAK_KINDS_RUNTIME);
    id = flatpak_decomposed_dup_id (pref);
    g_assert_cmpstr (id, ==, "org.the.app.Locale");
    arch = flatpak_decomposed_dup_arch (pref);
    g_assert_cmpstr (arch, ==, "mips64");
    branch = flatpak_decomposed_dup_branch (pref);
    g_assert_cmpstr (branch, ==, "master");
  }


  {
    g_autoptr(FlatpakDecomposed) a = flatpak_decomposed_new_from_ref ("app/org.app.A/mips64/master", NULL);
    g_autoptr(FlatpakDecomposed) a_l = flatpak_decomposed_new_from_ref ("runtime/org.app.A.Locale/mips64/master", NULL);
    g_autoptr(FlatpakDecomposed) b = flatpak_decomposed_new_from_ref ("app/org.app.B/mips64/master", NULL);
    g_autoptr(FlatpakDecomposed) b_l = flatpak_decomposed_new_from_ref ("runtime/org.app.B.Locale/mips64/master", NULL);
    g_autoptr(FlatpakDecomposed) c = flatpak_decomposed_new_from_ref ("app/org.app.A/m68k/master", NULL);
    g_autoptr(FlatpakDecomposed) c_l = flatpak_decomposed_new_from_ref ("runtime/org.app.A.Locale/m68k/master", NULL);
    g_autoptr(FlatpakDecomposed) d = flatpak_decomposed_new_from_ref ("app/org.app.A/mips64/beta", NULL);
    g_autoptr(FlatpakDecomposed) d_l = flatpak_decomposed_new_from_ref ("runtime/org.app.A.Locale/mips64/beta", NULL);

    g_assert (flatpak_decomposed_id_is_subref_of (a_l, a));
    g_assert (!flatpak_decomposed_id_is_subref_of (b_l, a));
    g_assert (!flatpak_decomposed_id_is_subref_of (c_l, a));
    g_assert (!flatpak_decomposed_id_is_subref_of (d_l, a));
    g_assert (!flatpak_decomposed_id_is_subref_of (a, a));
    g_assert (!flatpak_decomposed_id_is_subref_of (b, a));
    g_assert (!flatpak_decomposed_id_is_subref_of (c, a));
    g_assert (!flatpak_decomposed_id_is_subref_of (d, a));

    g_assert (!flatpak_decomposed_id_is_subref_of (a_l, b));
    g_assert (flatpak_decomposed_id_is_subref_of (b_l, b));
    g_assert (!flatpak_decomposed_id_is_subref_of (c_l, b));
    g_assert (!flatpak_decomposed_id_is_subref_of (d_l, b));
    g_assert (!flatpak_decomposed_id_is_subref_of (a, b));
    g_assert (!flatpak_decomposed_id_is_subref_of (b, b));
    g_assert (!flatpak_decomposed_id_is_subref_of (c, b));
    g_assert (!flatpak_decomposed_id_is_subref_of (d, b));

    g_assert (!flatpak_decomposed_id_is_subref_of (a_l, c));
    g_assert (!flatpak_decomposed_id_is_subref_of (b_l, c));
    g_assert (flatpak_decomposed_id_is_subref_of (c_l, c));
    g_assert (!flatpak_decomposed_id_is_subref_of (d_l, c));
    g_assert (!flatpak_decomposed_id_is_subref_of (a, c));
    g_assert (!flatpak_decomposed_id_is_subref_of (b, c));
    g_assert (!flatpak_decomposed_id_is_subref_of (c, c));
    g_assert (!flatpak_decomposed_id_is_subref_of (d, c));

    g_assert (!flatpak_decomposed_id_is_subref_of (a_l, d));
    g_assert (!flatpak_decomposed_id_is_subref_of (b_l, d));
    g_assert (!flatpak_decomposed_id_is_subref_of (c_l, d));
    g_assert (flatpak_decomposed_id_is_subref_of (d_l, d));
    g_assert (!flatpak_decomposed_id_is_subref_of (a, d));
    g_assert (!flatpak_decomposed_id_is_subref_of (b, d));
    g_assert (!flatpak_decomposed_id_is_subref_of (c, d));
    g_assert (!flatpak_decomposed_id_is_subref_of (d, d));
  }
}


typedef struct
{
  const gchar *str;
  guint        base;
  gint         min;
  gint         max;
  gint         expected;
  gboolean     should_fail;
  GNumberParserError code;
} TestData;

const TestData test_data[] = {
  /* typical cases for unsigned */
  { "-1", 10, 0, 2, 0, TRUE, G_NUMBER_PARSER_ERROR_INVALID },
  { "1", 10, 0, 2, 1, FALSE, 0},
  { "+1", 10, 0, 2, 0, TRUE, G_NUMBER_PARSER_ERROR_INVALID },
  { "0", 10, 0, 2, 0, FALSE, 0},
  { "+0", 10, 0, 2, 0, TRUE, G_NUMBER_PARSER_ERROR_INVALID },
  { "-0", 10, 0, 2, 0, TRUE, G_NUMBER_PARSER_ERROR_INVALID },
  { "2", 10, 0, 2, 2, FALSE, 0},
  { "+2", 10, 0, 2, 0, TRUE, G_NUMBER_PARSER_ERROR_INVALID },
  { "3", 10, 0, 2, 0, TRUE, G_NUMBER_PARSER_ERROR_OUT_OF_BOUNDS },
  { "+3", 10, 0, 2, 0, TRUE, G_NUMBER_PARSER_ERROR_INVALID },

  /* min == max cases for unsigned */
  { "2", 10, 2, 2, 2, FALSE, 0 },
  { "3", 10, 2, 2, 0, TRUE, G_NUMBER_PARSER_ERROR_OUT_OF_BOUNDS },
  { "1", 10, 2, 2, 0, TRUE, G_NUMBER_PARSER_ERROR_OUT_OF_BOUNDS },

  /* invalid inputs */
  { "",   10,  0,  2,  0, TRUE, G_NUMBER_PARSER_ERROR_INVALID },
  { "a",  10,  0,  2,  0, TRUE, G_NUMBER_PARSER_ERROR_INVALID },
  { "1a", 10,  0,  2,  0, TRUE, G_NUMBER_PARSER_ERROR_INVALID },

  /* leading/trailing whitespace */
  { " 1", 10,  0,  2,  0, TRUE, G_NUMBER_PARSER_ERROR_INVALID },
  { "1 ", 10,  0,  2,  0, TRUE, G_NUMBER_PARSER_ERROR_INVALID },

  /* hexadecimal numbers */
  { "a",    16,   0, 15, 10, FALSE, 0 },
  { "0xa",  16,   0, 15,  0, TRUE, G_NUMBER_PARSER_ERROR_INVALID },
  { "-0xa", 16,   0, 15,  0, TRUE, G_NUMBER_PARSER_ERROR_INVALID },
  { "+0xa", 16,   0, 15,  0, TRUE, G_NUMBER_PARSER_ERROR_INVALID },
  { "- 0xa", 16,   0, 15,  0, TRUE, G_NUMBER_PARSER_ERROR_INVALID },
  { "+ 0xa", 16,   0, 15,  0, TRUE, G_NUMBER_PARSER_ERROR_INVALID },
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
      result = g_ascii_string_to_unsigned (data->str,
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
          g_assert_error (error, G_NUMBER_PARSER_ERROR, data->code);
          g_clear_error (&error);
        }
      else
        {
          g_assert_true (result);
          g_assert_no_error (error);
          g_assert_cmpint (value, ==, data->expected);
          g_assert_cmpint (data->code, ==, 0);
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

      g_assert_cmpint (flatpak_levenshtein_distance (data->a, -1, data->b, -1), ==, data->distance);
      g_assert_cmpint (flatpak_levenshtein_distance (data->b, -1, data->a, -1), ==, data->distance);
    }
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
      g_assert_true (ret == FALSE);
      g_assert_true (allow_refs == NULL);
      g_assert_true (deny_refs == NULL);
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
     { "app/org.filter.this/mips64/stable", FALSE },
     { "app/com.filter.this/arm/stable", FALSE },
     /* But net. not denied */
     { "app/net.dont.filter.this/mips64/stable", TRUE },
     { "runtime/net.dont.filter.this/mips64/1.0", TRUE },

     /* Special allow overrides */

     /* allow com.foo.bar */
     { "app/com.foo.bar/mips64/stable", TRUE },
     { "app/com.foo.bar/arm/foo", TRUE },
     { "runtime/com.foo.bar/mips64/1.0", TRUE },

     /* allow app/com.bar.foo* / * /stable */
     { "app/com.bar.foo/mips64/stable", TRUE },
     { "app/com.bar.foo/arm/stable", TRUE },
     { "app/com.bar.foobar/mips64/stable", TRUE },
     { "app/com.bar.foobar/arm/stable", TRUE },
     { "app/com.bar.foo.bar/mips64/stable", TRUE },
     { "app/com.bar.foo.bar/arm/stable", TRUE },
     { "app/com.bar.foo/mips64/unstable", FALSE },
     { "app/com.bar.foobar/mips64/unstable", FALSE },
     { "runtime/com.bar.foo/mips64/stable", FALSE },

     /* allow app/com.armed.foo* /arm */
     { "app/com.armed.foo/arm/stable", TRUE },
     { "app/com.armed.foo/arm/unstable", TRUE },
     { "app/com.armed.foo/mips64/stable", FALSE },
     { "app/com.armed.foo/mips64/unstable", FALSE },
     { "app/com.armed.foobar/arm/stable", TRUE },
     { "app/com.armed.foobar/arm/unstable", TRUE },
     { "app/com.armed.foobar/mips64/stable", FALSE },
     { "app/com.armed.foobar/mips64/unstable", FALSE },
     { "runtime/com.armed.foo/arm/stable", FALSE },
     { "runtime/com.armed.foobar/arm/stable", FALSE },
     { "runtime/com.armed.foo/mips64/stable", FALSE },
     { "runtime/com.armed.foobar/mips64/stable", FALSE },

     /* allow runtime/com.gazonk */
     /* allow runtime/com.gazonk.* */
     { "runtime/com.gazonk/mips64/1.0", TRUE },
     { "runtime/com.gazonk.Locale/mips64/1.0", TRUE },
     { "runtime/com.gazonked/mips64/1.0", FALSE },
     { "runtime/com.gazonk/arm/1.0", TRUE },
     { "runtime/com.gazonk.Locale/arm/1.0", TRUE },
     { "app/com.gazonk/mips64/stable", FALSE },
     { "app/com.gazonk.Locale/mips64/stable", FALSE },

  };

  ret = flatpak_parse_filters (filter, &allow_refs, &deny_refs, &error);
  g_assert_no_error (error);
  g_assert_true (ret == TRUE);

  g_assert_true (allow_refs != NULL);
  g_assert_true (deny_refs != NULL);

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
    { "/org/gnome/Rhythmbox3/", "/org/gnome/rhythmbox/", 1 },
    { "/org/gnome/Rhythmbox3/", "/org/gnome/rhythmbox", 0 },
    { "/org/gnome1/Rhythmbox/", "/org/gnome/rhythmbox", 0 },
    { "/org/gnome1/Rhythmbox", "/org/gnome/rhythmbox/", 0 },
    { "/org/gnome/Rhythmbox3plus/", "/org/gnome/rhythmbox/", 0 },
    { "/org/gnome/SoundJuicer/", "/org/gnome/sound-juicer/", 1 },
    { "/org/gnome/Sound-Juicer/", "/org/gnome/sound-juicer/", 1 },
    { "/org/gnome/Soundjuicer/", "/org/gnome/sound-juicer/", 0 },
    { "/org/gnome/Soundjuicer/", "/org/gnome/soundjuicer/", 1 },
    { "/org/gnome/sound-juicer/", "/org/gnome/SoundJuicer/", 1 },
  };
  int i;

  for (i = 0; i < G_N_ELEMENTS (tests); i++)
    {
      gboolean result;

      result = flatpak_dconf_path_is_similar (tests[i].path1, tests[i].path2);
      if (result != tests[i].result)
        g_error ("Unexpected %s: flatpak_dconf_path_is_similar (%s, %s) = %d",
                 result ? "success" : "failure",
                 tests[i].path1,
                 tests[i].path2,
                 result);
    }
}

static void
test_envp_cmp (void)
{
  static const char * const unsorted[] =
  {
    "SAME_NAME=2",
    "EARLY_NAME=a",
    "SAME_NAME=222",
    "Z_LATE_NAME=b",
    "SUFFIX_ADDED=23",
    "SAME_NAME=1",
    "SAME_NAME=",
    "SUFFIX=42",
    "SAME_NAME=3",
    "SAME_NAME",
  };
  static const char * const sorted[] =
  {
    "EARLY_NAME=a",
    "SAME_NAME",
    "SAME_NAME=",
    "SAME_NAME=1",
    "SAME_NAME=2",
    "SAME_NAME=222",
    "SAME_NAME=3",
    "SUFFIX=42",
    "SUFFIX_ADDED=23",
    "Z_LATE_NAME=b",
  };
  const char **sort_this = NULL;
  gsize i, j;

  G_STATIC_ASSERT (G_N_ELEMENTS (sorted) == G_N_ELEMENTS (unsorted));

  for (i = 0; i < G_N_ELEMENTS (sorted); i++)
    {
      g_autofree gchar *copy = g_strdup (sorted[i]);

      g_test_message ("%s == %s", copy, sorted[i]);
      g_assert_cmpint (flatpak_envp_cmp (&copy, &sorted[i]), ==, 0);
      g_assert_cmpint (flatpak_envp_cmp (&sorted[i], &copy), ==, 0);

      for (j = i + 1; j < G_N_ELEMENTS (sorted); j++)
        {
          g_test_message ("%s < %s", sorted[i], sorted[j]);
          g_assert_cmpint (flatpak_envp_cmp (&sorted[i], &sorted[j]), <, 0);
          g_assert_cmpint (flatpak_envp_cmp (&sorted[j], &sorted[i]), >, 0);
        }
    }

  sort_this = g_new0 (const char *, G_N_ELEMENTS (unsorted));

  for (i = 0; i < G_N_ELEMENTS (unsorted); i++)
    sort_this[i] = unsorted[i];

  qsort (sort_this, G_N_ELEMENTS (unsorted), sizeof (char *),
         flatpak_envp_cmp);

  for (i = 0; i < G_N_ELEMENTS (sorted); i++)
    g_assert_cmpstr (sorted[i], ==, sort_this[i]);

  g_free (sort_this);
}

static void
test_needs_quoting (void)
{
  static const char * const needs_quoting[] =
  {
    "",
    "$var",
    "{}",
    "()",
    "[]",
    "*",
    "?",
    "`exec`",
    "has space",
    "quoted-\"",
    "quoted-'",
    "back\\slash",
    "control\001char",
  };
  static const char * const does_not_need_quoting[] =
  {
    "foo",
    "--foo=bar",
    "-x",
    "foo@bar:/srv/big_files",
    "~smcv",
    "7-zip.org",
  };
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (needs_quoting); i++)
    {
      const char *orig = needs_quoting[i];
      g_autoptr(GError) error = NULL;
      g_autofree char *quoted = NULL;
      int argc = -1;
      g_auto(GStrv) argv = NULL;
      gboolean ok;

      g_assert_true (flatpak_argument_needs_quoting (orig));
      quoted = flatpak_quote_argv (&orig, 1);
      g_test_message ("Unquoted: \"%s\"", orig);
      g_test_message ("  Quoted: \"%s\"", quoted);
      g_assert_cmpstr (quoted, !=, orig);

      ok = g_shell_parse_argv (quoted, &argc, &argv, &error);
      g_assert_no_error (error);
      g_assert_true (ok);
      g_assert_cmpint (argc, ==, 1);
      g_assert_nonnull (argv);
      g_assert_cmpstr (argv[0], ==, orig);
      g_assert_cmpstr (argv[1], ==, NULL);
    }

  for (i = 0; i < G_N_ELEMENTS (does_not_need_quoting); i++)
    {
      const char *orig = does_not_need_quoting[i];
      g_autoptr(GError) error = NULL;
      g_autofree char *quoted = NULL;
      int argc = -1;
      g_auto(GStrv) argv = NULL;
      gboolean ok;

      g_assert_false (flatpak_argument_needs_quoting (orig));
      quoted = flatpak_quote_argv (&orig, 1);
      g_assert_cmpstr (quoted, ==, orig);

      ok = g_shell_parse_argv (quoted, &argc, &argv, &error);
      g_assert_no_error (error);
      g_assert_true (ok);
      g_assert_cmpint (argc, ==, 1);
      g_assert_nonnull (argv);
      g_assert_cmpstr (argv[0], ==, orig);
      g_assert_cmpstr (argv[1], ==, NULL);
    }
}

static void
test_quote_argv (void)
{
  static const char * const orig[] =
  {
    "foo",
    "--bar",
    "",
    "baz",
    NULL
  };
  gsize i;
  g_autofree char *quoted = NULL;
  g_autoptr(GError) error = NULL;
  int argc = -1;
  g_auto(GStrv) argv = NULL;
  gboolean ok;

  quoted = flatpak_quote_argv ((const char **) orig, -1);
  ok = g_shell_parse_argv (quoted, &argc, &argv, &error);
  g_assert_no_error (error);
  g_assert_true (ok);
  g_assert_cmpint (argc, >, 0);
  g_assert_cmpuint ((gsize) argc, ==, G_N_ELEMENTS (orig) - 1);
  g_assert_nonnull (argv);

  for (i = 0; i < G_N_ELEMENTS (orig); i++)
    g_assert_cmpstr (argv[i], ==, orig[i]);

  g_clear_pointer (&quoted, g_free);
  g_clear_pointer (&argv, g_strfreev);

  quoted = flatpak_quote_argv ((const char **) orig, 3);
  ok = g_shell_parse_argv (quoted, &argc, &argv, &error);
  g_assert_no_error (error);
  g_assert_true (ok);
  g_assert_cmpint (argc, ==, 3);
  g_assert_nonnull (argv);

  for (i = 0; i < 3; i++)
    g_assert_cmpstr (argv[i], ==, orig[i]);

  g_assert_cmpstr (argv[i], ==, NULL);
}

static void
test_str_is_integer (void)
{
  g_assert_true (flatpak_str_is_integer ("0"));
  g_assert_true (flatpak_str_is_integer ("1234567890987654356765432121245674"));
  g_assert_false (flatpak_str_is_integer (NULL));
  g_assert_false (flatpak_str_is_integer (""));
  g_assert_false (flatpak_str_is_integer ("0.0"));
  g_assert_false (flatpak_str_is_integer ("0e0"));
  g_assert_false (flatpak_str_is_integer ("bees"));
  g_assert_false (flatpak_str_is_integer ("1234a"));
  g_assert_false (flatpak_str_is_integer ("a1234"));
}

/* These are part of the X11 protocol, so we can safely hard-code them here */
#define FamilyInternet6 6
#define FamilyLocal 256
#define FamilyWild 65535

typedef struct
{
  const char *display;
  int family;
  const char *x11_socket;
  const char *remote_host;
  const char *display_number;
} DisplayTest;

static const DisplayTest x11_display_tests[] =
{
  /* Valid test-cases */
  { ":0", FamilyLocal, "/tmp/.X11-unix/X0", NULL, "0" },
  { ":0.0", FamilyLocal, "/tmp/.X11-unix/X0", NULL, "0" },
  { ":42.0", FamilyLocal, "/tmp/.X11-unix/X42", NULL, "42" },
  { "unix:42", FamilyLocal, "/tmp/.X11-unix/X42", NULL, "42" },
  { "othermachine:23", FamilyWild, NULL, "othermachine", "23" },
  { "bees.example.com:23", FamilyWild, NULL, "bees.example.com", "23" },
  { "[::1]:0", FamilyInternet6, NULL, "::1", "0" },

  /* Invalid test-cases */
  { "", 0 },
  { "nope", 0 },
  { ":!", 0 },
  { "othermachine::" },
};

static void
test_parse_x11_display (void)
{
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (x11_display_tests); i++)
    {
      const DisplayTest *test = &x11_display_tests[i];
      int family = -1;
      g_autofree char *x11_socket = NULL;
      g_autofree char *remote_host = NULL;
      g_autofree char *display_number = NULL;
      gboolean ok;
      g_autoptr(GError) error = NULL;

      g_test_message ("%s", test->display);

      ok = flatpak_run_parse_x11_display (test->display,
                                          &family,
                                          &x11_socket,
                                          &remote_host,
                                          &display_number,
                                          &error);

      if (test->family == 0)
        {
          g_assert_nonnull (error);
          g_assert_false (ok);
          g_assert_null (x11_socket);
          g_assert_null (remote_host);
          g_assert_null (display_number);
          g_test_message ("-> could not parse: %s", error->message);
        }
      else
        {
          g_assert_no_error (error);
          g_assert_true (ok);
          g_assert_cmpint (family, ==, test->family);
          g_assert_cmpstr (x11_socket, ==, test->x11_socket);
          g_assert_cmpstr (remote_host, ==, test->remote_host);
          g_assert_cmpstr (display_number, ==, test->display_number);
          g_test_message ("-> successfully parsed");
        }
    }
}

typedef struct {
  const char        *in;
  FlatpakEscapeFlags flags;
  const char        *out;
} EscapeData;

static EscapeData escapes[] = {
  {"abc def", FLATPAK_ESCAPE_DEFAULT, "abc def"},
  {"やあ", FLATPAK_ESCAPE_DEFAULT, "やあ"},
  {"\033[;1m", FLATPAK_ESCAPE_DEFAULT, "'\\x1B[;1m'"},
  /* U+061C ARABIC LETTER MARK, non-printable */
  {"\u061C", FLATPAK_ESCAPE_DEFAULT, "'\\u061C'"},
  /* U+1343F EGYPTIAN HIEROGLYPH END WALLED ENCLOSURE, non-printable and
   * outside BMP */
  {"\xF0\x93\x90\xBF", FLATPAK_ESCAPE_DEFAULT, "'\\U0001343F'"},
  /* invalid utf-8 */
  {"\xD8\1", FLATPAK_ESCAPE_DEFAULT, "'\\xD8\\x01'"},
  {"\b \n abc ' \\", FLATPAK_ESCAPE_DEFAULT, "'\\x08 \\x0A abc \\' \\\\'"},
  {"\b \n abc ' \\", FLATPAK_ESCAPE_DO_NOT_QUOTE, "\\x08 \\x0A abc ' \\\\"},
  {"abc\tdef\n\033[;1m ghi\b", FLATPAK_ESCAPE_ALLOW_NEWLINES | FLATPAK_ESCAPE_DO_NOT_QUOTE,
   "abc\\x09def\n\\x1B[;1m ghi\\x08"},
};

/* CVE-2023-28101 */
static void
test_string_escape (void)
{
  gsize idx;

  for (idx = 0; idx < G_N_ELEMENTS (escapes); idx++)
    {
      EscapeData *data = &escapes[idx];
      g_autofree char *ret = NULL;

      ret = flatpak_escape_string (data->in, data->flags);
      g_assert_cmpstr (ret, ==, data->out);
    }
}

typedef struct {
  const char *path;
  gboolean ret;
} PathValidityData;

static PathValidityData paths[] = {
  {"/a/b/../c.def", TRUE},
  {"やあ", TRUE},
  /* U+061C ARABIC LETTER MARK, non-printable */
  {"\u061C", FALSE},
  /* U+1343F EGYPTIAN HIEROGLYPH END WALLED ENCLOSURE, non-printable and
   * outside BMP */
  {"\xF0\x93\x90\xBF", FALSE},
  /* invalid utf-8 */
  {"\xD8\1", FALSE},
};

/* CVE-2023-28101 */
static void
test_validate_path_characters (void)
{
  gsize idx;

  for (idx = 0; idx < G_N_ELEMENTS (paths); idx++)
    {
      PathValidityData *data = &paths[idx];
      gboolean ret = FALSE;

      ret = flatpak_validate_path_characters (data->path, NULL);
      g_assert_cmpint (ret, ==, data->ret);
    }
}

int
main (int argc, char *argv[])
{
  int res;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/common/has-path-prefix", test_has_path_prefix);
  g_test_add_func ("/common/path-match-prefix", test_path_match_prefix);
  g_test_add_func ("/common/arches", test_arches);
  g_test_add_func ("/common/extension-matches", test_extension_matches);
  g_test_add_func ("/common/valid-name", test_valid_name);
  g_test_add_func ("/common/string-to-unsigned", test_string_to_unsigned);
  g_test_add_func ("/common/levenshtein", test_levenshtein);
  g_test_add_func ("/common/subpaths-merge", test_subpaths_merge);
  g_test_add_func ("/common/appdata", test_parse_appdata);
  g_test_add_func ("/common/name-matching", test_name_matching);
  g_test_add_func ("/common/filter_parser", test_filter_parser);
  g_test_add_func ("/common/filter", test_filter);
  g_test_add_func ("/common/dconf-app-id", test_dconf_app_id);
  g_test_add_func ("/common/dconf-paths", test_dconf_paths);
  g_test_add_func ("/common/decompose-ref", test_decompose);
  g_test_add_func ("/common/envp-cmp", test_envp_cmp);
  g_test_add_func ("/common/needs-quoting", test_needs_quoting);
  g_test_add_func ("/common/quote-argv", test_quote_argv);
  g_test_add_func ("/common/str-is-integer", test_str_is_integer);
  g_test_add_func ("/common/parse-x11-display", test_parse_x11_display);
  g_test_add_func ("/common/string-escape", test_string_escape);
  g_test_add_func ("/common/validate-path-characters", test_validate_path_characters);

  res = g_test_run ();

  return res;
}
