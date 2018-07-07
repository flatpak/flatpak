#include "config.h"

#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <glib.h>
#include <ostree.h>

#include "libglnx/libglnx.h"
#include "flatpak.h"

static char *testdir;
static char *flatpak_runtimedir;
static char *flatpak_systemdir;
static char *flatpak_systemcachedir;
static char *flatpak_configdir;
static char *flatpak_installationsdir;
static char *gpg_homedir;
static char *gpg_args;
static char *repo_url;
static char *repo_collection_id;
int httpd_pid = -1;

static const char *gpg_id = "7B0961FD";
const char *repo_name = "test-repo";

typedef enum {
  RUN_TEST_SUBPROCESS_DEFAULT = 0,
  RUN_TEST_SUBPROCESS_IGNORE_FAILURE = (1 << 0),
  RUN_TEST_SUBPROCESS_NO_CAPTURE = (1 << 1),
} RunTestSubprocessFlags;

static void run_test_subprocess (char                 **argv,
                                 RunTestSubprocessFlags flags);

typedef struct
{
  const char        *id;
  const char        *display_name;
  gint               priority;
  FlatpakStorageType storage_type;
} InstallationExtraData;

static void
test_library_version (void)
{
  g_autofree char *version = NULL;

  version = g_strdup_printf ("%d.%d.%d" G_STRINGIFY (PACKAGE_EXTRA_VERSION),
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
  g_assert_no_error (error);
  g_assert_nonnull (inst);

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
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  g_assert_false (flatpak_installation_get_is_user (inst));

  dir = flatpak_installation_get_path (inst);
  path = g_file_get_path (dir);
  g_assert_cmpstr (path, ==, flatpak_systemdir);
}

static void
test_multiple_system_installations (void)
{
  /* This is sorted according to the specific priority of each installation */
  static InstallationExtraData expected_installations[] = {
    { "extra-installation-2", "Extra system installation 2", 25, FLATPAK_STORAGE_TYPE_SDCARD},
    { "extra-installation-1", "Extra system installation 1", 10, FLATPAK_STORAGE_TYPE_MMC},
    { "extra-installation-3", NULL, 0, FLATPAK_STORAGE_TYPE_DEFAULT},
    { "default", "Default system directory", 0, FLATPAK_STORAGE_TYPE_DEFAULT},
  };

  g_autoptr(GPtrArray) system_dirs = NULL;
  g_autoptr(GError) error = NULL;

  FlatpakInstallation *installation = NULL;
  const char *current_id = NULL;
  const char *current_display_name = NULL;
  gint current_priority = 0;
  FlatpakStorageType current_storage_type = FLATPAK_STORAGE_TYPE_DEFAULT;
  int i;

  system_dirs = flatpak_get_system_installations (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (system_dirs);
  g_assert_cmpint (system_dirs->len, ==, 4);

  for (i = 0; i < system_dirs->len; i++)
    {
      g_autoptr(FlatpakInstallation) new_install = NULL;
      g_autoptr(GFile) installation_path = NULL;
      g_autofree char *path_str = NULL;

      installation = (FlatpakInstallation *) g_ptr_array_index (system_dirs, i);
      g_assert_false (flatpak_installation_get_is_user (installation));

      installation_path = flatpak_installation_get_path (installation);
      g_assert_nonnull (installation_path);

      current_id = flatpak_installation_get_id (installation);
      g_assert_cmpstr (current_id, ==, expected_installations[i].id);

      path_str = g_file_get_path (installation_path);
      if (g_strcmp0 (current_id, "default") == 0)
        g_assert_cmpstr (path_str, ==, flatpak_systemdir);
      else
        g_assert_cmpstr (path_str, !=, flatpak_systemdir);

      current_display_name = flatpak_installation_get_display_name (installation);
      g_assert_cmpstr (current_display_name, ==, expected_installations[i].display_name);

      current_priority = flatpak_installation_get_priority (installation);
      g_assert_cmpint (current_priority, ==, expected_installations[i].priority);

      current_storage_type = flatpak_installation_get_storage_type (installation);
      g_assert_cmpint (current_storage_type, ==, expected_installations[i].storage_type);

      /* Now test that flatpak_installation_new_system_with_id() works too */

      new_install = flatpak_installation_new_system_with_id (current_id, NULL, &error);
      g_assert_nonnull (new_install);

      g_assert_cmpstr (current_id, ==, flatpak_installation_get_id (new_install));
      g_assert_cmpstr (current_display_name, ==, flatpak_installation_get_display_name (new_install));
      g_assert_cmpint (current_priority, ==, flatpak_installation_get_priority (new_install));
      g_assert_cmpint (current_storage_type, ==, flatpak_installation_get_storage_type (new_install));
    }
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
  g_assert_no_error (error);
  g_assert (FLATPAK_IS_REF (ref));
  g_assert_cmpint (flatpak_ref_get_kind (ref), ==, FLATPAK_REF_KIND_APP);
  g_assert_cmpstr (flatpak_ref_get_name (ref), ==, "org.flatpak.Hello");
  g_assert_cmpstr (flatpak_ref_get_arch (ref), ==, "x86_64");
  g_assert_cmpstr (flatpak_ref_get_branch (ref), ==, "master");
  g_assert_null (flatpak_ref_get_collection_id (ref));

  formatted = flatpak_ref_format_ref (ref);
  g_assert_cmpstr (formatted, ==, valid);
}

static void
test_list_remotes (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) remotes = NULL;
  g_autoptr(GPtrArray) remotes2 = NULL;
  FlatpakRemote *remote;
  const FlatpakRemoteType types[] = { FLATPAK_REMOTE_TYPE_STATIC };
  const FlatpakRemoteType types2[] = { FLATPAK_REMOTE_TYPE_LAN };

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  remotes = flatpak_installation_list_remotes (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (remotes);
  g_assert (remotes->len == 1);

  remote = g_ptr_array_index (remotes, 0);
  g_assert (FLATPAK_IS_REMOTE (remote));

  remotes2 = flatpak_installation_list_remotes_by_type (inst, types,
                                                        G_N_ELEMENTS (types),
                                                        NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (remotes2->len, ==, remotes->len);

  for (guint i = 0; i < remotes->len; ++i)
    {
      FlatpakRemote *remote1 = g_ptr_array_index (remotes, i);
      FlatpakRemote *remote2 = g_ptr_array_index (remotes2, i);
      g_assert_cmpstr (flatpak_remote_get_name (remote1), ==,
                       flatpak_remote_get_name (remote2));
      g_assert_cmpstr (flatpak_remote_get_url (remote1), ==,
                       flatpak_remote_get_url (remote2));
    }

  g_ptr_array_unref (remotes2);
  remotes2 = flatpak_installation_list_remotes_by_type (inst,
                                                        types2,
                                                        G_N_ELEMENTS (types2),
                                                        NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpuint (remotes2->len, ==, 0);
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
  g_assert_cmpint (flatpak_remote_get_remote_type (remote), ==, FLATPAK_REMOTE_TYPE_STATIC);
  g_assert_false (flatpak_remote_get_noenumerate (remote));
  g_assert_false (flatpak_remote_get_disabled (remote));
  g_assert_true (flatpak_remote_get_gpg_verify (remote));
  g_assert_cmpint (flatpak_remote_get_prio (remote), ==, 1);

  g_assert_cmpstr (flatpak_remote_get_collection_id (remote), ==, repo_collection_id);
}

static void
test_remote (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakRemote) remote = NULL;
  g_autoptr(GFile) inst_file = NULL;
  g_autoptr(GFile) repo_file = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  gboolean gpg_verify_summary;
  gboolean res;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  remote = flatpak_installation_get_remote_by_name (inst, repo_name, NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpstr (flatpak_remote_get_collection_id (remote), ==, repo_collection_id);

  /* Flatpak doesn't provide access to gpg-verify-summary, so use ostree */
  res = flatpak_installation_modify_remote (inst, remote, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);
  inst_file = flatpak_installation_get_path (inst);
  repo_file = g_file_get_child (inst_file, "repo");
  repo = ostree_repo_new (repo_file);
  res = ostree_repo_open (repo, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);
  res = ostree_repo_get_remote_boolean_option (repo, repo_name, "gpg-verify-summary", TRUE, &gpg_verify_summary, &error);
  g_assert_no_error (error);
  g_assert_true (res);
  g_assert_false (gpg_verify_summary);

  /* Temporarily unset the collection ID */
  flatpak_remote_set_collection_id (remote, NULL);
  g_assert_cmpstr (flatpak_remote_get_collection_id (remote), ==, NULL);

  res = flatpak_installation_modify_remote (inst, remote, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);
  res = ostree_repo_reload_config (repo, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);
  res = ostree_repo_get_remote_boolean_option (repo, repo_name, "gpg-verify-summary", FALSE, &gpg_verify_summary, &error);
  g_assert_no_error (error);
  g_assert_true (res);
  g_assert_true (gpg_verify_summary);

  flatpak_remote_set_collection_id (remote, repo_collection_id);
  g_assert_cmpstr (flatpak_remote_get_collection_id (remote), ==, repo_collection_id);

  g_assert_cmpstr (flatpak_remote_get_title (remote), ==, NULL);
  flatpak_remote_set_title (remote, "Test Repo");
  g_assert_cmpstr (flatpak_remote_get_title (remote), ==, "Test Repo");

  g_assert_cmpint (flatpak_remote_get_prio (remote), ==, 1);
  flatpak_remote_set_prio (remote, 15);
  g_assert_cmpint (flatpak_remote_get_prio (remote), ==, 15);

  res = flatpak_installation_modify_remote (inst, remote, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

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
create_multi_collection_id_repo (const char *repo_dir)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *arg0 = NULL;

  /* Create a repository in which each app has a different collection-id */
  arg0 = g_test_build_filename (G_TEST_DIST, "make-multi-collection-id-repo.sh", NULL);
  const char *argv[] = { arg0, repo_dir, NULL };
  run_test_subprocess ((char **) argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
test_list_refs_in_remotes (void)
{
  const char *repo_name = "multi-refs-repo";
  g_autofree char *repo_url = NULL;

  g_autoptr(GPtrArray) refs1 = NULL;
  g_autoptr(GPtrArray) refs2 = NULL;
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakRemote) remote = NULL;
  g_autofree char *repo_dir = g_build_filename (testdir, repo_name, NULL);
  g_autofree char *repo_uri = NULL;
  g_autoptr(GHashTable) collection_ids = g_hash_table_new_full (g_str_hash,
                                                                g_str_equal,
                                                                NULL, NULL);
  g_autoptr(GHashTable) ref_specs = g_hash_table_new_full (g_str_hash,
                                                           g_str_equal,
                                                           g_free,
                                                           NULL);

  create_multi_collection_id_repo (repo_dir);

  repo_url = g_strdup_printf ("file://%s", repo_dir);

  const char *argv[] = { "flatpak", "remote-add", "--user", "--no-gpg-verify",
                         repo_name, repo_url, NULL };

  /* Add the repo we created above, which holds one collection ID per ref */
  run_test_subprocess ((char **) argv, RUN_TEST_SUBPROCESS_DEFAULT);

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  /* Ensure the remote can be successfully found */
  remote = flatpak_installation_get_remote_by_name (inst, repo_name, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (remote);

  /* List the refs in the remote we've just added */
  refs1 = flatpak_installation_list_remote_refs_sync (inst, repo_name, NULL, &error);

  g_assert_no_error (error);
  g_assert_nonnull (refs1);
  g_assert (refs1->len > 1);

  /* Ensure that the number of different collection IDs is the same as the
   * number of apps */
  for (guint i = 0; i < refs1->len; ++i)
    {
      FlatpakRef *ref = g_ptr_array_index (refs1, i);
      g_hash_table_add (collection_ids, (gchar *) flatpak_ref_get_collection_id (ref));
      g_hash_table_add (ref_specs, flatpak_ref_format_ref (ref));
    }

  g_assert_cmpuint (g_hash_table_size (collection_ids), ==, refs1->len);

  /* Ensure that listing the refs by using a remote's URI will get us the
   * same results as using the name */
  repo_uri = flatpak_remote_get_url (remote);
  refs2 = flatpak_installation_list_remote_refs_sync (inst, repo_uri, NULL, &error);

  g_assert_no_error (error);
  g_assert_nonnull (refs2);
  g_assert_cmpuint (refs2->len, ==, refs1->len);

  for (guint i = 0; i < refs2->len; ++i)
    {
      FlatpakRef *ref = g_ptr_array_index (refs2, i);
      g_autofree char *ref_spec = flatpak_ref_format_ref (ref);
      g_assert_nonnull (g_hash_table_lookup (ref_specs, ref_spec));
    }
}

static void
test_list_remote_refs (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) refs = NULL;
  int i;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  refs = flatpak_installation_list_remote_refs_sync (inst, repo_name, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (refs);
  g_assert_cmpint (refs->len, ==, 2);


  for (i = 0; i < refs->len; i++)
    {
      FlatpakRemoteRef *remote_ref = g_ptr_array_index (refs, i);
      FlatpakRef *ref = FLATPAK_REF (remote_ref);
      GBytes *metadata;

      g_assert (ref != NULL);

      if (strcmp ("org.test.Hello", flatpak_ref_get_name (ref)) == 0)
        {
          g_assert_cmpint (flatpak_ref_get_kind (ref), ==, FLATPAK_REF_KIND_APP);
        }
      else
        {
          g_assert_cmpstr (flatpak_ref_get_name (ref), ==, "org.test.Platform");
          g_assert_cmpint (flatpak_ref_get_kind (ref), ==, FLATPAK_REF_KIND_RUNTIME);
        }

      g_assert_cmpstr (flatpak_ref_get_branch (ref), ==, "master");
      g_assert_cmpstr (flatpak_ref_get_commit (ref), !=, NULL);
      g_assert_cmpstr (flatpak_ref_get_arch (ref), ==, flatpak_get_default_arch ());

      g_assert_cmpstr (flatpak_remote_ref_get_remote_name (remote_ref), ==, repo_name);
      g_assert_cmpstr (flatpak_remote_ref_get_eol (remote_ref), ==, NULL);
      g_assert_cmpstr (flatpak_remote_ref_get_eol_rebase (remote_ref), ==, NULL);

      g_assert_cmpuint (flatpak_remote_ref_get_installed_size (remote_ref), >, 0);
      g_assert_cmpuint (flatpak_remote_ref_get_download_size (remote_ref), >, 0);

      metadata = flatpak_remote_ref_get_metadata (remote_ref);
      g_assert (metadata != NULL);

      if (strcmp ("org.test.Hello", flatpak_ref_get_name (ref)) == 0)
        g_assert (g_str_has_prefix ((char *) g_bytes_get_data (metadata, NULL), "[Application]"));
      else
        g_assert (g_str_has_prefix ((char *) g_bytes_get_data (metadata, NULL), "[Runtime]"));
    }
}

static void
progress_cb (const char *status,
             guint       progress,
             gboolean    estimating,
             gpointer    user_data)
{
  int *count = user_data;

  *count += 1;
}

static void
changed_cb (GFileMonitor     *monitor,
            GFile            *file,
            GFile            *other_file,
            GFileMonitorEvent event_type,
            gpointer          user_data)
{
  int *count = user_data;

  *count += 1;
}

static gboolean
timeout_cb (gpointer data)
{
  gboolean *timeout_reached = data;

  *timeout_reached = TRUE;
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
  int progress_count, changed_count;
  gboolean timeout_reached;
  guint timeout_id;
  gboolean res;
  const char *bwrap = g_getenv ("FLATPAK_BWRAP");

  if (bwrap != NULL)
    {
      gint exit_code = 0;
      char *argv[] = { (char *) bwrap, "--unshare-ipc", "--unshare-net",
                       "--unshare-pid", "--ro-bind", "/", "/", "/bin/true", NULL };
      g_autofree char *argv_str = g_strjoinv (" ", argv);
      g_test_message ("Spawning %s", argv_str);
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
  g_assert_no_error (error);
  g_assert (G_IS_FILE_MONITOR (monitor));
  g_file_monitor_set_rate_limit (monitor, 100);

  g_signal_connect (monitor, "changed", G_CALLBACK (changed_cb), &changed_count);

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (refs->len, ==, 0);
  g_ptr_array_unref (refs);

  changed_count = 0;
  progress_count = 0;
  timeout_reached = FALSE;
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

  timeout_id = g_timeout_add (20000, timeout_cb, &timeout_reached);
  while (!timeout_reached && changed_count == 0)
    g_main_context_iteration (NULL, TRUE);
  g_source_remove (timeout_id);

  g_assert_cmpint (changed_count, >, 0);

  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (ref)), ==, "org.test.Platform");
  g_assert_cmpstr (flatpak_ref_get_arch (FLATPAK_REF (ref)), ==, flatpak_get_default_arch ());
  g_assert_cmpstr (flatpak_ref_get_branch (FLATPAK_REF (ref)), ==, "master");
  g_assert_cmpint (flatpak_ref_get_kind (FLATPAK_REF (ref)), ==, FLATPAK_REF_KIND_RUNTIME);
  g_assert_null (flatpak_ref_get_collection_id (FLATPAK_REF (ref)));

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
  timeout_reached = FALSE;
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

  timeout_id = g_timeout_add (20000, timeout_cb, &timeout_reached);
  while (!timeout_reached && changed_count == 0)
    g_main_context_iteration (NULL, TRUE);
  g_source_remove (timeout_id);

  g_assert_cmpint (changed_count, >, 0);

  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (ref)), ==, "org.test.Hello");
  g_assert_cmpstr (flatpak_ref_get_arch (FLATPAK_REF (ref)), ==, flatpak_get_default_arch ());
  g_assert_cmpstr (flatpak_ref_get_branch (FLATPAK_REF (ref)), ==, "master");
  g_assert_cmpint (flatpak_ref_get_kind (FLATPAK_REF (ref)), ==, FLATPAK_REF_KIND_APP);
  g_assert_null (flatpak_ref_get_collection_id (FLATPAK_REF (ref)));

  g_assert_cmpuint (flatpak_installed_ref_get_installed_size (ref), >, 0);
  g_assert_true (flatpak_installed_ref_get_is_current (ref));

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (refs->len, ==, 2);

  g_ptr_array_unref (refs);

  res = flatpak_installation_launch (inst, "org.test.Hello", NULL, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  timeout_reached = FALSE;
  timeout_id = g_timeout_add (500, timeout_cb, &timeout_reached);
  while (!timeout_reached)
    g_main_context_iteration (NULL, TRUE);
  g_source_remove (timeout_id);

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

  timeout_reached = FALSE;
  timeout_id = g_timeout_add (500, timeout_cb, &timeout_reached);
  while (!timeout_reached && changed_count == 0)
    g_main_context_iteration (NULL, TRUE);
  g_source_remove (timeout_id);

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

  timeout_reached = FALSE;
  timeout_id = g_timeout_add (500, timeout_cb, &timeout_reached);
  while (!timeout_reached && changed_count == 0)
    g_main_context_iteration (NULL, TRUE);
  g_source_remove (timeout_id);

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (refs->len, ==, 0);

  g_ptr_array_unref (refs);
}

static void update_test_app (void);
static void update_repo (void);

static void
test_list_updates (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) refs = NULL;
  g_autoptr(FlatpakInstalledRef) ref = NULL;
  g_autoptr(FlatpakInstalledRef) runtime_ref = NULL;
  FlatpakInstalledRef *update_ref = NULL;
  gboolean res;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  /* Install a runtime and app */
  runtime_ref = flatpak_installation_install (inst,
                                              repo_name,
                                              FLATPAK_REF_KIND_RUNTIME,
                                              "org.test.Platform",
                                              NULL, NULL, NULL, NULL, NULL,
                                              &error);
  g_assert_no_error (error);
  g_assert (FLATPAK_IS_INSTALLED_REF (runtime_ref));

  ref = flatpak_installation_install (inst,
                                      repo_name,
                                      FLATPAK_REF_KIND_APP,
                                      "org.test.Hello",
                                      NULL, NULL, NULL, NULL, NULL,
                                      &error);
  g_assert_no_error (error);
  g_assert (FLATPAK_IS_INSTALLED_REF (ref));

  /* Update the test app and list the update */
  update_test_app ();
  update_repo ();

  /* Drop all in-memory summary caches so we can find the new update */
  flatpak_installation_drop_caches (inst, NULL, &error);
  g_assert_no_error (error);

  refs = flatpak_installation_list_installed_refs_for_update (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (refs);
  g_assert_cmpint (refs->len, ==, 1);
  update_ref = g_ptr_array_index (refs, 0);
  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (update_ref)), ==, "org.test.Hello");
  g_assert_cmpint (flatpak_ref_get_kind (FLATPAK_REF (update_ref)), ==, FLATPAK_REF_KIND_APP);

  /* Uninstall the runtime and app */
  res = flatpak_installation_uninstall (inst,
                                        flatpak_ref_get_kind (FLATPAK_REF (ref)),
                                        flatpak_ref_get_name (FLATPAK_REF (ref)),
                                        flatpak_ref_get_arch (FLATPAK_REF (ref)),
                                        flatpak_ref_get_branch (FLATPAK_REF (ref)),
                                        NULL, NULL, NULL,
                                        &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_installation_uninstall (inst,
                                        flatpak_ref_get_kind (FLATPAK_REF (runtime_ref)),
                                        flatpak_ref_get_name (FLATPAK_REF (runtime_ref)),
                                        flatpak_ref_get_arch (FLATPAK_REF (runtime_ref)),
                                        flatpak_ref_get_branch (FLATPAK_REF (runtime_ref)),
                                        NULL, NULL, NULL,
                                        &error);
  g_assert_no_error (error);
  g_assert_true (res);
}

static void
run_test_subprocess (char                 **argv,
                     RunTestSubprocessFlags flags)
{
  int status;

  g_autoptr(GError) error = NULL;
  g_autofree char *argv_str = g_strjoinv (" ", argv);
  g_autofree char *output = NULL;
  g_autofree char *errors = NULL;

  g_test_message ("Spawning %s", argv_str);

  if (flags & RUN_TEST_SUBPROCESS_NO_CAPTURE)
    g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL, NULL, &status, &error);
  else
    g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &output, &errors, &status, &error);

  g_assert_no_error (error);

  if (output != NULL && output[0] != '\0')
    {
      g_autofree char *escaped = g_strescape (output, NULL);

      g_test_message ("\"%s\" stdout: %s", argv_str, escaped);
    }

  if (errors != NULL && errors[0] != '\0')
    {
      g_autofree char *escaped = g_strescape (errors, NULL);

      g_test_message ("\"%s\" stderr: %s", argv_str, escaped);
    }

  g_test_message ("\"%s\" wait status: %d", argv_str, status);

  if (WIFEXITED (status))
    g_test_message ("\"%s\" exited %d", argv_str, WEXITSTATUS (status));

  if (WIFSIGNALED (status))
    g_test_message ("\"%s\" killed by signal %d", argv_str, WTERMSIG (status));

  if (g_spawn_check_exit_status (status, &error))
    return;
  else if (flags & RUN_TEST_SUBPROCESS_IGNORE_FAILURE)
    g_test_message ("\"%s\" failed: %s", argv_str, error->message);
  else
    g_assert_no_error (error);
}

static void
make_test_runtime (void)
{
  g_autofree char *arg0 = NULL;
  char *argv[] = {
    NULL, "test", "org.test.Platform", "", "bash", "ls", "cat", "echo", "readlink", NULL
  };

  arg0 = g_test_build_filename (G_TEST_DIST, "make-test-runtime.sh", NULL);
  argv[0] = arg0;
  argv[3] = repo_collection_id;

  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
make_test_app (void)
{
  g_autofree char *arg0 = NULL;
  char *argv[] = { NULL, "test", "", "", NULL };

  arg0 = g_test_build_filename (G_TEST_DIST, "make-test-app.sh", NULL);
  argv[0] = arg0;
  argv[3] = repo_collection_id;

  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
update_test_app (void)
{
  g_autofree char *arg0 = NULL;
  char *argv[] = { NULL, "test", "", "", "UPDATED", NULL };

  arg0 = g_test_build_filename (G_TEST_DIST, "make-test-app.sh", NULL);
  argv[0] = arg0;
  argv[3] = repo_collection_id;

  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
update_repo (void)
{
  char *argv[] = { "flatpak", "build-update-repo", "--gpg-homedir=", "--gpg-sign=", "repos/test", NULL };

  g_auto(GStrv) gpgargs = NULL;

  gpgargs = g_strsplit (gpg_args, " ", 0);
  argv[2] = gpgargs[0];
  argv[3] = gpgargs[1];
  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
launch_httpd (void)
{
  g_autofree char *path = g_test_build_filename (G_TEST_DIST, "test-webserver.sh", NULL);
  char *argv[] = {path, "repos", NULL };

  /* The web server puts itself in the background, so we can't wait
   * for EOF on its stdout, stderr */
  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_NO_CAPTURE);
}

static void
add_remote (void)
{
  g_autoptr(GError) error = NULL;
  char *argv[] = { "flatpak", "remote-add", "--user", "--gpg-import=", "--collection-id=", "name", "url", NULL };
  g_autofree char *gpgimport = NULL;
  g_autofree char *port = NULL;
  g_autofree char *pid = NULL;
  g_autofree char *collection_id_arg = NULL;

  launch_httpd ();

  g_file_get_contents ("httpd-pid", &pid, NULL, &error);
  g_assert_no_error (error);

  httpd_pid = atoi (pid);
  g_assert_cmpint (httpd_pid, !=, 0);

  g_file_get_contents ("httpd-port", &port, NULL, &error);
  g_assert_no_error (error);

  if (port[strlen (port) - 1] == '\n')
    port[strlen (port) - 1] = '\0';

  gpgimport = g_strdup_printf ("--gpg-import=%s/pubring.gpg", gpg_homedir);
  repo_url = g_strdup_printf ("http://127.0.0.1:%s/test", port);
  collection_id_arg = g_strdup_printf ("--collection-id=%s", repo_collection_id);

  argv[3] = gpgimport;
  argv[4] = collection_id_arg;
  argv[5] = (char *) repo_name;
  argv[6] = repo_url;
  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
add_extra_installation (const char *id,
                        const char *display_name,
                        const char *storage_type,
                        const char *priority)
{
  g_autofree char *conffile_path = NULL;
  g_autofree char *contents_string = NULL;
  g_autofree char *path = NULL;

  g_autoptr(GPtrArray) contents_array = NULL;
  g_autoptr(GError) error = NULL;

  path = g_strconcat (testdir, "/system-", id, NULL);
  g_mkdir_with_parents (path, S_IRWXU | S_IRWXG | S_IRWXO);

  contents_array = g_ptr_array_new_with_free_func ((GDestroyNotify) g_free);

  g_ptr_array_add (contents_array,
                   g_strdup_printf ("[Installation \"%s\"]\n"
                                    "Path=%s",
                                    id, path));

  if (display_name != NULL)
    g_ptr_array_add (contents_array, g_strdup_printf ("DisplayName=%s", display_name));

  if (storage_type != NULL)
    g_ptr_array_add (contents_array, g_strdup_printf ("StorageType=%s", storage_type));

  if (priority != NULL)
    g_ptr_array_add (contents_array, g_strdup_printf ("Priority=%s", priority));

  g_ptr_array_add (contents_array, NULL);
  contents_string = g_strjoinv ("\n", (char **) contents_array->pdata);

  conffile_path = g_strconcat (flatpak_installationsdir, "/", id, ".conf", NULL);
  g_file_set_contents (conffile_path, contents_string, -1, &error);
  g_assert_no_error (error);
}

static void
setup_multiple_installations (void)
{
  flatpak_installationsdir = g_strconcat (flatpak_configdir, "/installations.d", NULL);
  g_mkdir_with_parents (flatpak_installationsdir, S_IRWXU | S_IRWXG | S_IRWXO);

  add_extra_installation ("extra-installation-1", "Extra system installation 1", "mmc", "10");
  add_extra_installation ("extra-installation-2", "Extra system installation 2", "sdcard", "25");
  add_extra_installation ("extra-installation-3", NULL, NULL, NULL);
}

static void
setup_repo (void)
{
  repo_collection_id = "com.example.Test";

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

  g_test_message ("copying %s to %s", src, dest);

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
  g_autofree char *cachedir = NULL;
  g_autofree char *configdir = NULL;
  g_autofree char *datadir = NULL;
  g_autofree char *homedir = NULL;

  testdir = g_strdup ("/var/tmp/flatpak-test-XXXXXX");
  g_mkdtemp (testdir);
  g_test_message ("testdir: %s", testdir);

  homedir = g_strconcat (testdir, "/home", NULL);
  g_mkdir_with_parents (homedir, S_IRWXU | S_IRWXG | S_IRWXO);

  g_setenv ("HOME", homedir, TRUE);
  g_test_message ("setting HOME=%s", homedir);

  cachedir = g_strconcat (testdir, "/home/cache", NULL);
  g_mkdir_with_parents (cachedir, S_IRWXU | S_IRWXG | S_IRWXO);
  g_setenv ("XDG_CACHE_HOME", cachedir, TRUE);
  g_test_message ("setting XDG_CACHE_HOME=%s", cachedir);

  configdir = g_strconcat (testdir, "/home/config", NULL);
  g_mkdir_with_parents (configdir, S_IRWXU | S_IRWXG | S_IRWXO);
  g_setenv ("XDG_CONFIG_HOME", configdir, TRUE);
  g_test_message ("setting XDG_CONFIG_HOME=%s", configdir);

  datadir = g_strconcat (testdir, "/home/share", NULL);
  g_mkdir_with_parents (datadir, S_IRWXU | S_IRWXG | S_IRWXO);
  g_setenv ("XDG_DATA_HOME", datadir, TRUE);
  g_test_message ("setting XDG_DATA_HOME=%s", datadir);

  flatpak_runtimedir = g_strconcat (testdir, "/runtime", NULL);
  g_mkdir_with_parents (flatpak_runtimedir, S_IRWXU | S_IRWXG | S_IRWXO);
  g_test_message ("setting XDG_RUNTIME_DIR=%s", flatpak_runtimedir);

  flatpak_systemdir = g_strconcat (testdir, "/system", NULL);
  g_mkdir_with_parents (flatpak_systemdir, S_IRWXU | S_IRWXG | S_IRWXO);
  g_setenv ("FLATPAK_SYSTEM_DIR", flatpak_systemdir, TRUE);
  g_test_message ("setting FLATPAK_SYSTEM_DIR=%s", flatpak_systemdir);

  flatpak_systemcachedir = g_strconcat (testdir, "/system-cache", NULL);
  g_mkdir_with_parents (flatpak_systemcachedir, S_IRWXU | S_IRWXG | S_IRWXO);
  g_setenv ("FLATPAK_SYSTEM_CACHE_DIR", flatpak_systemcachedir, TRUE);
  g_test_message ("setting FLATPAK_SYSTEM_CACHE_DIR=%s", flatpak_systemcachedir);

  flatpak_configdir = g_strconcat (testdir, "/config", NULL);
  g_mkdir_with_parents (flatpak_configdir, S_IRWXU | S_IRWXG | S_IRWXO);
  g_setenv ("FLATPAK_CONFIG_DIR", flatpak_configdir, TRUE);
  g_test_message ("setting FLATPAK_CONFIG_DIR=%s", flatpak_configdir);

  gpg_homedir = g_strconcat (testdir, "/gpghome", NULL);
  g_mkdir_with_parents (gpg_homedir, S_IRWXU | S_IRWXG | S_IRWXO);

  gpg_args = g_strdup_printf ("--gpg-homedir=%s --gpg-sign=%s", gpg_homedir, gpg_id);
  g_setenv ("GPGARGS", gpg_args, TRUE);
  g_test_message ("setting GPGARGS=%s", gpg_args);

  copy_gpg ();
  setup_multiple_installations ();
  setup_repo ();
}

static void
global_teardown (void)
{
  char *argv[] = { "gpg-connect-agent", "--homedir", "<placeholder>", "killagent", "/bye", NULL };

  if (g_getenv ("SKIP_TEARDOWN"))
    return;

  argv[2] = gpg_homedir;

  if (httpd_pid != -1)
    kill (httpd_pid, SIGKILL);

  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_IGNORE_FAILURE);

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
  g_test_add_func ("/library/multiple-system-installation", test_multiple_system_installations);
  g_test_add_func ("/library/arches", test_arches);
  g_test_add_func ("/library/ref", test_ref);
  g_test_add_func ("/library/list-remotes", test_list_remotes);
  g_test_add_func ("/library/remote-by-name", test_remote_by_name);
  g_test_add_func ("/library/remote", test_remote);
  g_test_add_func ("/library/list-remote-refs", test_list_remote_refs);
  g_test_add_func ("/library/list-refs", test_list_refs);
  g_test_add_func ("/library/install-launch-uninstall", test_install_launch_uninstall);
  g_test_add_func ("/library/list-refs-in-remote", test_list_refs_in_remotes);
  g_test_add_func ("/library/list-updates", test_list_updates);

  global_setup ();

  res = g_test_run ();

  global_teardown ();

  return res;
}
