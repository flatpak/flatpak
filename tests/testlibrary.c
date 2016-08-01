#include "config.h"
#include <glib.h>
#include "flatpak.h"
#include "glnx-shutil.h"

static char *testdir;
static char *flatpak_runtimedir;
static char *flatpak_systemdir;
static char *gpg_homedir;
static char *gpg_args;
static char *repo_url;

static const char *gpg_id = "7B0961FD";
static const char *repo_name = "test-repo";

static void
test_library_version (void)
{
  g_autofree char *version = NULL;

  version = g_strdup_printf ("%d.%d.%d",
                             FLATPAK_MAJOR_VERSION,
                             FLATPAK_MINOR_VERSION,
                             FLATPAK_MICRO_VERSION);
  g_assert_cmpstr (version, ==, PACKAGE_VERSION);
}

static void
test_user_installation (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) dir = NULL;
  g_autofree char *path = NULL;
  g_autofree char *expected_path = NULL;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_nonnull (inst);
  g_assert_no_error (error);

  g_assert_true (flatpak_installation_get_is_user (inst));

  dir = flatpak_installation_get_path (inst);
  path = g_file_get_path (dir);
  expected_path = g_build_filename (g_get_user_data_dir (), "flatpak", NULL);
  g_assert_cmpstr (path, ==, expected_path);
}

static void
test_system_installation (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) dir = NULL;
  g_autofree char *path = NULL;

  inst = flatpak_installation_new_system (NULL, &error);
  g_assert_nonnull (inst);
  g_assert_no_error (error);

  g_assert_false (flatpak_installation_get_is_user (inst));

  dir = flatpak_installation_get_path (inst);
  path = g_file_get_path (dir);
  g_assert_cmpstr (path, ==, flatpak_systemdir);
}

static void
test_arches (void)
{
  const char *default_arch;
  const char *const *supported_arches;

  default_arch = flatpak_get_default_arch ();
  supported_arches = flatpak_get_supported_arches ();

  g_assert_nonnull (default_arch);
  g_assert_cmpstr (default_arch, !=, "");

  g_assert_nonnull (supported_arches);
  g_assert (g_strv_contains (supported_arches, default_arch));
}

static void
test_list_remotes (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) remotes = NULL;
  FlatpakRemote *remote;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  remotes = flatpak_installation_list_remotes (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (remotes);
  g_assert (remotes->len == 1);

  remote = g_ptr_array_index (remotes, 0);
  g_assert (FLATPAK_IS_REMOTE (remote));
}

static void
test_remote_by_name (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  FlatpakRemote *remote;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  remote = flatpak_installation_get_remote_by_name (inst, repo_name, NULL, &error);
  g_assert_no_error (error);

  g_assert (FLATPAK_IS_REMOTE (remote));
  g_assert_cmpstr (flatpak_remote_get_name (remote), ==, repo_name);
  g_assert_cmpstr (flatpak_remote_get_url (remote), ==, repo_url);
  g_assert_cmpstr (flatpak_remote_get_title (remote), ==, NULL);
  g_assert_false (flatpak_remote_get_noenumerate (remote));
  g_assert_false (flatpak_remote_get_disabled (remote));
  g_assert_true (flatpak_remote_get_gpg_verify (remote));
  g_assert_cmpint (flatpak_remote_get_prio (remote), ==, 1);
}

static void
make_test_runtime (void)
{
  int status;
  g_autoptr(GError) error = NULL;
  g_autofree char *arg0 = NULL;
  char *argv[] = {
    NULL, "org.test.Platform", "bash", "ls", "cat", "echo", "readlink", NULL
  };
  GSpawnFlags flags = G_SPAWN_DEFAULT;

  arg0 = g_test_build_filename (G_TEST_DIST, "make-test-runtime.sh", NULL);
  argv[0] = arg0;

  if (g_test_verbose ())
    {
      g_autofree char *commandline = g_strjoinv (" ", argv);
      g_print ("running %s\n", commandline);
    }
  else
    flags |= G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL;

  g_spawn_sync (NULL, (char **)argv, NULL, flags, NULL, NULL, NULL, NULL, &status, &error);
  g_assert_no_error (error);
  g_assert_cmpint (status, ==, 0);
}

