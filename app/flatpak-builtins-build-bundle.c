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

#include <json-glib/json-glib.h>

#include <glib/gi18n.h>

#include <gio/gunixinputstream.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils.h"
#include "flatpak-oci-registry.h"
#include "flatpak-chain-input-stream.h"
#include "flatpak-builtins-utils.h"

#include <archive.h>
#include <archive_entry.h>

static char *opt_arch;
static char *opt_repo_url;
static char *opt_runtime_repo;
static gboolean opt_runtime = FALSE;
static char **opt_gpg_file;
static gboolean opt_oci = FALSE;
static char **opt_gpg_key_ids;
static char *opt_gpg_homedir;

static GOptionEntry options[] = {
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Export runtime instead of app"), NULL },
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to bundle for"), N_("ARCH") },
  { "repo-url", 0, 0, G_OPTION_ARG_STRING, &opt_repo_url, N_("Url for repo"), N_("URL") },
  { "runtime-repo", 0, 0, G_OPTION_ARG_STRING, &opt_runtime_repo, N_("Url for runtime flatpakrepo file"), N_("URL") },
  { "gpg-keys", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_gpg_file, N_("Add GPG key from FILE (- for stdin)"), N_("FILE") },
  { "oci", 0, 0, G_OPTION_ARG_NONE, &opt_oci, N_("Export oci image instead of flatpak bundle"), NULL },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids, N_("GPG Key ID to sign the OCI image with"), N_("KEY-ID") },
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, N_("GPG Homedir to use when looking for keyrings"), N_("HOMEDIR") },

  { NULL }
};

static GBytes *
read_gpg_data (GCancellable *cancellable,
               GError      **error)
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
          g_autoptr(GFile) file = g_file_new_for_commandline_arg (opt_gpg_file[ii]);
          input_stream = G_INPUT_STREAM (g_file_read (file, cancellable, error));

          if (input_stream == NULL)
            return NULL;
        }

      /* Takes ownership. */
      g_ptr_array_add (streams, input_stream);
    }

  /* Chain together all the --keyring options as one long stream. */
  source_stream = (GInputStream *) flatpak_chain_input_stream_new (streams);

  return flatpak_read_stream (source_stream, FALSE, error);
}

