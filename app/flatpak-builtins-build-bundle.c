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
#include "flatpak-oci.h"
#include "flatpak-chain-input-stream.h"
#include "flatpak-builtins-utils.h"

#include <archive.h>
#include <archive_entry.h>

static char *opt_arch;
static char *opt_repo_url;
static gboolean opt_runtime = FALSE;
static char **opt_gpg_file;
static gboolean opt_oci = FALSE;

static GOptionEntry options[] = {
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Export runtime instead of app"), NULL },
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to bundle for"), N_("ARCH") },
  { "repo-url", 0, 0, G_OPTION_ARG_STRING, &opt_repo_url, N_("Url for repo"), N_("URL") },
  { "gpg-keys", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_gpg_file, N_("Add GPG key from FILE (- for stdin)"), N_("FILE") },
  { "oci", 0, 0, G_OPTION_ARG_NONE, &opt_oci, N_("Export oci image instead of flatpak bundle"), NULL },

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

  if (!ostree_repo_static_delta_generate (repo,
                                          OSTREE_STATIC_DELTA_GENERATE_OPT_LOWLATENCY,
                                          /* from */ NULL,
                                          commit_checksum,
                                          g_variant_builder_end (&metadata_builder),
                                          g_variant_builder_end (&param_builder),
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

static GBytes *
generate_config_json (guint64 timestamp,
                      const char *layer_sha256,
                      const char *arch)
{
  g_autoptr(FlatpakJsonWriter) writer = flatpak_json_writer_new ();
  g_autofree char *created = timestamp_to_iso8601 (timestamp);
  g_autofree char *layer_digest = g_strdup_printf ("sha256:%s", layer_sha256);

  flatpak_json_writer_add_string_property (writer, "created", created);
  flatpak_json_writer_add_string_property (writer, "architecture", flatpak_arch_to_oci_arch (arch));
  flatpak_json_writer_add_string_property (writer, "os", "linux");

  flatpak_json_writer_add_struct_property (writer, "rootfs");
  {
    flatpak_json_writer_add_array_property (writer, "diff_ids");
    {
      flatpak_json_writer_add_array_string (writer, layer_digest);
      flatpak_json_writer_close (writer);
    }
    flatpak_json_writer_add_string_property (writer, "type", "layers");
    flatpak_json_writer_close (writer);
  }

  return flatpak_json_writer_get_result (writer);
}

static GBytes *
generate_manifest_json (guint64 config_size,
                        const char *config_sha256,
                        guint64 layer_size,
                        const char *layer_sha256,
                        const char *ref,
                        const char *checksum,
                        GVariant *commit)
{
  g_autoptr(FlatpakJsonWriter) writer = flatpak_json_writer_new ();
  g_autofree char *config_digest = g_strdup_printf ("sha256:%s", config_sha256);
  g_autofree char *layer_digest = g_strdup_printf ("sha256:%s", layer_sha256);

  flatpak_json_writer_add_uint64_property (writer, "schemaVersion", 2);
  flatpak_json_writer_add_string_property (writer, "mediaType", "application/vnd.oci.image.manifest.v1+json");
  flatpak_json_writer_add_struct_property (writer, "config");
  {
    flatpak_json_writer_add_string_property (writer, "mediaType", "application/vnd.oci.image.config.v1+json");
    flatpak_json_writer_add_uint64_property (writer, "size", config_size);
    flatpak_json_writer_add_string_property (writer, "digest", config_digest);
    flatpak_json_writer_close (writer);
  }

  flatpak_json_writer_add_array_property (writer, "layers");
  {
    flatpak_json_writer_add_array_struct (writer);
    {
      flatpak_json_writer_add_string_property (writer, "mediaType", "application/vnd.oci.image.layer.v1.tar+gzip");
      flatpak_json_writer_add_uint64_property (writer, "size", layer_size);
      flatpak_json_writer_add_string_property (writer, "digest", layer_digest);
      flatpak_json_writer_close (writer);
    }
    flatpak_json_writer_close (writer);
  }

  flatpak_json_writer_add_struct_property (writer, "annotations");
  {
    g_autofree char *parent = NULL;
    g_autofree char *subject = NULL;
    g_autofree char *body = NULL;
    g_autoptr(GVariant) metadata = NULL;
    g_autofree char *metadata_base64 = NULL;

    flatpak_json_writer_add_string_property (writer, "org.flatpak.Ref", ref);

    parent = ostree_commit_get_parent (commit);
    flatpak_json_writer_add_string_property (writer, "org.flatpak.ParentCommit", parent);

    flatpak_json_writer_add_string_property (writer, "org.flatpak.Commit", checksum);

    metadata = g_variant_get_child_value (commit, 0);
    if (g_variant_get_size (metadata) > 0)
      {
        metadata_base64 = g_base64_encode (g_variant_get_data (metadata), g_variant_get_size (metadata));
        flatpak_json_writer_add_string_property (writer, "org.flatpak.Metadata", metadata_base64);
      }

    g_variant_get_child (commit, 3, "s", &subject);
    flatpak_json_writer_add_string_property (writer, "org.flatpak.Subject", subject);

    g_variant_get_child (commit, 4, "s", &body);
    flatpak_json_writer_add_string_property (writer, "org.flatpak.Body", body);

    flatpak_json_writer_close (writer);
  }

  return flatpak_json_writer_get_result (writer);
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
  g_autoptr(GFile) refs_tag = NULL;
  g_autoptr(GFile) refs = NULL;
  g_autoptr(GVariant) commit_data = NULL;
  g_autoptr(GVariant) commit_metadata = NULL;
  g_autofree char *commit_checksum = NULL;
  g_auto(GStrv) ref_parts = g_strsplit (ref, "/", -1);
  glnx_fd_close int dfd = -1;
  g_autofree char *layer_compressed_sha256 = NULL;
  g_autofree char *layer_uncompressed_sha256 = NULL;
  guint64 layer_compressed_size;
  guint64 layer_uncompressed_size;
  g_autoptr(GBytes) config = NULL;
  g_autofree char *config_sha256 = NULL;
  g_autoptr(GBytes) manifest = NULL;
  g_autoptr(FlatpakOciDir) oci_dir = NULL;
  g_autoptr(FlatpakOciLayerWriter) layer_writer = NULL;
  g_autofree char *manifest_sha256 = NULL;
  struct archive *archive;

  if (!ostree_repo_resolve_rev (repo, ref, FALSE, &commit_checksum, error))
    return FALSE;

  if (!ostree_repo_read_commit (repo, commit_checksum, &root, NULL, NULL, error))
    return FALSE;

  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit_checksum, &commit_data, error))
    return FALSE;

  if (!ostree_repo_read_commit_detached_metadata (repo, commit_checksum, &commit_metadata, cancellable, error))
    return FALSE;

  oci_dir = flatpak_oci_dir_new ();

  if (!flatpak_oci_dir_ensure (oci_dir, dir, cancellable, error))
    return FALSE;

  layer_writer = flatpak_oci_layer_writer_new (oci_dir);

  archive = flatpak_oci_layer_writer_open (layer_writer, cancellable, error);
  if (archive == NULL)
    return FALSE;

  if (!export_commit_to_archive (repo, root, ostree_commit_get_timestamp (commit_data),
                                 archive, cancellable, error))
    return FALSE;

  if (!flatpak_oci_layer_writer_close (layer_writer,
                                       &layer_uncompressed_sha256,
                                       &layer_uncompressed_size,
                                       &layer_compressed_sha256,
                                       &layer_compressed_size,
                                       cancellable, error))
    return FALSE;

  config = generate_config_json (ostree_commit_get_timestamp (commit_data),
                                 layer_uncompressed_sha256,
                                 ref_parts[2]);
  config_sha256 = flatpak_oci_dir_write_blob (oci_dir, config, cancellable, error);
  if (config_sha256 == NULL)
    return FALSE;

  manifest = generate_manifest_json (g_bytes_get_size (config), config_sha256,
                                     layer_compressed_size, layer_compressed_sha256,
                                     ref, commit_checksum, commit_data);
  manifest_sha256 = flatpak_oci_dir_write_blob (oci_dir, manifest, cancellable, error);
  if (manifest_sha256 == NULL)
    return FALSE;

  if (!flatpak_oci_dir_set_ref (oci_dir, "latest",
                                g_bytes_get_size (manifest), manifest_sha256,
                                cancellable, error))
    return FALSE;

  g_print ("WARNING: the oci format produced by flatpak is experimental and unstable.\n"
           "Don't use this for anything but experiments for now\n");

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

  file = g_file_new_for_commandline_arg (filename);

  if (!flatpak_is_valid_name (name, &my_error))
    return flatpak_fail (error, _("'%s' is not a valid name: %s"), name, my_error->message);

  if (!flatpak_is_valid_branch (branch, &my_error))
    return flatpak_fail (error, _("'%s' is not a valid branch name: %s"), branch, &my_error);

  if (opt_runtime)
    full_branch = flatpak_build_runtime_ref (name, branch, opt_arch);
  else
    full_branch = flatpak_build_app_ref (name, branch, opt_arch);

  if (!ostree_repo_open (repo, cancellable, error))
    return FALSE;

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
