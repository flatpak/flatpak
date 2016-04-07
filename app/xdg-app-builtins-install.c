/*
 * Copyright © 2014 Red Hat, Inc
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
#include <unistd.h>
#include <string.h>

#include <gio/gunixinputstream.h>

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"
#include "xdg-app-chain-input-stream.h"

static char *opt_arch;
static char **opt_gpg_file;
static char **opt_subpaths;
static gboolean opt_no_pull;
static gboolean opt_no_deploy;
static gboolean opt_runtime;
static gboolean opt_app;
static gboolean opt_bundle;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to install for", "ARCH" },
  { "no-pull", 0, 0, G_OPTION_ARG_NONE, &opt_no_pull, "Don't pull, only install from local cache", },
  { "no-deploy", 0, 0, G_OPTION_ARG_NONE, &opt_no_deploy, "Don't deploy, only download to local cache", },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, "Look for runtime with the specified name", },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, "Look for app with the specified name", },
  { "bundle", 0, 0, G_OPTION_ARG_NONE, &opt_bundle, "Install from local bundle file", },
  { "gpg-file", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_gpg_file, "Check bundle signatures with GPG key from FILE (- for stdin)", "FILE" },
  { "subpath", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_subpaths, "Only install this subpath", "path" },
  { NULL }
};

static GBytes *
read_gpg_data (GCancellable *cancellable,
               GError **error)
{
  g_autoptr(GInputStream) source_stream = NULL;
  guint n_keyrings = 0;
  g_autoptr(GPtrArray) streams = NULL;

  if (opt_gpg_file != NULL)
    n_keyrings = g_strv_length (opt_gpg_file);

  guint ii;

  streams = g_ptr_array_new_with_free_func (g_object_unref);

  for (ii = 0; ii < n_keyrings; ii++)
    {
      GInputStream *input_stream = NULL;

      if (strcmp (opt_gpg_file[ii], "-") == 0)
        {
          input_stream = g_unix_input_stream_new (STDIN_FILENO, FALSE);
        }
      else
        {
          g_autoptr(GFile) file = g_file_new_for_path (opt_gpg_file[ii]);
          input_stream = G_INPUT_STREAM(g_file_read (file, cancellable, error));

          if (input_stream == NULL)
            return NULL;
        }

      /* Takes ownership. */
      g_ptr_array_add (streams, input_stream);
    }

  /* Chain together all the --keyring options as one long stream. */
  source_stream = (GInputStream *) xdg_app_chain_input_stream_new (streams);

  return xdg_app_read_stream (source_stream, FALSE, error);
}

gboolean
install_bundle (XdgAppDir *dir,
                GOptionContext *context,
                int argc, char **argv,
                GCancellable *cancellable,
                GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GFile) deploy_base = NULL;
  g_autoptr(GFile) file = NULL;
  const char *filename;
  g_autofree char *ref = NULL;
  g_autofree char *origin = NULL;
  gboolean created_deploy_base = FALSE;
  gboolean added_remote = FALSE;
  g_autofree char *to_checksum = NULL;
  g_auto(GStrv) parts = NULL;
  g_autoptr(GBytes) gpg_data = NULL;
  g_autofree char *remote = NULL;
  OstreeRepo *repo;
  g_auto(GLnxLockFile) lock = GLNX_LOCK_FILE_INIT;
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *basename = NULL;

  if (argc < 2)
    return usage_error (context, "bundle filename must be specified", error);

  filename = argv[1];

  repo = xdg_app_dir_get_repo (dir);

  file = g_file_new_for_commandline_arg (filename);

  metadata = xdg_app_bundle_load (file, &to_checksum,
                                  &ref,
                                  &origin,
                                  NULL,
                                  &gpg_data,
                                  error);
  if (metadata == NULL)
    return FALSE;

  if (opt_gpg_file != NULL)
    {
      /* Override gpg_data from file */
      gpg_data = read_gpg_data (cancellable, error);
      if (gpg_data == NULL)
        return FALSE;
    }

  parts = xdg_app_decompose_ref (ref, error);
  if (parts == NULL)
    return FALSE;

  deploy_base = xdg_app_dir_get_deploy_dir (dir, ref);

  if (g_file_query_exists (deploy_base, cancellable))
    return xdg_app_fail (error, "%s branch %s already installed", parts[1], parts[3]);

  /* Add a remote for later updates */
  basename = g_file_get_basename (file);
  remote = xdg_app_dir_create_origin_remote (dir,
                                             origin,
                                             parts[1],
                                             basename,
                                             gpg_data,
                                             cancellable,
                                             error);
  if (remote == NULL)
    return FALSE;

  /* From here we need to goto out on error, to clean up */
  added_remote = TRUE;

  if (!xdg_app_dir_pull_from_bundle (dir,
                                     file,
                                     remote,
                                     ref,
                                     gpg_data != NULL,
                                     cancellable,
                                     error))
    goto out;

  if (!xdg_app_dir_lock (dir, &lock,
                         cancellable, error))
    return FALSE;

  if (!g_file_make_directory_with_parents (deploy_base, cancellable, error))
    goto out;

  created_deploy_base = TRUE;

  if (!xdg_app_dir_set_origin (dir, ref, remote, cancellable, error))
    goto out;

  if (!xdg_app_dir_deploy (dir, ref, to_checksum, cancellable, error))
    goto out;

  if (strcmp (parts[0], "app") == 0)
    {
      if (!xdg_app_dir_make_current_ref (dir, ref, cancellable, error))
        goto out;

      if (!xdg_app_dir_update_exports (dir, parts[1], cancellable, error))
        goto out;
    }

  glnx_release_lock_file (&lock);

  xdg_app_dir_cleanup_removed (dir, cancellable, NULL);

  if (!xdg_app_dir_mark_changed (dir, error))
    goto out;

  ret = TRUE;

 out:
  if (created_deploy_base && !ret)
    gs_shutil_rm_rf (deploy_base, cancellable, NULL);

  if (added_remote && !ret)
    ostree_repo_remote_delete (repo, remote, NULL, NULL);

  return ret;
}

