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
#include "flatpak-utils-private.h"
#include "flatpak-oci-registry-private.h"
#include "flatpak-chain-input-stream-private.h"
#include "flatpak-builtins-utils.h"

#include <archive.h>
#include <archive_entry.h>

static char *opt_arch;
static char *opt_repo_url;
static char *opt_runtime_repo;
static gboolean opt_runtime = FALSE;
static char **opt_gpg_file;
static gboolean opt_oci = FALSE;
static gboolean opt_oci_use_labels = TRUE; // Unused now
static char **opt_gpg_key_ids;
static char *opt_gpg_homedir;
static char *opt_from_commit;

static GOptionEntry options[] = {
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Export runtime instead of app"), NULL },
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to bundle for"), N_("ARCH") },
  { "repo-url", 0, 0, G_OPTION_ARG_STRING, &opt_repo_url, N_("Url for repo"), N_("URL") },
  { "runtime-repo", 0, 0, G_OPTION_ARG_STRING, &opt_runtime_repo, N_("Url for runtime flatpakrepo file"), N_("URL") },
  { "gpg-keys", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_gpg_file, N_("Add GPG key from FILE (- for stdin)"), N_("FILE") },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids, N_("GPG Key ID to sign the OCI image with"), N_("KEY-ID") },
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, N_("GPG Homedir to use when looking for keyrings"), N_("HOMEDIR") },
  { "from-commit", 0, 0, G_OPTION_ARG_STRING, &opt_from_commit, N_("OSTree commit to create a delta bundle from"), N_("COMMIT") },
  { "oci", 0, 0, G_OPTION_ARG_NONE, &opt_oci, N_("Export oci image instead of flatpak bundle"), NULL },
  // This is not used anymore as it is the default, but accept it if old code uses it
  { "oci-use-labels", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_oci_use_labels, NULL, NULL },
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
get_bundle_appstream_data (GFile        *root,
                           const char   *full_branch,
                           const char   *name,
                           GKeyFile     *metadata,
                           gboolean      compress,
                           GBytes      **result,
                           GCancellable *cancellable,
                           GError      **error)
{
  g_autoptr(GFile) xmls_dir = NULL;
  g_autofree char *appstream_basename = NULL;
  g_autoptr(GFile) appstream_file = NULL;
  g_autoptr(GInputStream) xml_in = NULL;

  *result = NULL;

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
                                         full_branch, name, metadata))
        {
          g_autoptr(GBytes) xml_data = NULL;
          gboolean success = FALSE;

          if (compress)
            success = flatpak_appstream_xml_root_to_data (appstream_root, NULL, &xml_data, error);
          else
            success = flatpak_appstream_xml_root_to_data (appstream_root, &xml_data, NULL, error);

          if (!success)
            return FALSE;

          *result = g_steal_pointer (&xml_data);
        }
    }

  return TRUE;
}

typedef void (*IterateBundleIconsCallback) (const char *icon_size_name,
                                            GBytes     *png_data,
                                            gpointer    user_data);

static gboolean
iterate_bundle_icons (GFile                     *root,
                      const char                *name,
                      IterateBundleIconsCallback callback,
                      gpointer                   user_data,
                      GCancellable              *cancellable,
                      GError                   **error)
{
  g_autoptr(GFile) icons_dir =
    g_file_resolve_relative_path (root,
                                  "files/share/app-info/icons/flatpak");
  const char *icon_sizes[] = { "64x64", "128x128" };
  const char *icon_sizes_key[] = { "icon-64", "icon-128" };
  g_autofree char *icon_name = g_strconcat (name, ".png", NULL);
  gint i;

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

          callback (icon_sizes_key[i], png_data, user_data);
        }
    }

  return TRUE;
}

static void
add_icon_to_metadata (const char *icon_size_name,
                      GBytes     *png_data,
                      gpointer    user_data)
{
  GVariantBuilder *metadata_builder = user_data;

  g_variant_builder_add (metadata_builder, "{sv}", icon_size_name,
                         g_variant_new_from_bytes (G_VARIANT_TYPE_BYTESTRING,
                                                   png_data, TRUE));
}

