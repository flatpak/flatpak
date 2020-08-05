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
static char *httpd_port;
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
static void empty_installation (FlatpakInstallation *inst);

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
test_library_types (void)
{
  g_assert_true (g_type_is_a (FLATPAK_TYPE_REF, G_TYPE_OBJECT));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_INSTALLED_REF, FLATPAK_TYPE_REF));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_REMOTE_REF, FLATPAK_TYPE_REF));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_BUNDLE_REF, FLATPAK_TYPE_REF));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_RELATED_REF, FLATPAK_TYPE_REF));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_INSTALLATION, G_TYPE_OBJECT));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_INSTANCE, G_TYPE_OBJECT));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_REMOTE, G_TYPE_OBJECT));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_TRANSACTION, G_TYPE_OBJECT));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_TRANSACTION_OPERATION, G_TYPE_OBJECT));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_TRANSACTION_PROGRESS, G_TYPE_OBJECT));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_ERROR, G_TYPE_ENUM));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_PORTAL_ERROR, G_TYPE_ENUM));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_INSTALL_FLAGS, G_TYPE_FLAGS));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_UPDATE_FLAGS, G_TYPE_FLAGS));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_UNINSTALL_FLAGS, G_TYPE_FLAGS));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_STORAGE_TYPE, G_TYPE_ENUM));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_REF_KIND, G_TYPE_ENUM));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_REMOTE_TYPE, G_TYPE_ENUM));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_TRANSACTION_OPERATION_TYPE, G_TYPE_ENUM));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_TRANSACTION_ERROR_DETAILS, G_TYPE_FLAGS));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_TRANSACTION_RESULT, G_TYPE_FLAGS));
  g_assert_true (g_type_is_a (FLATPAK_TYPE_TRANSACTION_REMOTE_REASON, G_TYPE_ENUM));
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
    { "extra-installation-3", "System (extra-installation-3) installation", 0, FLATPAK_STORAGE_TYPE_DEFAULT},
    { "default", "Default system installation", 0, FLATPAK_STORAGE_TYPE_DEFAULT},
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
test_installation_config (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *value;
  gboolean res;
  guint64 bytes;

  path = g_build_filename (g_get_user_data_dir (), "flatpak", NULL);
  file = g_file_new_for_path (path);
  inst = flatpak_installation_new_for_path (file, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  value = flatpak_installation_get_config (inst, "test", NULL, &error);
  g_assert_null (value);
  g_assert_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND);
  g_clear_error (&error);

  res = flatpak_installation_set_config_sync (inst, "test", "hello", NULL, &error);
  g_assert_true (res);
  g_assert_no_error (error);

  value = flatpak_installation_get_config (inst, "test", NULL, &error);
  g_assert_cmpstr (value, ==, "hello");
  g_assert_no_error (error);
  g_clear_pointer (&value, g_free);

  g_clear_object (&inst);

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  value = flatpak_installation_get_config (inst, "test", NULL, &error);
  g_assert_cmpstr (value, ==, "hello");
  g_assert_no_error (error);
  g_clear_pointer (&value, g_free);

  res = flatpak_installation_get_min_free_space_bytes (inst, &bytes, &error);
  g_assert_true (res);
  g_assert_no_error (error);
}

