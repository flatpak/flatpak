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
#include <unistd.h>
#include <string.h>

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils.h"

static char *opt_ref;
static gboolean opt_oci = FALSE;
static char **opt_gpg_key_ids;
static char *opt_gpg_homedir;
static gboolean opt_update_appstream;
static gboolean opt_no_update_summary;

static GOptionEntry options[] = {
  { "ref", 0, 0, G_OPTION_ARG_STRING, &opt_ref, N_("Override the ref used for the imported bundle"), N_("REF") },
  { "oci", 0, 0, G_OPTION_ARG_NONE, &opt_oci, N_("Import oci image instead of flatpak bundle"), NULL },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids, N_("GPG Key ID to sign the commit with"), N_("KEY-ID") },
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, N_("GPG Homedir to use when looking for keyrings"), N_("HOMEDIR") },
  { "update-appstream", 0, 0, G_OPTION_ARG_NONE, &opt_update_appstream, N_("Update the appstream branch"), NULL },
  { "no-update-summary", 0, 0, G_OPTION_ARG_NONE, &opt_no_update_summary, N_("Don't update the summary"), NULL },
  { NULL }
};

static gboolean
import_oci (OstreeRepo *repo, GFile *file,
            GCancellable *cancellable, GError **error)
{
#if !defined(HAVE_OSTREE_EXPORT_PATH_PREFIX)
  /* This code actually doesn't user path_prefix, but it need the support
     for reading commits from the transaction that was added at the same time. */
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               _("This version of ostree is to old to support OCI exports"));
  return FALSE;
#elif !defined(HAVE_LIBARCHIVE)
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               _("This version of flatpak is not compiled with libarchive support"));
  return FALSE;
#else
  g_autoptr(OstreeMutableTree) archive_mtree = NULL;
  g_autoptr(OstreeMutableTree) mtree = NULL;
  g_autoptr(OstreeMutableTree) files_mtree = NULL;
  g_autoptr(OstreeMutableTree) export_mtree = NULL;
  g_autoptr(GFile) archive_root = NULL;
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) files = NULL;
  g_autoptr(GFile) export = NULL;
  g_autoptr(GFile) ref = NULL;
  g_autoptr(GFile) commit = NULL;
  g_autoptr(GFile) commitmeta = NULL;
  g_autoptr(GFile) metadata = NULL;
  g_autofree char *commit_checksum = NULL;
  g_autofree char *ref_data = NULL;
  g_autofree char *commit_data = NULL;
  gsize commit_size;
  g_autofree char *commitmeta_data = NULL;
  g_autofree char *parent = NULL;
  const char *subject;
  const char *body;
  const char *target_ref;
  const char *files_source;
  gsize commitmeta_size;
  g_autoptr(GVariant) commitv = NULL;
  g_autoptr(GVariant) commitv_metadata = NULL;
  g_autoptr(GVariant) commitmetav = NULL;

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return FALSE;

  /* There is no way to write a subset of the archive to a mtree, so instead
     we write all of it and then build a new mtree with the subset */
  archive_mtree = ostree_mutable_tree_new ();
  if (!ostree_repo_write_archive_to_mtree (repo, file, archive_mtree, NULL,
                                           TRUE,
                                           cancellable, error))
    return FALSE;

  if (!ostree_repo_write_mtree (repo, archive_mtree, &archive_root, cancellable, error))
    return FALSE;

  if (!ostree_repo_file_ensure_resolved ((OstreeRepoFile *) archive_root, error))
    return FALSE;

  ref = g_file_resolve_relative_path (archive_root, "rootfs/ref");
  metadata = g_file_resolve_relative_path (archive_root, "rootfs/metadata");
  commit = g_file_resolve_relative_path (archive_root, "rootfs/commit");
  commitmeta = g_file_resolve_relative_path (archive_root, "rootfs/commitmeta");

  if (!g_file_query_exists (ref, NULL))
    return flatpak_fail (error, "Required file ref not in tarfile");
  if (!g_file_query_exists (metadata, NULL))
    return flatpak_fail (error, "Required file metadata not in tarfile");
  if (!g_file_query_exists (commit, NULL))
    return flatpak_fail (error, "Required file commit not in tarfile");

  if (!g_file_load_contents (ref, cancellable,
                             &ref_data, NULL,
                             NULL, error))
    return FALSE;

  if (g_str_has_prefix (ref_data, "app/"))
    files_source = "rootfs/app";
  else
    files_source = "rootfs/usr";

  files = g_file_resolve_relative_path (archive_root, files_source);
  if (!g_file_query_exists (files, NULL))
    return flatpak_fail (error, "Required directory %s not in tarfile", files_source);

  export = g_file_resolve_relative_path (archive_root, "rootfs/export");

  if (!g_file_load_contents (commit, cancellable,
                             &commit_data, &commit_size,
                             NULL, error))
    return FALSE;

  commitv = g_variant_new_from_data (OSTREE_COMMIT_GVARIANT_FORMAT,
                                     g_steal_pointer (&commit_data), commit_size,
                                     FALSE, g_free, commit_data);
  if (!ostree_validate_structureof_commit (commitv, error))
    return FALSE;

  if (g_file_query_exists (commitmeta, NULL) &&
      !g_file_load_contents (commitmeta, cancellable,
                             &commitmeta_data, &commitmeta_size,
                             NULL, error))
    return FALSE;

  if (commitmeta_data != NULL)
    commitmetav = g_variant_new_from_data (G_VARIANT_TYPE ("a{sv}"),
                                           g_steal_pointer (&commitmeta_data), commitmeta_size,
                                           FALSE, g_free, commitmeta_data);

  mtree = ostree_mutable_tree_new ();

  if (!flatpak_mtree_create_root (repo, mtree, cancellable, error))
    return FALSE;

  if (!ostree_mutable_tree_ensure_dir (mtree, "files", &files_mtree, error))
    return FALSE;

  if (!ostree_repo_write_directory_to_mtree (repo, files, files_mtree, NULL,
                                             cancellable, error))
    return FALSE;

  if (g_file_query_exists (export, NULL))
    {
      if (!ostree_mutable_tree_ensure_dir (mtree, "export", &export_mtree, error))
        return FALSE;

      if (!ostree_repo_write_directory_to_mtree (repo, export, export_mtree, NULL,
                                                 cancellable, error))
        return FALSE;
    }

  if (!ostree_mutable_tree_replace_file (mtree, "metadata",
                                         ostree_repo_file_get_checksum ((OstreeRepoFile *) metadata),
                                         error))
    return FALSE;

  if (!ostree_repo_write_mtree (repo, mtree, &root, cancellable, error))
    return FALSE;

  /* Verify that we created the same contents */
  {
    g_autoptr(GVariant) tree_contents_bytes = NULL;
    g_autofree char *tree_contents_checksum = NULL;
    g_autoptr(GVariant) tree_metadata_bytes = NULL;
    g_autofree char *tree_metadata_checksum = NULL;
    tree_contents_bytes = g_variant_get_child_value (commitv, 6);
    tree_contents_checksum = ostree_checksum_from_bytes_v (tree_contents_bytes);
    tree_metadata_bytes = g_variant_get_child_value (commitv, 7);
    tree_metadata_checksum = ostree_checksum_from_bytes_v (tree_metadata_bytes);

    if (strcmp (tree_contents_checksum, ostree_repo_file_tree_get_contents_checksum ((OstreeRepoFile *) root)))
      return flatpak_fail (error, "Imported content checksum (%s) does not match original checksum (%s)",
                           tree_contents_checksum, ostree_repo_file_tree_get_contents_checksum ((OstreeRepoFile *) root));

    if (strcmp (tree_metadata_checksum, ostree_repo_file_tree_get_metadata_checksum ((OstreeRepoFile *) root)))
      return flatpak_fail (error, "Imported metadata checksum (%s) does not match original checksum (%s)",
                           tree_metadata_checksum, ostree_repo_file_tree_get_metadata_checksum ((OstreeRepoFile *) root));
  }

  commitv_metadata = g_variant_get_child_value (commitv, 0);
  parent = ostree_commit_get_parent (commitv);
  g_variant_get_child (commitv, 3, "s", &subject);
  g_variant_get_child (commitv, 4, "s", &body);

  if (!ostree_repo_write_commit_with_time (repo,
                                           parent,
                                           subject,
                                           body,
                                           commitv_metadata,
                                           OSTREE_REPO_FILE (root),
                                           ostree_commit_get_timestamp (commitv),
                                           &commit_checksum,
                                           cancellable, error))
    return FALSE;

  if (commitmetav != NULL &&
      !ostree_repo_write_commit_detached_metadata (repo, commit_checksum,
                                                   commitmetav, cancellable, error))
    return FALSE;

  if (opt_ref != NULL)
    target_ref = opt_ref;
  else
    target_ref = ref_data;

  ostree_repo_transaction_set_ref (repo, NULL, target_ref, commit_checksum);

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    return FALSE;

  g_print ("Importing %s (%s)\n", target_ref, commit_checksum);

  return TRUE;
