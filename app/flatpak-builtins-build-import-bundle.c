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
#include "flatpak-utils-private.h"
#include "flatpak-oci-registry-private.h"

static char *opt_ref;
static gboolean opt_oci = FALSE;
static char **opt_gpg_key_ids = NULL;
static char *opt_gpg_homedir = NULL;
static char **opt_sign_keys = NULL;
static char *opt_sign_name = NULL;
static char **opt_sign_verify = NULL;
static gboolean opt_update_appstream;
static gboolean opt_no_update_summary;
static gboolean opt_no_summary_index = FALSE;

static GOptionEntry options[] = {
  { "ref", 0, 0, G_OPTION_ARG_STRING, &opt_ref, N_("Override the ref used for the imported bundle"), N_("REF") },
  { "oci", 0, 0, G_OPTION_ARG_NONE, &opt_oci, N_("Import oci image instead of flatpak bundle"), NULL },
#ifndef FLATPAK_DISABLE_GPG
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids, N_("GPG Key ID to sign the commit with"), N_("KEY-ID") },
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, N_("GPG Homedir to use when looking for keyrings"), N_("HOMEDIR") },
#endif
  { "sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_sign_keys, "Key ID to sign the commit with", "KEY-ID"},
  { "sign-type", 0, 0, G_OPTION_ARG_STRING, &opt_sign_name, "Signature type to use (defaults to 'ed25519')", "NAME"},
  { "sign-verify", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_sign_verify, N_("Verify signatures using KEYTYPE=inline:PUBKEY or KEYTYPE=file:/path/to/key"), N_("KEYTYPE=[inline|file]:PUBKEY") },
  { "update-appstream", 0, 0, G_OPTION_ARG_NONE, &opt_update_appstream, N_("Update the appstream branch"), NULL },
  { "no-update-summary", 0, 0, G_OPTION_ARG_NONE, &opt_no_update_summary, N_("Don't update the summary"), NULL },
  { "no-summary-index", 0, 0, G_OPTION_ARG_NONE, &opt_no_summary_index, N_("Don't generate a summary index"), NULL },
  { NULL }
};

static char *
import_oci (OstreeRepo *repo, GFile *file,
            GCancellable *cancellable, GError **error)
{
  g_autofree char *commit_checksum = NULL;
  g_autofree char *dir_uri = NULL;
  g_autofree char *target_ref = NULL;
  const char *oci_digest;
  g_autoptr(FlatpakOciRegistry) registry = NULL;
  g_autoptr(FlatpakOciVersioned) versioned = NULL;
  g_autoptr(FlatpakOciImage) image_config = NULL;
  FlatpakOciManifest *manifest = NULL;
  g_autoptr(FlatpakOciIndex) index = NULL;
  const FlatpakOciManifestDescriptor *desc;
  GHashTable *labels;

  dir_uri = g_file_get_uri (file);
  registry = flatpak_oci_registry_new (dir_uri, FALSE, -1, cancellable, error);
  if (registry == NULL)
    return NULL;

  index = flatpak_oci_registry_load_index (registry, cancellable, error);
  if (index == NULL)
    return NULL;

  if (opt_ref)
    {
      desc = flatpak_oci_index_get_manifest (index, opt_ref);
      if (desc == NULL)
        {
          flatpak_fail (error, _("Ref '%s' not found in registry"), opt_ref);
          return NULL;
        }
    }
  else
    {
      desc = flatpak_oci_index_get_only_manifest (index);
      if (desc == NULL)
        {
          flatpak_fail (error, _("Multiple images in registry, specify a ref with --ref"));
          return NULL;
        }
    }

  oci_digest = desc->parent.digest;

  versioned = flatpak_oci_registry_load_versioned (registry, NULL,
                                                   oci_digest, NULL, NULL,
                                                   cancellable, error);
  if (versioned == NULL)
    return NULL;

  manifest = FLATPAK_OCI_MANIFEST (versioned);

  image_config = flatpak_oci_registry_load_image_config (registry, NULL,
                                                         manifest->config.digest, NULL,
                                                         NULL, cancellable, error);
  if (image_config == NULL)
    return FALSE;

  labels = flatpak_oci_image_get_labels (image_config);
  if (labels)
    flatpak_oci_parse_commit_labels (labels, NULL, NULL, NULL,
                                     &target_ref, NULL, NULL, NULL);
  if (target_ref == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "The OCI image didn't specify a ref, use --ref to specify one");
      return NULL;
    }

  commit_checksum = flatpak_pull_from_oci (repo, registry, NULL, oci_digest, NULL, manifest, image_config,
                                           NULL, target_ref, FLATPAK_PULL_FLAGS_NONE, NULL, NULL, cancellable, error);
  if (commit_checksum == NULL)
    return NULL;

  g_print (_("Importing %s (%s)\n"), target_ref, commit_checksum);

  return g_strdup (commit_checksum);
}

