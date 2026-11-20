/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "config.h"

#include <sys/file.h>
#include <unistd.h>
#include <glib.h>
#include <ostree.h>

#include "libglnx.h"
#include "flatpak-utils-private.h"

static const char *deployed_commit = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *downloaded_commit = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

static GVariant *
make_metadata (void)
{
  GVariantBuilder builder;
  GVariantBuilder extra_data;
  GVariantBuilder signatures;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init (&extra_data, G_VARIANT_TYPE ("a(ayay)"));
  g_variant_builder_add (&extra_data, "(^ay^ay)", "installer", "installer contents");
  g_variant_builder_add (&builder, "{sv}", "xa.extra-data", g_variant_builder_end (&extra_data));
  g_variant_builder_init (&signatures, G_VARIANT_TYPE ("aay"));
  g_variant_builder_add (&signatures, "^ay", "signature contents");
  g_variant_builder_add (&builder, "{sv}", "ostree.gpgsigs", g_variant_builder_end (&signatures));
  g_variant_builder_add (&builder, "{sv}", "test.unrelated", g_variant_new_string ("keep me"));
  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static char *
run_clean (const char *installation, gboolean dry_run, gboolean success)
{
  g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                                       G_SUBPROCESS_FLAGS_STDERR_PIPE);
  g_autoptr(GSubprocess) process = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *stderr_output = NULL;
  char *output = NULL;

  g_subprocess_launcher_setenv (launcher, "FLATPAK_USER_DIR", installation, TRUE);
  g_subprocess_launcher_setenv (launcher, "LC_ALL", "C", TRUE);
  process = g_subprocess_launcher_spawn (launcher, &error, "flatpak", "clean", "--user",
                                         dry_run ? "--dry-run" : NULL, NULL);
  g_assert_no_error (error);
  g_assert_nonnull (process);
  g_assert_true (g_subprocess_communicate_utf8 (process, NULL, NULL, &output, &stderr_output, &error));
  g_assert_no_error (error);
  g_test_message ("clean stderr: %s", stderr_output);
  g_assert_cmpint (g_subprocess_get_successful (process), ==, success);
  return output;
}

static void
test_clean (void)
{
  g_auto(GLnxTmpDir) tmpdir = { 0, };
  g_autoptr(GError) error = NULL;
  g_autofree char *repo_path = NULL;
  g_autoptr(GFile) repo_file = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  g_autoptr(GVariant) metadata = make_metadata ();
  g_autoptr(GVariant) after = NULL;
  g_autofree char *deploy_path = NULL;
  g_autofree char *deploy_file = NULL;
  g_autofree char *active_path = NULL;
  g_autoptr(GVariant) deploy_data = NULL;
  g_autofree char *output = NULL;
  const char *subpaths[] = { NULL };

  g_assert_true (glnx_mkdtemp ("test-clean-XXXXXX", 0700, &tmpdir, &error));
  g_assert_no_error (error);
  repo_path = g_build_filename (tmpdir.path, "repo", NULL);
  repo_file = g_file_new_for_path (repo_path);
  repo = ostree_repo_new (repo_file);
  g_assert_true (ostree_repo_create (repo, OSTREE_REPO_MODE_BARE_USER_ONLY, NULL, &error));
  g_assert_no_error (error);

  output = run_clean (tmpdir.path, FALSE, TRUE);
  g_assert_cmpstr (output, ==, "");
  g_clear_pointer (&output, g_free);

  /* Only the detached metadata and deployment records are needed. A second
   * commit models a download saved for a later --no-pull deployment. */
  g_assert_true (ostree_repo_write_commit_detached_metadata (repo, deployed_commit, metadata, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (ostree_repo_write_commit_detached_metadata (repo, downloaded_commit, metadata, NULL, &error));
  g_assert_no_error (error);

  deploy_path = g_build_filename (tmpdir.path, "app", "org.test.Clean", "x86_64", "master", deployed_commit, NULL);
  g_assert_cmpint (g_mkdir_with_parents (deploy_path, 0700), ==, 0);
  deploy_file = g_build_filename (deploy_path, "deploy", NULL);
  deploy_data = g_variant_ref_sink (g_variant_new ("(ss^ast@a{sv})", "test", deployed_commit,
                                                   subpaths, (guint64) 0,
                                                   g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0)));
  g_assert_true (g_file_set_contents (deploy_file, g_variant_get_data (deploy_data),
                                      g_variant_get_size (deploy_data), &error));
  g_assert_no_error (error);
  active_path = g_build_filename (tmpdir.path, "app", "org.test.Clean", "x86_64", "master", "active", NULL);
  g_assert_cmpint (symlink (deployed_commit, active_path), ==, 0);

  output = run_clean (tmpdir.path, TRUE, TRUE);
  g_assert_nonnull (strstr (output, "Would remove cached extra data"));
  g_assert_true (ostree_repo_read_commit_detached_metadata (repo, deployed_commit, &after, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (g_variant_equal (metadata, after));
  g_clear_pointer (&after, g_variant_unref);
  g_clear_pointer (&output, g_free);

  /* A shared lock models a pull or a deployment. Neither dry-run nor cleanup
   * may race with its detached metadata reads and writes. */
  {
    g_auto(GLnxLockFile) lock = { 0, };
    g_assert_true (glnx_make_lock_file (tmpdir.fd, "repo-lock", LOCK_SH, &lock, &error));
    g_assert_no_error (error);
    output = run_clean (tmpdir.path, TRUE, FALSE);
    g_clear_pointer (&output, g_free);
    output = run_clean (tmpdir.path, FALSE, FALSE);
    g_clear_pointer (&output, g_free);
  }

  output = run_clean (tmpdir.path, FALSE, TRUE);
  g_assert_nonnull (strstr (output, "Removed cached extra data"));
  g_clear_pointer (&output, g_free);
  g_assert_true (ostree_repo_read_commit_detached_metadata (repo, deployed_commit, &after, NULL, &error));
  g_assert_no_error (error);
  {
    g_auto(GVariantDict) dict = FLATPAK_VARIANT_DICT_INITIALIZER;
    g_autoptr(GVariant) expected = NULL;
    g_variant_dict_init (&dict, metadata);
    g_variant_dict_remove (&dict, "xa.extra-data");
    expected = g_variant_ref_sink (g_variant_dict_end (&dict));
    g_assert_true (g_variant_equal (expected, after));
  }
  g_clear_pointer (&after, g_variant_unref);

  g_assert_true (ostree_repo_read_commit_detached_metadata (repo, downloaded_commit, &after, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (g_variant_equal (metadata, after));

  output = run_clean (tmpdir.path, FALSE, TRUE);
  g_assert_cmpstr (output, ==, "");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/clean/extra-data", test_clean);
  return g_test_run ();
}