static void
make_test_app (void)
{
  int status;
  g_autoptr(GError) error = NULL;
  g_autofree char *arg0 = NULL;
  char *argv[] = { NULL, NULL };
  GSpawnFlags flags = G_SPAWN_DEFAULT;

  arg0 = g_test_build_filename (G_TEST_DIST, "make-test-app.sh", NULL);
  argv[0] = arg0;

  if (g_test_verbose ())
    {
      g_autofree char *commandline = g_strjoinv (" ", argv);
      g_print ("running %s\n", commandline);
    }
  else
    flags |= G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL;

  g_spawn_sync (NULL, (char **)argv, NULL, flags, NULL, NULL, NULL, NULL, &status, &error);
  g_assert_no_error (error);
  g_assert_cmpint (status, ==, 0);
}

static void
update_repo (void)
{
  int status;
  g_autoptr(GError) error = NULL;
  char *argv[] = { "flatpak", "build-update-repo", "--gpg-homedir=", "--gpg-sign=", "repo", NULL };
  GSpawnFlags flags = G_SPAWN_SEARCH_PATH;
  g_auto(GStrv) gpgargs = NULL;

  gpgargs = g_strsplit (gpg_args, " ", 0);
  argv[2] = gpgargs[0];
  argv[3] = gpgargs[1];

  if (g_test_verbose ())
    {
      g_autofree char *commandline = g_strjoinv (" ", argv);
      g_print ("running %s\n", commandline);
    }
  else
    flags |= G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL;

  g_spawn_sync (NULL, (char **)argv, NULL, flags, NULL, NULL, NULL, NULL, &status, &error);
  g_assert_no_error (error);
  g_assert_cmpint (status, ==, 0);
}

static void
launch_httpd (void)
{
  int status;
  g_autoptr(GError) error = NULL;
  char *argv[] = { "ostree", "trivial-httpd", "--autoexit", "--daemonize", "-p", "http-port", ".", NULL };
  GSpawnFlags flags = G_SPAWN_SEARCH_PATH;

  if (g_test_verbose ())
    {
      g_autofree char *commandline = g_strjoinv (" ", argv);
      g_print ("running %s\n", commandline);
    }
  else
    flags |= G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL;

  g_spawn_sync (NULL, (char **)argv, NULL, flags, NULL, NULL, NULL, NULL, &status, &error);
  g_assert_no_error (error);
  g_assert_cmpint (status, ==, 0);
}

static void
add_remote (void)
{
  int status;
  g_autoptr(GError) error = NULL;
  char *argv[] = { "flatpak", "remote-add", "--user", "--gpg-import=", "name", "url", NULL };
  g_autofree char *gpgimport = NULL;
  g_autofree char *port = NULL;
  GSpawnFlags flags = G_SPAWN_SEARCH_PATH;

  launch_httpd ();

  g_file_get_contents ("http-port", &port, NULL, &error);
  g_assert_no_error (error);

  gpgimport = g_strdup_printf ("--gpg-import=%s/pubring.gpg", gpg_homedir);
  repo_url = g_strdup_printf ("http://127.0.0.1:%s/repo", port);

  argv[3] = gpgimport;
  argv[4] = (char *)repo_name;
  argv[5] = repo_url;

  if (g_test_verbose ())
    {
      g_autofree char *commandline = g_strjoinv (" ", argv);
      g_print ("running %s\n", commandline);
    }
  else
    flags |= G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL;

  g_spawn_sync (NULL, (char **)argv, NULL, flags, NULL, NULL, NULL, NULL, &status, &error);
  g_assert_no_error (error);
  g_assert_cmpint (status, ==, 0);
}

