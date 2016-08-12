#include "config.h"
#include <string.h>
#include <glib.h>
#include "libglnx/libglnx.h"
#include "flatpak.h"

#if GLIB_CHECK_VERSION (2, 44, 0)
#define strv_contains(strv, str) g_strv_contains (strv, str)
#else
static gboolean
strv_contains (const gchar * const *strv,
               const gchar *str)
{
  guint i;

  g_return_val_if_fail (strv != NULL, FALSE);

  for (i = 0; strv[i] != NULL; i++)
    {
      if (strcmp (strv[i], str) == 0)
        return TRUE;
    }

  return FALSE;
}
#endif

static char *testdir;
static char *flatpak_runtimedir;
static char *flatpak_systemdir;
static char *gpg_homedir;
static char *gpg_args;
static char *repo_url;

static const char *gpg_id = "7B0961FD";
const char *repo_name = "test-repo";

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
  g_assert (strv_contains (supported_arches, default_arch));
}

static void
test_ref (void)
{
  g_autoptr(FlatpakRef) ref = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *formatted = NULL;
  const char *valid;

  ref = flatpak_ref_parse ("", &error);
  g_assert_null (ref);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  ref = flatpak_ref_parse ("ref/or not", &error);
  g_assert_null (ref);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  ref = flatpak_ref_parse ("ref/one/2/3", &error);
  g_assert_null (ref);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  ref = flatpak_ref_parse ("app/a/b/c", &error);
  g_assert_null (ref);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  ref = flatpak_ref_parse ("app/org.flatpak.Hello/b/.", &error);
  g_assert_null (ref);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  valid = "app/org.flatpak.Hello/x86_64/master";
  ref = flatpak_ref_parse (valid, &error);
  g_assert (FLATPAK_IS_REF (ref));
  g_assert_no_error (error);
  g_assert_cmpint (flatpak_ref_get_kind (ref), ==, FLATPAK_REF_KIND_APP);
  g_assert_cmpstr (flatpak_ref_get_name (ref), ==, "org.flatpak.Hello");
  g_assert_cmpstr (flatpak_ref_get_arch (ref), ==, "x86_64");
  g_assert_cmpstr (flatpak_ref_get_branch (ref), ==, "master");

  formatted = flatpak_ref_format_ref (ref);
  g_assert_cmpstr (formatted, ==, valid);
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
test_remote (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakRemote) remote = NULL;
  gboolean res;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  remote = flatpak_installation_get_remote_by_name (inst, repo_name, NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpstr (flatpak_remote_get_title (remote), ==, NULL);
  flatpak_remote_set_title (remote, "Test Repo");
  g_assert_cmpstr (flatpak_remote_get_title (remote), ==, "Test Repo");

  g_assert_cmpint (flatpak_remote_get_prio (remote), ==, 1);
  flatpak_remote_set_prio (remote, 15);
  g_assert_cmpint (flatpak_remote_get_prio (remote), ==, 15);

  res = flatpak_installation_modify_remote (inst, remote, NULL, &error);
  g_assert_true (res);
  g_assert_no_error (error);

  g_clear_object (&remote);

  remote = flatpak_installation_get_remote_by_name (inst, repo_name, NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpstr (flatpak_remote_get_title (remote), ==, "Test Repo");
  g_assert_cmpint (flatpak_remote_get_prio (remote), ==, 15);
}

static void
test_list_refs (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) refs = NULL;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (refs);
  g_assert_cmpint (refs->len, ==, 0);
}

static void
progress_cb (const char *status,
             guint progress,
             gboolean estimating,
             gpointer user_data)
{
  int *count = user_data;
  *count += 1;
}

static int changed_count;

static void
changed_cb (GFileMonitor *monitor,
            GFile *file,
            GFile *other_file,
            GFileMonitorEvent event_type,
            gpointer user_data)
{
  GMainLoop *loop = user_data;
  g_main_loop_quit (loop);
  changed_count += 1;
}

static gboolean
quit (gpointer data)
{
  GMainLoop *loop = data;
  g_main_loop_quit (loop);
  return G_SOURCE_CONTINUE;
}

static void
test_install_launch_uninstall (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GFileMonitor) monitor = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakInstalledRef) ref = NULL;
  g_autoptr(FlatpakInstalledRef) runtime_ref = NULL;
  FlatpakInstalledRef *ref1 = NULL;
  GPtrArray *refs = NULL;
  g_autofree char *s = NULL;
  g_autofree char *s1 = NULL;
  int progress_count;
  g_autoptr(GMainLoop) loop = NULL;
  guint quit_id;
  gboolean res;
  const char *bwrap = g_getenv ("FLATPAK_BWRAP");

  if (bwrap != NULL)
    {
      gint exit_code = 0;
      char *argv[] = { (char *)bwrap, "--ro-bind", "/", "/", "/bin/true", NULL };
      g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &exit_code, &error);
      g_assert_no_error (error);
      if (exit_code != 0)
        {
          g_test_skip ("bwrap not supported");
          return;
        }
    }

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  monitor = flatpak_installation_create_monitor (inst, NULL, &error);
  g_file_monitor_set_rate_limit (monitor, 100);
  g_assert (G_IS_FILE_MONITOR (monitor));
  g_assert_no_error (error);

  loop = g_main_loop_new (NULL, TRUE);

  g_signal_connect (monitor, "changed", G_CALLBACK (changed_cb), loop);

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (refs->len, ==, 0);
  g_ptr_array_unref (refs);

  changed_count = 0;
  progress_count = 0;
  ref = flatpak_installation_install (inst,
                                      repo_name,
                                      FLATPAK_REF_KIND_RUNTIME,
                                      "org.test.Platform",
                                      NULL,
                                      NULL,
                                      progress_cb,
                                      &progress_count,
                                      NULL,
                                      &error);
  g_assert_no_error (error);
  g_assert (FLATPAK_IS_INSTALLED_REF (ref));
  g_assert_cmpint (progress_count, >, 0);

  quit_id = g_timeout_add (500, quit, NULL);
  g_main_loop_run (loop);
  g_source_remove (quit_id);

  g_assert_cmpint (changed_count, >, 0);

  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (ref)), ==, "org.test.Platform");
  g_assert_cmpstr (flatpak_ref_get_arch (FLATPAK_REF (ref)), ==, flatpak_get_default_arch ());
  g_assert_cmpstr (flatpak_ref_get_branch (FLATPAK_REF (ref)), ==, "master");
  g_assert_cmpint (flatpak_ref_get_kind (FLATPAK_REF (ref)), ==, FLATPAK_REF_KIND_RUNTIME);

  g_assert_cmpuint (flatpak_installed_ref_get_installed_size (ref), >, 0);

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (refs->len, ==, 1);

  ref1 = g_ptr_array_index (refs, 0);
  g_assert_cmpstr (flatpak_ref_get_commit (FLATPAK_REF (ref1)), ==, flatpak_ref_get_commit (FLATPAK_REF (ref)));

  s = flatpak_ref_format_ref (FLATPAK_REF (ref));
  s1 = flatpak_ref_format_ref (FLATPAK_REF (ref1));
  g_assert_cmpstr (s, ==, s1);

  g_ptr_array_unref (refs);

  runtime_ref = g_object_ref (ref);
  g_clear_object (&ref);

  changed_count = 0;
  progress_count = 0;
  ref = flatpak_installation_install (inst,
                                      repo_name,
                                      FLATPAK_REF_KIND_APP,
                                      "org.test.Hello",
                                      NULL,
                                      NULL,
                                      progress_cb,
                                      &progress_count,
                                      NULL,
                                      &error);
  g_assert_no_error (error);
  g_assert (FLATPAK_IS_INSTALLED_REF (ref));
  g_assert_cmpint (progress_count, >, 0);

  quit_id = g_timeout_add (500, quit, loop);
  g_main_loop_run (loop);
  g_source_remove (quit_id);

  g_assert_cmpint (changed_count, >, 0);

  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (ref)), ==, "org.test.Hello");
  g_assert_cmpstr (flatpak_ref_get_arch (FLATPAK_REF (ref)), ==, flatpak_get_default_arch ());
  g_assert_cmpstr (flatpak_ref_get_branch (FLATPAK_REF (ref)), ==, "master");
  g_assert_cmpint (flatpak_ref_get_kind (FLATPAK_REF (ref)), ==, FLATPAK_REF_KIND_APP);

  g_assert_cmpuint (flatpak_installed_ref_get_installed_size (ref), >, 0);
  g_assert_true (flatpak_installed_ref_get_is_current (ref));

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (refs->len, ==, 2);

  g_ptr_array_unref (refs);

  res = flatpak_installation_launch (inst, "org.test.Hello", NULL, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  quit_id = g_timeout_add (500, quit, loop);
  g_main_loop_run (loop);
  g_source_remove (quit_id);

  changed_count = 0;
  progress_count = 0;
  res = flatpak_installation_uninstall (inst,
                                        flatpak_ref_get_kind (FLATPAK_REF (ref)),
                                        flatpak_ref_get_name (FLATPAK_REF (ref)),
                                        flatpak_ref_get_arch (FLATPAK_REF (ref)),
                                        flatpak_ref_get_branch (FLATPAK_REF (ref)),
                                        progress_cb,
                                        &progress_count,
                                        NULL,
                                        &error);
  g_assert_no_error (error);
  g_assert_true (res);
  //FIXME: no progress for uninstall
  //g_assert_cmpint (progress_count, >, 0);

  quit_id = g_timeout_add (500, quit, loop);
  g_main_loop_run (loop);
  g_source_remove (quit_id);

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (refs->len, ==, 1);

  g_ptr_array_unref (refs);

  changed_count = 0;
  progress_count = 0;
  res = flatpak_installation_uninstall (inst,
                                        flatpak_ref_get_kind (FLATPAK_REF (runtime_ref)),
                                        flatpak_ref_get_name (FLATPAK_REF (runtime_ref)),
                                        flatpak_ref_get_arch (FLATPAK_REF (runtime_ref)),
                                        flatpak_ref_get_branch (FLATPAK_REF (runtime_ref)),
                                        progress_cb,
                                        &progress_count,
                                        NULL,
                                        &error);
  g_assert_no_error (error);
  g_assert_true (res);
  //FIXME: no progress for uninstall
  //g_assert_cmpint (progress_count, >, 0);

  quit_id = g_timeout_add (500, quit, loop);
  g_main_loop_run (loop);
  g_source_remove (quit_id);

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (refs->len, ==, 0);

  g_ptr_array_unref (refs);
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

  if (port[strlen (port) - 1] == '\n')
    port[strlen (port) - 1] = '\0';

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
  if (g_test_verbose ())
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
  int status;
  g_autoptr (GError) error = NULL;
  char *argv[] = { "gpg-connect-agent", "--homedir", "<placeholder>", "killagent", "/bye", NULL };
  GSpawnFlags flags = G_SPAWN_SEARCH_PATH;

  if (g_getenv ("SKIP_TEARDOWN"))
    return;

  argv[2] = gpg_homedir;

  if (g_test_verbose ())
    {
      g_autofree char *commandline = g_strjoinv (" ", argv);
      g_print ("running %s\n", commandline);
    }
  else
    {
      flags |= G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL;
    }

  /* mostly ignore failure here */
  if (!g_spawn_sync (NULL, (char **)argv, NULL, flags, NULL, NULL, NULL, NULL, &status, &error) ||
      !g_spawn_check_exit_status (status, &error))
    {
      g_print ("# failed to run gpg-connect-agent to stop gpg-agent: %s\n", error->message);
    }

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
  g_test_add_func ("/library/ref", test_ref);
  g_test_add_func ("/library/list-remotes", test_list_remotes);
  g_test_add_func ("/library/remote-by-name", test_remote_by_name);
  g_test_add_func ("/library/remote", test_remote);
  g_test_add_func ("/library/list-refs", test_list_refs);
  g_test_add_func ("/library/install-launch-uninstall", test_install_launch_uninstall);

  global_setup ();

  res = g_test_run ();

  global_teardown ();

  return res;
}
