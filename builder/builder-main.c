/*
 * Copyright © 2015 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
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

#include "builder-manifest.h"
#include "builder-utils.h"
#include "builder-git.h"

static gboolean opt_verbose;
static gboolean opt_version;
static gboolean opt_run;
static gboolean opt_disable_cache;
static gboolean opt_disable_rofiles;
static gboolean opt_download_only;
static gboolean opt_build_only;
static gboolean opt_finish_only;
static gboolean opt_show_deps;
static gboolean opt_disable_download;
static gboolean opt_disable_updates;
static gboolean opt_ccache;
static gboolean opt_require_changes;
static gboolean opt_keep_build_dirs;
static gboolean opt_force_clean;
static gboolean opt_allow_missing_runtimes;
static gboolean opt_sandboxed;
static gboolean opt_rebuild_on_sdk_change;
static gboolean opt_skip_if_unchanged;
static char *opt_from_git;
static char *opt_from_git_branch;
static char *opt_stop_at;
static char *opt_build_shell;
static char *opt_arch;
static char *opt_default_branch;
static char *opt_repo;
static char *opt_subject;
static char *opt_body;
static char *opt_gpg_homedir;
static char **opt_key_ids;
static int opt_jobs;

static GOptionEntry entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version information and exit", NULL },
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Architecture to build for (must be host compatible)", "ARCH" },
  { "default-branch", 0, 0, G_OPTION_ARG_STRING, &opt_default_branch, "Change the default branch", "BRANCH" },
  { "run", 0, 0, G_OPTION_ARG_NONE, &opt_run, "Run a command in the build directory (see --run --help)", NULL },
  { "ccache", 0, 0, G_OPTION_ARG_NONE, &opt_ccache, "Use ccache", NULL },
  { "disable-cache", 0, 0, G_OPTION_ARG_NONE, &opt_disable_cache, "Disable cache lookups", NULL },
  { "disable-rofiles-fuse", 0, 0, G_OPTION_ARG_NONE, &opt_disable_rofiles, "Disable rofiles-fuse use", NULL },
  { "disable-download", 0, 0, G_OPTION_ARG_NONE, &opt_disable_download, "Don't download any new sources", NULL },
  { "disable-updates", 0, 0, G_OPTION_ARG_NONE, &opt_disable_updates, "Only download missing sources, never update to latest vcs version", NULL },
  { "download-only", 0, 0, G_OPTION_ARG_NONE, &opt_download_only, "Only download sources, don't build", NULL },
  { "build-only", 0, 0, G_OPTION_ARG_NONE, &opt_build_only, "Stop after build, don't run clean and finish phases", NULL },
  { "finish-only", 0, 0, G_OPTION_ARG_NONE, &opt_finish_only, "Only run clean and finish and export phases", NULL },
  { "allow-missing-runtimes", 0, 0, G_OPTION_ARG_NONE, &opt_allow_missing_runtimes, "Don't fail if runtime and sdk missing", NULL },
  { "show-deps", 0, 0, G_OPTION_ARG_NONE, &opt_show_deps, "List the dependencies of the json file (see --show-deps --help)", NULL },
  { "require-changes", 0, 0, G_OPTION_ARG_NONE, &opt_require_changes, "Don't create app dir or export if no changes", NULL },
  { "keep-build-dirs", 0, 0, G_OPTION_ARG_NONE, &opt_keep_build_dirs, "Don't remove build directories after install", NULL },
  { "repo", 0, 0, G_OPTION_ARG_STRING, &opt_repo, "Repo to export into", "DIR"},
  { "subject", 's', 0, G_OPTION_ARG_STRING, &opt_subject, "One line subject (passed to build-export)", "SUBJECT" },
  { "body", 'b', 0, G_OPTION_ARG_STRING, &opt_body, "Full description (passed to build-export)", "BODY" },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_key_ids, "GPG Key ID to sign the commit with", "KEY-ID"},
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "HOMEDIR"},
  { "force-clean", 0, 0, G_OPTION_ARG_NONE, &opt_force_clean, "Erase previous contents of DIRECTORY", NULL },
  { "sandbox", 0, 0, G_OPTION_ARG_NONE, &opt_sandboxed, "Enforce sandboxing, disabling build-args", NULL },
  { "stop-at", 0, 0, G_OPTION_ARG_STRING, &opt_stop_at, "Stop building at this module (implies --build-only)", "MODULENAME"},
  { "jobs", 0, 0, G_OPTION_ARG_INT, &opt_jobs, "Number of parallel jobs to build (default=NCPU)", "JOBS"},
  { "rebuild-on-sdk-change", 0, 0, G_OPTION_ARG_NONE, &opt_rebuild_on_sdk_change, "Rebuild if sdk changes", NULL },
  { "skip-if-unchanged", 0, 0, G_OPTION_ARG_NONE, &opt_skip_if_unchanged, "Don't do anything if the json didn't change", NULL },
  { "build-shell", 0, 0, G_OPTION_ARG_STRING, &opt_build_shell, "Extract and prepare sources for module, then start build shell", "MODULENAME"},
  { "from-git", 0, 0, G_OPTION_ARG_STRING, &opt_from_git, "Get input files from git repo", "URL"},
  { "from-git-branch", 0, 0, G_OPTION_ARG_STRING, &opt_from_git_branch, "Branch to use in --from-git", "BRANCH"},
  { NULL }
};

static GOptionEntry run_entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Architecture to build for (must be host compatible)", "ARCH" },
  { "run", 0, 0, G_OPTION_ARG_NONE, &opt_run, "Run a command in the build directory", NULL },
  { "ccache", 0, 0, G_OPTION_ARG_NONE, &opt_ccache, "Use ccache", NULL },
  { NULL }
};

static GOptionEntry show_deps_entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { "show-deps", 0, 0, G_OPTION_ARG_NONE, &opt_show_deps, "List the dependencies of the json file (see --show-deps --help)", NULL },
  { NULL }
};


static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
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

static const char skip_arg[] = "skip";

static gboolean
do_export (BuilderContext *build_context,
           GError        **error,
           gboolean        runtime,
           ...)
{
  va_list ap;
  const char *arg;
  int i;

  g_autoptr(GPtrArray) args = NULL;
  g_autoptr(GSubprocess) subp = NULL;

  args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("flatpak"));
  g_ptr_array_add (args, g_strdup ("build-export"));

  g_ptr_array_add (args, g_strdup_printf ("--arch=%s", builder_context_get_arch (build_context)));

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
    if (arg != skip_arg)
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
  g_autoptr(GOptionContext) context = NULL;
  const char *app_dir_path = NULL, *manifest_rel_path;
  g_autofree gchar *json = NULL;
  g_autofree gchar *json_sha256 = NULL;
  g_autofree gchar *old_json_sha256 = NULL;
  g_autoptr(BuilderContext) build_context = NULL;
  g_autoptr(GFile) base_dir = NULL;
  g_autoptr(GFile) manifest_file = NULL;
  g_autoptr(GFile) app_dir = NULL;
  g_autoptr(BuilderCache) cache = NULL;
  g_autofree char *cache_branch = NULL;
  g_autoptr(GFileEnumerator) dir_enum = NULL;
  g_autoptr(GFileEnumerator) dir_enum2 = NULL;
  g_autofree char *cwd = NULL;
  g_autoptr(GFile) cwd_dir = NULL;
  GFileInfo *next = NULL;
  const char *platform_id = NULL;
  g_autofree char **orig_argv = NULL;
  gboolean is_run = FALSE;
  gboolean is_show_deps = FALSE;
  gboolean app_dir_is_empty = FALSE;
  g_autoptr(FlatpakContext) arg_context = NULL;
  g_autoptr(FlatpakTempDir) cleanup_manifest_dir = NULL;
  g_autofree char *manifest_basename = NULL;
  int i, first_non_arg, orig_argc;
  int argnr;

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

  orig_argv = g_memdup (argv, sizeof (char *) * argc);
  orig_argc = argc;

  first_non_arg = 1;
  for (i = 1; i < argc; i++)
    {
      if (argv[i][0] != '-')
        break;
      first_non_arg = i + 1;
      if (strcmp (argv[i], "--run") == 0)
        is_run = TRUE;
      if (strcmp (argv[i], "--show-deps") == 0)
        is_show_deps = TRUE;
    }

  if (is_run)
    {
      context = g_option_context_new ("DIRECTORY MANIFEST COMMAND [args] - Run command in build sandbox");
      g_option_context_add_main_entries (context, run_entries, NULL);
      arg_context = flatpak_context_new ();
      g_option_context_add_group (context, flatpak_context_get_options (arg_context));

      /* We drop the post-command part from the args, these go with the command in the sandbox */
      argc = MIN (first_non_arg + 3, argc);
    }
  else if (is_show_deps)
    {
      context = g_option_context_new ("MANIFEST - Show manifest dependencies");
      g_option_context_add_main_entries (context, show_deps_entries, NULL);
    }
  else
    {
      context = g_option_context_new ("DIRECTORY MANIFEST - Build manifest");
      g_option_context_add_main_entries (context, entries, NULL);
    }

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("Option parsing failed: %s\n", error->message);
      return 1;
    }

  if (opt_version)
    {
      g_print ("%s\n", PACKAGE_STRING);
      exit (EXIT_SUCCESS);
    }

  if (opt_verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  argnr = 1;

  if (!is_show_deps)
    {
      if (argc == argnr)
        return usage (context, "DIRECTORY must be specified");
      app_dir_path = argv[argnr++];
    }

  if (argc == argnr)
    return usage (context, "MANIFEST must be specified");
  manifest_rel_path = argv[argnr++];
  manifest_basename = g_path_get_basename (manifest_rel_path);

  if (app_dir_path)
    app_dir = g_file_new_for_path (app_dir_path);
  cwd = g_get_current_dir ();
  cwd_dir = g_file_new_for_path (cwd);

  build_context = builder_context_new (cwd_dir, app_dir);

  builder_context_set_use_rofiles (build_context, !opt_disable_rofiles);
  builder_context_set_keep_build_dirs (build_context, opt_keep_build_dirs);
  builder_context_set_sandboxed (build_context, opt_sandboxed);
  builder_context_set_jobs (build_context, opt_jobs);
  builder_context_set_rebuild_on_sdk_change (build_context, opt_rebuild_on_sdk_change);

  if (opt_arch)
    builder_context_set_arch (build_context, opt_arch);

  if (opt_stop_at)
    {
      opt_build_only = TRUE;
      builder_context_set_stop_at (build_context, opt_stop_at);
    }

  if (opt_ccache &&
      !builder_context_enable_ccache (build_context, &error))
    {
      g_printerr ("Can't initialize ccache use: %s\n", error->message);
      return 1;
  }

  if (opt_from_git)
    {
      g_autofree char *manifest_dirname = g_path_get_dirname (manifest_rel_path);
      const char *git_branch = opt_from_git_branch ? opt_from_git_branch : "master";
      g_autofree char *git_origin_branch = g_strconcat ("origin/", git_branch, NULL);
      g_autoptr(GFile) build_subdir = NULL;

      if (!builder_git_mirror_repo (opt_from_git,
                                    !opt_disable_updates, FALSE, FALSE,
                                    git_branch, build_context, &error))
        {
          g_printerr ("Can't clone manifest repo: %s\n", error->message);
          return 1;
        }

      build_subdir = builder_context_allocate_build_subdir (build_context, manifest_basename, &error);
      if (build_subdir == NULL)
        {
          g_printerr ("Can't check out manifest repo: %s\n", error->message);
          return 1;
        }

      cleanup_manifest_dir = g_object_ref (build_subdir);

      if (!builder_git_checkout_dir (opt_from_git,
                                     git_origin_branch,
                                     manifest_dirname,
                                     build_subdir,
                                     build_context,
                                     &error))
        {
          g_printerr ("Can't check out manifest repo: %s\n", error->message);
          return 1;
        }

      manifest_file = g_file_get_child (build_subdir, manifest_rel_path);
      base_dir = g_file_resolve_relative_path (build_subdir, manifest_dirname);
    }
  else
    {
      manifest_file = g_file_new_for_path (manifest_rel_path);
      base_dir = g_file_get_parent (manifest_file);
    }

  builder_context_set_base_dir (build_context, base_dir);

  if (!g_file_get_contents (flatpak_file_get_path_cached (manifest_file), &json, NULL, &error))
    {
      g_printerr ("Can't load '%s': %s\n", manifest_rel_path, error->message);
      return 1;
    }

  json_sha256 = g_compute_checksum_for_string (G_CHECKSUM_SHA256, json, -1);

  if (opt_skip_if_unchanged)
    {
      old_json_sha256 = builder_context_get_checksum_for (build_context, manifest_basename);
      if (old_json_sha256 != NULL && strcmp (json_sha256, old_json_sha256) == 0)
        {
          g_print ("No changes to manifest, skipping\n");
          return 42;
        }
    }

  /* Can't push this as user data to the demarshalling :/ */
  builder_manifest_set_demarshal_buid_context (build_context);

  manifest = (BuilderManifest *) json_gobject_from_data (BUILDER_TYPE_MANIFEST,
                                                         json, -1, &error);

  builder_manifest_set_demarshal_buid_context (NULL);

  if (manifest == NULL)
    {
      g_printerr ("Can't parse '%s': %s\n", manifest_rel_path, error->message);
      return 1;
    }

  if (is_run && argc == 3)
    return usage (context, "Program to run must be specified");

  if (is_show_deps)
    {
      if (!builder_manifest_show_deps (manifest, build_context, &error))
        {
          g_printerr ("Error running %s: %s\n", argv[3], error->message);
          return 1;
        }

      return 0;
    }

  app_dir_is_empty = !g_file_query_exists (app_dir, NULL) ||
                     directory_is_empty (app_dir_path);

  if (is_run)
    {
      g_assert (opt_run);

      if (app_dir_is_empty)
        {
          g_printerr ("App dir '%s' is empty or doesn't exist.\n", app_dir_path);
          return 1;
        }

      if (!builder_manifest_run (manifest, build_context, arg_context,
                                 orig_argv + first_non_arg + 2,
                                 orig_argc - first_non_arg - 2, &error))
        {
          g_printerr ("Error running %s: %s\n", argv[3], error->message);
          return 1;
        }

      return 0;
    }

  g_assert (!opt_run);
  g_assert (!opt_show_deps);

  if (opt_finish_only || opt_build_shell)
    {
      if (app_dir_is_empty)
        {
          g_printerr ("App dir '%s' is empty or doesn't exist.\n", app_dir_path);
          return 1;
        }
    }
  else
    {
      if (!app_dir_is_empty)
        {
          if (opt_force_clean)
            {
              g_print ("Emptying app dir '%s'\n", app_dir_path);
              if (!flatpak_rm_rf (app_dir, NULL, &error))
                {
                  g_printerr ("Couldn't empty app dir '%s': %s",
                              app_dir_path, error->message);
                  return 1;
                }
            }
          else
            {
              g_printerr ("App dir '%s' is not empty. Please delete "
                          "the existing contents or use --force-clean.\n", app_dir_path);
              return 1;
            }
        }
    }

  builder_context_set_checksum_for (build_context, manifest_basename, json_sha256);

  if (!builder_manifest_start (manifest, opt_allow_missing_runtimes, build_context, &error))
    {
      g_printerr ("Failed to init: %s\n", error->message);
      return 1;
    }

  if (!opt_finish_only &&
      !opt_disable_download &&
      !builder_manifest_download (manifest, !opt_disable_updates, opt_build_shell, build_context, &error))
    {
      g_printerr ("Failed to download sources: %s\n", error->message);
      return 1;
    }

  if (opt_download_only)
    return 0;

  if (opt_build_shell)
    {
      if (!builder_manifest_build_shell (manifest, build_context, opt_build_shell, &error))
        {
          g_printerr ("Failed to setup module: %s\n", error->message);
          return 1;
        }

      return 0;
    }

  cache_branch = g_strconcat (builder_context_get_arch (build_context), "-", manifest_basename, NULL);

  cache = builder_cache_new (build_context, app_dir, cache_branch);
  if (!builder_cache_open (cache, &error))
    {
      g_printerr ("Error opening cache: %s\n", error->message);
      return 1;
    }

  if (opt_disable_cache) /* This disables *lookups*, but we still build the cache */
    builder_cache_disable_lookups (cache);

  builder_manifest_checksum (manifest, cache, build_context);

  if (!opt_finish_only)
    {
      if (!builder_cache_lookup (cache, "init"))
        {
          g_autofree char *body =
            g_strdup_printf ("Initialized %s\n",
                             builder_manifest_get_id (manifest));
          if (!builder_manifest_init_app_dir (manifest, cache, build_context, &error))
            {
              g_printerr ("Error: %s\n", error->message);
              return 1;
            }

          if (!builder_cache_commit (cache, body, &error))
            {
              g_printerr ("Error: %s\n", error->message);
              return 1;
            }
        }

      if (!builder_manifest_build (manifest, cache, build_context, &error))
        {
          g_printerr ("Error: %s\n", error->message);
          return 1;
        }
    }

  if (!opt_build_only)
    {
      if (!builder_manifest_cleanup (manifest, cache, build_context, &error))
        {
          g_printerr ("Error: %s\n", error->message);
          return 1;
        }

      if (!builder_manifest_finish (manifest, cache, build_context, &error))
        {
          g_printerr ("Error: %s\n", error->message);
          return 1;
        }

      if (!builder_manifest_create_platform (manifest, cache, build_context, &error))
        {
          g_printerr ("Error: %s\n", error->message);
          return 1;
        }
    }

  if (!opt_require_changes)
    builder_cache_ensure_checkout (cache);

  if (!opt_build_only && opt_repo && builder_cache_has_checkout (cache))
    {
      g_autoptr(GFile) debuginfo_metadata = NULL;

      g_print ("Exporting %s to repo\n", builder_manifest_get_id (manifest));

      if (!do_export (build_context, &error,
                      FALSE,
                      "--exclude=/lib/debug/*",
                      "--include=/lib/debug/app",
                      builder_context_get_separate_locales (build_context) ? "--exclude=/share/runtime/locale/*/*" : skip_arg,
                      opt_repo, app_dir_path, builder_manifest_get_branch (manifest, opt_default_branch), NULL))
        {
          g_printerr ("Export failed: %s\n", error->message);
          return 1;
        }

      /* Export regular locale extensions */
      dir_enum = g_file_enumerate_children (app_dir, "standard::name,standard::type",
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            NULL, NULL);
      while (dir_enum != NULL &&
             (next = g_file_enumerator_next_file (dir_enum, NULL, NULL)))
        {
          g_autoptr(GFileInfo) child_info = next;
          const char *name = g_file_info_get_name (child_info);
          g_autofree char *metadata_arg = NULL;
          g_autofree char *files_arg = NULL;
          g_autofree char *locale_id = builder_manifest_get_locale_id (manifest);

          if (strcmp (name, "metadata.locale") == 0)
            g_print ("Exporting %s to repo\n", locale_id);
          else
            continue;

          metadata_arg = g_strdup_printf ("--metadata=%s", name);
          files_arg = g_strconcat (builder_context_get_build_runtime (build_context) ? "--files=usr" : "--files=files",
                                   "/share/runtime/locale/", NULL);
          if (!do_export (build_context, &error, TRUE,
                          metadata_arg,
                          files_arg,
                          opt_repo, app_dir_path, builder_manifest_get_branch (manifest, opt_default_branch), NULL))
            {
              g_printerr ("Export failed: %s\n", error->message);
              return 1;
            }
        }

      /* Export debug extensions */
      debuginfo_metadata = g_file_get_child (app_dir, "metadata.debuginfo");
      if (g_file_query_exists (debuginfo_metadata, NULL))
        {
          g_autofree char *debug_id = builder_manifest_get_debug_id (manifest);
          g_print ("Exporting %s to repo\n", debug_id);

          if (!do_export (build_context, &error, TRUE,
                          "--metadata=metadata.debuginfo",
                          builder_context_get_build_runtime (build_context) ? "--files=usr/lib/debug" : "--files=files/lib/debug",
                          opt_repo, app_dir_path, builder_manifest_get_branch (manifest, opt_default_branch), NULL))
            {
              g_printerr ("Export failed: %s\n", error->message);
              return 1;
            }
        }

      /* Export platform */
      platform_id = builder_manifest_get_id_platform (manifest);
      if (builder_context_get_build_runtime (build_context) &&
          platform_id != NULL)
        {
          g_print ("Exporting %s to repo\n", platform_id);

          if (!do_export (build_context, &error, TRUE,
                          "--metadata=metadata.platform",
                          "--files=platform",
                          builder_context_get_separate_locales (build_context) ? "--exclude=/share/runtime/locale/*/*" : skip_arg,
                          opt_repo, app_dir_path, builder_manifest_get_branch (manifest, opt_default_branch), NULL))
            {
              g_printerr ("Export failed: %s\n", error->message);
              return 1;
            }
        }

      /* Export platform locales */
      dir_enum2 = g_file_enumerate_children (app_dir, "standard::name,standard::type",
                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                             NULL, NULL);
      while (dir_enum2 != NULL &&
             (next = g_file_enumerator_next_file (dir_enum2, NULL, NULL)))
        {
          g_autoptr(GFileInfo) child_info = next;
          const char *name = g_file_info_get_name (child_info);
          g_autofree char *metadata_arg = NULL;
          g_autofree char *files_arg = NULL;
          g_autofree char *locale_id = builder_manifest_get_locale_id_platform (manifest);

          if (strcmp (name, "metadata.platform.locale") == 0)
            g_print ("Exporting %s to repo\n", locale_id);
          else
            continue;

          metadata_arg = g_strdup_printf ("--metadata=%s", name);
          files_arg = g_strconcat ("--files=platform/share/runtime/locale/", NULL);
          if (!do_export (build_context, &error, TRUE,
                          metadata_arg,
                          files_arg,
                          opt_repo, app_dir_path, builder_manifest_get_branch (manifest, opt_default_branch), NULL))
            {
              g_printerr ("Export failed: %s\n", error->message);
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