static gboolean
build_bundle (OstreeRepo *repo, GFile *file,
              const char *name, const char *full_branch,
              GCancellable *cancellable, GError **error)
{
  GVariantBuilder metadata_builder;
  GVariantBuilder param_builder;

  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GFile) xmls_dir = NULL;
  g_autoptr(GFile) metadata_file = NULL;
  g_autoptr(GFile) appstream_file = NULL;
  g_autofree char *appstream_basename = NULL;
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(GInputStream) xml_in = NULL;
  g_autoptr(GFile) root = NULL;
  g_autofree char *commit_checksum = NULL;
  g_autoptr(GBytes) gpg_data = NULL;
  g_autoptr(GVariant) params = NULL;
  g_autoptr(GVariant) metadata = NULL;

  if (!ostree_repo_resolve_rev (repo, full_branch, FALSE, &commit_checksum, error))
    return FALSE;

  if (!ostree_repo_read_commit (repo, commit_checksum, &root, NULL, NULL, error))
    return FALSE;

  g_variant_builder_init (&metadata_builder, G_VARIANT_TYPE ("a{sv}"));

  /* We add this first in the metadata, so this will become the file
   * format header.  The first part is readable to make it easy to
   * figure out the type. The uint32 is basically a random value, but
   * it ensures we have both zero and high bits sets, so we don't get
   * sniffed as text. Also, the last 01 can be used as a version
   * later.  Furthermore, the use of an uint32 lets use detect
   * byteorder issues.
   */
  g_variant_builder_add (&metadata_builder, "{sv}", "flatpak",
                         g_variant_new_uint32 (0xe5890001));

  g_variant_builder_add (&metadata_builder, "{sv}", "ref", g_variant_new_string (full_branch));

  metadata_file = g_file_resolve_relative_path (root, "metadata");

  keyfile = g_key_file_new ();

  in = (GInputStream *) g_file_read (metadata_file, cancellable, NULL);
  if (in != NULL)
    {
      g_autoptr(GBytes) bytes = flatpak_read_stream (in, TRUE, error);

      if (bytes == NULL)
        return FALSE;

      if (!g_key_file_load_from_data (keyfile,
                                      g_bytes_get_data (bytes, NULL),
                                      g_bytes_get_size (bytes),
                                      G_KEY_FILE_NONE, error))
        return FALSE;

      g_variant_builder_add (&metadata_builder, "{sv}", "metadata",
                             g_variant_new_string (g_bytes_get_data (bytes, NULL)));
    }

  xmls_dir = g_file_resolve_relative_path (root, "files/share/app-info/xmls");
  appstream_basename = g_strconcat (name, ".xml.gz", NULL);
  appstream_file = g_file_get_child (xmls_dir, appstream_basename);

  xml_in = (GInputStream *) g_file_read (appstream_file, cancellable, NULL);
  if (xml_in)
    {
      g_autoptr(FlatpakXml) appstream_root = NULL;
      g_autoptr(FlatpakXml) xml_root = flatpak_xml_parse (xml_in, TRUE,
                                                          cancellable, error);
      if (xml_root == NULL)
        return FALSE;

      appstream_root = flatpak_appstream_xml_new ();
      if (flatpak_appstream_xml_migrate (xml_root, appstream_root,
                                         full_branch, name, keyfile))
        {
          g_autoptr(GBytes) xml_data = flatpak_appstream_xml_root_to_data (appstream_root, error);
          int i;
          g_autoptr(GFile) icons_dir =
            g_file_resolve_relative_path (root,
                                          "files/share/app-info/icons/flatpak");
          const char *icon_sizes[] = { "64x64", "128x128" };
          const char *icon_sizes_key[] = { "icon-64", "icon-128" };
          g_autofree char *icon_name = g_strconcat (name, ".png", NULL);

          if (xml_data == NULL)
            return FALSE;

          g_variant_builder_add (&metadata_builder, "{sv}", "appdata",
                                 g_variant_new_from_bytes (G_VARIANT_TYPE_BYTESTRING,
                                                           xml_data, TRUE));

          for (i = 0; i < G_N_ELEMENTS (icon_sizes); i++)
            {
              g_autoptr(GFile) size_dir = g_file_get_child (icons_dir, icon_sizes[i]);
              g_autoptr(GFile) icon_file = g_file_get_child (size_dir, icon_name);
              g_autoptr(GInputStream) png_in = NULL;

              png_in = (GInputStream *) g_file_read (icon_file, cancellable, NULL);
              if (png_in != NULL)
                {
                  g_autoptr(GBytes) png_data = flatpak_read_stream (png_in, FALSE, error);
                  if (png_data == NULL)
                    return FALSE;

                  g_variant_builder_add (&metadata_builder, "{sv}", icon_sizes_key[i],
                                         g_variant_new_from_bytes (G_VARIANT_TYPE_BYTESTRING,
                                                                   png_data, TRUE));
                }
            }
        }

    }

  if (opt_repo_url)
    g_variant_builder_add (&metadata_builder, "{sv}", "origin", g_variant_new_string (opt_repo_url));

  if (opt_runtime_repo)
    g_variant_builder_add (&metadata_builder, "{sv}", "runtime-repo", g_variant_new_string (opt_runtime_repo));

  if (opt_gpg_file != NULL)
    {
      gpg_data = read_gpg_data (cancellable, error);
      if (gpg_data == NULL)
        return FALSE;
    }

  if (gpg_data)
    {
      g_variant_builder_add (&metadata_builder, "{sv}", "gpg-keys",
                             g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                        g_bytes_get_data (gpg_data, NULL),
                                                        g_bytes_get_size (gpg_data),
                                                        1));
    }

  g_variant_builder_init (&param_builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&param_builder, "{sv}", "min-fallback-size", g_variant_new_uint32 (0));
  g_variant_builder_add (&param_builder, "{sv}", "compression", g_variant_new_byte ('x'));
  g_variant_builder_add (&param_builder, "{sv}", "bsdiff-enabled", g_variant_new_boolean (FALSE));
  g_variant_builder_add (&param_builder, "{sv}", "inline-parts", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&param_builder, "{sv}", "include-detached", g_variant_new_boolean (TRUE));
  g_variant_builder_add (&param_builder, "{sv}", "filename", g_variant_new_bytestring (flatpak_file_get_path_cached (file)));

  params = g_variant_ref_sink (g_variant_builder_end (&param_builder));
  metadata = g_variant_ref_sink (g_variant_builder_end (&metadata_builder));

  if (!ostree_repo_static_delta_generate (repo,
                                          OSTREE_STATIC_DELTA_GENERATE_OPT_LOWLATENCY,
                                          /* from */ NULL,
                                          commit_checksum,
                                          metadata,
                                          params,
                                          cancellable,
                                          error))
    return FALSE;

  return TRUE;
}