static void
configure_languages (const char *lang)
{
  const char *argv[] = { "flatpak", "config", "--user", "--set", "languages", lang, NULL };

  run_test_subprocess ((char **)argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
clean_languages (void)
{
  char *argv[] = { "flatpak", "config", "--user", "--unset", "languages", NULL };

  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
configure_extra_languages (void)
{
  char *argv[] = { "flatpak", "config", "--user", "--set", "extra-languages", "de_DE", NULL };

  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
clean_extra_languages (void)
{
  char *argv[] = { "flatpak", "config", "--user", "--unset", "extra-languages", NULL };

  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
test_languages_config (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) value = NULL;
  gboolean res;

  clean_languages ();
  path = g_build_filename (g_get_user_data_dir (), "flatpak", NULL);
  file = g_file_new_for_path (path);
  inst = flatpak_installation_new_for_path (file, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  value = flatpak_installation_get_default_languages (inst, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (value[0], ==, "en");
  g_assert_null (value[1]);

  g_clear_pointer (&value, g_strfreev);

  res = flatpak_installation_set_config_sync (inst, "extra-languages", "pt_BR;uz_UZ.utf8@cyrillic;es;zh_HK.big5hkscs;uz_UZ@cyrillic", NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  value = flatpak_installation_get_default_languages (inst, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (value[0], ==, "en");
  g_assert_cmpstr (value[1], ==, "es");
  g_assert_cmpstr (value[2], ==, "pt");
  g_assert_cmpstr (value[3], ==, "uz");
  g_assert_cmpstr (value[4], ==, "zh");
  g_assert_null (value[5]);

  g_clear_pointer (&value, g_strfreev);

  value = flatpak_installation_get_default_locales (inst, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (value[0], ==, "en");
  g_assert_cmpstr (value[1], ==, "es");
  g_assert_cmpstr (value[2], ==, "pt_BR");
  g_assert_cmpstr (value[3], ==, "uz_UZ.utf8@cyrillic");
  g_assert_cmpstr (value[4], ==, "uz_UZ@cyrillic");
  g_assert_cmpstr (value[5], ==, "zh_HK.big5hkscs");
  g_assert_null (value[6]);

  g_clear_pointer (&value, g_strfreev);

  res = flatpak_installation_set_config_sync (inst, "languages", "ar;es", NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  value = flatpak_installation_get_default_languages (inst, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (value[0], ==, "ar");
  g_assert_cmpstr (value[1], ==, "es");
  g_assert_null (value[2]);

  g_clear_pointer (&value, g_strfreev);
  clean_extra_languages ();
  configure_languages ("de");
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
  g_assert_true (g_strv_contains (supported_arches, default_arch));
}

static void
test_ref (void)
{
  g_autoptr(FlatpakRef) ref = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *formatted = NULL;
  const char *valid;
  FlatpakRefKind kind;
  g_autofree char *name = NULL;
  g_autofree char *arch = NULL;
  g_autofree char *branch = NULL;
  g_autofree char *commit = NULL;
  g_autofree char *collection_id = NULL;

  ref = flatpak_ref_parse ("", &error);
  g_assert_null (ref);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  ref = flatpak_ref_parse ("ref/or not", &error);
  g_assert_null (ref);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  ref = flatpak_ref_parse ("ref/one/2/3", &error);
  g_assert_null (ref);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  ref = flatpak_ref_parse ("app/a/b/c", &error);
  g_assert_null (ref);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  ref = flatpak_ref_parse ("app/org.flatpak.Hello/b/.", &error);
  g_assert_null (ref);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  ref = flatpak_ref_parse ("foo/org.flatpak.Hello/b/.", &error);
  g_assert_null (ref);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  ref = flatpak_ref_parse ("app//m68k/master", &error);
  g_assert_null (ref);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  ref = flatpak_ref_parse ("app/org.test.Hello/m68k/", &error);
  g_assert_null (ref);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  ref = flatpak_ref_parse ("app/org.test.Hello/m68k/a[b]c", &error);
  g_assert_null (ref);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  ref = flatpak_ref_parse ("app/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                           "/m68k/master", &error);
  g_assert_null (ref);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  ref = flatpak_ref_parse ("app/.abc/m68k/master", &error);
  g_assert_null (ref);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  ref = flatpak_ref_parse ("app/0abc/m68k/master", &error);
  g_assert_null (ref);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_REF);
  g_clear_error (&error);

  valid = "app/org.flatpak.Hello/m68k/master";
  ref = flatpak_ref_parse (valid, &error);
  g_assert_no_error (error);
  g_assert_true (FLATPAK_IS_REF (ref));
  g_assert_cmpint (flatpak_ref_get_kind (ref), ==, FLATPAK_REF_KIND_APP);
  g_assert_cmpstr (flatpak_ref_get_name (ref), ==, "org.flatpak.Hello");
  g_assert_cmpstr (flatpak_ref_get_arch (ref), ==, "m68k");
  g_assert_cmpstr (flatpak_ref_get_branch (ref), ==, "master");
  g_assert_null (flatpak_ref_get_collection_id (ref));

  formatted = flatpak_ref_format_ref (ref);
  g_assert_cmpstr (formatted, ==, valid);
  g_clear_pointer (&formatted, g_free);

  g_clear_object (&ref);

  valid = "runtime/org.gnome.Platform/m68k/stable";
  ref = flatpak_ref_parse (valid, &error);
  g_assert_no_error (error);
  g_assert_true (FLATPAK_IS_REF (ref));

  g_object_get (ref,
                "kind", &kind,
                "name", &name,
                "arch", &arch,
                "branch", &branch,
                "commit", &commit,
                "collection-id", &collection_id,
                NULL);
  g_assert_cmpint (kind, ==, FLATPAK_REF_KIND_RUNTIME);
  g_assert_cmpstr (name, ==, "org.gnome.Platform");
  g_assert_cmpstr (arch, ==, "m68k");
  g_assert_cmpstr (branch, ==, "stable");
  g_assert_null (commit);
  g_assert_null (collection_id);

  formatted = flatpak_ref_format_ref (ref);
  g_assert_cmpstr (formatted, ==, valid);
  g_clear_pointer (&formatted, g_free);

  g_clear_object (&ref);

  ref = g_object_new (FLATPAK_TYPE_REF,
                      "kind", FLATPAK_REF_KIND_RUNTIME,
                      "name", "org.gnome.Platform",
                      "arch", "m68k",
                      "branch", "stable",
                      "commit", "0123456789",
                      "collection-id", "org.flathub.Stable",
                      NULL);

  g_assert_cmpstr (flatpak_ref_get_commit (ref), ==, "0123456789");
  g_assert_cmpstr (flatpak_ref_get_collection_id (ref), ==, "org.flathub.Stable");

  g_clear_object (&ref);
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
  gboolean res;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  res = flatpak_installation_update_remote_sync (inst, repo_name, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_installation_update_appstream_sync (inst, repo_name, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  remotes = flatpak_installation_list_remotes (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (remotes);
  g_assert_true (remotes->len == 1);

  remote = g_ptr_array_index (remotes, 0);
  g_assert_true (FLATPAK_IS_REMOTE (remote));

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
  g_autoptr(FlatpakRemote) remote = NULL;
  g_autofree char *name = NULL;
  FlatpakRemoteType type;
  g_autoptr(GFile) file = NULL;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  remote = flatpak_installation_get_remote_by_name (inst, repo_name, NULL, &error);
  g_assert_no_error (error);

  g_assert_true (FLATPAK_IS_REMOTE (remote));
  g_assert_cmpstr (flatpak_remote_get_name (remote), ==, repo_name);
  g_assert_cmpstr (flatpak_remote_get_url (remote), ==, repo_url);
  g_assert_cmpstr (flatpak_remote_get_title (remote), ==, NULL);
  g_assert_cmpint (flatpak_remote_get_remote_type (remote), ==, FLATPAK_REMOTE_TYPE_STATIC);
  g_assert_false (flatpak_remote_get_noenumerate (remote));
  g_assert_false (flatpak_remote_get_disabled (remote));
  g_assert_true (flatpak_remote_get_gpg_verify (remote));
  g_assert_cmpint (flatpak_remote_get_prio (remote), ==, 1);

  g_assert_cmpstr (flatpak_remote_get_collection_id (remote), ==, repo_collection_id);

  g_object_get (remote,
                "name", &name,
                "type", &type,
                NULL);

  g_assert_cmpstr (name, ==, repo_name);
  g_assert_cmpint (type, ==, FLATPAK_REMOTE_TYPE_STATIC);

  file = flatpak_remote_get_appstream_dir (remote, NULL);
  g_assert_nonnull (file);
  g_clear_object (&file);

  file = flatpak_remote_get_appstream_timestamp (remote, NULL);
  g_assert_nonnull (file);
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
  res = ostree_repo_get_remote_boolean_option (repo, repo_name, "gpg-verify-summary", FALSE, &gpg_verify_summary, &error);
  g_assert_no_error (error);
  g_assert_true (res);
  g_assert_true (gpg_verify_summary);

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

  g_assert_false (flatpak_remote_get_noenumerate (remote));
  flatpak_remote_set_noenumerate (remote, TRUE);
  g_assert_true (flatpak_remote_get_noenumerate (remote));

  g_assert_false (flatpak_remote_get_nodeps (remote));
  flatpak_remote_set_nodeps (remote, TRUE);
  g_assert_true (flatpak_remote_get_nodeps (remote));

  g_assert_false (flatpak_remote_get_disabled (remote));
  flatpak_remote_set_disabled (remote, TRUE);
  g_assert_true (flatpak_remote_get_disabled (remote));

  g_assert_null (flatpak_remote_get_default_branch (remote));
  flatpak_remote_set_default_branch (remote, "master");
  g_assert_cmpstr (flatpak_remote_get_default_branch (remote), ==, "master");

  /* It should be an error to disable GPG while a collection ID is set. */
  g_assert_true (flatpak_remote_get_gpg_verify (remote));
  flatpak_remote_set_gpg_verify (remote, FALSE);
  g_assert_false (flatpak_remote_get_gpg_verify (remote));
  res = flatpak_installation_modify_remote (inst, remote, NULL, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_DATA);
  g_clear_error (&error);
  g_assert_false (res);

  /* Unset the collection ID and try again. */
  flatpak_remote_set_collection_id (remote, NULL);
  g_assert_cmpstr (flatpak_remote_get_collection_id (remote), ==, NULL);
  g_assert_false (flatpak_remote_get_gpg_verify (remote));

  res = flatpak_installation_modify_remote (inst, remote, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_clear_object (&remote);

  remote = flatpak_installation_get_remote_by_name (inst, repo_name, NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpstr (flatpak_remote_get_title (remote), ==, "Test Repo");
  g_assert_cmpint (flatpak_remote_get_prio (remote), ==, 15);
  g_assert_true (flatpak_remote_get_noenumerate (remote));
  g_assert_true (flatpak_remote_get_nodeps (remote));
  g_assert_false (flatpak_remote_get_gpg_verify (remote));
  g_assert_cmpstr (flatpak_remote_get_default_branch (remote), ==, "master");

  /* back to defaults */
  flatpak_remote_set_title (remote, NULL);
  flatpak_remote_set_prio (remote, 1);
  flatpak_remote_set_noenumerate (remote, FALSE);
  flatpak_remote_set_nodeps (remote, FALSE);
  flatpak_remote_set_disabled (remote, FALSE);
  flatpak_remote_set_default_branch (remote, NULL);
  flatpak_remote_set_gpg_verify (remote, TRUE);
  flatpak_remote_set_collection_id (remote, repo_collection_id);

  res = flatpak_installation_modify_remote (inst, remote, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);
}

static void
test_remote_new (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(FlatpakRemote) remote = NULL;
  g_autoptr(GError) error = NULL;
  gboolean res;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  remote = flatpak_installation_get_remote_by_name (inst, "my-first-remote", NULL, &error);
  g_assert_null (remote);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_REMOTE_NOT_FOUND);
  g_clear_error (&error);

  remote = flatpak_remote_new ("my-first-remote");

  g_assert_null (flatpak_remote_get_appstream_dir (remote, NULL));
  g_assert_null (flatpak_remote_get_appstream_timestamp (remote, NULL));
  g_assert_null (flatpak_remote_get_url (remote));
  g_assert_null (flatpak_remote_get_collection_id (remote));
  g_assert_null (flatpak_remote_get_title (remote));
  g_assert_null (flatpak_remote_get_default_branch (remote));
  g_assert_false (flatpak_remote_get_noenumerate (remote));
  g_assert_false (flatpak_remote_get_nodeps (remote));
  g_assert_false (flatpak_remote_get_disabled (remote));
  g_assert_cmpint (flatpak_remote_get_prio (remote), ==, 1);
  g_assert_false (flatpak_remote_get_gpg_verify (remote));

  res = flatpak_installation_add_remote (inst, remote, FALSE, NULL, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_DATA);
  g_assert_false (res);
  g_clear_error (&error);

  flatpak_remote_set_url (remote, "http://127.0.0.1/nowhere");

  res = flatpak_installation_add_remote (inst, remote, FALSE, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_clear_object (&remote);

  remote = flatpak_installation_get_remote_by_name (inst, "my-first-remote", NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpstr (flatpak_remote_get_url (remote), ==, "http://127.0.0.1/nowhere");

  g_clear_object (&remote);

  remote = flatpak_remote_new ("my-first-remote");
  flatpak_remote_set_url (remote, "http://127.0.0.1/elsewhere");

  res = flatpak_installation_add_remote (inst, remote, FALSE, NULL, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED);
  g_clear_error (&error);
  g_assert_false (res);

  /* add if needed */
  res = flatpak_installation_add_remote (inst, remote, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_clear_object (&remote);

  remote = flatpak_installation_get_remote_by_name (inst, "my-first-remote", NULL, &error);
  g_assert_no_error (error);

  /* Should be the old value */
  g_assert_cmpstr (flatpak_remote_get_url (remote), ==, "http://127.0.0.1/nowhere");

  g_clear_object (&remote);

  res = flatpak_installation_remove_remote (inst, "my-first-remote", NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  remote = flatpak_installation_get_remote_by_name (inst, "my-first-remote", NULL, &error);
  g_assert_null (remote);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_REMOTE_NOT_FOUND);
  g_clear_error (&error);
}

static void
test_remote_new_from_file (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(FlatpakRemote) remote = NULL;
  g_autoptr(GError) error = NULL;
  gboolean res;
  const char *_data =
    "[Flatpak Repo]\n"
    "Url=http://127.0.0.1/repo\n"
    "Title=The Title\n"
    "Comment=The Comment\n"
    "Description=The Description\n"
    "Homepage=https://the.homepage/\n"
    "Icon=https://the.icon/\n"
    "DefaultBranch=default-branch\n"
    "NoDeps=true\n";
  g_autoptr(GBytes) data = g_bytes_new_static (_data, strlen (_data));

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  remote = flatpak_installation_get_remote_by_name (inst, "file-remote", NULL, &error);
  g_assert_null (remote);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_REMOTE_NOT_FOUND);
  g_clear_error (&error);

  remote = flatpak_remote_new_from_file ("file-remote", data, &error);
  g_assert_nonnull (remote);
  g_assert_no_error (error);

  res = flatpak_installation_add_remote (inst, remote, FALSE, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_clear_object (&remote);

  remote = flatpak_installation_get_remote_by_name (inst, "file-remote", NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpstr (flatpak_remote_get_url (remote), ==, "http://127.0.0.1/repo");
  g_assert_cmpstr (flatpak_remote_get_title (remote), ==, "The Title");
  g_assert_cmpstr (flatpak_remote_get_comment (remote), ==, "The Comment");
  g_assert_cmpstr (flatpak_remote_get_description (remote), ==, "The Description");
  g_assert_cmpstr (flatpak_remote_get_homepage (remote), ==, "https://the.homepage/");
  g_assert_cmpstr (flatpak_remote_get_icon (remote), ==, "https://the.icon/");
  g_assert_cmpstr (flatpak_remote_get_default_branch (remote), ==, "default-branch");
  g_assert_cmpint (flatpak_remote_get_nodeps (remote), ==, TRUE);

  g_clear_object (&remote);

  remote = flatpak_installation_get_remote_by_name (inst, "file-remote", NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpstr (flatpak_remote_get_filter (remote), ==, NULL);

  flatpak_remote_set_url (remote, "http://127.0.0.1/other");
  flatpak_remote_set_filter (remote, "/some/path/to/filter");

  res = flatpak_installation_modify_remote (inst, remote, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_clear_object (&remote);

  remote = flatpak_installation_get_remote_by_name (inst, "file-remote", NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpstr (flatpak_remote_get_url (remote), ==, "http://127.0.0.1/other");
  g_assert_cmpstr (flatpak_remote_get_filter (remote), ==, "/some/path/to/filter");

  g_clear_object (&remote);

  remote = flatpak_remote_new_from_file ("file-remote", data, &error);
  g_assert_nonnull (remote);
  g_assert_no_error (error);

  /* regular add fails */
  res = flatpak_installation_add_remote (inst, remote, FALSE, NULL, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED);
  g_clear_error (&error);
  g_assert_false (res);

  /* add if needed succeeds and resets filter */
  res = flatpak_installation_add_remote (inst, remote, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_clear_object (&remote);

  remote = flatpak_installation_get_remote_by_name (inst, "file-remote", NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpstr (flatpak_remote_get_url (remote), ==, "http://127.0.0.1/other");
  g_assert_cmpstr (flatpak_remote_get_filter (remote), ==, NULL);

  g_clear_object (&remote);
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
  g_assert_true (refs1->len > 1);

  /* Ensure that the number of different collection IDs is the same as the
   * number of apps */
  for (guint i = 0; i < refs1->len; ++i)
    {
      FlatpakRef *ref = g_ptr_array_index (refs1, i);
      g_hash_table_add (ref_specs, flatpak_ref_format_ref (ref));
    }

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
  g_assert_cmpint (refs->len, >, 1);


  for (i = 0; i < refs->len; i++)
    {
      FlatpakRemoteRef *remote_ref = g_ptr_array_index (refs, i);
      FlatpakRef *ref = FLATPAK_REF (remote_ref);
      GBytes *metadata;
      g_autoptr(GBytes) metadata2 = NULL;
      g_autofree char *name = NULL;
      guint64 installed_size;
      guint64 download_size;
      g_autofree char *eol = NULL;
      g_autofree char *eol_rebase = NULL;

      g_assert_true (ref != NULL);

      if (strcmp ("org.test.Hello", flatpak_ref_get_name (ref)) == 0)
        {
          g_assert_cmpint (flatpak_ref_get_kind (ref), ==, FLATPAK_REF_KIND_APP);
        }
      else if (strcmp ("org.test.Hello.Locale", flatpak_ref_get_name (ref)) == 0)
        {
          g_assert_cmpint (flatpak_ref_get_kind (ref), ==, FLATPAK_REF_KIND_RUNTIME);
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
      g_assert_true (metadata != NULL);

      if (strcmp ("org.test.Hello", flatpak_ref_get_name (ref)) == 0)
        g_assert_true (g_str_has_prefix ((char *) g_bytes_get_data (metadata, NULL), "[Application]"));
      else
        g_assert_true (g_str_has_prefix ((char *) g_bytes_get_data (metadata, NULL), "[Runtime]"));

      g_object_get (ref,
                    "remote-name", &name,
                    "installed-size", &installed_size,
                    "download-size", &download_size,
                    "metadata", &metadata2,
                    "end-of-life", &eol,
                    "end-of-life-rebase", &eol_rebase,
                    NULL);

      g_assert_cmpstr (name, ==, repo_name);
      g_assert_cmpuint (installed_size, >, 0);
      g_assert_cmpuint (download_size, >, 0);
      g_assert_true (metadata2 == metadata);
      g_assert_null (eol);
      g_assert_null (eol_rebase);
    }
}

static void
test_update_installed_ref_if_missing_runtime (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(GPtrArray) updatable_refs = NULL;
  g_autoptr(GError) error = NULL;
  FlatpakInstalledRef *iref = NULL;
  gboolean res;
  g_autofree char *app = NULL;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  empty_installation (inst);

  /* Install the app */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  iref = flatpak_installation_install (inst,
                                       repo_name,
                                       FLATPAK_REF_KIND_APP,
                                       "org.test.Hello",
                                       flatpak_get_default_arch (),
                                       "master",
                                       NULL, NULL, NULL, &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);
  g_assert_true (FLATPAK_IS_INSTALLED_REF (iref));
  iref = NULL;

  /* Install the Locale extension */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  iref = flatpak_installation_install (inst,
                                       repo_name,
                                       FLATPAK_REF_KIND_RUNTIME,
                                       "org.test.Hello.Locale",
                                       flatpak_get_default_arch (),
                                       "master",
                                       NULL, NULL, NULL, &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);
  g_assert_true (FLATPAK_IS_INSTALLED_REF (iref));
  iref = NULL;

  updatable_refs = flatpak_installation_list_installed_refs_for_update (inst, NULL, &error);
  g_assert_cmpint (updatable_refs->len, ==, 1);
  iref = g_ptr_array_index (updatable_refs, 0);
  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (iref)), ==, "org.test.Hello");
  iref = NULL;

  /* Prepare an update transaction to update org.test.Hello. The missing org.test.Platform
     runtime should automatically be installed with it. */
  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_update (transaction, app, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  iref = flatpak_installation_get_installed_ref (inst, FLATPAK_REF_KIND_RUNTIME, "org.test.Platform", NULL, NULL, NULL, &error);
  g_assert_nonnull (iref);
  g_assert_no_error (error);
}

static void
test_update_related_refs (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(GPtrArray) updatable_refs = NULL;
  g_autoptr(GPtrArray) related_refs = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakInstalledRef) iref = NULL;
  g_autoptr(FlatpakInstalledRef) runtime_ref = NULL;
  gboolean res;
  g_autofree char *app = NULL;
  g_autofree char *app_locale = NULL;
  const char * const *subpaths;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  empty_installation (inst);

  /* Install a runtime */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  runtime_ref = flatpak_installation_install (inst,
                                              repo_name,
                                              FLATPAK_REF_KIND_RUNTIME,
                                              "org.test.Platform",
                                              NULL, "master", NULL, NULL, NULL,
                                              &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);
  g_assert_true (FLATPAK_IS_INSTALLED_REF (runtime_ref));

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  iref = flatpak_installation_install (inst,
                                       repo_name,
                                       FLATPAK_REF_KIND_APP,
                                       "org.test.Hello",
                                       NULL, "master", NULL, NULL, NULL,
                                       &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);
  g_assert_true (FLATPAK_IS_INSTALLED_REF (iref));
  g_clear_object (&iref);

  /* We expect no installed related refs (i.e. org.test.Hello.Locale) at this point */
  related_refs = flatpak_installation_list_installed_related_refs_sync (inst, repo_name, app, NULL, &error);
  g_assert_cmpint (related_refs->len, ==, 0);

  updatable_refs = flatpak_installation_list_installed_refs_for_update (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (updatable_refs->len, ==, 1);
  iref = g_ptr_array_index (updatable_refs, 0);
  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (iref)), ==, "org.test.Hello");

  /* Prepare an update transaction to update org.test.Hello. The missing related .Locale
     extension should automatically be installed with it. */
  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_update (transaction, app, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  iref = flatpak_installation_get_installed_ref (inst, FLATPAK_REF_KIND_RUNTIME, "org.test.Hello.Locale", NULL, NULL, NULL, &error);
  g_assert_nonnull (iref);
  g_assert_no_error (error);

  /* Now check that when we have only subpaths of the locale extension
   * installed and we change the configured languages, it shows as needing an
   * update.
   */
  subpaths = flatpak_installed_ref_get_subpaths (iref);
  g_assert_cmpint (g_strv_length ((char **) subpaths), ==, 1);
  g_assert_cmpstr (subpaths[0], ==, "/de");
  g_clear_object (&iref);

  configure_languages ("es");
  flatpak_installation_drop_caches (inst, NULL, &error);
  g_assert_no_error (error);

  g_clear_pointer (&updatable_refs, g_ptr_array_unref);
  updatable_refs = flatpak_installation_list_installed_refs_for_update (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (updatable_refs->len, ==, 1);
  iref = g_ptr_array_index (updatable_refs, 0);
  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (iref)), ==, "org.test.Hello.Locale");

  /* Now update org.test.Hello.Locale and check that the subpaths are updated. */
  g_clear_object (&transaction);
  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  app_locale = g_strdup_printf ("runtime/org.test.Hello.Locale/%s/master",
                                flatpak_get_default_arch ());

  res = flatpak_transaction_add_update (transaction, app_locale, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  flatpak_installation_drop_caches (inst, NULL, &error);
  g_assert_no_error (error);
  iref = flatpak_installation_get_installed_ref (inst, FLATPAK_REF_KIND_RUNTIME, "org.test.Hello.Locale", NULL, NULL, NULL, &error);
  g_assert_nonnull (iref);
  g_assert_no_error (error);

  subpaths = flatpak_installed_ref_get_subpaths (iref);
  g_assert_cmpint (g_strv_length ((char **) subpaths), ==, 2);
  g_assert_cmpstr (subpaths[0], ==, "/de");
  g_assert_cmpstr (subpaths[1], ==, "/es");

  /* Reset things */
  configure_languages ("de");
  empty_installation (inst);
}

static void
test_list_remote_related_refs (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) refs = NULL;
  FlatpakRelatedRef *ref;
  g_auto(GStrv) subpaths = NULL;
  gboolean should_download;
  gboolean should_delete;
  gboolean should_autoprune;
  g_autofree char *app = NULL;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());
  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  refs = flatpak_installation_list_remote_related_refs_sync (inst, repo_name, app, NULL, &error);
  g_assert_nonnull (refs);
  g_assert_no_error (error);

  g_assert_cmpint (refs->len, ==, 1);
  ref = g_ptr_array_index (refs, 0);

  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (ref)), ==, "org.test.Hello.Locale");
  g_assert_true (flatpak_related_ref_should_download (ref));
  g_assert_true (flatpak_related_ref_should_delete (ref));
  g_assert_false (flatpak_related_ref_should_autoprune (ref));
  g_assert_true (g_strv_length ((char **) flatpak_related_ref_get_subpaths (ref)) == 1);
  g_assert_cmpstr (flatpak_related_ref_get_subpaths (ref)[0], ==, "/de");

  g_object_get (ref,
                "subpaths", &subpaths,
                "should-download", &should_download,
                "should-delete", &should_delete,
                "should-autoprune", &should_autoprune,
                NULL);

  g_assert_true (g_strv_length (subpaths) == 1);
  g_assert_cmpstr (subpaths[0], ==, "/de");
  g_assert_true (should_download);
  g_assert_true (should_delete);
  g_assert_false (should_autoprune);

  // Make the test with extra-languages, instead of languages
  clean_languages();
  configure_extra_languages();

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  refs = flatpak_installation_list_remote_related_refs_sync (inst, repo_name, app, NULL, &error);
  g_assert_nonnull (refs);
  g_assert_no_error (error);

  g_assert_cmpint (refs->len, ==, 1);
  ref = g_ptr_array_index (refs, 0);

  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (ref)), ==, "org.test.Hello.Locale");
  g_assert_true (flatpak_related_ref_should_download (ref));
  g_assert_true (flatpak_related_ref_should_delete (ref));
  g_assert_false (flatpak_related_ref_should_autoprune (ref));
  g_assert_true (g_strv_length ((char **) flatpak_related_ref_get_subpaths (ref)) == 2);
  g_assert_cmpstr (flatpak_related_ref_get_subpaths (ref)[0], ==, "/de");
  g_assert_cmpstr (flatpak_related_ref_get_subpaths (ref)[1], ==, "/en");

  g_object_get (ref,
                "subpaths", &subpaths,
                "should-download", &should_download,
                "should-delete", &should_delete,
                "should-autoprune", &should_autoprune,
                NULL);

  g_assert_true (g_strv_length (subpaths) == 2);
  g_assert_cmpstr (subpaths[0], ==, "/de");
  g_assert_cmpstr (subpaths[1], ==, "/en");
  g_assert_true (should_download);
  g_assert_true (should_delete);
  g_assert_false (should_autoprune);

  configure_languages ("de");
  clean_extra_languages ();
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

static gboolean
check_bwrap_support (void)
{
  g_autoptr(GError) error = NULL;
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
        return FALSE;
    }

  return TRUE;
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

  if (!check_bwrap_support ())
    {
      g_test_skip ("bwrap not supported");
      return;
    }

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  monitor = flatpak_installation_create_monitor (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (G_IS_FILE_MONITOR (monitor));
  g_file_monitor_set_rate_limit (monitor, 100);

  g_signal_connect (monitor, "changed", G_CALLBACK (changed_cb), &changed_count);

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (refs->len, ==, 0);
  g_ptr_array_unref (refs);

  changed_count = 0;
  progress_count = 0;
  timeout_reached = FALSE;
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
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
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);
  g_assert_true (FLATPAK_IS_INSTALLED_REF (ref));
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
  g_assert_cmpstr (flatpak_ref_get_collection_id (FLATPAK_REF (ref)), ==, repo_collection_id);

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
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
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
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);
  g_assert_true (FLATPAK_IS_INSTALLED_REF (ref));
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
  g_assert_cmpstr (flatpak_ref_get_collection_id (FLATPAK_REF (ref)), ==, repo_collection_id);

  g_assert_cmpuint (flatpak_installed_ref_get_installed_size (ref), >, 0);
  g_assert_true (flatpak_installed_ref_get_is_current (ref));

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (refs->len, ==, 2);

  g_ptr_array_unref (refs);

  /* first test an error */
  res = flatpak_installation_launch (inst, "org.test.Hellooo", NULL, NULL, NULL, NULL, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED);
  g_assert_false (res);
  g_clear_error (&error);

  /* now launch the right thing */
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
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  res = flatpak_installation_uninstall (inst,
                                        flatpak_ref_get_kind (FLATPAK_REF (ref)),
                                        flatpak_ref_get_name (FLATPAK_REF (ref)),
                                        flatpak_ref_get_arch (FLATPAK_REF (ref)),
                                        flatpak_ref_get_branch (FLATPAK_REF (ref)),
                                        progress_cb,
                                        &progress_count,
                                        NULL,
                                        &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
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
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  res = flatpak_installation_uninstall (inst,
                                        flatpak_ref_get_kind (FLATPAK_REF (runtime_ref)),
                                        flatpak_ref_get_name (FLATPAK_REF (runtime_ref)),
                                        flatpak_ref_get_arch (FLATPAK_REF (runtime_ref)),
                                        flatpak_ref_get_branch (FLATPAK_REF (runtime_ref)),
                                        progress_cb,
                                        &progress_count,
                                        NULL,
                                        &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
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
static void update_test_runtime (void);
static void update_repo (const char *update_repo_name);

static const char *
flatpak_deploy_data_get_origin (GVariant *deploy_data)
{
  const char *origin;

  g_variant_get_child (deploy_data, 0, "&s", &origin);
  return origin;
}

static const char *
flatpak_deploy_data_get_commit (GVariant *deploy_data)
{
  const char *commit;

  g_variant_get_child (deploy_data, 1, "&s", &commit);
  return commit;
}

static const char *
flatpak_deploy_data_get_runtime (GVariant *deploy_data)
{
  g_autoptr(GVariant) metadata = g_variant_get_child_value (deploy_data, 4);
  const char *runtime = NULL;

  g_variant_lookup (metadata, "runtime", "&s", &runtime);

  return runtime;
}

/**
 * flatpak_deploy_data_get_subpaths:
 *
 * Returns: (array length=length zero-terminated=1) (transfer container): an array of constant strings
 **/
static const char **
flatpak_deploy_data_get_subpaths (GVariant *deploy_data)
{
  const char **subpaths;

  g_variant_get_child (deploy_data, 2, "^a&s", &subpaths);
  return subpaths;
}

static guint64
flatpak_deploy_data_get_installed_size (GVariant *deploy_data)
{
  guint64 size;

  g_variant_get_child (deploy_data, 3, "t", &size);
  return GUINT64_FROM_BE (size);
}

static GVariant *
flatpak_dir_new_deploy_data (const char *origin,
                             const char *commit,
                             char      **subpaths,
                             guint64     installed_size,
                             GVariant   *metadata)
{
  char *empty_subpaths[] = {NULL};
  GVariantBuilder builder;

  if (metadata == NULL)
    {
      g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
      metadata = g_variant_builder_end (&builder);
    }

  return g_variant_ref_sink (g_variant_new ("(ss^ast@a{sv})",
                                            origin,
                                            commit,
                                            subpaths ? subpaths : empty_subpaths,
                                            GUINT64_TO_BE (installed_size),
                                            metadata));
}

#define FLATPAK_DEPLOY_DATA_GVARIANT_STRING "(ssasta{sv})"
#define FLATPAK_DEPLOY_DATA_GVARIANT_FORMAT G_VARIANT_TYPE (FLATPAK_DEPLOY_DATA_GVARIANT_STRING)

static GVariant *
flatpak_load_deploy_data (GFile        *deploy_dir)
{
  g_autoptr(GFile) data_file = NULL;
  g_autoptr(GError) error = NULL;
  char *data = NULL;
  gsize data_size;

  data_file = g_file_get_child (deploy_dir, "deploy");
  g_file_load_contents (data_file, NULL, &data, &data_size, NULL, &error);
  g_assert_no_error (error);

  return g_variant_ref_sink (g_variant_new_from_data (FLATPAK_DEPLOY_DATA_GVARIANT_FORMAT,
                                                      data, data_size,
                                                      FALSE, g_free, data));
}

static gboolean
flatpak_variant_save (GFile        *dest,
                      GVariant     *variant,
                      GCancellable *cancellable,
                      GError      **error)
{
  g_autoptr(GOutputStream) out = NULL;
  gsize bytes_written;

  out = (GOutputStream *) g_file_replace (dest, NULL, FALSE,
                                          G_FILE_CREATE_REPLACE_DESTINATION,
                                          cancellable, error);
  if (out == NULL)
    return FALSE;

  if (!g_output_stream_write_all (out,
                                  g_variant_get_data (variant),
                                  g_variant_get_size (variant),
                                  &bytes_written,
                                  cancellable,
                                  error))
    return FALSE;

  if (!g_output_stream_close (out, cancellable, error))
    return FALSE;

  return TRUE;
}

static void
mangle_deploy_file (FlatpakInstalledRef *ref)
{
  g_autoptr(GFile) dir = NULL;
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GVariant) new_data = NULL;
  g_autoptr(GFile) deploy_data_file = NULL;
  GVariantBuilder metadata_builder;
  g_autoptr(GError) error = NULL;
  const char * const previous_ids[] = { "net.example.Goodbye", NULL };

  dir = g_file_new_for_path (flatpak_installed_ref_get_deploy_dir (ref));
  data = flatpak_load_deploy_data (dir);

  g_variant_builder_init (&metadata_builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&metadata_builder, "{s@v}", "runtime",
                         g_variant_new_variant (g_variant_new_string (flatpak_deploy_data_get_runtime (data))));
  g_variant_builder_add (&metadata_builder, "{s@v}", "previous-ids",
                         g_variant_new_variant (g_variant_new_strv (previous_ids, -1)));

  new_data = flatpak_dir_new_deploy_data (flatpak_deploy_data_get_origin (data),
                                          flatpak_deploy_data_get_commit (data),
                                          (char **) flatpak_deploy_data_get_subpaths (data),
                                          flatpak_deploy_data_get_installed_size (data),
                                          g_variant_builder_end (&metadata_builder));

  deploy_data_file = g_file_get_child (dir, "deploy");
  flatpak_variant_save (deploy_data_file, new_data, NULL, &error);
  g_assert_no_error (error);
}

static void
check_desktop_file (FlatpakInstalledRef *ref,
                    const char          *file,
                    const char          *expected_renamed_from)
{
  g_autofree char *path = g_build_path (G_DIR_SEPARATOR_S,
                                        flatpak_installed_ref_get_deploy_dir (ref),
                                        "export",
                                        "share",
                                        "applications",
                                        file,
                                        NULL);
  g_autoptr(GKeyFile) kf = g_key_file_new ();
  g_autofree char *renamed_from = NULL;
  g_autoptr(GError) error = NULL;

  g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &error);
  g_assert_no_error (error);

  renamed_from = g_key_file_get_value (kf,
                                       G_KEY_FILE_DESKTOP_GROUP,
                                       "X-Flatpak-RenamedFrom",
                                       &error);
  g_assert_no_error (error);
  g_assert_cmpstr (renamed_from, ==, expected_renamed_from);
}

static void
test_list_updates (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakInstalledRef) ref = NULL;
  g_autoptr(GPtrArray) refs = NULL;
  FlatpakInstalledRef *update_ref = NULL;
  g_autoptr(FlatpakInstalledRef) updated_ref = NULL;
  g_autofree gchar *app = NULL;
  g_autofree gchar *runtime = NULL;
  gboolean res;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());

  runtime = g_strdup_printf ("runtime/org.test.Platform/%s/master",
                             flatpak_get_default_arch ());

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  empty_installation (inst);

  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  /* install org.test.Hello, and have org.test.Hello.Locale and org.test.Platform
   * added as deps/related
   */
  res = flatpak_transaction_add_install (transaction, repo_name, app, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);
  g_clear_object (&transaction);

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (refs);
  g_assert_cmpint (refs->len, ==, 3);
  g_clear_pointer (&refs, g_ptr_array_unref);

  ref = flatpak_installation_get_installed_ref (inst,
                                                FLATPAK_REF_KIND_APP,
                                                "org.test.Hello",
                                                flatpak_get_default_arch (),
                                                "master", NULL, &error);
  g_assert_nonnull (ref);
  g_assert_no_error (error);

  /* Add a previous-id to the deploy file */
  mangle_deploy_file (ref);

  /* Update the test app and the runtime and list the updates */
  update_test_app ();
  update_test_runtime ();
  update_repo ("test");

  /* Drop all in-memory summary caches so we can find the new update */
  flatpak_installation_drop_caches (inst, NULL, &error);
  g_assert_no_error (error);

  refs = flatpak_installation_list_installed_refs_for_update (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (refs);
  g_assert_cmpint (refs->len, ==, 3);
  update_ref = g_ptr_array_index (refs, 0);
  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (update_ref)), ==, "org.test.Hello");
  g_assert_cmpint (flatpak_ref_get_kind (FLATPAK_REF (update_ref)), ==, FLATPAK_REF_KIND_APP);
  update_ref = g_ptr_array_index (refs, 1);
  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (update_ref)), ==, "org.test.Hello.Locale");
  g_assert_cmpint (flatpak_ref_get_kind (FLATPAK_REF (update_ref)), ==, FLATPAK_REF_KIND_RUNTIME);
  update_ref = g_ptr_array_index (refs, 2);
  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (update_ref)), ==, "org.test.Platform");
  g_assert_cmpint (flatpak_ref_get_kind (FLATPAK_REF (update_ref)), ==, FLATPAK_REF_KIND_RUNTIME);

  /* Install the app update */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  updated_ref = flatpak_installation_update (inst,
                                             FLATPAK_UPDATE_FLAGS_NONE,
                                             FLATPAK_REF_KIND_APP,
                                             "org.test.Hello",
                                             flatpak_get_default_arch (), "master",
                                             NULL, NULL, NULL, &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);
  g_assert_true (FLATPAK_IS_INSTALLED_REF (updated_ref));

  check_desktop_file (updated_ref,
                      "org.test.Hello.desktop",
                      "net.example.Goodbye.desktop;");
  check_desktop_file (updated_ref,
                      "org.test.Hello.Again.desktop",
                      "hello-again.desktop;net.example.Goodbye.Again.desktop;");

  /* Uninstall the runtime and app */
  empty_installation (inst);
}

static void
test_list_undeployed_updates (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) refs = NULL;
  FlatpakInstalledRef *update_ref = NULL;
  g_autoptr(FlatpakInstalledRef) updated_ref = NULL;
  g_autofree gchar *app = NULL;
  gboolean res;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  empty_installation (inst);

  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  /* install org.test.Hello, and have org.test.Hello.Locale and org.test.Platform
   * added as deps/related
   */
  res = flatpak_transaction_add_install (transaction, repo_name, app, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);
  g_clear_object (&transaction);

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (refs);
  g_assert_cmpint (refs->len, ==, 3);
  g_clear_pointer (&refs, g_ptr_array_unref);

  update_test_app ();
  update_repo ("test");

  /* Drop all in-memory summary caches so we can find the new update */
  flatpak_installation_drop_caches (inst, NULL, &error);
  g_assert_no_error (error);

  /* Install the app update but don't deploy it */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  updated_ref = flatpak_installation_update (inst,
                                             FLATPAK_UPDATE_FLAGS_NO_DEPLOY,
                                             FLATPAK_REF_KIND_APP,
                                             "org.test.Hello",
                                             flatpak_get_default_arch (), "master",
                                             NULL, NULL, NULL, &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);
  g_assert_true (FLATPAK_IS_INSTALLED_REF (updated_ref));

  refs = flatpak_installation_list_installed_refs_for_update (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (refs);
  g_assert_cmpint (refs->len, ==, 2);
  update_ref = g_ptr_array_index (refs, 0);
  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (update_ref)), ==, "org.test.Hello");
  g_assert_cmpint (flatpak_ref_get_kind (FLATPAK_REF (update_ref)), ==, FLATPAK_REF_KIND_APP);
  update_ref = g_ptr_array_index (refs, 1);
  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (update_ref)), ==, "org.test.Hello.Locale");
  g_assert_cmpint (flatpak_ref_get_kind (FLATPAK_REF (update_ref)), ==, FLATPAK_REF_KIND_RUNTIME);

  /* Uninstall the runtime and app */
  empty_installation (inst);
}

static void
test_list_updates_offline (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) refs = NULL;
  g_autoptr(FlatpakInstalledRef) runtime_ref = NULL;
  g_autoptr(FlatpakRemote) remote = NULL;
  gboolean res;
  g_autofree char *saved_url = NULL;
  g_autofree char *wrong_url = NULL;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  /* Install a runtime */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  runtime_ref = flatpak_installation_install (inst,
                                              repo_name,
                                              FLATPAK_REF_KIND_RUNTIME,
                                              "org.test.Platform",
                                              NULL, NULL, NULL, NULL, NULL,
                                              &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);
  g_assert_true (FLATPAK_IS_INSTALLED_REF (runtime_ref));

  /* Note: here we could create an update for the runtime, but it won't be
   * found anyway since the URL will be wrong.
   */

  /* Make the URL wrong to simulate being offline */
  remote = flatpak_installation_get_remote_by_name (inst, repo_name, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (remote);
  saved_url = flatpak_remote_get_url (remote);
  wrong_url = g_strdup_printf ("http://127.0.0.1:%s/nonexistent", httpd_port);
  flatpak_remote_set_url (remote, wrong_url);
  res = flatpak_installation_modify_remote (inst, remote, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  /* Listing updates offline should return an empty array */
  refs = flatpak_installation_list_installed_refs_for_update (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (refs);
  g_assert_cmpint (refs->len, ==, 0);

  /* Restore the correct URL to the remote */
  flatpak_remote_set_url (remote, saved_url);
  res = flatpak_installation_modify_remote (inst, remote, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  /* Uninstall the runtime */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  res = flatpak_installation_uninstall (inst,
                                        flatpak_ref_get_kind (FLATPAK_REF (runtime_ref)),
                                        flatpak_ref_get_name (FLATPAK_REF (runtime_ref)),
                                        flatpak_ref_get_arch (FLATPAK_REF (runtime_ref)),
                                        flatpak_ref_get_branch (FLATPAK_REF (runtime_ref)),
                                        NULL, NULL, NULL,
                                        &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
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
make_bundle (void)
{
  g_autofree char *repo_url = g_strdup_printf ("http://127.0.0.1:%s/test", httpd_port);
  g_autofree char *arg2 = g_strdup_printf ("--repo-url=%s", repo_url);
  g_autofree char *path = g_build_filename (testdir, "bundles", NULL);
  g_autofree char *file = g_build_filename (path, "hello.flatpak", NULL);
  char *argv[] = { "flatpak", "build-bundle", "repo-url", "repos/test", "filename", "org.test.Hello", NULL };

  argv[2] = arg2;
  argv[4] = file;

  g_debug ("Making dir %s", path);
  g_mkdir_with_parents (path, S_IRWXU | S_IRWXG | S_IRWXO);

  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
make_test_runtime (const char *runtime_repo_name)
{
  g_autofree char *arg0 = NULL;
  g_autofree char *arg1 = NULL;
  char *argv[] = {
    NULL, NULL, "org.test.Platform", "master", "", "", NULL
  };

  arg0 = g_test_build_filename (G_TEST_DIST, "make-test-runtime.sh", NULL);
  arg1 = g_strdup_printf ("repos/%s", runtime_repo_name);
  argv[0] = arg0;
  argv[1] = arg1;
  argv[4] = repo_collection_id;

  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
make_test_app (const char *app_repo_name)
{
  g_autofree char *arg0 = NULL;
  g_autofree char *arg1 = NULL;
  char *argv[] = { NULL, NULL, "", "master", "", NULL };

  arg0 = g_test_build_filename (G_TEST_DIST, "make-test-app.sh", NULL);
  arg1 = g_strdup_printf ("repos/%s", app_repo_name);
  argv[0] = arg0;
  argv[1] = arg1;
  argv[4] = repo_collection_id;

  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
update_test_app (void)
{
  g_autofree char *arg0 = NULL;
  char *argv[] = { NULL, "repos/test", "", "master", "", "SPIN", NULL };

  arg0 = g_test_build_filename (G_TEST_DIST, "make-test-app.sh", NULL);
  argv[0] = arg0;
  argv[4] = repo_collection_id;

  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
update_test_runtime (void)
{
  g_autofree char *arg0 = NULL;
  char *argv[] = { NULL, "repos/test", "org.test.Platform", "master", "", "UPDATED", NULL };

  arg0 = g_test_build_filename (G_TEST_DIST, "make-test-runtime.sh", NULL);
  argv[0] = arg0;
  argv[4] = repo_collection_id;

  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
update_repo (const char *update_repo_name)
{
  g_autofree char *arg4 = NULL;
  char *argv[] = { "flatpak", "build-update-repo", "--gpg-homedir=", "--gpg-sign=", NULL, NULL };
  g_auto(GStrv) gpgargs = NULL;

  gpgargs = g_strsplit (gpg_args, " ", 0);
  arg4 = g_strdup_printf ("repos/%s", update_repo_name);
  argv[2] = gpgargs[0];
  argv[3] = gpgargs[1];
  argv[4] = arg4;

  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
launch_httpd (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *port = NULL;
  g_autofree char *pid = NULL;
  g_autofree char *path = g_test_build_filename (G_TEST_DIST, "test-webserver.sh", NULL);
  char *argv[] = {path, "repos", NULL };

  /* The web server puts itself in the background, so we can't wait
   * for EOF on its stdout, stderr */
  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_NO_CAPTURE);

  g_file_get_contents ("httpd-pid", &pid, NULL, &error);
  g_assert_no_error (error);

  httpd_pid = atoi (pid);
  g_assert_cmpint (httpd_pid, !=, 0);

  g_file_get_contents ("httpd-port", &port, NULL, &error);
  g_assert_no_error (error);

  if (port[strlen (port) - 1] == '\n')
    port[strlen (port) - 1] = '\0';

  httpd_port = g_strdup (port);
}

static void
_add_remote (const char *remote_repo_name,
             const char *remote_name_override,
             gboolean    system)
{
  char *argv[] = { "flatpak", "remote-add", NULL, "--gpg-import=", "--collection-id=", "name", "url", NULL };
  g_autofree char *gpgimport = NULL;
  g_autofree char *collection_id_arg = NULL;
  g_autofree char *remote_name = NULL;

  gpgimport = g_strdup_printf ("--gpg-import=%s/pubring.gpg", gpg_homedir);
  repo_url = g_strdup_printf ("http://127.0.0.1:%s/%s", httpd_port, remote_repo_name);
  collection_id_arg = g_strdup_printf ("--collection-id=%s", repo_collection_id);
  if (remote_name_override != NULL)
    remote_name = g_strdup (remote_name_override);
  else
    remote_name = g_strdup_printf ("%s-repo", remote_repo_name);

  argv[2] = system ? "--system" : "--user";
  argv[3] = gpgimport;
  argv[4] = collection_id_arg;
  argv[5] = remote_name;
  argv[6] = repo_url;

  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
add_remote_system (const char *remote_repo_name,
                   const char *remote_name_override)
{
  _add_remote (remote_repo_name, remote_name_override, TRUE);
}

static void
add_remote_user (const char *remote_repo_name,
                 const char *remote_name_override)
{
  _add_remote (remote_repo_name, remote_name_override, FALSE);
}

static void
_remove_remote (const char *remote_repo_name,
                gboolean    system)
{
  char *argv[] = { "flatpak", "remote-delete", NULL, "name", NULL };
  g_autofree char *remote_name = NULL;

  remote_name = g_strdup_printf ("%s-repo", remote_repo_name);
  argv[2] = system ? "--system" : "--user";
  argv[3] = remote_name;

  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_DEFAULT);
}

static void
remove_remote_system (const char *remote_repo_name)
{
  _remove_remote (remote_repo_name, TRUE);
}

static void
remove_remote_user (const char *remote_repo_name)
{
  _remove_remote (remote_repo_name, FALSE);
}

static void
add_flatpakrepo (const char *flatpakrepo_repo_name)
{
  g_autofree char *data = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *file_path = NULL;

  data = g_strconcat ("[Flatpak Repo]\n"
                      "Version=1\n"
                      "Url=http://127.0.0.1:", httpd_port, "/", flatpakrepo_repo_name, "\n"
                      "DefaultBranch=master\n"
                      "Title=Test repo\n", NULL);

  file_path = g_strdup_printf ("repos/%s/%s-repo.flatpakrepo", flatpakrepo_repo_name, flatpakrepo_repo_name);
  g_file_set_contents (file_path, data, -1, &error);
  g_assert_no_error (error);
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

  make_test_runtime ("test");
  make_test_app ("test");
  update_repo ("test");
  launch_httpd ();
  add_remote_user ("test", NULL);
  add_flatpakrepo ("test");
  configure_languages ("de");

  /* another copy of the same repo, with different url */
  g_assert_cmpint (symlink ("test", "repos/copy-of-test"), ==, 0);

  /* another repo, with only the app */
  make_test_app ("test-without-runtime");
  update_repo ("test-without-runtime");

  /* another repo, with only the runtime */
  make_test_runtime ("test-runtime-only");
  update_repo ("test-runtime-only");
  add_flatpakrepo ("test-runtime-only");
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

GTestDBus *test_bus = NULL;

static void
global_setup (void)
{
  g_autofree char *cachedir = NULL;
  g_autofree char *configdir = NULL;
  g_autofree char *datadir = NULL;
  g_autofree char *homedir = NULL;
  g_autofree char *services_dir = NULL;

  testdir = g_strdup ("/tmp/flatpak-test-XXXXXX");
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
  g_setenv ("XDG_RUNTIME_DIR", flatpak_runtimedir, TRUE);
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

  g_reload_user_special_dirs_cache ();

  g_assert_cmpstr (g_get_user_cache_dir (), ==, cachedir);
  g_assert_cmpstr (g_get_user_config_dir (), ==, configdir);
  g_assert_cmpstr (g_get_user_data_dir (), ==, datadir);
  g_assert_cmpstr (g_get_user_runtime_dir (), ==, flatpak_runtimedir);

  g_setenv ("FLATPAK_SYSTEM_HELPER_ON_SESSION", "1", TRUE);
  g_setenv ("LANGUAGE", "en", TRUE);

  test_bus = g_test_dbus_new (G_TEST_DBUS_NONE);

  services_dir = g_test_build_filename (G_TEST_BUILT, "services", NULL);
  g_test_dbus_add_service_dir (test_bus, services_dir);

  g_test_dbus_up (test_bus);

  copy_gpg ();
  setup_multiple_installations ();
  setup_repo ();
  make_bundle ();
}

static void
global_teardown (void)
{
  char *argv[] = { "gpg-connect-agent", "--homedir", "<placeholder>", "killagent", "/bye", NULL };

  if (g_getenv ("SKIP_TEARDOWN"))
    return;

  g_test_dbus_down (test_bus);

  argv[2] = gpg_homedir;

  if (httpd_pid != -1)
    kill (httpd_pid, SIGKILL);

  run_test_subprocess (argv, RUN_TEST_SUBPROCESS_IGNORE_FAILURE);

  glnx_shutil_rm_rf_at (-1, testdir, NULL, NULL);
  g_free (testdir);
}

/* Check some basic transaction getters, without running a transaction
 * or adding ops.
 */
static void
test_misc_transaction (void)
{
  struct { int         op;
           const char *name;
  } kinds[] = {
    { FLATPAK_TRANSACTION_OPERATION_INSTALL, "install" },
    { FLATPAK_TRANSACTION_OPERATION_UPDATE, "update" },
    { FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE, "install-bundle" },
    { FLATPAK_TRANSACTION_OPERATION_UNINSTALL, "uninstall" },
    { FLATPAK_TRANSACTION_OPERATION_LAST_TYPE, NULL }
  };
  int i;
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(FlatpakInstallation) inst2 = NULL;
  g_autoptr(FlatpakInstallation) inst3 = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(FlatpakTransactionOperation) op = NULL;
  GList *list = NULL;

  for (i = 0; i < G_N_ELEMENTS (kinds); i++)
    g_assert_cmpstr (kinds[i].name, ==, flatpak_transaction_operation_type_to_string (kinds[i].op));

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  transaction = flatpak_transaction_new_for_installation (NULL, NULL, &error);
  g_assert_nonnull (error);
  g_assert_null (transaction);
  g_clear_error (&error);

  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  g_object_get (transaction, "installation", &inst2, NULL);
  g_assert_true (inst2 == inst);

  inst3 = flatpak_transaction_get_installation (transaction);
  g_assert_true (inst3 == inst);

  op = flatpak_transaction_get_current_operation (transaction);
  g_assert_null (op);

  list = flatpak_transaction_get_operations (transaction);
  g_assert_null (list);

  flatpak_transaction_set_no_deploy (transaction, TRUE);
  g_assert_true (flatpak_transaction_get_no_deploy (transaction) == TRUE);

  flatpak_transaction_set_no_pull (transaction, TRUE);
  g_assert_true (flatpak_transaction_get_no_pull (transaction) == TRUE);

  g_assert_true (flatpak_transaction_is_empty (transaction));
}

static void
empty_installation (FlatpakInstallation *inst)
{
  g_autoptr(GPtrArray) refs = NULL;
  g_autoptr(GError) error = NULL;
  int i;

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);

  for (i = 0; i < refs->len; i++)
    {
      FlatpakRef *ref = g_ptr_array_index (refs, i);

      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      flatpak_installation_uninstall_full (inst,
                                           FLATPAK_UNINSTALL_FLAGS_NO_TRIGGERS,
                                           flatpak_ref_get_kind (ref),
                                           flatpak_ref_get_name (ref),
                                           flatpak_ref_get_arch (ref),
                                           flatpak_ref_get_branch (ref),
                                           NULL, NULL, NULL, &error);
      G_GNUC_END_IGNORE_DEPRECATIONS
      g_assert_no_error (error);
    }

  flatpak_installation_run_triggers (inst, NULL, &error);
  g_assert_no_error (error);

  flatpak_installation_prune_local_repo (inst, NULL, &error);
  g_assert_no_error (error);
}

static int ready_count;
static int new_op_count;
static int op_done_count;

static gboolean
op_error (FlatpakTransaction             *transaction,
          FlatpakTransactionOperation    *op,
          GError                         *error,
          FlatpakTransactionErrorDetails *details)
{
  g_assert_not_reached ();
  return TRUE;
}

static gboolean
choose_remote (FlatpakTransaction *transaction,
               const char         *ref,
               const char         *runtime,
               const char        **remotes)
{
  g_assert_cmpint (g_strv_length ((char **) remotes), ==, 1);
  return 0;
}

static void
end_of_lifed (FlatpakTransaction *transaction,
              const char         *ref,
              const char         *reason,
              const char         *rebase)
{
  g_assert_not_reached ();
}

static gboolean
add_new_remote (FlatpakTransaction             *transaction,
                FlatpakTransactionRemoteReason  reason,
                const char                     *from_id,
                const char                     *suggested_name,
                const char                     *url)
{
  g_assert_not_reached ();
  return TRUE;
}

static gboolean
ready (FlatpakTransaction *transaction)
{
  GList *ops, *l;

  ready_count++;

  ops = flatpak_transaction_get_operations (transaction);
  g_assert_cmpint (g_list_length (ops), ==, 3);

  for (l = ops; l; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;

      g_assert_cmpint (flatpak_transaction_operation_get_operation_type (op), ==, FLATPAK_TRANSACTION_OPERATION_INSTALL);
      g_assert_nonnull (flatpak_transaction_operation_get_commit (op));
    }

  g_list_free_full (ops, g_object_unref);

  return TRUE;
}

static void
new_op (FlatpakTransaction          *transaction,
        FlatpakTransactionOperation *op,
        FlatpakTransactionProgress  *progress)
{
  g_autofree char *status = NULL;
  g_autoptr(FlatpakTransactionOperation) current = NULL;
  g_auto(GStrv) refs = NULL;

  refs = g_new0 (gchar *, 4);
  refs[0] = g_strdup_printf ("runtime/org.test.Platform/%s/master",
                             flatpak_get_default_arch ());
  refs[1] = g_strdup_printf ("app/org.test.Hello/%s/master",
                             flatpak_get_default_arch ());
  refs[2] = g_strdup_printf ("runtime/org.test.Hello.Locale/%s/master",
                             flatpak_get_default_arch ());
  refs[3] = NULL;

  new_op_count++;

  current = flatpak_transaction_get_current_operation (transaction);
  g_assert_true (op == current);

  g_assert_cmpint (flatpak_transaction_operation_get_operation_type (op), ==, FLATPAK_TRANSACTION_OPERATION_INSTALL);
  g_assert_true (g_strv_contains ((const gchar * const *) refs, flatpak_transaction_operation_get_ref (op)));

  status = flatpak_transaction_progress_get_status (progress);
  g_assert_cmpstr (status, ==, "Initializing");
  g_assert_true (flatpak_transaction_progress_get_is_estimating (progress));
  g_assert_cmpint (flatpak_transaction_progress_get_progress (progress), ==, 0);
}

static void
op_done (FlatpakTransaction          *transaction,
         FlatpakTransactionOperation *op,
         const char                  *commit,
         int                          result)
{
  g_auto(GStrv) refs = NULL;

  refs = g_new0 (gchar *, 4);
  refs[0] = g_strdup_printf ("runtime/org.test.Platform/%s/master",
                             flatpak_get_default_arch ());
  refs[1] = g_strdup_printf ("app/org.test.Hello/%s/master",
                             flatpak_get_default_arch ());
  refs[2] = g_strdup_printf ("runtime/org.test.Hello.Locale/%s/master",
                             flatpak_get_default_arch ());
  refs[3] = NULL;

  op_done_count++;

  g_assert_cmpint (flatpak_transaction_operation_get_operation_type (op), ==, FLATPAK_TRANSACTION_OPERATION_INSTALL);
  g_assert_true (g_strv_contains ((const gchar * const *) refs, flatpak_transaction_operation_get_ref (op)));

  g_assert_cmpint (result, ==, 0);
}

static void
op_done_no_change (FlatpakTransaction          *transaction,
                   FlatpakTransactionOperation *op,
                   const char                  *commit,
                   int                          result)
{
  g_autofree char *app = NULL;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());
  g_assert_cmpstr (flatpak_transaction_operation_get_ref (op), ==, app);
  g_assert_cmpint (flatpak_transaction_operation_get_operation_type (op), ==, FLATPAK_TRANSACTION_OPERATION_UPDATE);
  g_assert_cmpint (result, ==, FLATPAK_TRANSACTION_RESULT_NO_CHANGE);
}

static void
op_done_with_change (FlatpakTransaction          *transaction,
                     FlatpakTransactionOperation *op,
                     const char                  *commit,
                     int                          result)
{
  g_autofree char *app = NULL;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());
  g_assert_cmpstr (flatpak_transaction_operation_get_ref (op), ==, app);
  g_assert_cmpint (flatpak_transaction_operation_get_operation_type (op), ==, FLATPAK_TRANSACTION_OPERATION_UPDATE);
  g_assert_cmpint (result, ==, 0);
}

/* Do a bunch of installs and uninstalls with a transaction, and check
 * that ops looks as expected, and that signal are fired.
 */
static void
test_transaction_install_uninstall (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(FlatpakTransactionOperation) op = NULL;
  gboolean res;
  g_autoptr(GError) error = NULL;
  GList *list;
  g_autoptr(GPtrArray) refs = NULL;
  g_autoptr(FlatpakInstalledRef) ref = NULL;
  gboolean is_current;
  g_autofree char *origin = NULL;
  guint64 size;
  g_auto(GStrv) subpaths = NULL;
  g_autofree char *eol = NULL;
  g_autofree char *eol_rebase = NULL;
  g_autofree char *commit = NULL;
  g_autofree char *deploy = NULL;
  GBytes *bytes = NULL;
  const char *empty_subpaths[] = { "", NULL };
  g_autofree char *app = NULL;
  g_autofree char *runtime = NULL;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());
  runtime = g_strdup_printf ("runtime/org.test.Platform/%s/master",
                             flatpak_get_default_arch ());

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  /* start from a clean slate */
  empty_installation (inst);

  /* Check that it is indeed empty */
  ref = flatpak_installation_get_current_installed_app (inst, "org.test.Hello", NULL, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED);
  g_assert_null (ref);
  g_clear_error (&error);

  /* update org.test.Hello, we expect a non-installed error */
  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  g_assert_true (flatpak_transaction_is_empty (transaction));

  res = flatpak_transaction_add_update (transaction, app, NULL, NULL, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED);
  g_assert_false (res);
  g_clear_error (&error);

  g_clear_object (&transaction);

  /* install org.test.Hello, and have org.test.Hello.Locale and org.test.Platform
   * added as deps/related
   */
  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_install (transaction, repo_name, app, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_assert_true (!flatpak_transaction_is_empty (transaction));

  list = flatpak_transaction_get_operations (transaction);
  g_assert_cmpint (g_list_length (list), ==, 1);
  op = (FlatpakTransactionOperation *) list->data;

  g_list_free (list);

  g_assert_cmpint (flatpak_transaction_operation_get_operation_type (op), ==, FLATPAK_TRANSACTION_OPERATION_INSTALL);
  g_assert_cmpstr (flatpak_transaction_operation_get_ref (op), ==, app);
  g_assert_cmpstr (flatpak_transaction_operation_get_remote (op), ==, repo_name);
  g_assert_null (flatpak_transaction_operation_get_bundle_path (op));
  g_assert_null (flatpak_transaction_operation_get_commit (op));

  g_signal_connect (transaction, "ready", G_CALLBACK (ready), NULL);
  g_signal_connect (transaction, "new-operation", G_CALLBACK (new_op), NULL);
  g_signal_connect (transaction, "operation-done", G_CALLBACK (op_done), NULL);
  g_signal_connect (transaction, "operation-error", G_CALLBACK (op_error), NULL);
  g_signal_connect (transaction, "choose-remote-for-ref", G_CALLBACK (choose_remote), NULL);
  g_signal_connect (transaction, "end-of-lifed", G_CALLBACK (end_of_lifed), NULL);
  g_signal_connect (transaction, "add-new-remote", G_CALLBACK (add_new_remote), NULL);

  ready_count = 0;
  new_op_count = 0;
  op_done_count = 0;

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_assert_cmpint (ready_count, ==, 1);
  g_assert_cmpint (new_op_count, ==, 3);
  g_assert_cmpint (op_done_count, ==, 3);

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (refs);
  g_assert_cmpint (refs->len, ==, 3);
  g_clear_pointer (&refs, g_ptr_array_unref);

  ref = flatpak_installation_get_current_installed_app (inst, "org.test.Hello", NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (ref);

  g_assert_cmpstr (flatpak_installed_ref_get_origin (ref), ==, repo_name);
  g_assert_null (flatpak_installed_ref_get_subpaths (ref));
  g_assert_cmpuint (flatpak_installed_ref_get_installed_size (ref), >, 0);
  g_assert_true (flatpak_installed_ref_get_is_current (ref));
  g_assert_nonnull (flatpak_installed_ref_get_latest_commit (ref));
  g_assert_nonnull (flatpak_installed_ref_get_deploy_dir (ref));
  g_assert_null (flatpak_installed_ref_get_eol (ref));
  g_assert_null (flatpak_installed_ref_get_eol_rebase (ref));
  g_assert_cmpstr (flatpak_installed_ref_get_appdata_name (ref), ==, "Hello world test app: org.test.Hello");
  g_assert_cmpstr (flatpak_installed_ref_get_appdata_summary (ref), ==, "Print a greeting");
  g_assert_cmpstr (flatpak_installed_ref_get_appdata_version (ref), ==, "0.0.1");
  g_object_get (ref,
                "is-current", &is_current,
                "origin", &origin,
                "installed-size", &size,
                "latest-commit", &commit,
                "deploy-dir", &deploy,
                "subpaths", &subpaths,
                "end-of-life", &eol,
                "end-of-life-rebase", &eol_rebase,
                NULL);
  g_assert_true (is_current);
  g_assert_cmpstr (origin, ==, repo_name);
  g_assert_cmpuint (size, >, 0);
  g_assert_nonnull (commit);
  g_assert_nonnull (deploy);
  g_assert_null (subpaths);
  g_assert_null (eol);
  g_assert_null (eol_rebase);
  g_clear_object (&ref);

  refs = flatpak_installation_list_installed_refs_by_kind (inst,
                                                           FLATPAK_REF_KIND_RUNTIME,
                                                           NULL,
                                                           &error);
  g_assert_no_error (error);
  g_assert_cmpint (refs->len, ==, 2);

  ref = g_object_ref (g_ptr_array_index (refs, 0));
  bytes = flatpak_installed_ref_load_metadata (ref, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (bytes);
  g_bytes_unref (bytes);
  g_clear_object (&ref);
  g_clear_pointer (&refs, g_ptr_array_unref);

  g_clear_object (&transaction);

  /* install org.test.Hello again. we expect an already-installed error */
  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_install (transaction, repo_name, app, NULL, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED);
  g_assert_false (res);
  g_clear_error (&error);

  g_clear_object (&transaction);

  /* uninstall org.test.Hello, we expect org.test.Hello.Locale to be
   * removed with it, but org.test.Platform to stay
   */
  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_uninstall (transaction, app, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (refs);
  g_assert_cmpint (refs->len, ==, 1);
  ref = g_object_ref (g_ptr_array_index (refs, 0));
  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (ref)), ==, "org.test.Platform");
  g_clear_object (&ref);
  g_clear_pointer (&refs, g_ptr_array_unref);

  /* run the transaction again, expect an error */
  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_nonnull (error);
  g_assert_false (res);
  g_clear_error (&error);

  g_clear_object (&transaction);

  /* install org.test.Hello and uninstall org.test.Platform. This is
   * expected to yield an error
   */
  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_install (transaction, repo_name, app, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_add_uninstall (transaction, runtime, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_RUNTIME_USED);
  g_assert_false (res);
  g_clear_error (&error);

  g_clear_object (&transaction);

  /* try again to install org.test.Hello. We'll end up with 3 refs */
  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_install (transaction, repo_name, app, empty_subpaths, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (refs);
  g_assert_cmpint (refs->len, ==, 3);
  g_clear_pointer (&refs, g_ptr_array_unref);

  ref = flatpak_installation_get_installed_ref (inst, FLATPAK_REF_KIND_APP, "org.test.Hello", "xzy", "master", NULL, &error);
  g_assert_null (ref);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED);
  g_clear_error (&error);

  ref = flatpak_installation_get_installed_ref (inst, FLATPAK_REF_KIND_APP, "org.test.Hello", NULL, "master", NULL, &error);
  g_assert_nonnull (ref);
  g_assert_no_error (error);

  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (ref)), ==, "org.test.Hello");
  g_clear_object (&ref);

  g_clear_object (&transaction);

  /* update org.test.Hello. Check that this is a no-op */
  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_update (transaction, app, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_signal_connect (transaction, "operation-done", G_CALLBACK (op_done_no_change), NULL);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_clear_object (&transaction);

  /* update again, using { "", NULL } as subpaths, to install all */
  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_update (transaction, app, empty_subpaths, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_signal_connect (transaction, "operation-done", G_CALLBACK (op_done_with_change), NULL);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_clear_object (&transaction);

  /* uninstall both org.test.Hello and org.test.Platform, leaving an empty installation */
  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_uninstall (transaction, app, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_add_uninstall (transaction, runtime, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  /* uninstall again, expect a not-installed error */
  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_uninstall (transaction, app, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED);
  g_assert_false (res);
}

static int remote_added;

static gboolean
add_new_remote2 (FlatpakTransaction             *transaction,
                 FlatpakTransactionRemoteReason  reason,
                 const char                     *from_id,
                 const char                     *suggested_name,
                 const char                     *url)
{
  remote_added++;
  g_assert_cmpstr (suggested_name, ==, "my-little-repo");
  return TRUE;
}
/* test installing a flatpakref with a transaction */
static void
test_transaction_install_flatpakref (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) refs = NULL;
  gboolean res;
  g_autofree char *s = NULL;
  g_autoptr(GBytes) data = NULL;
  g_autofree char *app = NULL;
  g_autofree char *runtime = NULL;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());
  runtime = g_strdup_printf ("runtime/org.test.Platform/%s/master",
                             flatpak_get_default_arch ());

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  /* start from a clean slate */
  empty_installation (inst);

  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  /* pointless, but do it anyway */
  flatpak_transaction_add_dependency_source (transaction, inst);

  data = g_bytes_new ("shoobidoo", strlen ("shoobidoo"));
  res = flatpak_transaction_add_install_flatpakref (transaction, data, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_DATA);
  g_clear_error (&error);
  g_assert_false (res);
  g_clear_pointer (&data, g_bytes_unref);

  s = g_strconcat ("[Flatpak Ref]\n"
                   "Title=Test App\n"
                   "Name=org.test.Hello\n"
                   "Branch=master\n"
                   /* Uses a different url for the repo, so we can ensure it gets added */
                   "Url=http://127.0.0.1:", httpd_port, "/copy-of-test\n"
                   "IsRuntime=False\n"
                   "SuggestRemoteName=my-little-repo\n"
                   "RuntimeRepo=http://127.0.0.1:", httpd_port, "/test/test-repo.flatpakrepo\n",
                   NULL);

  data = g_bytes_new (s, strlen (s));
  res = flatpak_transaction_add_install_flatpakref (transaction, data, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  remote_added = 0;
  g_signal_connect (transaction, "add-new-remote", G_CALLBACK (add_new_remote2), NULL);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_assert_cmpint (remote_added, >, 0);

  refs = flatpak_installation_list_installed_refs (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (refs);
  g_assert_cmpint (refs->len, ==, 3);
  g_clear_pointer (&refs, g_ptr_array_unref);

  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_uninstall (transaction, app, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_add_uninstall (transaction, runtime, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);
}

static gboolean
_is_remote_in_installation (FlatpakInstallation *installation,
                            const char          *remote_repo_name)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) remotes = NULL;
  g_autofree char *remote_name = NULL;

  remotes = flatpak_installation_list_remotes (installation, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (remotes);

  remote_name = g_strdup_printf ("%s-repo", remote_repo_name);
  for (guint i = 0; i < remotes->len; ++i)
    {
      FlatpakRemote *remote = g_ptr_array_index (remotes, i);
      if (g_strcmp0 (remote_name, flatpak_remote_get_name (remote)) == 0)
        return TRUE;
    }

  return FALSE;
}

static void
assert_remote_in_installation (FlatpakInstallation *installation,
                               const char          *remote_repo_name)
{
  g_assert_true (_is_remote_in_installation (installation, remote_repo_name));
}

static void
assert_remote_not_in_installation (FlatpakInstallation *installation,
                                   const char          *remote_repo_name)
{
  g_assert_true (!_is_remote_in_installation (installation, remote_repo_name));
}

static gboolean
add_new_remote3 (FlatpakTransaction             *transaction,
                 FlatpakTransactionRemoteReason  reason,
                 const char                     *from_id,
                 const char                     *suggested_name,
                 const char                     *url)
{
  return TRUE;
}

/* Test that installing a flatpakref causes both the origin remote and the
 * runtime remote to be created, even if they already exist in another
 * installation */
static void
test_transaction_flatpakref_remote_creation (void)
{
  g_autoptr(FlatpakInstallation) user_inst = NULL;
  g_autoptr(FlatpakInstallation) system_inst = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *s = NULL;
  g_autoptr(GBytes) data = NULL;
  gboolean res;

  system_inst = flatpak_installation_new_system (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (system_inst);

  empty_installation (system_inst);

  add_remote_system ("test-without-runtime", NULL);
  add_remote_system ("test-runtime-only", NULL);

  user_inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (user_inst);

  empty_installation (user_inst);

  assert_remote_not_in_installation (user_inst, "test-without-runtime");
  assert_remote_not_in_installation (user_inst, "test-runtime-only");

  transaction = flatpak_transaction_new_for_installation (user_inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  s = g_strconcat ("[Flatpak Ref]\n"
                   "Title=Test App\n"
                   "Name=org.test.Hello\n"
                   "Branch=master\n"
                   "Url=http://127.0.0.1:", httpd_port, "/test-without-runtime\n"
                   "IsRuntime=False\n"
                   "SuggestRemoteName=test-without-runtime-repo\n"
                   "RuntimeRepo=http://127.0.0.1:", httpd_port, "/test-runtime-only/test-runtime-only-repo.flatpakrepo\n",
                   NULL);

  data = g_bytes_new (s, strlen (s));
  res = flatpak_transaction_add_install_flatpakref (transaction, data, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_signal_connect (transaction, "add-new-remote", G_CALLBACK (add_new_remote3), NULL);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  assert_remote_in_installation (user_inst, "test-without-runtime");
  assert_remote_in_installation (user_inst, "test-runtime-only");

  empty_installation (user_inst);
  remove_remote_user ("test-without-runtime");
  remove_remote_user ("test-runtime-only");
  remove_remote_system ("test-without-runtime");
  remove_remote_system ("test-runtime-only");
}

static gboolean
check_ready1_abort (FlatpakTransaction *transaction)
{
  GList *ops;
  FlatpakTransactionOperation *op;
  g_autofree char *app = NULL;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());

  ops = flatpak_transaction_get_operations (transaction);
  g_assert_cmpint (g_list_length (ops), ==, 1);
  op = ops->data;

  g_assert_cmpint (flatpak_transaction_operation_get_operation_type (op), ==, FLATPAK_TRANSACTION_OPERATION_INSTALL);
  g_assert_cmpstr (flatpak_transaction_operation_get_ref (op), ==, app);

  g_list_free_full (ops, g_object_unref);

  return FALSE;
}

static gboolean
check_ready3_abort (FlatpakTransaction *transaction)
{
  GList *ops;
  FlatpakTransactionOperation *op;
  g_autofree char *app = NULL;
  g_autofree char *runtime = NULL;
  g_autofree char *locale = NULL;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());
  runtime = g_strdup_printf ("runtime/org.test.Platform/%s/master",
                             flatpak_get_default_arch ());
  locale = g_strdup_printf ("runtime/org.test.Hello.Locale/%s/master",
                            flatpak_get_default_arch ());

  ops = flatpak_transaction_get_operations (transaction);
  g_assert_cmpint (g_list_length (ops), ==, 3);
  op = ops->data;
  g_assert_cmpint (flatpak_transaction_operation_get_operation_type (op), ==, FLATPAK_TRANSACTION_OPERATION_INSTALL);
  g_assert_cmpstr (flatpak_transaction_operation_get_ref (op), ==, locale);

  op = ops->next->data;
  g_assert_cmpint (flatpak_transaction_operation_get_operation_type (op), ==, FLATPAK_TRANSACTION_OPERATION_INSTALL);
  g_assert_cmpstr (flatpak_transaction_operation_get_ref (op), ==, runtime);

  op = ops->next->next->data;
  g_assert_cmpint (flatpak_transaction_operation_get_operation_type (op), ==, FLATPAK_TRANSACTION_OPERATION_INSTALL);
  g_assert_cmpstr (flatpak_transaction_operation_get_ref (op), ==, app);

  g_list_free_full (ops, g_object_unref);

  return FALSE;
}

/* test disabling dependencies and related */
static void
test_transaction_deps (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(GError) error = NULL;
  gboolean res;
  g_autofree char *app = NULL;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  empty_installation (inst);

  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  flatpak_transaction_set_disable_dependencies (transaction, TRUE);
  flatpak_transaction_set_disable_related (transaction, TRUE);

  res = flatpak_transaction_add_install (transaction, repo_name, app, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_signal_connect (transaction, "ready", G_CALLBACK (check_ready1_abort), NULL);
  flatpak_transaction_run (transaction, NULL, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED);
  g_clear_error (&error);

  g_clear_object (&transaction);

  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  flatpak_transaction_set_disable_dependencies (transaction, FALSE);
  flatpak_transaction_set_disable_related (transaction, FALSE);

  res = flatpak_transaction_add_install (transaction, repo_name, app, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_signal_connect (transaction, "ready", G_CALLBACK (check_ready3_abort), NULL);
  flatpak_transaction_run (transaction, NULL, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED);
}

/* install from a local repository */
static void
test_transaction_install_local (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(GError) error = NULL;
  gboolean res;
  g_autofree char *dir = NULL;
  g_autofree char *path = NULL;
  g_autofree char *url = NULL;
  g_autoptr(FlatpakRemote) remote = NULL;
  g_autofree char *app = NULL;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  empty_installation (inst);

  remote = flatpak_installation_get_remote_by_name (inst, "hello-origin", NULL, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_REMOTE_NOT_FOUND);
  g_assert_null (remote);
  g_clear_error (&error);

  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  dir = g_get_current_dir ();
  path = g_build_filename (dir, "repos", "test", NULL);
  url = g_strconcat ("file://", path, NULL);
  res = flatpak_transaction_add_install (transaction, url, app, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  remote = flatpak_installation_get_remote_by_name (inst, "hello-origin", NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (remote);
}

/* Test that we pull the runtime from the same remote as the app even if
 * another remote also provides the runtime and sorts earlier via strcmp() */
static void
test_transaction_app_runtime_same_remote (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(FlatpakInstalledRef) installed_runtime = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *app = NULL;
  const char *runtime_origin;
  gboolean res;
  int old_prio;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  empty_installation (inst);

  add_remote_user ("test-runtime-only", "aaatest-runtime-only-repo");

  /* Drop caches so we find the new remote */
  flatpak_installation_drop_caches (inst, NULL, &error);
  g_assert_no_error (error);

  /* Even with aaatest-runtime-only-repo sorting before test-repo, the runtime
   * should come from test-repo */
  g_assert_true (strcmp ("aaatest-runtime-only-repo", "test-repo") < 0);

  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_install (transaction, repo_name, app, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  installed_runtime = flatpak_installation_get_installed_ref (inst,
                                                              FLATPAK_REF_KIND_RUNTIME,
                                                              "org.test.Platform",
                                                              flatpak_get_default_arch (),
                                                              "master", NULL, &error);
  g_assert_nonnull (installed_runtime);
  g_assert_no_error (error);

  runtime_origin = flatpak_installed_ref_get_origin (installed_runtime);
  g_assert_nonnull (runtime_origin);
  g_assert_cmpstr (runtime_origin, ==, repo_name);

  /* Reset things */
  empty_installation (inst);
  remove_remote_user ("aaatest-runtime-only");
}

typedef struct
{
  GMainLoop *loop;
  guint      stop_waiting_id;
  guint      hello_dead_cb_id;
  gboolean   hello_dead;
} TestInstanceContext;

static gboolean
stop_waiting (gpointer data)
{
  TestInstanceContext *context = data;

  g_main_loop_quit (context->loop);
  context->stop_waiting_id = 0;

  return G_SOURCE_REMOVE;
}

static void
hello_dead_cb (GPid     pid,
               int      status,
               gpointer data)
{
  TestInstanceContext *context = data;

  context->hello_dead = TRUE;
  context->hello_dead_cb_id = 0;

  g_main_loop_quit (context->loop);
}

/* test the instance api: install an app, launch it, get the instance,
 * kill it, wait for it to die
 */
static void
test_instance (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(GError) error = NULL;
  gboolean res;
  g_autoptr(GPtrArray) instances = NULL;
  FlatpakInstance *instance;
  GKeyFile *info;
  g_autofree char *value = NULL;
  int i;
  g_autofree char *app = NULL;
  g_autofree char *runtime = NULL;
  TestInstanceContext context = { NULL, 0, 0, FALSE };

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());
  runtime = g_strdup_printf ("runtime/org.test.Platform/%s/master",
                             flatpak_get_default_arch ());

  update_test_app ();
  update_repo ("test");

  if (!check_bwrap_support ())
    {
      g_test_skip ("bwrap not supported");
      return;
    }

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  empty_installation (inst);

  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_install (transaction, repo_name, app, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_clear_object (&transaction);

  res = flatpak_installation_launch_full (inst, FLATPAK_LAUNCH_FLAGS_DO_NOT_REAP,
                                          "org.test.Hello", NULL, NULL, NULL, &instance, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);
  g_assert_nonnull (instance);

  instances = flatpak_instance_get_all ();
  for (i = 0; i < instances->len; i++)
    {
      FlatpakInstance *instance2 = g_ptr_array_index (instances, i);
      if (strcmp (flatpak_instance_get_id (instance), flatpak_instance_get_id (instance2)) == 0)
        break;
    }
  g_assert_cmpint (i, <, instances->len);
  g_clear_pointer (&instances, g_ptr_array_unref);

  g_assert_true (flatpak_instance_is_running (instance));

  g_assert_nonnull (flatpak_instance_get_id (instance));
  info = flatpak_instance_get_info (instance);
  g_assert_nonnull (info);
  value = g_key_file_get_string (info, "Application", "name", &error);
  g_assert_cmpstr (value, ==, "org.test.Hello");
  g_clear_pointer (&value, g_free);
  value = g_key_file_get_string (info, "Instance", "instance-id", &error);
  g_assert_cmpstr (value, ==, flatpak_instance_get_id (instance));
  g_clear_pointer (&value, g_free);

  g_assert_cmpstr (flatpak_instance_get_app (instance), ==, "org.test.Hello");
  g_assert_cmpstr (flatpak_instance_get_arch (instance), ==,
                   flatpak_get_default_arch ());
  g_assert_cmpstr (flatpak_instance_get_branch (instance), ==, "master");
  g_assert_nonnull (flatpak_instance_get_commit (instance));
  g_assert_cmpstr (flatpak_instance_get_runtime (instance), ==, runtime);
  g_assert_nonnull (flatpak_instance_get_runtime_commit (instance));
  g_assert_cmpint (flatpak_instance_get_pid (instance), >, 0);
  while (flatpak_instance_get_child_pid (instance) == 0)
    g_usleep (10000);
  g_assert_cmpint (flatpak_instance_get_child_pid (instance), >, 0);

  context.loop = g_main_loop_new (NULL, FALSE);

  context.hello_dead = FALSE;
  context.hello_dead_cb_id = g_child_watch_add (flatpak_instance_get_pid (instance),
                                                hello_dead_cb, &context);
  context.stop_waiting_id = g_timeout_add (5000, stop_waiting, &context);

  kill (flatpak_instance_get_child_pid (instance), SIGKILL);

  g_main_loop_run (context.loop);

  if (context.hello_dead_cb_id != 0)
    g_source_remove (context.hello_dead_cb_id);

  if (context.stop_waiting_id != 0)
    g_source_remove (context.stop_waiting_id);

  g_main_loop_unref (context.loop);

  g_assert_true (context.hello_dead);
  g_assert_false (flatpak_instance_is_running (instance));

  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_uninstall (transaction, app, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);
}

static void
test_update_subpaths (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(FlatpakInstalledRef) ref = NULL;
  gboolean res;
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  const char * const *subpaths;
  const char * subpaths2[] = { "/de", "/fr", NULL };
  g_autofree char *app = NULL;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  empty_installation (inst);

  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_install (transaction, repo_name, app, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_clear_object (&transaction);

  ref = flatpak_installation_get_installed_ref (inst, FLATPAK_REF_KIND_RUNTIME, "org.test.Hello.Locale", flatpak_get_default_arch (), "master", NULL, &error);
  g_assert_no_error (error);

  subpaths = flatpak_installed_ref_get_subpaths (ref);
  g_assert_cmpint (g_strv_length ((char **) subpaths), ==, 1);
  g_assert_cmpstr (subpaths[0], ==, "/de");

  g_clear_object (&transaction);
  g_clear_object (&ref);

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  ref = flatpak_installation_update_full (inst, 0, FLATPAK_REF_KIND_RUNTIME, "org.test.Hello.Locale", flatpak_get_default_arch (), "master", subpaths2, NULL, NULL, NULL, &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);

  subpaths = flatpak_installed_ref_get_subpaths (ref);
  g_assert_cmpint (g_strv_length ((char **) subpaths), ==, 2);
  g_assert_cmpstr (subpaths[0], ==, "/de");
  g_assert_cmpstr (subpaths[1], ==, "/fr");
}

static void
test_overrides (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *data = NULL;
  g_autoptr(GKeyFile) overrides = NULL;
  gboolean res;
  g_autofree char *value = NULL;
  g_autoptr(FlatpakInstalledRef) ref = NULL;
  g_auto(GStrv) list = NULL;
  gsize len;

  if (!check_bwrap_support ())
    {
      g_test_skip ("bwrap not supported");
      return;
    }

  /* no library api to set overrides, so... */
  const char *argv[] = { "flatpak", "override", "--user",
                         "--allow=bluetooth",
                         "--disallow=canbus",
                         "--device=dri",
                         "--nodevice=kvm",
                         "--filesystem=xdg-music",
                         "--filesystem=~/foo:ro",
                         "--filesystem=xdg-download/subdir:create",
                         "--env=FOO=BAR",
                         "--own-name=foo.bar.baz",
                         "--talk-name=hello.bla.bla.*",
                         "--socket=wayland",
                         "--nosocket=pulseaudio",
                         "org.test.Hello",
                         NULL };
  run_test_subprocess ((char **) argv, RUN_TEST_SUBPROCESS_DEFAULT);

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  empty_installation (inst);

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  ref = flatpak_installation_update (inst, 0, FLATPAK_REF_KIND_APP, "org.test.Hello", NULL, "master", NULL, NULL, NULL, &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED);
  g_assert_null (ref);
  g_clear_error (&error);

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  ref = flatpak_installation_install (inst, repo_name, FLATPAK_REF_KIND_APP, "org.test.Hello", NULL, "master", NULL, NULL, NULL, &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);
  g_assert_nonnull (ref);

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  ref = flatpak_installation_install (inst, repo_name, FLATPAK_REF_KIND_RUNTIME, "org.test.Platform", NULL, "master", NULL, NULL, NULL, &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);
  g_assert_nonnull (ref);

  res = flatpak_installation_launch (inst, "org.test.Hello", NULL, "master", NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (ref);

  data = flatpak_installation_load_app_overrides (inst, "org.test.Hello", NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (data);

  overrides = g_key_file_new ();
  res = g_key_file_load_from_data (overrides, data, -1, 0, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  value = g_key_file_get_string (overrides, "Context", "devices", &error);
  g_assert_cmpstr (value, ==, "dri;!kvm;");
  g_clear_pointer (&value, g_free);

  value = g_key_file_get_string (overrides, "Context", "features", &error);
  g_assert_cmpstr (value, ==, "bluetooth;!canbus;");
  g_clear_pointer (&value, g_free);

  list = g_key_file_get_string_list (overrides, "Context", "filesystems", &len, &error);
  g_assert_cmpint (len, ==, 3);
  g_assert_true (g_strv_contains ((const char * const *) list, "xdg-download/subdir:create"));
  g_assert_true (g_strv_contains ((const char * const *) list, "xdg-music"));
  g_assert_true (g_strv_contains ((const char * const *) list, "~/foo:ro"));
  g_clear_pointer (&list, g_strfreev);

  list = g_key_file_get_string_list (overrides, "Context", "sockets", &len, &error);
  g_assert_cmpint (len, ==, 2);
  g_assert_true (g_strv_contains ((const char * const *) list, "wayland"));
  g_assert_true (g_strv_contains ((const char * const *) list, "!pulseaudio"));
  g_clear_pointer (&list, g_strfreev);

  value = g_key_file_get_string (overrides, "Session Bus Policy", "hello.bla.bla.*", &error);
  g_assert_cmpstr (value, ==, "talk");
  g_clear_pointer (&value, g_free);

  value = g_key_file_get_string (overrides, "Session Bus Policy", "foo.bar.baz", &error);
  g_assert_cmpstr (value, ==, "own");
  g_clear_pointer (&value, g_free);

  value = g_key_file_get_string (overrides, "Environment", "FOO", &error);
  g_assert_cmpstr (value, ==, "BAR");
  g_clear_pointer (&value, g_free);

  const char *argv2[] = { "flatpak", "override", "--user", "--reset", "org.test.Hello", NULL };
  run_test_subprocess ((char **) argv2, RUN_TEST_SUBPROCESS_DEFAULT);
}

/* basic tests for bundle ref apis */
static void
test_bundle (void)
{
  g_autoptr(FlatpakBundleRef) ref = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GFile) file2 = NULL;
  g_autofree char *origin = NULL;
  g_autofree char *repo_url = g_strdup_printf ("http://127.0.0.1:%s/test", httpd_port);
  g_autoptr(GBytes) metadata = NULL;
  g_autoptr(GBytes) appstream = NULL;
  g_autoptr(GBytes) icon = NULL;

  file = g_file_new_for_path ("/dev/null");

  ref = flatpak_bundle_ref_new (file, &error);
  g_assert_nonnull (error);
  g_assert_null (ref);
  g_clear_error (&error);

  g_clear_object (&file);

  path = g_build_filename (testdir, "bundles", "hello.flatpak", NULL);
  file = g_file_new_for_path (path);
  ref = flatpak_bundle_ref_new (file, &error);
  g_assert_no_error (error);
  g_assert_nonnull (ref);

  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (ref)), ==, "org.test.Hello");
  g_assert_cmpstr (flatpak_ref_get_arch (FLATPAK_REF (ref)), ==, flatpak_get_default_arch ());
  g_assert_cmpstr (flatpak_ref_get_branch (FLATPAK_REF (ref)), ==, "master");
  g_assert_cmpint (flatpak_ref_get_kind (FLATPAK_REF (ref)), ==, FLATPAK_REF_KIND_APP);
  g_assert_cmpstr (flatpak_ref_get_collection_id (FLATPAK_REF (ref)), ==, "com.example.Test");

  file2 = flatpak_bundle_ref_get_file (ref);
  g_assert_true (g_file_equal (file, file2));

  origin = flatpak_bundle_ref_get_origin (ref);
  g_assert_cmpstr (origin, ==, repo_url);

  g_assert_null (flatpak_bundle_ref_get_runtime_repo_url (ref));

  g_assert_cmpint (flatpak_bundle_ref_get_installed_size (ref), >, 0);

  metadata = flatpak_bundle_ref_get_metadata (ref);
  g_assert_nonnull (metadata);
  /* FIXME verify format */

  appstream = flatpak_bundle_ref_get_appstream (ref);
  g_assert_nonnull (appstream);
  /* FIXME verify format */

  icon = flatpak_bundle_ref_get_icon (ref, 64);
  g_assert_nonnull (icon);
  /* FIXME verify format */

  icon = flatpak_bundle_ref_get_icon (ref, 128);
  g_assert_null (icon);

  g_clear_object (&file2);

  g_object_get (ref, "file", &file2, NULL);
  g_assert_true (g_file_equal (file, file2));
}

/* use the installation api to install a bundle */
static void
test_install_bundle (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  g_autofree char *path = NULL;
  g_autoptr(FlatpakInstalledRef) ref = NULL;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  empty_installation (inst);

  path = g_build_filename (testdir, "bundles", "hello.flatpak", NULL);
  file = g_file_new_for_path (path);

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  ref = flatpak_installation_install_bundle (inst, file, NULL, NULL, NULL, &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);
  g_assert_nonnull (ref);
}

/* use the installation api to install a flatpakref */
static void
test_install_flatpakref (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakRemoteRef) ref = NULL;
  g_autofree char *s = NULL;
  g_autoptr(GBytes) data = NULL;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  empty_installation (inst);

  s = g_strconcat ("[Flatpak Ref]\n"
                   "Title=Test App\n"
                   "Name=org.test.Hello\n"
                   "Branch=master\n"
                   "Url=http://127.0.0.1:", httpd_port, "/test\n"
                   "IsRuntime=False\n"
                   "SuggestRemoteName=test-repo\n"
                   "RuntimeRepo=http://127.0.0.1:", httpd_port, "/test/test-repo.flatpakrepo\n",
                   NULL);
  data = g_bytes_new (s, strlen (s));

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  ref = flatpak_installation_install_ref_file (inst, data, NULL, &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);
  g_assert_nonnull (ref);
}

/* test the installation method to list installed related refs */
static void
test_list_installed_related_refs (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(GPtrArray) refs = NULL;
  g_autoptr(GError) error = NULL;
  FlatpakRelatedRef *ref;
  FlatpakInstalledRef *iref;
  gboolean res;
  g_autofree char *app = NULL;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  empty_installation (inst);

  refs = flatpak_installation_list_installed_related_refs_sync (inst, repo_name, app, NULL, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED);
  g_assert_null (refs);
  g_clear_error (&error);

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  iref = flatpak_installation_install (inst, repo_name, FLATPAK_REF_KIND_APP, "org.test.Hello", NULL, "master", NULL, NULL, NULL, &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);
  g_assert_nonnull (iref);
  g_clear_object (&iref);

  refs = flatpak_installation_list_installed_related_refs_sync (inst, repo_name, app, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (refs);
  g_assert_cmpint (refs->len, ==, 0);
  g_clear_pointer (&refs, g_ptr_array_unref);

  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_update (transaction, app, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_clear_object (&transaction);

  refs = flatpak_installation_list_installed_related_refs_sync (inst, repo_name, app, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (refs);
  g_assert_cmpint (refs->len, ==, 1);

  ref = g_ptr_array_index (refs, 0);

  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (ref)), ==, "org.test.Hello.Locale");
  g_assert_true (flatpak_related_ref_should_download (ref));
  g_assert_true (flatpak_related_ref_should_delete (ref));
  g_assert_false (flatpak_related_ref_should_autoprune (ref));
  g_assert_true (g_strv_length ((char **) flatpak_related_ref_get_subpaths (ref)) == 1);
  g_assert_cmpstr (flatpak_related_ref_get_subpaths (ref)[0], ==, "/de");

  // Make the test with extra-languages, instead of languages
  clean_languages();
  configure_extra_languages();
  empty_installation (inst);

  refs = flatpak_installation_list_installed_related_refs_sync (inst, repo_name, app, NULL, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED);
  g_assert_null (refs);
  g_clear_error (&error);

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  iref = flatpak_installation_install (inst, repo_name, FLATPAK_REF_KIND_APP, "org.test.Hello", NULL, "master", NULL, NULL, NULL, &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);
  g_assert_nonnull (iref);
  g_clear_object (&iref);

  refs = flatpak_installation_list_installed_related_refs_sync (inst, repo_name, app, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (refs);
  g_assert_cmpint (refs->len, ==, 0);
  g_clear_pointer (&refs, g_ptr_array_unref);

  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_update (transaction, app, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  g_clear_object (&transaction);

  refs = flatpak_installation_list_installed_related_refs_sync (inst, repo_name, app, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (refs);
  g_assert_cmpint (refs->len, ==, 1);

  ref = g_ptr_array_index (refs, 0);

  g_assert_cmpstr (flatpak_ref_get_name (FLATPAK_REF (ref)), ==, "org.test.Hello.Locale");
  g_assert_true (flatpak_related_ref_should_download (ref));
  g_assert_true (flatpak_related_ref_should_delete (ref));
  g_assert_false (flatpak_related_ref_should_autoprune (ref));
  g_assert_true (g_strv_length ((char **) flatpak_related_ref_get_subpaths (ref)) == 2);
  g_assert_cmpstr (flatpak_related_ref_get_subpaths (ref)[0], ==, "/de");
  g_assert_cmpstr (flatpak_related_ref_get_subpaths (ref)[1], ==, "/en");

  configure_languages ("de");
  clean_extra_languages ();
}

static void
test_no_deploy (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakInstalledRef) ref = NULL;
  gboolean res;
  g_autofree char *app = NULL;

  app = g_strdup_printf ("app/org.test.Hello/%s/master",
                         flatpak_get_default_arch ());

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  empty_installation (inst);

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  ref = flatpak_installation_install_full (inst,
                                           FLATPAK_INSTALL_FLAGS_NO_DEPLOY,
                                           repo_name,
                                           FLATPAK_REF_KIND_APP,
                                           "org.test.Hello",
                                           NULL,
                                           "master",
                                           NULL,
                                           NULL,
                                           NULL,
                                           NULL,
                                           &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_ONLY_PULLED);
  g_assert_null (ref);
  g_clear_error (&error);

  res = flatpak_installation_remove_local_ref_sync (inst,
                                                    repo_name,
                                                    app,
                                                    NULL,
                                                    &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_installation_prune_local_repo (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);
}

static void
test_bad_remote_name (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakRemote) remote = NULL;
  gboolean res;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  empty_installation (inst);

  remote = flatpak_remote_new ("3X \n bad");
  flatpak_remote_set_url (remote, "not a url at all");

  res = flatpak_installation_modify_remote (inst, remote, NULL, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_INVALID_DATA);
  g_assert_false (res);
}

/* Disable all remotes, and install an app from a repo that does not have the runtime.
 * We expect an error.
 */
static void
test_transaction_no_runtime (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  gboolean res;
  g_autofree char *s = NULL;
  g_autoptr(GBytes) data = NULL;
  FlatpakRemote *remote = NULL;
  g_autoptr(GPtrArray) remotes = NULL;
  int i;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  empty_installation (inst);

  remotes = flatpak_installation_list_remotes (inst, NULL, &error);
  g_assert_no_error (error);
  for (i = 0; i < remotes->len; i++)
    {
      remote = g_ptr_array_index (remotes, i);
      flatpak_remote_set_disabled (remote, TRUE);
      res = flatpak_installation_modify_remote (inst, remote, NULL, &error);
      g_assert_no_error (error);
      g_assert_true (res);
    }

  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  s = g_strconcat ("[Flatpak Ref]\n"
                   "Title=Test App\n"
                   "Name=org.test.Hello\n"
                   "Branch=master\n"
                   "Url=http://127.0.0.1:", httpd_port, "/test-without-runtime\n"
                   "IsRuntime=False\n"
                   "SuggestRemoteName=my-little-repo\n"
                   "RuntimeRepo=http://127.0.0.1:", httpd_port, "/test/test-repo.flatpakrepo\n",
                   NULL);

  data = g_bytes_new (s, strlen (s));
  res = flatpak_transaction_add_install_flatpakref (transaction, data, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_error (error, FLATPAK_ERROR, FLATPAK_ERROR_RUNTIME_NOT_FOUND);
  g_assert_false (res);
  g_clear_error (&error);

  for (i = 0; i < remotes->len; i++)
    {
      remote = g_ptr_array_index (remotes, i);
      flatpak_remote_set_disabled (remote, FALSE);
      res = flatpak_installation_modify_remote (inst, remote, NULL, &error);
      g_assert_no_error (error);
      g_assert_true (res);
    }
}

static void
test_installation_no_interaction (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  g_assert_false (flatpak_installation_get_no_interaction (inst));
  flatpak_installation_set_no_interaction (inst, TRUE);
  g_assert_true (flatpak_installation_get_no_interaction (inst));
}

static void
test_installation_unused_refs (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) refs = NULL;
  FlatpakInstalledRef *iref;

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);

  empty_installation (inst);

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  iref = flatpak_installation_install (inst, repo_name, FLATPAK_REF_KIND_APP, "org.test.Hello", NULL, "master", NULL, NULL, NULL, &error);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_no_error (error);
  g_assert_nonnull (iref);
  g_clear_object (&iref);

  refs = flatpak_installation_list_unused_refs (inst, NULL, NULL, &error);
  g_assert_nonnull (refs);
  g_assert_no_error (error);
  g_assert_cmpint (refs->len, ==, 0);
}

static void
test_installation_unused_refs_excludes_pins (void)
{
  g_autoptr(FlatpakInstallation) inst = NULL;
  g_autoptr(FlatpakTransaction) transaction = NULL;
  g_autoptr(GPtrArray) refs = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *runtime = NULL;
  gboolean res;

  runtime = g_strdup_printf ("runtime/org.test.Platform/%s/master",
                             flatpak_get_default_arch ());

  inst = flatpak_installation_new_user (NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (inst);

  empty_installation (inst);

  transaction = flatpak_transaction_new_for_installation (inst, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (transaction);

  res = flatpak_transaction_add_install (transaction, repo_name, runtime, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  res = flatpak_transaction_run (transaction, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (res);

  /* Even though the runtime is unused, it shouldn't show up in
   * list_unused_refs() because it was installed explicitly not as a dependency
   * of an app */
  refs = flatpak_installation_list_unused_refs (inst, NULL, NULL, &error);
  g_assert_nonnull (refs);
  g_assert_no_error (error);
  g_assert_cmpint (refs->len, ==, 0);
}

int
main (int argc, char *argv[])
{
  int res;

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/library/version", test_library_version);
  g_test_add_func ("/library/types", test_library_types);
  g_test_add_func ("/library/user-installation", test_user_installation);
  g_test_add_func ("/library/system-installation", test_system_installation);
  g_test_add_func ("/library/multiple-system-installation", test_multiple_system_installations);
  g_test_add_func ("/library/installation-config", test_installation_config);
  g_test_add_func ("/library/languages-config", test_languages_config);
  g_test_add_func ("/library/arches", test_arches);
  g_test_add_func ("/library/ref", test_ref);
  g_test_add_func ("/library/list-remotes", test_list_remotes);
  g_test_add_func ("/library/remote-by-name", test_remote_by_name);
  g_test_add_func ("/library/remote", test_remote);
  g_test_add_func ("/library/remote-new", test_remote_new);
  g_test_add_func ("/library/remote-new-from-file", test_remote_new_from_file);
  g_test_add_func ("/library/list-remote-refs", test_list_remote_refs);
  g_test_add_func ("/library/list-remote-related-refs", test_list_remote_related_refs);
  g_test_add_func ("/library/list-refs", test_list_refs);
  g_test_add_func ("/library/install-launch-uninstall", test_install_launch_uninstall);
  g_test_add_func ("/library/list-refs-in-remote", test_list_refs_in_remotes);
  g_test_add_func ("/library/list-updates", test_list_updates);
  g_test_add_func ("/library/list-undeployed-updates", test_list_undeployed_updates);
  g_test_add_func ("/library/list-updates-offline", test_list_updates_offline);
  g_test_add_func ("/library/transaction", test_misc_transaction);
  g_test_add_func ("/library/transaction-install-uninstall", test_transaction_install_uninstall);
  g_test_add_func ("/library/transaction-install-flatpakref", test_transaction_install_flatpakref);
  g_test_add_func ("/library/transaction-flatpakref-remote-creation", test_transaction_flatpakref_remote_creation);
  g_test_add_func ("/library/transaction-deps", test_transaction_deps);
  g_test_add_func ("/library/transaction-install-local", test_transaction_install_local);
  g_test_add_func ("/library/transaction-app-runtime-same-remote", test_transaction_app_runtime_same_remote);
  g_test_add_func ("/library/instance", test_instance);
  g_test_add_func ("/library/update-subpaths", test_update_subpaths);
  g_test_add_func ("/library/overrides", test_overrides);
  g_test_add_func ("/library/bundle", test_bundle);
  g_test_add_func ("/library/install-bundle", test_install_bundle);
  g_test_add_func ("/library/install-flatpakref", test_install_flatpakref);
  g_test_add_func ("/library/list-installed-related-refs", test_list_installed_related_refs);
  g_test_add_func ("/library/update-related-refs", test_update_related_refs);
  g_test_add_func ("/library/update-installed-ref-if-missing-runtime", test_update_installed_ref_if_missing_runtime);
  g_test_add_func ("/library/no-deploy", test_no_deploy);
  g_test_add_func ("/library/bad-remote-name", test_bad_remote_name);
  g_test_add_func ("/library/transaction-no-runtime", test_transaction_no_runtime);
  g_test_add_func ("/library/installation-no-interaction", test_installation_no_interaction);
  g_test_add_func ("/library/installation-unused-refs", test_installation_unused_refs);
  g_test_add_func ("/library/installation-unused-refs-excludes-pins", test_installation_unused_refs_excludes_pins);

  global_setup ();

  res = g_test_run ();

  global_teardown ();

  return res;
}