#endif
}

static gboolean
import_bundle (OstreeRepo *repo, GFile *file,
               GCancellable *cancellable, GError **error)
{
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *bundle_ref = NULL;
  g_autofree char *to_checksum = NULL;
  const char *ref;

  metadata = flatpak_bundle_load (file, &to_checksum,
                                  &bundle_ref,
                                  NULL,
                                  NULL,
                                  NULL,
                                  error);
  if (metadata == NULL)
    return FALSE;

  if (opt_ref != NULL)
    ref = opt_ref;
  else
    ref = bundle_ref;

  g_print ("Importing %s (%s)\n", ref, to_checksum);
  if (!flatpak_pull_from_bundle (repo, file,
                                 NULL, ref, FALSE,
                                 cancellable,
                                 error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_builtin_build_import (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GFile) repofile = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *location;
  const char *filename;

  context = g_option_context_new (_("LOCATION FILENAME - Import a file bundle into a local repository"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 3)
    return usage_error (context, _("LOCATION and FILENAME must be specified"), error);

  location = argv[1];
  filename = argv[2];

  repofile = g_file_new_for_commandline_arg (location);
  repo = ostree_repo_new (repofile);

  if (!g_file_query_exists (repofile, cancellable))
    return flatpak_fail (error, _("'%s' is not a valid repository"), location);

  file = g_file_new_for_commandline_arg (filename);

  if (!ostree_repo_open (repo, cancellable, error))
    return FALSE;

  if (opt_oci)
    {
      if (!import_oci (repo, file, cancellable, error))
        return FALSE;
    }
  else
    {
      if (!import_bundle (repo, file, cancellable, error))
        return FALSE;
    }

  if (opt_update_appstream &&
      !flatpak_repo_generate_appstream (repo, (const char **) opt_gpg_key_ids, opt_gpg_homedir, cancellable, error))
    return FALSE;

  if (!opt_no_update_summary &&
      !flatpak_repo_update (repo,
                            (const char **) opt_gpg_key_ids,
                            opt_gpg_homedir,
                            cancellable,
                            error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_complete_build_import (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new ("");

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* LOCATION */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      flatpak_complete_dir (completion);
      break;

    case 2: /* FILENAME */
      flatpak_complete_file (completion);
      break;
    }

  return TRUE;
}
