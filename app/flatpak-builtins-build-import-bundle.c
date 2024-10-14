/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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

#include "libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-image-source-private.h"
#include "flatpak-oci-registry-private.h"
#include "flatpak-repo-utils-private.h"
#include "flatpak-utils-private.h"

static char *opt_ref;
static gboolean opt_oci = FALSE;
static char **opt_gpg_key_ids;
static char *opt_gpg_homedir;
static gboolean opt_update_appstream;
static gboolean opt_no_update_summary;
static gboolean opt_no_summary_index = FALSE;

static GOptionEntry options[] = {
  { "ref", 0, 0, G_OPTION_ARG_STRING, &opt_ref, N_("Override the ref used for the imported bundle"), N_("REF") },
  { "oci", 0, 0, G_OPTION_ARG_NONE, &opt_oci, N_("Import oci image instead of flatpak bundle"), NULL },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids, N_("GPG Key ID to sign the commit with"), N_("KEY-ID") },
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, N_("GPG Homedir to use when looking for keyrings"), N_("HOMEDIR") },
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
  g_autofree char *target_ref = NULL;
  g_autoptr(FlatpakImageSource) image_source = NULL;
  GHashTable *labels;

  image_source = flatpak_image_source_new_local (file, opt_ref, cancellable, error);
  if (image_source == NULL)
    return NULL;

  labels = flatpak_image_source_get_labels (image_source);
  flatpak_oci_parse_commit_labels (labels, NULL, NULL, NULL,
                                   &target_ref, NULL, NULL, NULL);
  if (target_ref == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "The OCI image didn't specify a ref, use --ref to specify one");
      return NULL;
    }

  commit_checksum = flatpak_pull_from_oci (repo, image_source, NULL,
                                           NULL, target_ref, FLATPAK_PULL_FLAGS_NONE, NULL, NULL, cancellable, error);
  if (commit_checksum == NULL)
    return NULL;

  g_print (_("Importing %s (%s)\n"), target_ref, commit_checksum);

  return g_strdup (commit_checksum);
}

static char *
import_bundle (OstreeRepo *repo, GFile *file,
               GCancellable *cancellable, GError **error)
{
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(FlatpakDecomposed) bundle_ref = NULL;
  g_autofree char *to_checksum = NULL;
  const char *ref;

  /* Don’t need to check the collection ID of the bundle here;
   * flatpak_pull_from_bundle() does that. */
  metadata = flatpak_bundle_load (file, &to_checksum,
                                  &bundle_ref,
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
                                 cancellable,
                                 error))
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

  if (opt_oci)
    commit = import_oci (repo, file, cancellable, error);
  else
    commit = import_bundle (repo, file, cancellable, error);
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

  if (opt_update_appstream &&
      !flatpak_repo_generate_appstream (repo, (const char **) opt_gpg_key_ids, opt_gpg_homedir, 0, cancellable, error))
    return FALSE;

  if (!opt_no_update_summary)
    {
      FlatpakRepoUpdateFlags flags = FLATPAK_REPO_UPDATE_FLAG_NONE;

      if (opt_no_summary_index)
        flags |= FLATPAK_REPO_UPDATE_FLAG_DISABLE_INDEX;

      g_info ("Updating summary");
      if (!flatpak_repo_update (repo, flags,
                                (const char **) opt_gpg_key_ids,
                                opt_gpg_homedir,
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
