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
#include "flatpak-utils-base-private.h"

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
                     const char *path,
                     const char *dest)
{
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, how);
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, path);
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, dest);
  return i;
}

/* Assert that arguments starting from @i are --symlink @rel_target @path,
 * where @rel_target goes up from @path to the root and back down to the
 * target of the symlink. Return the new @i. */
G_GNUC_WARN_UNUSED_RESULT static gsize
assert_next_is_symlink (FlatpakBwrap *bwrap,
                      gsize i,
                      const char *target,
                      const char *path)
{
  const char *got_target;
  g_autofree gchar *dir = NULL;
  g_autofree gchar *resolved = NULL;
  g_autofree gchar *canon = NULL;
  g_autofree gchar *resolved_target = NULL;
  g_autofree gchar *expected = NULL;

  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, "--symlink");
  g_assert_cmpuint (i, <, bwrap->argv->len);

  got_target = bwrap->argv->pdata[i++];
  g_assert_false (g_path_is_absolute (got_target));
  dir = g_path_get_dirname (path);

  resolved = g_build_filename (dir, got_target, NULL);
  canon = flatpak_canonicalize_filename (resolved);

  if (g_path_is_absolute (target))
    resolved_target = g_strdup (target);
  else
    resolved_target = g_build_filename (dir, target, NULL);

  expected = flatpak_canonicalize_filename (resolved_target);

  g_assert_cmpstr (canon, ==, expected);
  g_assert_true (g_str_has_suffix (got_target, target));

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
  g_autofree gchar *in_expose_rw = g_build_filename (subdir, "expose-rw",
                                                     "file", NULL);
  g_autofree gchar *dangling_link_in_expose_rw = g_build_filename (subdir,
                                                                   "expose-rw",
                                                                   "dangling",
                                                                   NULL);
  g_autofree gchar *expose_ro = g_build_filename (subdir, "expose-ro", NULL);
  g_autofree gchar *in_expose_ro = g_build_filename (subdir, "expose-ro",
                                                     "file", NULL);
  g_autofree gchar *hide = g_build_filename (subdir, "hide", NULL);
  g_autofree gchar *dont_hide = g_build_filename (subdir, "dont-hide", NULL);
  g_autofree gchar *hide_below_expose = g_build_filename (subdir,
                                                          "expose-ro",
                                                          "hide-me",
                                                          NULL);
  g_autofree gchar *enoent = g_build_filename (subdir, "ENOENT", NULL);
  g_autofree gchar *one = g_build_filename (subdir, "1", NULL);
  g_autofree gchar *rel_link = g_build_filename (subdir, "1", "rel-link", NULL);
  g_autofree gchar *abs_link = g_build_filename (subdir, "1", "abs-link", NULL);
  g_autofree gchar *in_abs_link = g_build_filename (subdir, "1", "abs-link",
                                                    "file", NULL);
  g_autofree gchar *dangling = g_build_filename (subdir, "1", "dangling", NULL);
  g_autofree gchar *in_dangling = g_build_filename (subdir, "1", "dangling",
                                                    "file", NULL);
  g_autofree gchar *abs_target = g_build_filename (subdir, "2", "abs-target", NULL);
  g_autofree gchar *target = g_build_filename (subdir, "2", "target", NULL);
  g_autofree gchar *create_dir = g_build_filename (subdir, "create-dir", NULL);
  g_autofree gchar *create_dir2 = g_build_filename (subdir, "create-dir2", NULL);
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

  if (g_mkdir_with_parents (dont_hide, S_IRWXU) != 0)
    g_error ("mkdir: %s", g_strerror (errno));

  if (g_mkdir_with_parents (abs_target, S_IRWXU) != 0)
    g_error ("mkdir: %s", g_strerror (errno));

  if (g_mkdir_with_parents (target, S_IRWXU) != 0)
    g_error ("mkdir: %s", g_strerror (errno));

  if (g_mkdir_with_parents (one, S_IRWXU) != 0)
    g_error ("mkdir: %s", g_strerror (errno));

  if (g_mkdir_with_parents (create_dir, S_IRWXU) != 0)
    g_error ("mkdir: %s", g_strerror (errno));

  if (symlink (abs_target, abs_link) != 0)
    g_error ("symlink: %s", g_strerror (errno));

  if (symlink ("nope", dangling) != 0)
    g_error ("symlink: %s", g_strerror (errno));

  if (symlink ("nope", dangling_link_in_expose_rw) != 0)
    g_error ("symlink: %s", g_strerror (errno));

  if (symlink ("../2/target", rel_link) != 0)
    g_error ("symlink: %s", g_strerror (errno));

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
  flatpak_exports_add_path_expose_or_hide (exports,
                                           FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                           enoent);
  flatpak_exports_add_path_expose_or_hide (exports,
                                           FLATPAK_FILESYSTEM_MODE_READ_WRITE,
                                           rel_link);
  flatpak_exports_add_path_expose_or_hide (exports,
                                           FLATPAK_FILESYSTEM_MODE_READ_WRITE,
                                           abs_link);
  flatpak_exports_add_path_dir (exports, create_dir);
  flatpak_exports_add_path_dir (exports, create_dir2);

  g_assert_cmpuint (flatpak_exports_path_get_mode (exports, expose_rw), ==,
                    FLATPAK_FILESYSTEM_MODE_READ_WRITE);
  g_assert_cmpuint (flatpak_exports_path_get_mode (exports, expose_ro), ==,
                    FLATPAK_FILESYSTEM_MODE_READ_ONLY);
  g_assert_cmpuint (flatpak_exports_path_get_mode (exports, hide_below_expose), ==,
                    FLATPAK_FILESYSTEM_MODE_NONE);
  g_assert_cmpuint (flatpak_exports_path_get_mode (exports, hide), ==,
                    FLATPAK_FILESYSTEM_MODE_NONE);
  g_assert_cmpuint (flatpak_exports_path_get_mode (exports, dont_hide), ==,
                    FLATPAK_FILESYSTEM_MODE_READ_ONLY);
  /* It knows enoent didn't really exist */
  g_assert_cmpuint (flatpak_exports_path_get_mode (exports, enoent), ==,
                    FLATPAK_FILESYSTEM_MODE_NONE);
  g_assert_cmpuint (flatpak_exports_path_get_mode (exports, abs_link), ==,
                    FLATPAK_FILESYSTEM_MODE_READ_WRITE);
  g_assert_cmpuint (flatpak_exports_path_get_mode (exports, rel_link), ==,
                    FLATPAK_FILESYSTEM_MODE_READ_WRITE);

  /* Files the app would be allowed to create count as exposed */
  g_assert_cmpuint (flatpak_exports_path_get_mode (exports, in_expose_ro), ==,
                    FLATPAK_FILESYSTEM_MODE_NONE);
  g_assert_cmpuint (flatpak_exports_path_get_mode (exports, in_expose_rw), ==,
                    FLATPAK_FILESYSTEM_MODE_READ_WRITE);
  g_assert_cmpuint (flatpak_exports_path_get_mode (exports, in_abs_link), ==,
                    FLATPAK_FILESYSTEM_MODE_READ_WRITE);
  g_assert_cmpuint (flatpak_exports_path_get_mode (exports, in_dangling), ==,
                    FLATPAK_FILESYSTEM_MODE_NONE);

  flatpak_bwrap_add_arg (bwrap, "bwrap");
  flatpak_exports_append_bwrap_args (exports, bwrap);
  flatpak_bwrap_finish (bwrap);
  print_bwrap (bwrap);

  i = 0;
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, "bwrap");

  i = assert_next_is_symlink (bwrap, i, abs_target, abs_link);
  i = assert_next_is_symlink (bwrap, i, "../2/target", rel_link);
  i = assert_next_is_bind (bwrap, i, "--bind", abs_target, abs_target);
  i = assert_next_is_bind (bwrap, i, "--bind", target, target);
  i = assert_next_is_dir (bwrap, i, create_dir);

  /* create_dir2 is not currently created with --dir inside the container
   * because it doesn't exist outside the container.
   * (Is this correct? For now, tolerate either way) */
  if (i + 2 < bwrap->argv->len &&
      g_strcmp0 (bwrap->argv->pdata[i], "--dir") == 0 &&
      g_strcmp0 (bwrap->argv->pdata[i + 1], create_dir2) == 0)
    i += 2;

  i = assert_next_is_bind (bwrap, i, "--ro-bind", dont_hide, dont_hide);
  i = assert_next_is_bind (bwrap, i, "--ro-bind", expose_ro, expose_ro);

  /* We don't create a FAKE_MODE_TMPFS in the container unless there is
   * a directory on the host to mount it on.
   * Hiding $subdir/expose-ro/hide-me has to use --tmpfs because
   * $subdir/expose-ro *is* exposed. */
  i = assert_next_is_tmpfs (bwrap, i, hide_below_expose);

  i = assert_next_is_bind (bwrap, i, "--bind", expose_rw, expose_rw);

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

