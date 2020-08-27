/*
 * Copyright © 2020 Collabora Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib.h>
#include "flatpak.h"
#include "flatpak-bwrap-private.h"
#include "flatpak-context-private.h"
#include "flatpak-exports-private.h"
#include "flatpak-run-private.h"

static char *testdir;

static void
global_setup (void)
{
  g_autofree char *cachedir = NULL;
  g_autofree char *configdir = NULL;
  g_autofree char *datadir = NULL;
  g_autofree char *homedir = NULL;
  g_autofree char *runtimedir = NULL;

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

  runtimedir = g_strconcat (testdir, "/runtime", NULL);
  g_mkdir_with_parents (runtimedir, S_IRWXU);
  g_setenv ("XDG_RUNTIME_DIR", runtimedir, TRUE);
  g_test_message ("setting XDG_RUNTIME_DIR=%s", runtimedir);

  g_reload_user_special_dirs_cache ();

  g_assert_cmpstr (g_get_user_cache_dir (), ==, cachedir);
  g_assert_cmpstr (g_get_user_config_dir (), ==, configdir);
  g_assert_cmpstr (g_get_user_data_dir (), ==, datadir);
  g_assert_cmpstr (g_get_user_runtime_dir (), ==, runtimedir);
}

static void
global_teardown (void)
{
  if (g_getenv ("SKIP_TEARDOWN"))
    return;

  glnx_shutil_rm_rf_at (-1, testdir, NULL, NULL);
  g_free (testdir);
}

/*
 * Assert that the next few arguments starting from @i are setting up
 * /run/host/os-release. Return the next argument that hasn't been used.
 */
G_GNUC_WARN_UNUSED_RESULT static gsize
assert_next_is_os_release (FlatpakBwrap *bwrap,
                           gsize i)
{
  if (g_file_test ("/etc/os-release", G_FILE_TEST_EXISTS))
    {
      g_assert_cmpuint (i, <, bwrap->argv->len);
      g_assert_cmpstr (bwrap->argv->pdata[i++], ==, "--ro-bind");
      g_assert_cmpuint (i, <, bwrap->argv->len);
      g_assert_cmpstr (bwrap->argv->pdata[i++], ==, "/etc/os-release");
      g_assert_cmpuint (i, <, bwrap->argv->len);
      g_assert_cmpstr (bwrap->argv->pdata[i++], ==, "/run/host/os-release");
    }
  else if (g_file_test ("/usr/lib/os-release", G_FILE_TEST_EXISTS))
    {
      g_assert_cmpuint (i, <, bwrap->argv->len);
      g_assert_cmpstr (bwrap->argv->pdata[i++], ==, "--ro-bind");
      g_assert_cmpuint (i, <, bwrap->argv->len);
      g_assert_cmpstr (bwrap->argv->pdata[i++], ==, "/usr/lib/os-release");
      g_assert_cmpuint (i, <, bwrap->argv->len);
      g_assert_cmpstr (bwrap->argv->pdata[i++], ==, "/run/host/os-release");
    }

  return i;
}

/* Assert that arguments starting from @i are --dir @dir.
 * Return the new @i. */
G_GNUC_WARN_UNUSED_RESULT static gsize
assert_next_is_dir (FlatpakBwrap *bwrap,
                    gsize i,
                    const char *dir)
{
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, "--dir");
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, dir);
  return i;
}

/* Assert that arguments starting from @i are --tmpfs @dir.
 * Return the new @i. */
G_GNUC_WARN_UNUSED_RESULT static gsize
assert_next_is_tmpfs (FlatpakBwrap *bwrap,
                      gsize i,
                      const char *dir)
{
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, "--tmpfs");
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, dir);
  return i;
}

/* Assert that arguments starting from @i are @how @path @path.
 * Return the new @i. */
G_GNUC_WARN_UNUSED_RESULT static gsize
assert_next_is_bind (FlatpakBwrap *bwrap,
                     gsize i,
                     const char *how,
                     const char *path)
{
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, how);
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, path);
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, path);
  return i;
}