static char *
import_bundle (OstreeRepo *repo, GFile *file, GVariant *verify_keys,
               GCancellable *cancellable, GError **error)
{
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(FlatpakDecomposed) bundle_ref = NULL;
  g_autofree char *to_checksum = NULL;
  const char *ref;

  /* Don’t need to check the collection ID of the bundle here;
   * flatpak_pull_from_bundle() does that. */
  metadata = flatpak_bundle_load (file, &to_checksum,
                                  &bundle_ref, verify_keys,
                                  NULL, NULL, NULL,
                                  NULL, NULL, NULL, error);
  if (metadata == NULL)
    return NULL;

  if (opt_ref != NULL)
    ref = opt_ref;
  else
    ref = flatpak_decomposed_get_ref (bundle_ref);

  g_print (_("Importing %s (%s)\n"), ref, to_checksum);
  if (!flatpak_pull_from_bundle (repo, file,
                                 NULL, ref, FALSE,
                                 FALSE, verify_keys,
                                 cancellable, error))
    return NULL;

  return g_strdup (to_checksum);
}

gboolean
flatpak_builtin_build_import (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GFile) repofile = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  g_autoptr(GVariant) verify_keys = NULL;
  const char *location;
  const char *filename;
  g_autofree char *commit = NULL;

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
  if (flatpak_file_get_path_cached (file) == NULL)
    return flatpak_fail (error, _("'%s' is not a valid filename"), filename);

  if (!ostree_repo_open (repo, cancellable, error))
    return FALSE;

  if (opt_sign_verify)
    verify_keys = flatpak_verify_parse_keys (opt_sign_verify);

  if (opt_oci)
    commit = import_oci (repo, file, cancellable, error);
  else
    commit = import_bundle (repo, file, verify_keys, cancellable, error);
  if (commit == NULL)
    return FALSE;

  if (opt_gpg_key_ids)
    {
      char **iter;

      for (iter = opt_gpg_key_ids; iter && *iter; iter++)
        {
          const char *keyid = *iter;
          g_autoptr(GError) local_error = NULL;

          if (!ostree_repo_sign_commit (repo,
                                        commit,
                                        keyid,
                                        opt_gpg_homedir,
                                        cancellable,
                                        &local_error))
            {
              if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
                {
                  g_propagate_error (error, g_steal_pointer (&local_error));
                  return FALSE;
                }
            }
        }
    }

  if (opt_sign_keys)
    {
      char **iter;
      g_autoptr (OstreeSign) sign = NULL;

      /* Initialize crypto system */
      opt_sign_name = opt_sign_name ?: OSTREE_SIGN_NAME_ED25519;

      sign = ostree_sign_get_by_name (opt_sign_name, error);
      if (sign == NULL)
        return FALSE;

      for (iter = opt_sign_keys; iter && *iter; iter++)
        {
          const char *keyid = *iter;
          g_autoptr (GVariant) secret_key = NULL;

          secret_key = g_variant_new_string (keyid);
          if (!ostree_sign_set_sk (sign, secret_key, error))
            return FALSE;

          if (!ostree_sign_commit (sign,
                                   repo,
                                   commit,
                                   cancellable,
                                   error))
            return FALSE;
        }
    }

  if (opt_update_appstream &&
      !flatpak_repo_generate_appstream (repo,
                                        (const char **) opt_gpg_key_ids,
                                        opt_gpg_homedir,
                                        (const char **) opt_sign_keys,
                                        opt_sign_name,
                                        0,
                                        cancellable,
                                        error))
    return FALSE;

  if (!opt_no_update_summary)
    {
      FlatpakRepoUpdateFlags flags = FLATPAK_REPO_UPDATE_FLAG_NONE;

      if (opt_no_summary_index)
        flags |= FLATPAK_REPO_UPDATE_FLAG_DISABLE_INDEX;

      g_debug ("Updating summary");
      if (!flatpak_repo_update (repo, flags,
                                (const char **) opt_gpg_key_ids,
                                opt_gpg_homedir,
                                (const char **) opt_sign_keys,
                                opt_sign_name,
                                cancellable,
                                error))
        return FALSE;
    }

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
      flatpak_complete_file (completion, "__FLATPAK_BUNDLE_FILE");
      break;
    }

  return TRUE;
}