typedef enum
{
  FAKE_DIR,
  FAKE_FILE,
  FAKE_SYMLINK,
} FakeFileType;

typedef struct
{
  const char *name;
  FakeFileType type;
  const char *target;
} FakeFile;

static void
create_fake_files (const FakeFile *files)
{
  gsize i;

  for (i = 0; files[i].name != NULL; i++)
    {
      g_autoptr(GError) error = NULL;
      g_autofree gchar *path = g_build_filename (testdir, "host",
                                                 files[i].name, NULL);

      g_assert (files[i].name[0] != '/');

      switch (files[i].type)
        {
          case FAKE_DIR:
            if (g_mkdir_with_parents (path, S_IRWXU) != 0)
              g_error ("mkdir: %s", g_strerror (errno));

            break;

          case FAKE_FILE:
            g_file_set_contents (path, "", 0, &error);
            g_assert_no_error (error);
            break;


          case FAKE_SYMLINK:
            g_assert (files[i].target != NULL);

            if (symlink (files[i].target, path) != 0)
              g_error ("symlink: %s", g_strerror (errno));

            break;


          default:
            g_return_if_reached ();
        }
    }
}

static void
test_host_exports (const FakeFile *files,
                   FlatpakBwrap *bwrap,
                   FlatpakFilesystemMode etc_mode,
                   FlatpakFilesystemMode os_mode)
{
  g_autoptr(FlatpakExports) exports = flatpak_exports_new ();
  g_autoptr(GError) error = NULL;
  g_autofree gchar *host = g_build_filename (testdir, "host", NULL);
  glnx_autofd int fd = -1;

  glnx_shutil_rm_rf_at (-1, host, NULL, &error);

  if (error != NULL)
    {
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
      g_clear_error (&error);
    }

  create_fake_files (files);
  glnx_openat_rdonly (AT_FDCWD, host, TRUE, &fd, &error);
  g_assert_no_error (error);
  g_assert_cmpint (fd, >=, 0);
  flatpak_exports_take_host_fd (exports, glnx_steal_fd (&fd));

  if (etc_mode > FLATPAK_FILESYSTEM_MODE_NONE)
    flatpak_exports_add_host_etc_expose (exports, etc_mode);

  if (os_mode > FLATPAK_FILESYSTEM_MODE_NONE)
    flatpak_exports_add_host_os_expose (exports, os_mode);

  flatpak_bwrap_add_arg (bwrap, "bwrap");
  flatpak_exports_append_bwrap_args (exports, bwrap);
  flatpak_bwrap_finish (bwrap);
  print_bwrap (bwrap);

  glnx_shutil_rm_rf_at (-1, host, NULL, &error);

  if (error != NULL)
    {
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
      g_clear_error (&error);
    }
}

