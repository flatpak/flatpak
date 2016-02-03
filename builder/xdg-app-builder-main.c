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
#include "builder-utils.h"

static gboolean opt_verbose;
static gboolean opt_version;
static gboolean opt_disable_cache;
static gboolean opt_download_only;
static gboolean opt_build_only;
static gboolean opt_disable_download;
static gboolean opt_disable_updates;
static gboolean opt_ccache;
static gboolean opt_require_changes;
static gboolean opt_keep_build_dirs;
static char *opt_repo;
static char *opt_subject;
static char *opt_body;
static char *opt_gpg_homedir;
static char **opt_key_ids;

static GOptionEntry entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version information and exit", NULL },
  { "ccache", 0, 0, G_OPTION_ARG_NONE, &opt_ccache, "Use ccache", NULL },
  { "disable-cache", 0, 0, G_OPTION_ARG_NONE, &opt_disable_cache, "Disable cache", NULL },
  { "disable-download", 0, 0, G_OPTION_ARG_NONE, &opt_disable_download, "Don't download any new sources", NULL },
  { "disable-updates", 0, 0, G_OPTION_ARG_NONE, &opt_disable_updates, "Only download missing sources, never update to latest vcs version", NULL },
  { "download-only", 0, 0, G_OPTION_ARG_NONE, &opt_download_only, "Only download sources, don't build", NULL },
  { "build-only", 0, 0, G_OPTION_ARG_NONE, &opt_build_only, "Stop after build, don't run clean and finish phases", NULL },
  { "require-changes", 0, 0, G_OPTION_ARG_NONE, &opt_require_changes, "Don't create app dir or export if no changes", NULL },
  { "keep-build-dirs", 0, 0, G_OPTION_ARG_NONE, &opt_keep_build_dirs, "Don't remove build directories after install", NULL },
  { "repo", 0, 0, G_OPTION_ARG_STRING, &opt_repo, "Repo to export into", "DIR"},
  { "subject", 's', 0, G_OPTION_ARG_STRING, &opt_subject, "One line subject (passed to build-export)", "SUBJECT" },
  { "body", 'b', 0, G_OPTION_ARG_STRING, &opt_body, "Full description (passed to build-export)", "BODY" },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_key_ids, "GPG Key ID to sign the commit with", "KEY-ID"},
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "HOMEDIR"},
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

static gboolean
do_export (GError   **error,
           gboolean   runtime,
           ...)
{
  va_list ap;
  const char *arg;
  int i;

  g_autoptr(GPtrArray) args = NULL;
  g_autoptr(GSubprocess) subp = NULL;

  args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("xdg-app"));
  g_ptr_array_add (args, g_strdup ("build-export"));

  if (runtime)
    g_ptr_array_add (args, g_strdup ("--runtime"));

  if (opt_subject)
    g_ptr_array_add (args, g_strdup_printf ("--subject=%s", opt_subject));

  if (opt_body)
    g_ptr_array_add (args, g_strdup_printf ("--body=%s", opt_body));

  if (opt_gpg_homedir)
    g_ptr_array_add (args, g_strdup_printf ("--gpg-homedir=%s", opt_gpg_homedir));

  for (i = 0; opt_key_ids != NULL && opt_key_ids[i] != NULL; i++)
    g_ptr_array_add (args, g_strdup_printf ("--gpg-sign=%s", opt_key_ids[i]));

  va_start (ap, runtime);
  while ((arg = va_arg (ap, const gchar *)))
    g_ptr_array_add (args, g_strdup ((gchar *) arg));
  va_end (ap);

  g_ptr_array_add (args, NULL);

  subp =
    g_subprocess_newv ((const gchar * const *) args->pdata,
                       G_SUBPROCESS_FLAGS_NONE,
                       error);

  if (subp == NULL ||
      !g_subprocess_wait_check (subp, NULL, error))
    return FALSE;

  return TRUE;
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
  const char *platform_id = NULL;

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

  if (g_file_query_exists (app_dir, NULL) && !directory_is_empty (app_dir_path))
    {
      g_printerr ("App dir '%s' is not empty. Please delete "
                  "the existing contents.\n", app_dir_path);
      return 1;
    }

  build_context = builder_context_new (base_dir, app_dir);

  builder_context_set_keep_build_dirs (build_context, opt_keep_build_dirs);

  if (opt_ccache &&
      !builder_context_enable_ccache (build_context, &error))
    {
      g_printerr ("Can't initialize ccache use: %s\n", error->message);
      return 1;
    }

  if (!builder_manifest_start (manifest, build_context, &error))
    {
      g_print ("Failed to init: %s\n", error->message);
      return 1;
    }

  if (!opt_disable_download &&
      !builder_manifest_download (manifest, !opt_disable_updates, build_context, &error))
    {
      g_print ("Failed to download sources: %s\n", error->message);
      return 1;
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

  if (!builder_cache_lookup (cache, "init"))
    {
      g_autofree char *body =
        g_strdup_printf ("Initialized %s\n",
                         builder_manifest_get_id (manifest));
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

  if (!builder_manifest_build (manifest, cache, build_context, &error))
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

      if (!builder_manifest_create_platform (manifest, cache, build_context, &error))
        {
          g_print ("error: %s\n", error->message);
          return 1;
        }
    }

  if (!opt_require_changes)
    {
      builder_cache_ensure_checkout (cache);
    }

  if (opt_repo && builder_cache_has_checkout (cache))
    {
      g_autoptr(GFile) debuginfo_metadata = NULL;

      g_print ("exporting %s to repo\n", builder_manifest_get_id (manifest));

      if (!do_export (&error,
                      builder_context_get_build_runtime (build_context),
                      "--exclude=/lib/debug/*",
                      "--include=/lib/debug/app",
                      opt_repo, app_dir_path, NULL))
        {
          g_print ("Export failed: %s\n", error->message);
          return 1;
        }

      platform_id = builder_manifest_get_id_platform (manifest);
      if (builder_context_get_build_runtime (build_context) &&
          platform_id != NULL)
        {
          g_print ("exporting %s to repo\n", platform_id);

            if (!do_export (&error, TRUE,
                            "--metadata=metadata.platform",
                            "--files=platform",
                            opt_repo, app_dir_path, NULL))
              {
                g_print ("Export failed: %s\n", error->message);
                return 1;
              }
        }

      debuginfo_metadata = g_file_get_child (app_dir, "metadata.debuginfo");
      if (g_file_query_exists (debuginfo_metadata, NULL))
        {
          g_print ("exporting %s.Debug to repo\n", builder_manifest_get_id (manifest));

          if (!do_export (&error, TRUE,
                          "--metadata=metadata.debuginfo",
                          builder_context_get_build_runtime (build_context) ? "--files=usr/lib/debug" : "--files=files/lib/debug",
                          opt_repo, app_dir_path, NULL))
            {
              g_print ("Export failed: %s\n", error->message);
              return 1;
            }
        }
    }

  if (!builder_gc (cache, &error))
    {
      g_warning ("Failed to GC build cache: %s\n", error->message);
      g_clear_error (&error);
    }

  return 0;
}