/* Print the arguments of a call to bwrap. */
static void
print_bwrap (FlatpakBwrap *bwrap)
{
  guint i;

  for (i = 0; i < bwrap->argv->len && bwrap->argv->pdata[i] != NULL; i++)
    g_test_message ("%s", (const char *) bwrap->argv->pdata[i]);

  g_test_message ("--");
}

static void
test_empty_context (void)
{
  g_autoptr(FlatpakBwrap) bwrap = flatpak_bwrap_new (NULL);
  g_autoptr(FlatpakContext) context = flatpak_context_new ();
  g_autoptr(FlatpakExports) exports = NULL;

  g_assert_cmpuint (g_hash_table_size (context->env_vars), ==, 0);
  g_assert_cmpuint (g_hash_table_size (context->persistent), ==, 0);
  g_assert_cmpuint (g_hash_table_size (context->filesystems), ==, 0);
  g_assert_cmpuint (g_hash_table_size (context->session_bus_policy), ==, 0);
  g_assert_cmpuint (g_hash_table_size (context->system_bus_policy), ==, 0);
  g_assert_cmpuint (g_hash_table_size (context->generic_policy), ==, 0);
  g_assert_cmpuint (context->shares, ==, 0);
  g_assert_cmpuint (context->shares_valid, ==, 0);
  g_assert_cmpuint (context->sockets, ==, 0);
  g_assert_cmpuint (context->sockets_valid, ==, 0);
  g_assert_cmpuint (context->devices, ==, 0);
  g_assert_cmpuint (context->devices_valid, ==, 0);
  g_assert_cmpuint (context->features, ==, 0);
  g_assert_cmpuint (context->features_valid, ==, 0);
  g_assert_cmpuint (flatpak_context_get_run_flags (context), ==, 0);

  exports = flatpak_context_get_exports (context, "com.example.App");
  g_assert_nonnull (exports);

  g_clear_pointer (&exports, flatpak_exports_free);
  flatpak_context_append_bwrap_filesystem (context, bwrap,
                                           "com.example.App",
                                           NULL,
                                           NULL,
                                           &exports);
  print_bwrap (bwrap);
  g_assert_nonnull (exports);
}