/*
 * Test --filesystem=host-os with an OS that looks like Arch Linux.
 */
static void
test_exports_arch (void)
{
  static const FakeFile files[] =
  {
    { "etc", FAKE_DIR },
    { "etc/ld.so.cache", FAKE_FILE },
    { "etc/ld.so.conf", FAKE_FILE },
    { "etc/ld.so.conf.d", FAKE_DIR },
    { "bin", FAKE_SYMLINK, "usr/bin" },
    { "lib", FAKE_SYMLINK, "usr/lib" },
    { "lib64", FAKE_SYMLINK, "usr/lib" },
    { "sbin", FAKE_SYMLINK, "usr/bin" },
    { "usr/bin", FAKE_DIR },
    { "usr/lib", FAKE_DIR },
    { "usr/lib32", FAKE_DIR },
    { "usr/lib64", FAKE_SYMLINK, "lib" },
    { "usr/sbin", FAKE_SYMLINK, "bin" },
    { "usr/share", FAKE_DIR },
    { NULL }
  };
  g_autoptr(FlatpakBwrap) bwrap = flatpak_bwrap_new (NULL);
  gsize i;

  test_host_exports (files, bwrap, FLATPAK_FILESYSTEM_MODE_NONE,
                     FLATPAK_FILESYSTEM_MODE_READ_ONLY);

  i = 0;
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, "bwrap");

  i = assert_next_is_bind (bwrap, i, "--ro-bind", "/usr", "/run/host/usr");
  i = assert_next_is_symlink (bwrap, i, "usr/bin", "/run/host/bin");
  i = assert_next_is_symlink (bwrap, i, "usr/lib", "/run/host/lib");
  i = assert_next_is_symlink (bwrap, i, "usr/lib", "/run/host/lib64");
  i = assert_next_is_symlink (bwrap, i, "usr/bin", "/run/host/sbin");
  i = assert_next_is_bind (bwrap, i, "--ro-bind", "/etc/ld.so.cache",
                           "/run/host/etc/ld.so.cache");

  g_assert_cmpuint (i, ==, bwrap->argv->len - 1);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, NULL);
  g_assert_cmpuint (i, ==, bwrap->argv->len);
}