static gboolean
build_bundle (OstreeRepo *repo, const char *commit_checksum, GFile *file,
              const char *name, const char *full_branch,
              const char *from_commit,
              GCancellable *cancellable, GError **error)
{
  GVariantBuilder metadata_builder;
  GVariantBuilder param_builder;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GBytes) xml_data = NULL;
  g_autoptr(GFile) metadata_file = NULL;
  g_autoptr(GInputStream) in = NULL;
  g_autoptr(GFile) root = NULL;
  g_autoptr(GBytes) gpg_data = NULL;
  g_autoptr(GVariant) params = NULL;
  g_autoptr(GVariant) metadata = NULL;
  const char *collection_id;

  if (!ostree_repo_read_commit (repo, commit_checksum, &root, NULL, NULL, error))
    return FALSE;

  g_variant_builder_init (&metadata_builder, G_VARIANT_TYPE ("a{sv}"));

  /* We add this first in the metadata, so this will become the file
   * format header.  The first part is readable to make it easy to
   * figure out the type. The uint32 is basically a random value, but
   * it ensures we have both zero and high bits sets, so we don't get
   * sniffed as text. Also, the last 01 can be used as a version
   * later.  Furthermore, the use of an uint32 lets us detect
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

  if (!get_bundle_appstream_data (root, full_branch, name, keyfile, TRUE,
                                  &xml_data, cancellable, error))
    return FALSE;

  if (xml_data)
    {
      g_variant_builder_add (&metadata_builder, "{sv}", "appdata",
                             g_variant_new_from_bytes (G_VARIANT_TYPE_BYTESTRING,
                                                       xml_data, TRUE));

      if (!iterate_bundle_icons (root, name, add_icon_to_metadata,
                                 &metadata_builder, cancellable, error))
        return FALSE;
    }

  if (opt_repo_url)
    g_variant_builder_add (&metadata_builder, "{sv}", "origin", g_variant_new_string (opt_repo_url));

  if (opt_runtime_repo)
    g_variant_builder_add (&metadata_builder, "{sv}", "runtime-repo", g_variant_new_string (opt_runtime_repo));

  collection_id = ostree_repo_get_collection_id (repo);
  g_variant_builder_add (&metadata_builder, "{sv}", "collection-id",
                         g_variant_new_string (collection_id ? collection_id : ""));

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
                                          from_commit,
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

static void
add_icon_to_labels (const char *icon_size_name,
                    GBytes     *png_data,
                    gpointer    user_data)
{
  GHashTable *labels = user_data;
  g_autofree char *encoded = g_base64_encode (g_bytes_get_data (png_data, NULL),
                                              g_bytes_get_size (png_data));

  g_hash_table_replace (labels,
                        g_strconcat ("org.freedesktop.appstream.", icon_size_name, NULL),
                        g_strconcat ("data:image/png;base64,", encoded, NULL));
}

static GHashTable *
generate_labels (FlatpakOciDescriptor *layer_desc,
                 OstreeRepo *repo,
                 GFile *root,
                 const char *name,
                 const char *ref,
                 const char *commit_checksum,
                 GVariant   *commit_data,
                 GCancellable *cancellable,
                 GError **error)
{
  g_autoptr(GFile) metadata_file = NULL;
  gsize metadata_size;
  g_autofree char *metadata_contents = NULL;
  g_autoptr(GHashTable) labels = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GBytes) xml_data = NULL;
  guint64 installed_size = 0;

  flatpak_oci_add_labels_for_commit (labels, ref, commit_checksum, commit_data);

  metadata_file = g_file_get_child (root, "metadata");
  if (g_file_load_contents (metadata_file, cancellable, &metadata_contents, &metadata_size, NULL, NULL) &&
      g_utf8_validate (metadata_contents, -1, NULL))
    {
      keyfile = g_key_file_new ();

      if (!g_key_file_load_from_data (keyfile,
                                      metadata_contents,
                                      metadata_size,
                                      G_KEY_FILE_NONE, error))
        return NULL;

      g_hash_table_replace (labels,
                            g_strdup ("org.flatpak.metadata"),
                            g_steal_pointer (&metadata_contents));
    }

  if (!flatpak_repo_collect_sizes (repo, root, &installed_size, NULL, NULL, error))
    return NULL;

  g_hash_table_replace (labels,
                        g_strdup ("org.flatpak.installed-size"),
                        g_strdup_printf ("%" G_GUINT64_FORMAT, installed_size));

  g_hash_table_replace (labels,
                        g_strdup ("org.flatpak.download-size"),
                        g_strdup_printf ("%" G_GUINT64_FORMAT, layer_desc->size));


  if (!get_bundle_appstream_data (root, ref, name, keyfile, FALSE,
                                  &xml_data, cancellable, error))
    return FALSE;

  if (xml_data)
    {
      gsize xml_data_len;

      g_hash_table_replace (labels,
                            g_strdup ("org.freedesktop.appstream.appdata"),
                            g_bytes_unref_to_data (g_steal_pointer (&xml_data), &xml_data_len));

      if (!iterate_bundle_icons (root, name, add_icon_to_labels,
                                 labels, cancellable, error))
        return FALSE;
    }

  return g_steal_pointer (&labels);
}



static gboolean
build_oci (OstreeRepo *repo, const char *commit_checksum, GFile *dir,
           const char *name, const char *ref_str,
           GCancellable *cancellable, GError **error)
{
  g_autoptr(GFile) root = NULL;
  g_autoptr(GVariant) commit_data = NULL;
  g_autoptr(GVariant) commit_metadata = NULL;
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
  g_autoptr(GHashTable) flatpak_labels = NULL;
  g_autoptr(FlatpakDecomposed) ref = NULL;
  g_autofree char *arch = NULL;
  int history_index;
  GTimeVal tv;

  if (!ostree_repo_read_commit (repo, commit_checksum, &root, NULL, NULL, error))
    return FALSE;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit_checksum, &commit_data, error))
    return FALSE;

  if (!ostree_repo_read_commit_detached_metadata (repo, commit_checksum, &commit_metadata, cancellable, error))
    return FALSE;

  ref = flatpak_decomposed_new_from_ref (ref_str, error);
  if (ref == NULL)
    return FALSE;

  arch = flatpak_decomposed_dup_arch (ref);

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

  flatpak_labels = generate_labels (layer_desc, repo, root, name, flatpak_decomposed_get_ref (ref), commit_checksum, commit_data, cancellable, error);
  if (flatpak_labels == NULL)
    return FALSE;

  image = flatpak_oci_image_new ();
  flatpak_oci_image_set_layer (image, uncompressed_digest);
  flatpak_oci_image_set_architecture (image, flatpak_arch_to_oci_arch (arch));
  history_index = flatpak_oci_image_add_history (image);

  g_get_current_time (&tv);
  image->history[history_index]->created = g_time_val_to_iso8601 (&tv);
  image->history[history_index]->created_by = g_strdup ("flatpak build-bundle");

  flatpak_oci_copy_labels (flatpak_labels,
                           flatpak_oci_image_get_labels (image));

  timestamp = timestamp_to_iso8601 (ostree_commit_get_timestamp (commit_data));
  flatpak_oci_image_set_created (image, timestamp);

  image_desc = flatpak_oci_registry_store_json (registry, FLATPAK_JSON (image), cancellable, error);
  if (image_desc == NULL)
    return FALSE;

  manifest = flatpak_oci_manifest_new ();
  flatpak_oci_manifest_set_config (manifest, image_desc);
  flatpak_oci_manifest_set_layer (manifest, layer_desc);

  manifest_desc = flatpak_oci_registry_store_json (registry, FLATPAK_JSON (manifest), cancellable, error);
  if (manifest_desc == NULL)
    return FALSE;

  index = flatpak_oci_registry_load_index (registry, NULL, NULL);
  if (index == NULL)
    index = flatpak_oci_index_new ();

  flatpak_oci_index_add_manifest (index, flatpak_decomposed_get_ref (ref), manifest_desc);

  if (!flatpak_oci_registry_save_index (registry, index, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
_repo_resolve_rev (OstreeRepo *repo, const char *ref, char **out_rev,
                   GCancellable *cancellable, GError **error)
{
  g_autoptr(GError) my_error = NULL;

  g_return_val_if_fail (repo != NULL, FALSE);
  g_return_val_if_fail (ref != NULL, FALSE);
  g_return_val_if_fail (out_rev != NULL, FALSE);
  g_return_val_if_fail (*out_rev == NULL, FALSE);

  if (ostree_repo_resolve_rev (repo, ref, FALSE, out_rev, &my_error))
    return TRUE;
  else
    {
      g_autoptr(GHashTable) collection_refs = NULL;  /* (element-type OstreeCollectionRef utf8) */

      /* Fall back to iterating over every collection-ref. We can't use
       * ostree_repo_resolve_collection_ref() since we don't know the
       * collection ID. */
      if (!ostree_repo_list_collection_refs (repo, NULL, &collection_refs,
                                             OSTREE_REPO_LIST_REFS_EXT_NONE,
                                             cancellable, error))
        return FALSE;

      /* Take the checksum of the first matching ref. There's no point in
       * checking for duplicates because (a) it's not possible to install the
       * same app from two collections in the same flatpak installation and (b)
       * ostree_repo_resolve_rev() also takes the first matching ref. */
      GLNX_HASH_TABLE_FOREACH_KV (collection_refs, const OstreeCollectionRef *, c_r,
                                  const char*, checksum)
        {
          if (g_strcmp0 (c_r->ref_name, ref) == 0)
            {
              *out_rev = g_strdup (checksum);
              return TRUE;
            }
        }

      g_propagate_error (error, g_steal_pointer (&my_error));
      return FALSE;
    }
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
  g_autofree char *commit_checksum = NULL;

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
    {
      g_prefix_error (error, _("'%s' is not a valid repository: "), location);
      return FALSE;
    }

  /* We can't use flatpak_repo_resolve_rev() here because it takes a NULL
   * remote name to mean the ref is local. */
  if (_repo_resolve_rev (repo, name, &commit_checksum, NULL, NULL))
    full_branch = g_strdup (name);
  else
    {
      if (!flatpak_is_valid_name (name, -1, &my_error))
        return flatpak_fail (error, _("'%s' is not a valid name: %s"), name, my_error->message);

      if (!flatpak_is_valid_branch (branch, -1, &my_error))
        return flatpak_fail (error, _("'%s' is not a valid branch name: %s"), branch, my_error->message);

      if (opt_runtime)
        full_branch = flatpak_build_runtime_ref (name, branch, opt_arch);
      else
        full_branch = flatpak_build_app_ref (name, branch, opt_arch);

      if (!_repo_resolve_rev (repo, full_branch, &commit_checksum, cancellable, error))
        return FALSE;
    }

  file = g_file_new_for_commandline_arg (filename);

  if (flatpak_file_get_path_cached (file) == NULL)
    return flatpak_fail (error, _("'%s' is not a valid filename"), filename);

  if (opt_oci)
    {
      if (!build_oci (repo, commit_checksum, file, name, full_branch, cancellable, error))
        return FALSE;
    }
  else
    {
      if (!build_bundle (repo, commit_checksum, file, name, full_branch, opt_from_commit, cancellable, error))
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
      flatpak_complete_file (completion, "__FLATPAK_BUNDLE_FILE");
      break;
    }

  return TRUE;
}