static void
test_full_context (void)
{
  g_autoptr(FlatpakBwrap) bwrap = flatpak_bwrap_new (NULL);
  g_autoptr(FlatpakContext) context = flatpak_context_new ();
  g_autoptr(FlatpakExports) exports = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  g_autofree gchar *text = NULL;
  g_auto(GStrv) strv = NULL;
  gsize i, n;

  g_key_file_set_value (keyfile,
                        FLATPAK_METADATA_GROUP_CONTEXT,
                        FLATPAK_METADATA_KEY_SHARED,
                        "network;ipc;");
  g_key_file_set_value (keyfile,
                        FLATPAK_METADATA_GROUP_CONTEXT,
                        FLATPAK_METADATA_KEY_SOCKETS,
                        "x11;wayland;pulseaudio;session-bus;system-bus;"
                        "fallback-x11;ssh-auth;pcsc;cups;");
  g_key_file_set_value (keyfile,
                        FLATPAK_METADATA_GROUP_CONTEXT,
                        FLATPAK_METADATA_KEY_DEVICES,
                        "dri;all;kvm;shm;");
  g_key_file_set_value (keyfile,
                        FLATPAK_METADATA_GROUP_CONTEXT,
                        FLATPAK_METADATA_KEY_FEATURES,
                        "devel;multiarch;bluetooth;canbus;");
  g_key_file_set_value (keyfile,
                        FLATPAK_METADATA_GROUP_CONTEXT,
                        FLATPAK_METADATA_KEY_FILESYSTEMS,
                        "host;/home;!/opt");
  g_key_file_set_value (keyfile,
                        FLATPAK_METADATA_GROUP_CONTEXT,
                        FLATPAK_METADATA_KEY_PERSISTENT,
                        ".openarena;");
  g_key_file_set_value (keyfile,
                        FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY,
                        "org.example.SessionService",
                        "own");
  g_key_file_set_value (keyfile,
                        FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY,
                        "net.example.SystemService",
                        "talk");
  g_key_file_set_value (keyfile,
                        FLATPAK_METADATA_GROUP_ENVIRONMENT,
                        "HYPOTHETICAL_PATH", "/foo:/bar");
  g_key_file_set_value (keyfile,
                        FLATPAK_METADATA_GROUP_PREFIX_POLICY "MyPolicy",
                        "Colours", "blue;green;");

  flatpak_context_load_metadata (context, keyfile, &error);
  g_assert_no_error (error);

  g_assert_cmpuint (context->shares, ==,
                    (FLATPAK_CONTEXT_SHARED_NETWORK |
                     FLATPAK_CONTEXT_SHARED_IPC));
  g_assert_cmpuint (context->shares_valid, ==, context->shares);
  g_assert_cmpuint (context->devices, ==,
                    (FLATPAK_CONTEXT_DEVICE_DRI |
                     FLATPAK_CONTEXT_DEVICE_ALL |
                     FLATPAK_CONTEXT_DEVICE_KVM |
                     FLATPAK_CONTEXT_DEVICE_SHM));
  g_assert_cmpuint (context->devices_valid, ==, context->devices);
  g_assert_cmpuint (context->sockets, ==,
                    (FLATPAK_CONTEXT_SOCKET_X11 |
                     FLATPAK_CONTEXT_SOCKET_WAYLAND |
                     FLATPAK_CONTEXT_SOCKET_PULSEAUDIO |
                     FLATPAK_CONTEXT_SOCKET_SESSION_BUS |
                     FLATPAK_CONTEXT_SOCKET_SYSTEM_BUS |
                     FLATPAK_CONTEXT_SOCKET_FALLBACK_X11 |
                     FLATPAK_CONTEXT_SOCKET_SSH_AUTH |
                     FLATPAK_CONTEXT_SOCKET_PCSC |
                     FLATPAK_CONTEXT_SOCKET_CUPS));
  g_assert_cmpuint (context->sockets_valid, ==, context->sockets);
  g_assert_cmpuint (context->features, ==,
                    (FLATPAK_CONTEXT_FEATURE_DEVEL |
                     FLATPAK_CONTEXT_FEATURE_MULTIARCH |
                     FLATPAK_CONTEXT_FEATURE_BLUETOOTH |
                     FLATPAK_CONTEXT_FEATURE_CANBUS));
  g_assert_cmpuint (context->features_valid, ==, context->features);

  g_assert_cmpuint (flatpak_context_get_run_flags (context), ==,
                    (FLATPAK_RUN_FLAG_DEVEL |
                     FLATPAK_RUN_FLAG_MULTIARCH |
                     FLATPAK_RUN_FLAG_BLUETOOTH |
                     FLATPAK_RUN_FLAG_CANBUS));

  exports = flatpak_context_get_exports (context, "com.example.App");
  g_assert_nonnull (exports);

  g_clear_pointer (&exports, flatpak_exports_free);
  flatpak_context_append_bwrap_filesystem (context, bwrap,
                                           "com.example.App",
                                           NULL,
                                           NULL,
                                           &exports);
  print_bwrap (bwrap);
  g_assert_nonnull (exports);

  g_clear_pointer (&keyfile, g_key_file_unref);
  keyfile = g_key_file_new ();
  flatpak_context_save_metadata (context, FALSE, keyfile);
  text = g_key_file_to_data (keyfile, NULL, NULL);
  g_test_message ("Saved:\n%s", text);
  g_clear_pointer (&text, g_free);

  /* Test that keys round-trip back into the file */
  strv = g_key_file_get_string_list (keyfile, FLATPAK_METADATA_GROUP_CONTEXT,
                                     FLATPAK_METADATA_KEY_FILESYSTEMS,
                                     &n, &error);
  g_assert_nonnull (strv);
  /* The order is undefined, so sort them first */
  g_qsort_with_data (strv, n, sizeof (char *),
                     (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);
  i = 0;
  g_assert_cmpstr (strv[i++], ==, "!/opt");
  g_assert_cmpstr (strv[i++], ==, "/home");
  g_assert_cmpstr (strv[i++], ==, "host");
  g_assert_cmpstr (strv[i], ==, NULL);
  g_assert_cmpuint (i, ==, n);
  g_clear_pointer (&strv, g_strfreev);

  strv = g_key_file_get_string_list (keyfile, FLATPAK_METADATA_GROUP_CONTEXT,
                                     FLATPAK_METADATA_KEY_SHARED,
                                     &n, &error);
  g_assert_no_error (error);
  g_assert_nonnull (strv);
  g_qsort_with_data (strv, n, sizeof (char *),
                     (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);
  i = 0;
  g_assert_cmpstr (strv[i++], ==, "ipc");
  g_assert_cmpstr (strv[i++], ==, "network");
  g_assert_cmpstr (strv[i], ==, NULL);
  g_assert_cmpuint (i, ==, n);
  g_clear_pointer (&strv, g_strfreev);

  strv = g_key_file_get_string_list (keyfile, FLATPAK_METADATA_GROUP_CONTEXT,
                                     FLATPAK_METADATA_KEY_SOCKETS,
                                     &n, &error);
  g_assert_no_error (error);
  g_assert_nonnull (strv);
  g_qsort_with_data (strv, n, sizeof (char *),
                     (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);
  i = 0;
  g_assert_cmpstr (strv[i++], ==, "cups");
  g_assert_cmpstr (strv[i++], ==, "fallback-x11");
  g_assert_cmpstr (strv[i++], ==, "pcsc");
  g_assert_cmpstr (strv[i++], ==, "pulseaudio");
  g_assert_cmpstr (strv[i++], ==, "session-bus");
  g_assert_cmpstr (strv[i++], ==, "ssh-auth");
  g_assert_cmpstr (strv[i++], ==, "system-bus");
  g_assert_cmpstr (strv[i++], ==, "wayland");
  g_assert_cmpstr (strv[i++], ==, "x11");
  g_assert_cmpstr (strv[i], ==, NULL);
  g_assert_cmpuint (i, ==, n);
  g_clear_pointer (&strv, g_strfreev);

  strv = g_key_file_get_string_list (keyfile, FLATPAK_METADATA_GROUP_CONTEXT,
                                     FLATPAK_METADATA_KEY_DEVICES,
                                     &n, &error);
  g_assert_no_error (error);
  g_assert_nonnull (strv);
  g_qsort_with_data (strv, n, sizeof (char *),
                     (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);
  i = 0;
  g_assert_cmpstr (strv[i++], ==, "all");
  g_assert_cmpstr (strv[i++], ==, "dri");
  g_assert_cmpstr (strv[i++], ==, "kvm");
  g_assert_cmpstr (strv[i++], ==, "shm");
  g_assert_cmpstr (strv[i], ==, NULL);
  g_assert_cmpuint (i, ==, n);
  g_clear_pointer (&strv, g_strfreev);

  strv = g_key_file_get_string_list (keyfile, FLATPAK_METADATA_GROUP_CONTEXT,
                                     FLATPAK_METADATA_KEY_PERSISTENT,
                                     &n, &error);
  g_assert_no_error (error);
  g_assert_nonnull (strv);
  g_qsort_with_data (strv, n, sizeof (char *),
                     (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);
  i = 0;
  g_assert_cmpstr (strv[i++], ==, ".openarena");
  g_assert_cmpstr (strv[i], ==, NULL);
  g_assert_cmpuint (i, ==, n);
  g_clear_pointer (&strv, g_strfreev);

  strv = g_key_file_get_keys (keyfile, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY,
                              &n, &error);
  g_assert_no_error (error);
  g_assert_nonnull (strv);
  g_qsort_with_data (strv, n, sizeof (char *),
                     (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);
  i = 0;
  g_assert_cmpstr (strv[i++], ==, "org.example.SessionService");
  g_assert_cmpstr (strv[i], ==, NULL);
  g_assert_cmpuint (i, ==, n);
  g_clear_pointer (&strv, g_strfreev);

  text = g_key_file_get_string (keyfile, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY,
                                "org.example.SessionService", &error);
  g_assert_no_error (error);
  g_assert_cmpstr (text, ==, "own");
  g_clear_pointer (&text, g_free);

  strv = g_key_file_get_keys (keyfile, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY,
                              &n, &error);
  g_assert_no_error (error);
  g_assert_nonnull (strv);
  g_qsort_with_data (strv, n, sizeof (char *),
                     (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);
  i = 0;
  g_assert_cmpstr (strv[i++], ==, "net.example.SystemService");
  g_assert_cmpstr (strv[i], ==, NULL);
  g_assert_cmpuint (i, ==, n);
  g_clear_pointer (&strv, g_strfreev);

  text = g_key_file_get_string (keyfile, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY,
                                "net.example.SystemService", &error);
  g_assert_no_error (error);
  g_assert_cmpstr (text, ==, "talk");
  g_clear_pointer (&text, g_free);

  strv = g_key_file_get_keys (keyfile, FLATPAK_METADATA_GROUP_ENVIRONMENT,
                              &n, &error);
  g_assert_no_error (error);
  g_assert_nonnull (strv);
  g_qsort_with_data (strv, n, sizeof (char *),
                     (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);
  i = 0;
  g_assert_cmpstr (strv[i++], ==, "HYPOTHETICAL_PATH");
  g_assert_cmpstr (strv[i], ==, NULL);
  g_assert_cmpuint (i, ==, n);
  g_clear_pointer (&strv, g_strfreev);

  text = g_key_file_get_string (keyfile, FLATPAK_METADATA_GROUP_ENVIRONMENT,
                                "HYPOTHETICAL_PATH", &error);
  g_assert_no_error (error);
  g_assert_cmpstr (text, ==, "/foo:/bar");
  g_clear_pointer (&text, g_free);

  strv = g_key_file_get_keys (keyfile, FLATPAK_METADATA_GROUP_PREFIX_POLICY "MyPolicy",
                              &n, &error);
  g_assert_no_error (error);
  g_assert_nonnull (strv);
  g_qsort_with_data (strv, n, sizeof (char *),
                     (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);
  i = 0;
  g_assert_cmpstr (strv[i++], ==, "Colours");
  g_assert_cmpstr (strv[i], ==, NULL);
  g_assert_cmpuint (i, ==, n);
  g_clear_pointer (&strv, g_strfreev);

  strv = g_key_file_get_string_list (keyfile, FLATPAK_METADATA_GROUP_PREFIX_POLICY "MyPolicy",
                                     "Colours", &n, &error);
  g_assert_no_error (error);
  g_assert_nonnull (strv);
  g_qsort_with_data (strv, n, sizeof (char *),
                     (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);
  i = 0;
  g_assert_cmpstr (strv[i++], ==, "blue");
  g_assert_cmpstr (strv[i++], ==, "green");
  g_assert_cmpstr (strv[i], ==, NULL);
  g_assert_cmpuint (i, ==, n);
  g_clear_pointer (&strv, g_strfreev);
}

typedef struct
{
  const char *input;
  GOptionError code;
} NotFilesystem;

static const NotFilesystem not_filesystems[] =
{
  { "", G_OPTION_ERROR_FAILED },
  { "homework", G_OPTION_ERROR_FAILED },
  { "xdg-download/foo/bar/..", G_OPTION_ERROR_BAD_VALUE },
  { "xdg-download/../foo/bar", G_OPTION_ERROR_BAD_VALUE },
  { "xdg-download/foo/../bar", G_OPTION_ERROR_BAD_VALUE },
  { "xdg-run", G_OPTION_ERROR_FAILED },
  { "/", G_OPTION_ERROR_BAD_VALUE },
  { "/////././././././//////", G_OPTION_ERROR_BAD_VALUE },
};

typedef struct
{
  const char *input;
  FlatpakFilesystemMode mode;
  const char *fs;
} Filesystem;

static const Filesystem filesystems[] =
{
  { "home", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "host", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "host-etc", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "host-os", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "host:ro", FLATPAK_FILESYSTEM_MODE_READ_ONLY, "host" },
  { "home:rw", FLATPAK_FILESYSTEM_MODE_READ_WRITE, "home" },
  { "~/Music", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "/srv/obs/debian\\:sid\\:main:create", FLATPAK_FILESYSTEM_MODE_CREATE,
    "/srv/obs/debian:sid:main" },
  { "/srv/c\\:\\\\Program Files\\\\Steam", FLATPAK_FILESYSTEM_MODE_READ_WRITE,
    "/srv/c:\\Program Files\\Steam" },
  { "/srv/escaped\\unnecessarily", FLATPAK_FILESYSTEM_MODE_READ_WRITE,
    "/srv/escapedunnecessarily" },
  { "xdg-desktop", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-desktop/Stuff", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-documents", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-documents/Stuff", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-download", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-download/Stuff", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-music", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-music/Stuff", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-pictures", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-pictures/Stuff", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-public-share", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-public-share/Stuff", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-templates", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-templates/Stuff", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-videos", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-videos/Stuff", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-data", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-data/Stuff", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-cache", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-cache/Stuff", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-config", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-config/Stuff", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "xdg-config/././///.///././.", FLATPAK_FILESYSTEM_MODE_READ_WRITE, "xdg-config" },
  { "xdg-config/////", FLATPAK_FILESYSTEM_MODE_READ_WRITE, "xdg-config" },
  { "xdg-run/dbus", FLATPAK_FILESYSTEM_MODE_READ_WRITE },
  { "~", FLATPAK_FILESYSTEM_MODE_READ_WRITE, "home" },
  { "~/.", FLATPAK_FILESYSTEM_MODE_READ_WRITE, "home" },
  { "~/", FLATPAK_FILESYSTEM_MODE_READ_WRITE, "home" },
  { "~///././//", FLATPAK_FILESYSTEM_MODE_READ_WRITE, "home" },
  { "home/", FLATPAK_FILESYSTEM_MODE_READ_WRITE, "home" },
  { "home/Projects", FLATPAK_FILESYSTEM_MODE_READ_WRITE, "~/Projects" },
};

static void
test_filesystems (void)
{
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (filesystems); i++)
    {
      const Filesystem *fs = &filesystems[i];
      g_autoptr(GError) error = NULL;
      g_autofree char *normalized;
      FlatpakFilesystemMode mode;
      gboolean ret;

      g_test_message ("%s", fs->input);
      ret = flatpak_context_parse_filesystem (fs->input, &normalized, &mode,
                                              &error);
      g_assert_no_error (error);
      g_assert_true (ret);

      if (fs->fs == NULL)
        g_assert_cmpstr (normalized, ==, fs->input);
      else
        g_assert_cmpstr (normalized, ==, fs->fs);

      g_assert_cmpuint (mode, ==, fs->mode);
    }

  for (i = 0; i < G_N_ELEMENTS (not_filesystems); i++)
    {
      const NotFilesystem *not = &not_filesystems[i];
      g_autoptr(GError) error = NULL;
      char *normalized = NULL;
      FlatpakFilesystemMode mode;
      gboolean ret;

      g_test_message ("%s", not->input);
      ret = flatpak_context_parse_filesystem (not->input, &normalized, &mode,
                                              &error);
      g_test_message ("-> %s", error ? error->message : "(no error)");
      g_assert_error (error, G_OPTION_ERROR, not->code);
      g_assert_false (ret);
      g_assert_null (normalized);
    }
}

static void
test_empty (void)
{
  g_autoptr(FlatpakBwrap) bwrap = flatpak_bwrap_new (NULL);
  g_autoptr(FlatpakExports) exports = flatpak_exports_new ();
  gsize i;

  g_assert_false (flatpak_exports_path_is_visible (exports, "/run"));
  g_assert_cmpint (flatpak_exports_path_get_mode (exports, "/tmp"), ==,
                   FLATPAK_FILESYSTEM_MODE_NONE);

  flatpak_bwrap_add_arg (bwrap, "bwrap");
  flatpak_exports_append_bwrap_args (exports, bwrap);
  flatpak_bwrap_finish (bwrap);
  print_bwrap (bwrap);

  i = 0;
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, "bwrap");

  i = assert_next_is_os_release (bwrap, i);

  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, NULL);
  g_assert_cmpuint (i, ==, bwrap->argv->len);
}

static void
test_full (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FlatpakBwrap) bwrap = flatpak_bwrap_new (NULL);
  g_autoptr(FlatpakExports) exports = flatpak_exports_new ();
  g_autofree gchar *subdir = g_build_filename (testdir, "test_full", NULL);
  g_autofree gchar *expose_rw = g_build_filename (subdir, "expose-rw", NULL);
  g_autofree gchar *expose_ro = g_build_filename (subdir, "expose-ro", NULL);
  g_autofree gchar *hide = g_build_filename (subdir, "hide", NULL);
  g_autofree gchar *dont_hide = g_build_filename (subdir, "dont-hide", NULL);
  g_autofree gchar *hide_below_expose = g_build_filename (subdir,
                                                          "expose-ro",
                                                          "hide-me",
                                                          NULL);
  gsize i;

  glnx_shutil_rm_rf_at (-1, subdir, NULL, &error);

  if (error != NULL)
    {
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
      g_clear_error (&error);
    }

  if (g_mkdir_with_parents (expose_rw, S_IRWXU) != 0)
    g_error ("mkdir: %s", g_strerror (errno));

  if (g_mkdir_with_parents (expose_ro, S_IRWXU) != 0)
    g_error ("mkdir: %s", g_strerror (errno));

  if (g_mkdir_with_parents (hide_below_expose, S_IRWXU) != 0)
    g_error ("mkdir: %s", g_strerror (errno));

  if (g_mkdir_with_parents (hide, S_IRWXU) != 0)
    g_error ("mkdir: %s", g_strerror (errno));

  if (g_mkdir_with_parents (dont_hide, S_IRWXU) != 0)
    g_error ("mkdir: %s", g_strerror (errno));

  flatpak_exports_add_host_etc_expose (exports,
                                       FLATPAK_FILESYSTEM_MODE_READ_WRITE);
  flatpak_exports_add_host_os_expose (exports,
                                      FLATPAK_FILESYSTEM_MODE_READ_ONLY);
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_WRITE,
                                   expose_rw);
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   expose_ro);
  flatpak_exports_add_path_tmpfs (exports, hide_below_expose);
  flatpak_exports_add_path_expose_or_hide (exports,
                                           FLATPAK_FILESYSTEM_MODE_NONE,
                                           hide);
  flatpak_exports_add_path_expose_or_hide (exports,
                                           FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                           dont_hide);

  flatpak_bwrap_add_arg (bwrap, "bwrap");
  flatpak_exports_append_bwrap_args (exports, bwrap);
  flatpak_bwrap_finish (bwrap);
  print_bwrap (bwrap);

  i = 0;
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, "bwrap");

  i = assert_next_is_bind (bwrap, i, "--ro-bind", dont_hide);
  i = assert_next_is_bind (bwrap, i, "--ro-bind", expose_ro);

  /* We don't create a FAKE_MODE_TMPFS in the container unless there is
   * a directory on the host to mount it on.
   * Hiding $subdir/expose-ro/hide-me has to use --tmpfs because
   * $subdir/expose-ro *is* exposed. */
  i = assert_next_is_tmpfs (bwrap, i, hide_below_expose);

  i = assert_next_is_bind (bwrap, i, "--bind", expose_rw);

  /* Hiding $subdir/hide just uses --dir, because $subdir is not
   * exposed. */
  i = assert_next_is_dir (bwrap, i, hide);

  while (i < bwrap->argv->len && bwrap->argv->pdata[i] != NULL)
    {
      /* An unknown number of --bind, --ro-bind and --symlink,
       * depending how your /usr and /etc are set up.
       * About the only thing we can say is that they are in threes. */
      g_assert_cmpuint (i++, <, bwrap->argv->len);
      g_assert_cmpuint (i++, <, bwrap->argv->len);
      g_assert_cmpuint (i++, <, bwrap->argv->len);
    }

  g_assert_cmpuint (i, ==, bwrap->argv->len - 1);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, NULL);
  g_assert_cmpuint (i, ==, bwrap->argv->len);

  glnx_shutil_rm_rf_at (-1, subdir, NULL, &error);

  if (error != NULL)
    {
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
      g_clear_error (&error);
    }
}

int
main (int argc, char *argv[])
{
  int res;

  global_setup ();

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/context/empty", test_empty_context);
  g_test_add_func ("/context/filesystems", test_filesystems);
  g_test_add_func ("/context/full", test_full_context);
  g_test_add_func ("/exports/empty", test_empty);
  g_test_add_func ("/exports/full", test_full);

  res = g_test_run ();

  global_teardown ();

  return res;
}