/*
 * Test --filesystem=host-os with an OS that looks like Debian,
 * without the /usr merge, and with x86 and x32 multilib.
 */
static void
test_exports_debian (void)
{
  static const FakeFile files[] =
  {
    { "etc", FAKE_DIR },
    { "etc/alternatives", FAKE_DIR },
    { "etc/ld.so.cache", FAKE_FILE },
    { "etc/ld.so.conf", FAKE_FILE },
    { "etc/ld.so.conf.d", FAKE_DIR },
    { "etc/os-release", FAKE_FILE },
    { "bin", FAKE_DIR },
    { "lib", FAKE_DIR },
    { "lib32", FAKE_DIR },
    { "lib64", FAKE_DIR },
    { "libx32", FAKE_DIR },
    { "sbin", FAKE_DIR },
    { "usr/bin", FAKE_DIR },
    { "usr/lib", FAKE_DIR },
    { "usr/lib/os-release", FAKE_FILE },
    { "usr/lib32", FAKE_DIR },
    { "usr/lib64", FAKE_DIR },
    { "usr/libexec", FAKE_DIR },
    { "usr/libx32", FAKE_DIR },
    { "usr/sbin", FAKE_DIR },
    { "usr/share", FAKE_DIR },
    { NULL }
  };
  g_autoptr(FlatpakBwrap) bwrap = flatpak_bwrap_new (NULL);
  gsize i;

  test_host_exports (files, bwrap, FLATPAK_FILESYSTEM_MODE_NONE,
                     FLATPAK_FILESYSTEM_MODE_READ_ONLY);

  i = 0;
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, "bwrap");

  i = assert_next_is_bind (bwrap, i, "--ro-bind", "/usr", "/run/host/usr");
  i = assert_next_is_bind (bwrap, i, "--ro-bind", "/bin", "/run/host/bin");
  i = assert_next_is_bind (bwrap, i, "--ro-bind", "/lib", "/run/host/lib");
  i = assert_next_is_bind (bwrap, i, "--ro-bind", "/lib32", "/run/host/lib32");
  i = assert_next_is_bind (bwrap, i, "--ro-bind", "/lib64", "/run/host/lib64");
  /* libx32 is not currently implemented */
  i = assert_next_is_bind (bwrap, i, "--ro-bind", "/sbin", "/run/host/sbin");
  i = assert_next_is_bind (bwrap, i, "--ro-bind", "/etc/ld.so.cache",
                           "/run/host/etc/ld.so.cache");
  i = assert_next_is_bind (bwrap, i, "--ro-bind", "/etc/alternatives",
                           "/run/host/etc/alternatives");
  i = assert_next_is_bind (bwrap, i, "--ro-bind", "/etc/os-release",
                           "/run/host/os-release");

  g_assert_cmpuint (i, ==, bwrap->argv->len - 1);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, NULL);
  g_assert_cmpuint (i, ==, bwrap->argv->len);
}

