/*
 * Copyright © 2015 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gio/gio.h>
#include "libglnx/libglnx.h"
#include "libgsystem.h"

#include "builder-manifest.h"

static gboolean opt_verbose;
static gboolean opt_version;
static gboolean opt_disable_cache;
static gboolean opt_download_only;
static gboolean opt_build_only;
static gboolean opt_disable_download;
static gboolean opt_require_changes;
static gboolean opt_keep_build_dirs;

static GOptionEntry entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version information and exit", NULL },
  { "disable-cache", 0, 0, G_OPTION_ARG_NONE, &opt_disable_cache, "Disable cache", NULL },
  { "disable-download", 0, 0, G_OPTION_ARG_NONE, &opt_disable_download, "Don't download any new sources", NULL },
  { "download-only", 0, 0, G_OPTION_ARG_NONE, &opt_download_only, "Only download sources, don't build", NULL },
  { "build-only", 0, 0, G_OPTION_ARG_NONE, &opt_build_only, "Stop after build, don't run clean and finish phases", NULL },
  { "require-changes", 0, 0, G_OPTION_ARG_NONE, &opt_require_changes, "Don't create app dir if no changes", NULL },
  { "keep-build-dirs", 0, 0, G_OPTION_ARG_NONE, &opt_keep_build_dirs, "Don't remove build directories after install", NULL },
  { NULL }
};

static void
message_handler (const gchar *log_domain,
                 GLogLevelFlags log_level,
                 const gchar *message,
                 gpointer user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    g_printerr ("XAB: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

int
usage (GOptionContext *context, const char *message)
{
  g_autofree gchar *help = g_option_context_get_help (context, TRUE, NULL);
  g_printerr ("%s\n", message);
  g_printerr ("%s", help);
  return 1;
}

int
main (int    argc,
      char **argv)
{
  g_autofree const char *old_env = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(BuilderManifest) manifest = NULL;
  GOptionContext *context;
  const char *app_dir_path, *manifest_path;
  g_autofree gchar *json = NULL;
  g_autoptr(BuilderContext) build_context = NULL;
  g_autoptr(GFile) base_dir = NULL;
  g_autoptr(GFile) app_dir = NULL;
  g_autoptr(BuilderCache) cache = NULL;
  g_autofree char *cache_branch = NULL;

  setlocale (LC_ALL, "");

  g_log_set_handler (NULL, G_LOG_LEVEL_MESSAGE, message_handler, NULL);

  g_set_prgname (argv[0]);

  /* avoid gvfs (http://bugzilla.gnome.org/show_bug.cgi?id=526454) */
  old_env = g_strdup (g_getenv ("GIO_USE_VFS"));
  g_setenv ("GIO_USE_VFS", "local", TRUE);
  g_vfs_get_default ();
  if (old_env)
    g_setenv ("GIO_USE_VFS", old_env, TRUE);
  else
    g_unsetenv ("GIO_USE_VFS");

  context = g_option_context_new ("DIRECTORY MANIFEST - Build manifest");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("option parsing failed: %s\n", error->message);
      return 1;
    }

  if (opt_version)
    {
      g_print ("%s\n", PACKAGE_STRING);
      exit (EXIT_SUCCESS);
    }

  if (opt_verbose)
    g_log_set_handler (NULL, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  if (argc == 1)
    return usage (context, "DIRECTORY must be specified");

  app_dir_path = argv[1];

  if (argc == 2)
    return usage (context, "MANIFEST must be specified");

  manifest_path = argv[2];

  if (!g_file_get_contents (manifest_path, &json, NULL, &error))
    {
      g_printerr ("Can't load '%s': %s\n", manifest_path, error->message);
      return 1;
    }

  manifest = (BuilderManifest *)json_gobject_from_data (BUILDER_TYPE_MANIFEST,
                                                        json, -1, &error);
  if (manifest == NULL)
    {
      g_printerr ("Can't parse '%s': %s\n", manifest_path, error->message);
      return 1;
    }

  base_dir = g_file_new_for_path (g_get_current_dir ());
  app_dir = g_file_new_for_path (app_dir_path);

  if (!gs_shutil_rm_rf (app_dir, NULL, &error))
    {
      g_print ("error removing old app dir '%s': %s\n", app_dir_path, error->message);
      return 1;
    }

  build_context = builder_context_new (base_dir, app_dir);

  if (!opt_disable_download)
    {
      if (!builder_manifest_download (manifest, build_context, &error))
        {
          g_print ("error: %s\n", error->message);
          return 1;
        }
    }

  if (opt_download_only)
    return 0;

  cache_branch = g_path_get_basename (manifest_path);

  cache = builder_cache_new (builder_context_get_cache_dir (build_context), app_dir, cache_branch);
  if (!builder_cache_open (cache, &error))
    {
      g_print ("Error opening cache: %s\n", error->message);
      return 1;
    }

  if (opt_disable_cache) /* This disables *lookups*, but we still build the cache */
    builder_cache_disable_lookups (cache);

  builder_manifest_checksum (manifest, cache, build_context);

  if (!builder_cache_lookup (cache))
    {
      g_autofree char *body =
        g_strdup_printf ("Initialized %s\n",
                         builder_manifest_get_app_id (manifest));
      if (!builder_manifest_init_app_dir (manifest, build_context, &error))
        {
          g_print ("error: %s\n", error->message);
          return 1;
        }

      if (!builder_cache_commit (cache, body, &error))
        {
          g_print ("error: %s\n", error->message);
          return 1;
        }
    }

  if (!builder_manifest_build (manifest, opt_keep_build_dirs, cache, build_context, &error))
    {
      g_print ("error: %s\n", error->message);
      return 1;
    }

  if (!opt_build_only)
    {
      if (!builder_manifest_cleanup (manifest, cache, build_context, &error))
        {
          g_print ("error: %s\n", error->message);
          return 1;
        }

      if (!builder_manifest_finish (manifest, cache, build_context, &error))
        {
          g_print ("error: %s\n", error->message);
          return 1;
        }
    }

  if (!opt_require_changes)
    builder_cache_ensure_checkout (cache);

  if (!builder_gc (cache, &error))
    {
      g_warning ("Failed to GC build cache: %s\n", error->message);
      g_clear_error (&error);
    }

  return 0;
}