static gchar *
timestamp_to_iso8601 (guint64 timestamp)
{
  GTimeVal stamp;

  stamp.tv_sec = timestamp;
  stamp.tv_usec = 0;

  return g_time_val_to_iso8601 (&stamp);
}

static gboolean
export_commit_to_archive (OstreeRepo *repo,
                          GFile *root,
                          guint64 timestamp,
                          struct archive *a,
                          GCancellable *cancellable, GError **error)
{
  OstreeRepoExportArchiveOptions opts = { 0, };

  opts.timestamp_secs = timestamp;

  if (!ostree_repo_export_tree_to_archive (repo, &opts, (OstreeRepoFile *) root, a,
                                           cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
build_oci (OstreeRepo *repo, GFile *dir,
           const char *name, const char *ref,
           GCancellable *cancellable, GError **error)
{
  g_autoptr(GFile) root = NULL;
  g_autoptr(GVariant) commit_data = NULL;
  g_autoptr(GVariant) commit_metadata = NULL;
  g_autofree char *commit_checksum = NULL;
  g_autofree char *dir_uri = NULL;
  g_autoptr(FlatpakOciRegistry) registry = NULL;
  g_autoptr(FlatpakOciLayerWriter) layer_writer = NULL;
  struct archive *archive;
  g_autofree char *uncompressed_digest = NULL;
  g_autofree char *timestamp = NULL;
  g_autoptr(FlatpakOciImage) image = NULL;
  g_autoptr(FlatpakOciDescriptor) layer_desc = NULL;
  g_autoptr(FlatpakOciDescriptor) image_desc = NULL;
  g_autoptr(FlatpakOciDescriptor) manifest_desc = NULL;
  g_autoptr(FlatpakOciManifest) manifest = NULL;
  g_autoptr(FlatpakOciIndex) index = NULL;
  g_autoptr(GFile) metadata_file = NULL;
  guint64 installed_size = 0;
  GHashTable *annotations;
  gsize metadata_size;
  g_autofree char *metadata_contents = NULL;

  if (!ostree_repo_resolve_rev (repo, ref, FALSE, &commit_checksum, error))
    return FALSE;

  if (!ostree_repo_read_commit (repo, commit_checksum, &root, NULL, NULL, error))
    return FALSE;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit_checksum, &commit_data, error))
    return FALSE;

  if (!ostree_repo_read_commit_detached_metadata (repo, commit_checksum, &commit_metadata, cancellable, error))
    return FALSE;

  dir_uri = g_file_get_uri (dir);
  registry = flatpak_oci_registry_new (dir_uri, TRUE, -1, cancellable, error);
  if (registry == NULL)
    return FALSE;

  layer_writer = flatpak_oci_registry_write_layer (registry, cancellable, error);
  if (layer_writer == NULL)
    return FALSE;

  archive = flatpak_oci_layer_writer_get_archive (layer_writer);

  if (!export_commit_to_archive (repo, root, ostree_commit_get_timestamp (commit_data),
                                 archive, cancellable, error))
    return FALSE;

  if (!flatpak_oci_layer_writer_close (layer_writer,
                                       &uncompressed_digest,
                                       &layer_desc,
                                       cancellable,
                                       error))
    return FALSE;


  image = flatpak_oci_image_new ();
  flatpak_oci_image_set_layer (image, uncompressed_digest);

  timestamp = timestamp_to_iso8601 (ostree_commit_get_timestamp (commit_data));
  flatpak_oci_image_set_created (image, timestamp);

  image_desc = flatpak_oci_registry_store_json (registry, FLATPAK_JSON (image), cancellable, error);
  if (image_desc == NULL)
    return FALSE;

  manifest = flatpak_oci_manifest_new ();
  flatpak_oci_manifest_set_config (manifest, image_desc);
  flatpak_oci_manifest_set_layer (manifest, layer_desc);

  annotations = flatpak_oci_manifest_get_annotations (manifest);
  flatpak_oci_add_annotations_for_commit (annotations, ref, commit_checksum, commit_data);

  metadata_file = g_file_get_child (root, "metadata");
  if (g_file_load_contents (metadata_file, cancellable, &metadata_contents, &metadata_size, NULL, NULL) &&
      g_utf8_validate (metadata_contents, -1, NULL))
    {
      g_hash_table_replace (annotations,
                            g_strdup ("org.flatpak.metadata"),
                            g_steal_pointer (&metadata_contents));
    }

  if (!flatpak_repo_collect_sizes (repo, root, &installed_size, NULL, NULL, error))
    return FALSE;

  g_hash_table_replace (annotations,
                        g_strdup ("org.flatpak.installed-size"),
                        g_strdup_printf ("%" G_GUINT64_FORMAT, installed_size));

  g_hash_table_replace (annotations,
                        g_strdup ("org.flatpak.download-size"),
                        g_strdup_printf ("%" G_GUINT64_FORMAT, layer_desc->size));

  manifest_desc = flatpak_oci_registry_store_json (registry, FLATPAK_JSON (manifest), cancellable, error);
  if (manifest_desc == NULL)
    return FALSE;

  flatpak_oci_export_annotations (manifest->annotations, manifest_desc->annotations);

  if (opt_gpg_key_ids)
    {
      g_autoptr(FlatpakOciSignature) sig = flatpak_oci_signature_new (manifest_desc->digest, ref);
      g_autoptr(GBytes) sig_bytes = flatpak_json_to_bytes (FLATPAK_JSON (sig));
      g_autoptr(GBytes) res = NULL;
      g_autofree char *signature_digest = NULL;

      res = flatpak_oci_sign_data (sig_bytes, (const char **)opt_gpg_key_ids, opt_gpg_homedir, error);
      if (res == NULL)
        return FALSE;

      signature_digest = flatpak_oci_registry_store_blob (registry, res, cancellable, error);
      if (signature_digest == NULL)
        return FALSE;

      g_hash_table_replace (manifest_desc->annotations,
                            g_strdup ("org.flatpak.signature-digest"),
                            g_strdup (signature_digest));
    }

  index = flatpak_oci_registry_load_index (registry, NULL, NULL, NULL, NULL);
  if (index == NULL)
    index = flatpak_oci_index_new ();

  flatpak_oci_index_add_manifest (index, manifest_desc);

  if (!flatpak_oci_registry_save_index (registry, index, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_builtin_build_bundle (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GFile) repofile = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  g_autoptr(GError) my_error = NULL;
  const char *location;
  const char *filename;
  const char *name;
  const char *branch;
  g_autofree char *full_branch = NULL;

  context = g_option_context_new (_("LOCATION FILENAME NAME [BRANCH] - Create a single file bundle from a local repository"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 4)
    return usage_error (context, _("LOCATION, FILENAME and NAME must be specified"), error);

  if (argc > 5)
    return usage_error (context, _("Too many arguments"), error);

  location = argv[1];
  filename = argv[2];
  name = argv[3];

  if (argc >= 5)
    branch = argv[4];
  else
    branch = "master";

  repofile = g_file_new_for_commandline_arg (location);
  repo = ostree_repo_new (repofile);

  if (!g_file_query_exists (repofile, cancellable))
    return flatpak_fail (error, _("'%s' is not a valid repository"), location);

  if (!ostree_repo_open (repo, cancellable, error))
    return FALSE;

  file = g_file_new_for_commandline_arg (filename);

  if (ostree_repo_resolve_rev (repo, name, FALSE, NULL, NULL))
    full_branch = g_strdup (name);
  else
    {
      if (!flatpak_is_valid_name (name, &my_error))
        return flatpak_fail (error, _("'%s' is not a valid name: %s"), name, my_error->message);

      if (!flatpak_is_valid_branch (branch, &my_error))
        return flatpak_fail (error, _("'%s' is not a valid branch name: %s"), branch, my_error->message);

      if (opt_runtime)
        full_branch = flatpak_build_runtime_ref (name, branch, opt_arch);
      else
        full_branch = flatpak_build_app_ref (name, branch, opt_arch);
    }

  if (opt_oci)
    {
      if (!build_oci (repo, file, name, full_branch, cancellable, error))
        return FALSE;
    }
  else
    {
      if (!build_bundle (repo, file, name, full_branch, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_complete_build_bundle (FlatpakCompletion *completion)
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