/*
 * Test --filesystem=host-os and --filesystem=host-etc with an OS that
 * looks like Debian, with the /usr merge.
 */
static void
test_exports_debian_merged (void)
{
  static const FakeFile files[] =
  {
    { "etc", FAKE_DIR },
    { "etc/alternatives", FAKE_DIR },
    { "etc/ld.so.cache", FAKE_FILE },
    { "etc/ld.so.conf", FAKE_FILE },
    { "etc/ld.so.conf.d", FAKE_DIR },
    { "bin", FAKE_SYMLINK, "usr/bin" },
    { "lib", FAKE_SYMLINK, "usr/lib" },
    /* This one uses an absolute symlink just to check that we handle
     * that correctly */
    { "sbin", FAKE_SYMLINK, "/usr/sbin" },
    { "usr/bin", FAKE_DIR },
    { "usr/lib", FAKE_DIR },
    { "usr/lib/os-release", FAKE_FILE },
    { "usr/libexec", FAKE_DIR },
    { "usr/sbin", FAKE_DIR },
    { "usr/share", FAKE_DIR },
    { NULL }
  };
  g_autoptr(FlatpakBwrap) bwrap = flatpak_bwrap_new (NULL);
  gsize i;

  test_host_exports (files, bwrap, FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                     FLATPAK_FILESYSTEM_MODE_READ_ONLY);

  i = 0;
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, "bwrap");

  i = assert_next_is_bind (bwrap, i, "--ro-bind", "/usr", "/run/host/usr");
  i = assert_next_is_symlink (bwrap, i, "usr/bin", "/run/host/bin");
  i = assert_next_is_symlink (bwrap, i, "usr/lib", "/run/host/lib");
  i = assert_next_is_symlink (bwrap, i, "usr/sbin", "/run/host/sbin");
  i = assert_next_is_bind (bwrap, i, "--ro-bind", "/etc", "/run/host/etc");
  i = assert_next_is_bind (bwrap, i, "--ro-bind", "/usr/lib/os-release",
                           "/run/host/os-release");

  g_assert_cmpuint (i, ==, bwrap->argv->len - 1);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, NULL);
  g_assert_cmpuint (i, ==, bwrap->argv->len);
}

static void
test_exports_ignored (void)
{
  g_autoptr(FlatpakBwrap) bwrap = flatpak_bwrap_new (NULL);
  g_autoptr(FlatpakExports) exports = flatpak_exports_new ();
  gsize i;

  /* These paths are chosen so that they probably exist, with the
   * exception of /app */
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/app");
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/etc");
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/etc/passwd");
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/usr");
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/usr/bin/env");
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/dev");
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/dev/full");
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/proc");
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/proc/1");

  /* These probably exist, and are merged into /usr on systems with
   * the /usr merge */
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/bin");
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/bin/sh");
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/lib");
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/lib/ld-linux.so.2");
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/lib64");
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/lib64/ld-linux-x86-64.so.2");
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/sbin");
  flatpak_exports_add_path_expose (exports,
                                   FLATPAK_FILESYSTEM_MODE_READ_ONLY,
                                   "/sbin/ldconfig");

  flatpak_bwrap_add_arg (bwrap, "bwrap");
  flatpak_exports_append_bwrap_args (exports, bwrap);
  flatpak_bwrap_finish (bwrap);
  print_bwrap (bwrap);

  i = 0;
  g_assert_cmpuint (i, <, bwrap->argv->len);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, "bwrap");

  i = assert_next_is_os_release (bwrap, i);

  g_assert_cmpuint (i, ==, bwrap->argv->len - 1);
  g_assert_cmpstr (bwrap->argv->pdata[i++], ==, NULL);
  g_assert_cmpuint (i, ==, bwrap->argv->len);
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
  g_test_add_func ("/exports/host/arch", test_exports_arch);
  g_test_add_func ("/exports/host/debian", test_exports_debian);
  g_test_add_func ("/exports/host/debian-usrmerge", test_exports_debian_merged);
  g_test_add_func ("/exports/ignored", test_exports_ignored);

  res = g_test_run ();

  global_teardown ();

  return res;
}
