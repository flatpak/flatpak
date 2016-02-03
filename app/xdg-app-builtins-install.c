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
  { NULL }
};

static GBytes *
read_gpg_data (GCancellable *cancellable,
               GError **error)
{
  g_autoptr(GInputStream) source_stream = NULL;
  g_autoptr(GOutputStream) mem_stream = NULL;
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

  mem_stream = g_memory_output_stream_new_resizable ();
  if (g_output_stream_splice (mem_stream, source_stream, G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, cancellable, error) < 0)
    return NULL;

  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (mem_stream));
}

#define OSTREE_STATIC_DELTA_META_ENTRY_FORMAT "(uayttay)"
#define OSTREE_STATIC_DELTA_FALLBACK_FORMAT "(yaytt)"
#define OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT "(a{sv}tayay" OSTREE_COMMIT_GVARIANT_STRING "aya" OSTREE_STATIC_DELTA_META_ENTRY_FORMAT "a" OSTREE_STATIC_DELTA_FALLBACK_FORMAT ")"

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
  g_autoptr(GFile) gpg_tmp_file = NULL;
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
  g_autoptr(OstreeGpgVerifyResult) gpg_result = NULL;
  g_autoptr(GError) my_error = NULL;
  g_auto(GLnxLockFile) lock = GLNX_LOCK_FILE_INIT;

  if (argc < 2)
    return usage_error (context, "bundle filename must be specified", error);

  filename = argv[1];

  repo = xdg_app_dir_get_repo (dir);

  if (!xdg_app_supports_bundles (repo))
    return xdg_app_fail (error, "Your version of ostree is too old to support single-file bundles");

  if (!xdg_app_dir_lock (dir, &lock,
                         cancellable, error))
    goto out;

  file = g_file_new_for_commandline_arg (filename);

  {
    g_autoptr(GVariant) delta = NULL;
    g_autoptr(GVariant) metadata = NULL;
    g_autoptr(GBytes) bytes = NULL;
    g_autoptr(GVariant) to_csum_v = NULL;
    g_autoptr(GVariant) gpg_value = NULL;

    GMappedFile *mfile = g_mapped_file_new (gs_file_get_path_cached (file), FALSE, error);

    if (mfile == NULL)
      return FALSE;

    bytes = g_mapped_file_get_bytes (mfile);
    g_mapped_file_unref (mfile);

    delta = g_variant_new_from_bytes (G_VARIANT_TYPE (OSTREE_STATIC_DELTA_SUPERBLOCK_FORMAT), bytes, FALSE);
    g_variant_ref_sink (delta);

    to_csum_v = g_variant_get_child_value (delta, 3);
    if (!ostree_validate_structureof_csum_v (to_csum_v, error))
      return FALSE;

    to_checksum = ostree_checksum_from_bytes_v (to_csum_v);

    metadata = g_variant_get_child_value (delta, 0);

    if (!g_variant_lookup (metadata, "ref", "s", &ref))
      return xdg_app_fail (error, "Invalid bundle, no ref in metadata");

    if (!g_variant_lookup (metadata, "origin", "s", &origin))
      origin = NULL;

    gpg_value = g_variant_lookup_value (metadata, "gpg-keys", G_VARIANT_TYPE("ay"));
    if (gpg_value)
      {
        gsize n_elements;
        const char *data = g_variant_get_fixed_array (gpg_value, &n_elements, 1);

        gpg_data = g_bytes_new (data, n_elements);
      }
  }

  parts = xdg_app_decompose_ref (ref, error);
  if (parts == NULL)
    return FALSE;

  deploy_base = xdg_app_dir_get_deploy_dir (dir, ref);
  if (g_file_query_exists (deploy_base, cancellable))
    return xdg_app_fail (error, "%s branch %s already installed", parts[1], parts[3]);

  if (opt_gpg_file != NULL)
    {
      /* Override gpg_data from file */
      gpg_data = read_gpg_data (cancellable, error);
      if (gpg_data == NULL)
        return FALSE;
    }

  /* Add a remote for later updates */
  if (origin != NULL)
    {
      g_auto(GStrv) remotes = ostree_repo_remote_list (repo, NULL);
      int version = 0;

      do
        {
          g_autofree char *name = NULL;
          if (version == 0)
            name = g_strdup_printf ("%s-origin", parts[1]);
          else
            name = g_strdup_printf ("%s-%d-origin", parts[1], version);
          version++;

          if (remotes == NULL ||
              !g_strv_contains ((const char * const *) remotes, name))
            remote = g_steal_pointer (&name);
        }
      while (remote == NULL);
    }

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return FALSE;

  ostree_repo_transaction_set_ref (repo, remote, ref, to_checksum);

  if (!ostree_repo_static_delta_execute_offline (repo,
                                                 file,
                                                 FALSE,
                                                 cancellable,
                                                 error))
    return FALSE;

  if (gpg_data)
    {
      g_autoptr(GFileIOStream) stream;
      GOutputStream *o;

      gpg_tmp_file = g_file_new_tmp (".xdg-app-XXXXXX", &stream, error);
      if (gpg_tmp_file == NULL)
        return FALSE;
      o = g_io_stream_get_output_stream (G_IO_STREAM (stream));
      if (!g_output_stream_write_all (o, g_bytes_get_data (gpg_data, NULL), g_bytes_get_size (gpg_data), NULL, cancellable, error))
        return FALSE;
    }

  gpg_result = ostree_repo_verify_commit_ext (repo,
                                              to_checksum,
                                              NULL, gpg_tmp_file, cancellable, &my_error);

  if (gpg_tmp_file)
    g_file_delete (gpg_tmp_file, cancellable, NULL);

  if (gpg_result == NULL)
    {
      /* NOT_FOUND means no gpg signature, we ignore this *if* there
       * is no gpg key specified in the bundle or by the user */
      if (g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
          gpg_data == NULL)
        g_clear_error (&my_error);
      else
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return FALSE;
        }
    }
  else
    {
      /* If there is no valid gpg signature we fail, unless there is no gpg
         key specified (on the command line or in the file) because then we
         trust the source bundle. */
      if (ostree_gpg_verify_result_count_valid (gpg_result) == 0  &&
          gpg_data != NULL)
        return xdg_app_fail (error, "GPG signatures found, but none are in trusted keyring");
    }

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    return FALSE;

  if (!g_file_make_directory_with_parents (deploy_base, cancellable, error))
    return FALSE;

  /* From here we need to goto out on error, to clean up */
  created_deploy_base = TRUE;

  if (remote)
    {
      g_autoptr(GVariantBuilder) optbuilder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
      g_autofree char *basename = g_file_get_basename (file);

      g_variant_builder_add (optbuilder, "{s@v}",
                             "xa.title",
                             g_variant_new_variant (g_variant_new_string (basename)));

      g_variant_builder_add (optbuilder, "{s@v}",
                             "xa.noenumerate",
                             g_variant_new_variant (g_variant_new_boolean (TRUE)));

      g_variant_builder_add (optbuilder, "{s@v}",
                             "xa.prio",
                             g_variant_new_variant (g_variant_new_string ("0")));

      if (!ostree_repo_remote_add (repo,
                                   remote, origin, g_variant_builder_end (optbuilder), cancellable, error))
        goto out;

      added_remote = TRUE;

      if (gpg_data)
        {
          g_autoptr(GInputStream) gpg_data_as_stream = g_memory_input_stream_new_from_bytes (gpg_data);

          if (!ostree_repo_remote_gpg_import (repo, remote, gpg_data_as_stream,
                                              NULL, NULL, cancellable, error))
            goto out;
        }

      if (!xdg_app_dir_set_origin (dir, ref, remote, cancellable, error))
        goto out;
    }

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
      if (!xdg_app_dir_pull (dir, repository, ref, NULL,
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
