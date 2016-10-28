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
#include "flatpak-oci.h"

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

GLNX_DEFINE_CLEANUP_FUNCTION (void *, flatpak_local_free_read_archive, archive_read_free)
#define free_read_archive __attribute__((cleanup (flatpak_local_free_read_archive)))

static void
propagate_libarchive_error (GError      **error,
                            struct archive *a)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "%s", archive_error_string (a));
}

static gboolean
import_oci (OstreeRepo *repo, GFile *file,
            GCancellable *cancellable, GError **error)
{
  g_autoptr(OstreeMutableTree) archive_mtree = NULL;
  g_autoptr(GFile) archive_root = NULL;
  g_autofree char *commit_checksum = NULL;
  g_autofree char *config_digest = NULL;
  const char *parent = NULL;
  const char *metadata_base64;
  const char *subject;
  const char *body;
  const char *target_ref;
  guint64 timestamp;
  g_autoptr(FlatpakOciDir) oci_dir = NULL;
  g_autoptr(JsonObject) manifest = NULL;
  g_autoptr(JsonObject) config = NULL;
  g_autoptr(GHashTable) annotations = NULL;
  g_autoptr(GVariant) metadatav = NULL;
  g_auto(GStrv) layers = NULL;
  int i;

  oci_dir = flatpak_oci_dir_new ();

  if (!flatpak_oci_dir_open (oci_dir, file, cancellable, error))
    return FALSE;

  manifest = flatpak_oci_dir_find_manifest (oci_dir, "latest", "linux", "amd64",
                                            cancellable, error);
  if (manifest == NULL)
    return FALSE;

  annotations = flatpak_oci_manifest_get_annotations (manifest);

  if (opt_ref != NULL)
    target_ref = opt_ref;
  else
    {
      target_ref = g_hash_table_lookup (annotations, "org.flatpak.Ref");
      if (target_ref == NULL)
        return flatpak_fail (error, "No flatpak ref specified in image, must manually specify");
    }

  subject = g_hash_table_lookup (annotations, "org.flatpak.Subject");
  body = g_hash_table_lookup (annotations, "org.flatpak.Body");
  parent = g_hash_table_lookup (annotations, "org.flatpak.ParentCommit");
  metadata_base64 = g_hash_table_lookup (annotations, "org.flatpak.Metadata");
  if (metadata_base64)
    {
      gsize data_len;
      guchar *data = g_base64_decode (metadata_base64, &data_len);
      metadatav = g_variant_new_from_data (G_VARIANT_TYPE("a{sv}"), data, data_len,
                                           FALSE, g_free, data);
    }

  config_digest = flatpak_oci_manifest_get_config (manifest);
  if (config_digest == NULL)
    return flatpak_fail (error, "No oci config specified");

  config = flatpak_oci_dir_load_json (oci_dir, config_digest, cancellable, error);
  if (config == NULL)
    return FALSE;

  timestamp = flatpak_oci_config_get_created (config);

  if (!ostree_repo_prepare_transaction (repo, NULL, cancellable, error))
    return FALSE;

  /* There is no way to write a subset of the archive to a mtree, so instead
     we write all of it and then build a new mtree with the subset */
  archive_mtree = ostree_mutable_tree_new ();

  layers = flatpak_oci_manifest_get_layers (manifest);
  for (i = 0; layers[i] != NULL; i++)
    {
      OstreeRepoImportArchiveOptions opts = { 0, };
      free_read_archive struct archive *a = NULL;

      opts.autocreate_parents = TRUE;

      a = flatpak_oci_dir_load_layer (oci_dir, layers[i],
                                      cancellable, error);
      if (a == NULL)
        return FALSE;

      if (!ostree_repo_import_archive_to_mtree (repo, &opts, a, archive_mtree, NULL, cancellable, error))
        return FALSE;

      if (archive_read_close (a) != ARCHIVE_OK)
        {
          propagate_libarchive_error (error, a);
          return FALSE;
        }
    }

  if (!ostree_repo_write_mtree (repo, archive_mtree, &archive_root, cancellable, error))
    return FALSE;

  if (!ostree_repo_file_ensure_resolved ((OstreeRepoFile *) archive_root, error))
    return FALSE;

  if (!ostree_repo_write_commit_with_time (repo,
                                           parent,
                                           subject,
                                           body,
                                           metadatav,
                                           OSTREE_REPO_FILE (archive_root),
                                           timestamp,
                                           &commit_checksum,
                                           cancellable, error))
    return FALSE;

  ostree_repo_transaction_set_ref (repo, NULL, target_ref, commit_checksum);

  if (!ostree_repo_commit_transaction (repo, NULL, cancellable, error))
    return FALSE;

  g_print ("Importing %s (%s)\n", target_ref, commit_checksum);

  return TRUE;
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

  if (argc > 3)
    return usage_error (context, _("Too many arguments"), error);

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