gboolean
xdg_app_builtin_install (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  gboolean ret = FALSE;
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(XdgAppDir) dir = NULL;
  g_autoptr(GFile) deploy_base = NULL;
  const char *repository;
  const char *name;
  const char *branch = NULL;
  g_autofree char *ref = NULL;
  g_autofree char *installed_ref = NULL;
  gboolean is_app;
  gboolean created_deploy_base = FALSE;
  g_auto(GLnxLockFile) lock = GLNX_LOCK_FILE_INIT;
  g_autoptr(GError) my_error = NULL;

  context = g_option_context_new ("REPOSITORY NAME [BRANCH] - Install an application or runtime");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (opt_bundle)
    return install_bundle (dir, context, argc, argv, cancellable, error);

  if (argc < 3)
    return usage_error (context, "REPOSITORY and NAME must be specified", error);

  repository = argv[1];
  name  = argv[2];
  if (argc >= 4)
    branch = argv[3];

  if (!opt_app && !opt_runtime)
    opt_app = opt_runtime = TRUE;

  installed_ref = xdg_app_dir_find_installed_ref (dir,
                                                  name,
                                                  branch,
                                                  opt_arch,
                                                  opt_app, opt_runtime, &is_app,
                                                  &my_error);
  if (installed_ref != NULL)
    {
      return xdg_app_fail (error, "%s %s, branch %s is already installed",
                           is_app ? "App" : "Runtime", name, branch ? branch : "master");
    }

  if (!g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_propagate_error (error, g_steal_pointer (&my_error));
      return FALSE;
    }

  ref = xdg_app_dir_find_remote_ref (dir, repository, name, branch, opt_arch,
                                     opt_app, opt_runtime, &is_app, cancellable, error);
  if (ref == NULL)
    return FALSE;

  deploy_base = xdg_app_dir_get_deploy_dir (dir, ref);
  if (g_file_query_exists (deploy_base, cancellable))
    return xdg_app_fail (error, "Ref %s already deployed", ref);

  if (!opt_no_pull)
    {
      if (!xdg_app_dir_pull (dir, repository, ref, opt_subpaths, NULL,
                             cancellable, error))
        return FALSE;
    }

  /* After we create the deploy base we must goto out on errors */

  if (!opt_no_deploy)
    {
      if (!xdg_app_dir_lock (dir, &lock,
                             cancellable, error))
        goto out;

      if (!g_file_make_directory_with_parents (deploy_base, cancellable, error))
        goto out;
      created_deploy_base = TRUE;

      if (!xdg_app_dir_set_origin (dir, ref, repository, cancellable, error))
        goto out;

      if (!xdg_app_dir_set_subpaths (dir, ref, (const char **)opt_subpaths,
                                     cancellable, error))
        goto out;

      if (!xdg_app_dir_deploy (dir, ref, NULL, cancellable, error))
        goto out;

      if (is_app)
        {
          if (!xdg_app_dir_make_current_ref (dir, ref, cancellable, error))
            goto out;

          if (!xdg_app_dir_update_exports (dir, name, cancellable, error))
            goto out;
        }

      glnx_release_lock_file (&lock);
    }

  xdg_app_dir_cleanup_removed (dir, cancellable, NULL);

  if (!xdg_app_dir_mark_changed (dir, error))
    goto out;

  ret = TRUE;

 out:
  if (created_deploy_base && !ret)
    gs_shutil_rm_rf (deploy_base, cancellable, NULL);

  return ret;
}

gboolean
xdg_app_builtin_install_runtime (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  opt_runtime = TRUE;
  opt_app = FALSE;

  return xdg_app_builtin_install (argc, argv, cancellable, error);
}

gboolean
xdg_app_builtin_install_app (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  opt_runtime = FALSE;
  opt_app = TRUE;

  return xdg_app_builtin_install (argc, argv, cancellable, error);
}

gboolean
xdg_app_builtin_install_bundle (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  opt_bundle = TRUE;

  return xdg_app_builtin_install (argc, argv, cancellable, error);
}