static void
setup_repo (void)
{
  make_test_runtime ();
  make_test_app ();
  update_repo ();
  add_remote ();
}

static void
copy_file (const char *src, const char *dest)
{
  gchar *buffer = NULL;
  gsize length;
  g_autoptr(GError) error = NULL;

  if (g_test_verbose ())
    g_print ("copying %s to %s\n", src, dest);

  if (g_file_get_contents (src, &buffer, &length, &error))
    g_file_set_contents (dest, buffer, length, &error);
  g_assert_no_error (error);
  g_free (buffer);
}

static void
copy_gpg (void)
{
  char *src;
  char *dest;

  src = g_test_build_filename (G_TEST_DIST, "test-keyring", "pubring.gpg", NULL);
  dest = g_strconcat (gpg_homedir, "/pubring.gpg", NULL);
  copy_file (src, dest);
  g_free (src);
  g_free (dest);

  src = g_test_build_filename (G_TEST_DIST, "test-keyring", "secring.gpg", NULL);
  dest = g_strconcat (gpg_homedir, "/secring.gpg", NULL);
  copy_file (src, dest);
  g_free (src);
  g_free (dest);
}

static void
global_setup (void)
{
  g_autofree char *homedir = NULL;

  testdir = g_strdup ("/var/tmp/flatpak-test-XXXXXX");
  g_mkdtemp (testdir);
  g_print ("testdir: %s\n", testdir);

  homedir = g_strconcat (testdir, "/home/share", NULL);
  g_mkdir_with_parents (homedir, S_IRWXU|S_IRWXG|S_IRWXO);
  g_setenv ("XDG_DATA_HOME", homedir, TRUE);
  if (g_test_verbose ())
    g_print ("setting XDG_DATA_HOME=%s\n", homedir);

  flatpak_runtimedir = g_strconcat (testdir, "/runtime", NULL);
  g_mkdir_with_parents (flatpak_runtimedir, S_IRWXU|S_IRWXG|S_IRWXO);
  g_setenv ("XDG_RUNTIME_DIR", flatpak_runtimedir, TRUE);
  if (g_test_verbose ())
    g_print ("setting XDG_RUNTIME_DIR=%s\n", flatpak_runtimedir);

  flatpak_systemdir = g_strconcat (testdir, "/system", NULL);
  g_mkdir_with_parents (flatpak_systemdir, S_IRWXU|S_IRWXG|S_IRWXO);
  g_setenv ("FLATPAK_SYSTEM_DIR", flatpak_systemdir, TRUE);
  if (g_test_verbose ())
    g_print ("setting FLATPAK_SYSTEM_DIR=%s\n", flatpak_systemdir);

  gpg_homedir = g_strconcat (testdir, "/gpghome", NULL);
  g_mkdir_with_parents (gpg_homedir, S_IRWXU|S_IRWXG|S_IRWXO);

  gpg_args = g_strdup_printf ("--gpg-homedir=%s --gpg-sign=%s", gpg_homedir, gpg_id);
  g_setenv ("GPGARGS", gpg_args, TRUE);
  if (g_test_verbose ())
    g_print ("setting GPGARGS=%s\n", gpg_args);

  copy_gpg ();
  setup_repo ();
}

static void
global_teardown (void)
{
  if (g_getenv ("SKIP_TEARDOWN"))
    return;

  glnx_shutil_rm_rf_at (-1, testdir, NULL, NULL);
  g_free (testdir);
}

int
main (int argc, char *argv[])
{
  int res;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/library/version", test_library_version);
  g_test_add_func ("/library/user-installation", test_user_installation);
  g_test_add_func ("/library/system-installation", test_system_installation);
  g_test_add_func ("/library/arches", test_arches);
  g_test_add_func ("/library/list-remotes", test_list_remotes);
  g_test_add_func ("/library/remote-by-name", test_remote_by_name);

  global_setup ();

  res = g_test_run ();

  global_teardown ();

  return res;
}
