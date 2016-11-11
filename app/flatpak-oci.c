/*
 * Copyright © 2016 Red Hat, Inc
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

#include <glib/gi18n.h>

#include "flatpak-utils.h"
#include "flatpak-oci.h"

struct FlatpakOciDir
{
  GObject parent;

  int dfd;
};

typedef struct
{
  GObjectClass parent_class;
} FlatpakOciDirClass;

G_DEFINE_TYPE (FlatpakOciDir, flatpak_oci_dir, G_TYPE_OBJECT)

GLNX_DEFINE_CLEANUP_FUNCTION (void *, flatpak_local_free_write_archive, archive_write_free)
#define free_write_archive __attribute__((cleanup (flatpak_local_free_write_archive)))

GLNX_DEFINE_CLEANUP_FUNCTION (void *, flatpak_local_free_read_archive, archive_read_free)
#define free_read_archive __attribute__((cleanup (flatpak_local_free_read_archive)))

static gboolean
propagate_libarchive_error (GError        **error,
                            struct archive *a)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "%s", archive_error_string (a));
  return FALSE;
}

const char *
flatpak_arch_to_oci_arch (const char *flatpak_arch)
{
  if (strcmp (flatpak_arch, "x86_64") == 0)
    return "amd64";
  if (strcmp (flatpak_arch, "aarch64") == 0)
    return "arm64";
  if (strcmp (flatpak_arch, "i386") == 0)
    return "386";
  return flatpak_arch;
}

static int
open_file (int dfd, const char *subpath, GCancellable *cancellable, GError **error)
{
  glnx_fd_close int fd = -1;

  do
    fd = openat (dfd, subpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
  while (G_UNLIKELY (fd == -1 && errno == EINTR));
  if (fd == -1)
    {
      glnx_set_error_from_errno (error);
      return -1;
    }

  return glnx_steal_fd (&fd);
}


static GBytes *
load_file (int dfd, const char *subpath, GCancellable *cancellable, GError **error)
{
  glnx_fd_close int fd = -1;

  fd = open_file (dfd, subpath, cancellable, error);
  if (fd == -1)
    return NULL;

  return glnx_fd_readall_bytes (fd, cancellable, error);
}

static JsonObject *
load_json (int dfd, const char *subpath, GCancellable *cancellable, GError **error)
{
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(JsonParser) parser = NULL;
  JsonNode *root = NULL;

  bytes = load_file (dfd, subpath, cancellable, error);
  if (bytes == NULL)
    return NULL;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser,
                                   g_bytes_get_data (bytes, NULL),
                                   g_bytes_get_size (bytes),
                                   error))
    return NULL;

  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      flatpak_fail (error, _("Invalid json, no root object"));
      return NULL;
    }

  return json_node_dup_object (root);
}

static void
flatpak_oci_dir_finalize (GObject *object)
{
  FlatpakOciDir *self = FLATPAK_OCI_DIR (object);

  if (self->dfd != -1)
    close (self->dfd);

  G_OBJECT_CLASS (flatpak_oci_dir_parent_class)->finalize (object);
}

static void
flatpak_oci_dir_class_init (FlatpakOciDirClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = flatpak_oci_dir_finalize;

}

static void
flatpak_oci_dir_init (FlatpakOciDir *self)
{
}

FlatpakOciDir *
flatpak_oci_dir_new (void)
{
  FlatpakOciDir *oci_dir;

  oci_dir = g_object_new (FLATPAK_TYPE_OCI_DIR, NULL);

  return oci_dir;
}

static gboolean
verify_oci_version (JsonObject *oci_layout, GError **error)
{
  const char *version;

  version = json_object_get_string_member (oci_layout, "imageLayoutVersion");
  if (version == NULL)
    return flatpak_fail (error, _("Unsupported oci repo: oci-layout version missing"));
  if (strcmp (version, "1.0.0") != 0)
    return flatpak_fail (error, _("Unsupported existing oci-layout version %s (only 1.0.0 supported)"), version);

  return TRUE;
}

gboolean
flatpak_oci_dir_open (FlatpakOciDir *self,
                       GFile *dir,
                       GCancellable *cancellable,
                       GError **error)
{
  glnx_fd_close int dfd = -1;
  g_autoptr(JsonObject) oci_layout = NULL;
  g_autoptr(GError) local_error = NULL;

  if (!glnx_opendirat (AT_FDCWD,
                       flatpak_file_get_path_cached (dir),
                       TRUE, &dfd, error))
    return FALSE;

  oci_layout = load_json (dfd, "oci-layout", cancellable, &local_error);
  if (oci_layout == NULL)
    {
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }

      return flatpak_fail (error, _("Unsupported oci repo: oci-layout missing"));
    }

  if (!verify_oci_version (oci_layout, error))
    return FALSE;

  self->dfd = glnx_steal_fd (&dfd);
  return TRUE;
}

gboolean
flatpak_oci_dir_ensure (FlatpakOciDir *self,
                         GFile *dir,
                         GCancellable *cancellable,
                         GError **error)
{
  glnx_fd_close int dfd = -1;
  g_autoptr(GFile) dir_blobs = NULL;
  g_autoptr(GFile) dir_blobs_sha256 = NULL;
  g_autoptr(GFile) dir_refs = NULL;
  g_autoptr(JsonObject) oci_layout = NULL;
  g_autoptr(GError) local_error = NULL;

  dir_blobs = g_file_get_child (dir, "blobs");
  dir_blobs_sha256 = g_file_get_child (dir_blobs, "sha256");
  dir_refs = g_file_get_child (dir, "refs");

  if (!flatpak_mkdir_p (dir_blobs_sha256, cancellable, error))
    return FALSE;

  if (!flatpak_mkdir_p (dir_refs, cancellable, error))
    return FALSE;

  if (!glnx_opendirat (AT_FDCWD,
                       flatpak_file_get_path_cached (dir),
                       TRUE, &dfd, error))
    return FALSE;

  oci_layout = load_json (dfd, "oci-layout", cancellable, &local_error);
  if (oci_layout == NULL)
    {
      const char *new_layout_data = "{\"imageLayoutVersion\": \"1.0.0\"}";

      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }

      if (!glnx_file_replace_contents_at (dfd, "oci-layout",
                                          (const guchar *)new_layout_data,
                                          strlen (new_layout_data),
                                          0,
                                          cancellable, error))
        return FALSE;
    }
  else
    {
      if (!verify_oci_version (oci_layout, error))
        return FALSE;
    }

  self->dfd = glnx_steal_fd (&dfd);
  return TRUE;
}

char *
flatpak_oci_dir_write_blob (FlatpakOciDir  *self,
                            GBytes          *data,
                            GCancellable   *cancellable,
                            GError        **error)
{
  g_autofree char *sha256 = g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, data);
  g_autofree char *path = g_strdup_printf ("blobs/sha256/%s", sha256);

  if (!glnx_file_replace_contents_at (self->dfd, path,
                                      g_bytes_get_data (data, NULL),
                                      g_bytes_get_size (data),
                                      0, cancellable, error))
    return NULL;

  return g_steal_pointer (&sha256);
}

static GBytes *
generate_ref_json (guint64 manifest_size,
                   const char *manifest_sha256)
{
  g_autoptr(FlatpakJsonWriter) writer = flatpak_json_writer_new ();
  g_autofree char *manifest_digest = g_strdup_printf ("sha256:%s", manifest_sha256);

  flatpak_json_writer_add_uint64_property (writer, "size", manifest_size);
  flatpak_json_writer_add_string_property (writer, "digest", manifest_digest);
  flatpak_json_writer_add_string_property (writer, "mediaType", "application/vnd.oci.image.manifest.v1+json");

  return flatpak_json_writer_get_result (writer);
}


gboolean
flatpak_oci_dir_set_ref (FlatpakOciDir  *self,
                         const char *ref,
                         guint64 object_size,
                         const char *object_sha256,
                         GCancellable   *cancellable,
                         GError        **error)
{
  g_autofree char *path = g_strdup_printf ("refs/%s", ref);
  g_autoptr(GBytes) data = NULL;

  data = generate_ref_json (object_size, object_sha256);
  if (!glnx_file_replace_contents_at (self->dfd, path,
                                      g_bytes_get_data (data, NULL),
                                      g_bytes_get_size (data),
                                      0, cancellable, error))
    return FALSE;

  return TRUE;
}

GBytes *
flatpak_oci_dir_load_object (FlatpakOciDir  *self,
                             const char     *digest,
                             GCancellable   *cancellable,
                             GError        **error)
{
  g_autofree char *path = NULL;

  if (!g_str_has_prefix (digest, "sha256:"))
    {
      flatpak_fail (error, "Unsupported digest type %s", digest);
      return NULL;
    }

  path = g_strdup_printf ("blobs/sha256/%s", digest + strlen ("sha256:"));
  return load_file (self->dfd, path, cancellable, error);
}

JsonObject *
flatpak_oci_dir_load_json (FlatpakOciDir  *self,
                           const char     *digest,
                           GCancellable   *cancellable,
                           GError        **error)
{
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(JsonParser) parser = NULL;
  JsonNode *root = NULL;

  bytes = flatpak_oci_dir_load_object (self, digest, cancellable, error);
  if (bytes == NULL)
    return NULL;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser,
                                   g_bytes_get_data (bytes, NULL),
                                   g_bytes_get_size (bytes),
                                   error))
    return NULL;

  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      flatpak_fail (error, _("Invalid json, no root object"));
      return NULL;
    }

  return json_node_dup_object (root);
}

struct archive *
flatpak_oci_dir_load_layer (FlatpakOciDir  *self,
                            const char     *digest,
                            GCancellable   *cancellable,
                            GError        **error)
{
  g_autofree char *path = NULL;
  glnx_fd_close int fd = -1;
  free_read_archive struct archive *a = NULL;

  if (!g_str_has_prefix (digest, "sha256:"))
    {
      flatpak_fail (error, "Unsupported digest type %s", digest);
      return NULL;
    }

  path = g_strdup_printf ("blobs/sha256/%s", digest + strlen ("sha256:"));

  fd = open_file (self->dfd, path, cancellable, error);
  if (fd == -1)
    return NULL;

  a = archive_read_new ();
#ifdef HAVE_ARCHIVE_READ_SUPPORT_FILTER_ALL
  archive_read_support_filter_all (a);
#else
  archive_read_support_compression_all (a);
#endif
  archive_read_support_format_all (a);
  if (archive_read_open_fd (a, fd, 8192) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      return NULL;
    }

  fd = -1;
  return (struct archive *)g_steal_pointer (&a);
}


gboolean
flatpak_oci_dir_load_ref (FlatpakOciDir  *self,
                          const char     *ref,
                          guint64        *size_out,
                          char          **digest_out,
                          char          **mediatype_out,
                          GCancellable   *cancellable,
                          GError        **error)
{
  g_autoptr(JsonObject) ref_root = NULL;
  g_autofree char *path = g_strdup_printf ("refs/%s", ref);
  const char *mediatype, *digest;
  double size;

  ref_root = load_json (self->dfd, path, cancellable, error);
  if (ref_root == NULL)
    return FALSE;

  if (!json_object_has_member (ref_root, "mediaType") ||
      !json_object_has_member (ref_root, "digest") ||
      !json_object_has_member (ref_root, "size"))
    return flatpak_fail (error, _("Invalid ref format"));

  mediatype = json_object_get_string_member (ref_root, "mediaType");
  digest = json_object_get_string_member (ref_root, "digest");
  size = json_object_get_double_member (ref_root, "size");

  if (mediatype == NULL)
    return flatpak_fail (error, _("Invalid ref, no media type"));
  if (digest == NULL)
    return flatpak_fail (error, _("Invalid ref, no digest"));

  if (size_out)
    *size_out = (guint64)size;
  if (digest_out)
    *digest_out = g_strdup (digest);
  if (mediatype_out)
    *mediatype_out = g_strdup (mediatype);
  return TRUE;
}

JsonObject *
find_manifest (FlatpakOciDir  *self,
               const char     *digest,
               const char     *os,
               const char     *arch,
               GCancellable   *cancellable,
               GError        **error)
{
  g_autoptr(JsonObject) manifest = NULL;
  const char *mediatype;
  int version;

  manifest = flatpak_oci_dir_load_json (self, digest, cancellable, error);
  if (manifest == NULL)
    return NULL;

  mediatype = json_object_get_string_member (manifest, "mediaType");
  if (strcmp (mediatype, "application/vnd.oci.image.manifest.v1+json") != 0)
    {
      flatpak_fail (error, _("Unexpected media type %s, expected application/vnd.oci.image.manifest.v1+json"), mediatype);
      return NULL;
    }

  version = json_object_get_int_member (manifest, "schemaVersion");
  if (version != 2)
    {
      flatpak_fail (error, _("Unsupported manifest version %d"), version);
      return NULL;
    }

  return g_steal_pointer (&manifest);
}

JsonObject *
find_manifest_list (FlatpakOciDir  *self,
                    const char     *digest,
                    const char     *os,
                    const char     *arch,
                    GCancellable   *cancellable,
                    GError        **error)
{
  g_autoptr(JsonObject) list = NULL;
  g_autoptr(JsonArray) manifests = NULL;
  int version;
  const char *mediatype;
  guint i, n_elements;

  list = flatpak_oci_dir_load_json (self, digest, cancellable, error);
  if (list == NULL)
    return NULL;

  mediatype = json_object_get_string_member (list, "mediaType");
  if (strcmp (mediatype, "application/vnd.oci.image.manifest.list.v1+json") != 0)
    {
      flatpak_fail (error, _("Unexpected media type %s, expected application/vnd.oci.image.manifest.list.v1+json"), mediatype);
      return NULL;
    }

  version = json_object_get_int_member (list, "schemaVersion");
  if (version != 2)
    {
      flatpak_fail (error, _("Unsupported manifest list version %d"), version);
      return NULL;
    }

  manifests = json_object_get_array_member (list, "manifests");
  if (manifests == NULL)
    {
      flatpak_fail (error, _("Missing element 'manifests'"), version);
      return NULL;
    }

  n_elements = json_array_get_length (manifests);
  for (i = 0; i < n_elements; i++)
    {
      JsonObject *manifest = json_array_get_object_element (manifests, i);
      JsonObject *platform;
      const char *element_arch, *element_os, *mediatype, *element_digest;

      if (manifest == NULL)
        continue;

      mediatype = json_object_get_string_member (list, "mediaType");
      if (strcmp (mediatype, "application/vnd.oci.image.manifest.v1+json") != 0)
        continue;

      element_digest = json_object_get_string_member (list, "digest");
      if (element_digest == NULL)
        continue;

      platform = json_object_get_object_member (list, "platform");
      if (platform == NULL)
        continue;

      element_arch = json_object_get_string_member (list, "architecture");
      element_os = json_object_get_string_member (list, "mediaType");
      if (g_strcmp0 (arch, element_arch) == 0 &&
          g_strcmp0 (os, element_os) == 0)
        {
          return find_manifest (self, element_digest, os, arch, cancellable, error);
        }
    }

  flatpak_fail (error, _("No manfest found for arch %s, os %s"), arch, os);
  return NULL;
}

JsonObject *
flatpak_oci_dir_find_manifest (FlatpakOciDir  *self,
                               const char     *ref,
                               const char     *os,
                               const char     *arch,
                               GCancellable   *cancellable,
                               GError        **error)
{
  g_autofree char *digest = NULL;
  g_autofree char *mediatype = NULL;

  if (!flatpak_oci_dir_load_ref (self, ref, NULL, &digest, &mediatype,
                                 cancellable, error))
    return NULL;

  if (strcmp (mediatype, "application/vnd.oci.image.manifest.list.v1+json") == 0)
    return find_manifest_list (self, digest, os, arch, cancellable, error);
  else if (strcmp (mediatype, "application/vnd.oci.image.manifest.v1+json") == 0)
    return find_manifest (self, digest, os, arch, cancellable, error);
  else
    {
      flatpak_fail (error, _("Unsupported OCI media type %s"), mediatype);
      return NULL;
    }
}

char *
flatpak_oci_manifest_get_config (JsonObject *manifest)
{
  JsonObject *config = NULL;
  const char *mediatype, *digest;

  config = json_object_get_object_member (manifest, "config");

  mediatype = json_object_get_string_member (config, "mediaType");
  if (mediatype == NULL)
    return NULL;

  if (strcmp (mediatype, "application/vnd.oci.image.config.v1+json") != 0)
    return NULL;

  digest = json_object_get_string_member (config, "digest");

  return g_strdup (digest);
}

char **
flatpak_oci_manifest_get_layers (JsonObject *manifest)
{
  JsonArray *layers = NULL;
  guint i, n_elements;
  g_autoptr(GPtrArray) res = g_ptr_array_new_with_free_func (g_free);

  layers = json_object_get_array_member (manifest, "layers");

  n_elements = json_array_get_length (layers);
  for (i = 0; i < n_elements; i++)
    {
      JsonObject *layer = json_array_get_object_element (layers, i);
      const char *digest, *mediatype;

      mediatype = json_object_get_string_member (layer, "mediaType");
      if (strcmp (mediatype, "application/vnd.oci.image.layer.v1.tar+gzip") != 0)
        continue;

      digest = json_object_get_string_member (layer, "digest");
      if (digest == NULL)
        continue;

      g_ptr_array_add (res, g_strdup (digest));
    }

  g_ptr_array_add (res, NULL);

  return (char **)g_ptr_array_free (g_steal_pointer (&res), FALSE);
}

GHashTable *
flatpak_oci_manifest_get_annotations (JsonObject *manifest)
{
  JsonObject *annotations = NULL;
  g_autoptr(GHashTable) res = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  GList *members, *l;

  annotations = json_object_get_object_member (manifest, "annotations");

  members = json_object_get_members (annotations);
  for (l = members; l != NULL; l = l->next)
    {
      const char *member = l->data;
      const char *value;

      value = json_object_get_string_member (annotations, member);
      g_hash_table_insert (res, g_strdup (member), g_strdup (value));
    }
  g_list_free (members);

  return g_steal_pointer (&res);
}

guint64
flatpak_oci_config_get_created (JsonObject *config)
{
  const char *created;
  GTimeVal tv;

  created = json_object_get_string_member (config, "created");
  if (created == NULL)
    return 0;

  if (g_time_val_from_iso8601 (created, &tv))
    return tv.tv_sec;
  else
    return 0;
}

/*************************************************************************/

struct FlatpakOciLayerWriter
{
  GObject parent;

  FlatpakOciDir *dir;

  GChecksum *uncompressed_checksum;
  GChecksum *compressed_checksum;
  struct archive *archive;
  GZlibCompressor *compressor;
  guint64 uncompressed_size;
  guint64 compressed_size;
  char *tmp_path;
  int tmp_fd;
};

typedef struct
{
  GObjectClass parent_class;
} FlatpakOciLayerWriterClass;

G_DEFINE_TYPE (FlatpakOciLayerWriter, flatpak_oci_layer_writer, G_TYPE_OBJECT)

static void
flatpak_oci_layer_writer_reset (FlatpakOciLayerWriter *self)
{
  if (self->tmp_path)
    {
      (void) unlinkat (self->dir->dfd, self->tmp_path, 0);
      g_free (self->tmp_path);
      self->tmp_path = NULL;
    }

  if (self->tmp_fd != -1)
    {
      close (self->tmp_fd);
      self->tmp_fd = -1;
    }

  g_clear_object (&self->compressor);

  g_checksum_reset (self->uncompressed_checksum);
  g_checksum_reset (self->compressed_checksum);

  if (self->archive)
    {
      archive_write_free (self->archive);
      self->archive = NULL;
    }
}


static void
flatpak_oci_layer_writer_finalize (GObject *object)
{
  FlatpakOciLayerWriter *self = FLATPAK_OCI_LAYER_WRITER (object);

  flatpak_oci_layer_writer_reset (self);

  g_checksum_free (self->compressed_checksum);
  g_checksum_free (self->uncompressed_checksum);

  g_clear_object (&self->dir);

  G_OBJECT_CLASS (flatpak_oci_layer_writer_parent_class)->finalize (object);
}

static void
flatpak_oci_layer_writer_class_init (FlatpakOciLayerWriterClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = flatpak_oci_layer_writer_finalize;

}

static void
flatpak_oci_layer_writer_init (FlatpakOciLayerWriter *self)
{
  self->uncompressed_checksum = g_checksum_new (G_CHECKSUM_SHA256);
  self->compressed_checksum = g_checksum_new (G_CHECKSUM_SHA256);
}

FlatpakOciLayerWriter *
flatpak_oci_layer_writer_new (FlatpakOciDir *dir)
{
  FlatpakOciLayerWriter *oci_layer_writer;

  oci_layer_writer = g_object_new (FLATPAK_TYPE_OCI_LAYER_WRITER, NULL);
  oci_layer_writer->dir = g_object_ref (dir);

  return oci_layer_writer;
}

static int
flatpak_oci_layer_writer_open_cb (struct archive *archive,
                                  void *client_data)
{
  return ARCHIVE_OK;
}

static gssize
flatpak_oci_layer_writer_compress (FlatpakOciLayerWriter *self,
                                   const void *buffer,
                                   size_t length,
                                   gboolean at_end)
{
  guchar compressed_buffer[8192];
  GConverterResult res;
  gsize total_bytes_read, bytes_read, bytes_written, to_write_len;
  guchar *to_write;
  g_autoptr(GError) local_error = NULL;
  GConverterFlags flags = 0;
  bytes_read = 0;

  total_bytes_read = 0;

  if (at_end)
    flags |= G_CONVERTER_INPUT_AT_END;

  do
    {
      res = g_converter_convert (G_CONVERTER (self->compressor),
                                 buffer, length,
                                 compressed_buffer, sizeof (compressed_buffer),
                                 flags, &bytes_read, &bytes_written,
                                 &local_error);
      if (res == G_CONVERTER_ERROR)
        {
          archive_set_error (self->archive, EIO, "%s", local_error->message);
          return -1;
        }

      g_checksum_update (self->uncompressed_checksum, buffer, bytes_read);
      g_checksum_update (self->compressed_checksum, compressed_buffer, bytes_written);
      self->uncompressed_size += bytes_read;
      self->compressed_size += bytes_written;

      to_write_len = bytes_written;
      to_write = compressed_buffer;
      while (to_write_len > 0)
        {
          ssize_t res = write (self->tmp_fd, to_write, to_write_len);
          if (res <= 0)
            {
              if (errno == EINTR)
                continue;
              archive_set_error (self->archive, errno, "Write error");
              return -1;
            }

          to_write_len -= res;
          to_write += res;
        }

      total_bytes_read += bytes_read;
    }
  while ((length > 0 && bytes_read == 0) || /* Repeat if we consumed nothing */
         (at_end && res != G_CONVERTER_FINISHED)); /* Or until finished if at_end */

  return total_bytes_read;
}

static ssize_t
flatpak_oci_layer_writer_write_cb (struct archive *archive,
                                   void *client_data,
                                   const void *buffer,
                                   size_t length)
{
  FlatpakOciLayerWriter *self = FLATPAK_OCI_LAYER_WRITER (client_data);

  return flatpak_oci_layer_writer_compress (self, buffer, length, FALSE);
}

static int
flatpak_oci_layer_writer_close_cb (struct archive *archive,
                                   void *client_data)
{
  FlatpakOciLayerWriter *self = FLATPAK_OCI_LAYER_WRITER (client_data);
  gssize res;
  char buffer[1] = {0};

  res = flatpak_oci_layer_writer_compress (self, &buffer, 0, TRUE);
  if (res < 0)
    return ARCHIVE_FATAL;

  return ARCHIVE_OK;
}

struct archive *
flatpak_oci_layer_writer_open (FlatpakOciLayerWriter *self,
                               GCancellable *cancellable,
                               GError **error)
{
  free_write_archive struct archive *a = NULL;
  glnx_fd_close int tmp_fd = -1;
  g_autofree char *tmp_path = NULL;

  if (!glnx_open_tmpfile_linkable_at (self->dir->dfd,
                                      "blobs/sha256",
                                      O_WRONLY,
                                      &tmp_fd,
                                      &tmp_path,
                                      error))
    return NULL;

  a = archive_write_new ();
  if (archive_write_set_format_gnutar (a) != ARCHIVE_OK ||
      archive_write_add_filter_none (a) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      return NULL;
    }

  if (archive_write_open (a, self,
                          flatpak_oci_layer_writer_open_cb,
                          flatpak_oci_layer_writer_write_cb,
                          flatpak_oci_layer_writer_close_cb) != ARCHIVE_OK)
    {
      propagate_libarchive_error (error, a);
      return NULL;
    }

  flatpak_oci_layer_writer_reset (self);

  self->archive = g_steal_pointer (&a);
  self->tmp_fd = glnx_steal_fd (&tmp_fd);
  self->tmp_path = g_steal_pointer (&tmp_path);
  self->compressor = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1);

  return self->archive;
}

gboolean
flatpak_oci_layer_writer_close (FlatpakOciLayerWriter  *self,
                                char                  **uncompressed_sha256_out,
                                guint64                *uncompressed_size_out,
                                char                  **compressed_sha256_out,
                                guint64                *compressed_size_out,
                                GCancellable           *cancellable,
                                GError                **error)
{
  g_autofree char *path = NULL;

  if (archive_write_close (self->archive) != ARCHIVE_OK)
    return propagate_libarchive_error (error, self->archive);

  path = g_strdup_printf ("blobs/sha256/%s",
                          g_checksum_get_string (self->compressed_checksum));

  if (!glnx_link_tmpfile_at (self->dir->dfd,
                             GLNX_LINK_TMPFILE_REPLACE,
                             self->tmp_fd,
                             self->tmp_path,
                             self->dir->dfd,
                             path,
                             error))
    return FALSE;

  close (self->tmp_fd);
  self->tmp_fd = -1;
  g_free (self->tmp_path);
  self->tmp_path = NULL;

  if (uncompressed_sha256_out != NULL)
    *uncompressed_sha256_out = g_strdup (g_checksum_get_string (self->uncompressed_checksum));
  if (uncompressed_size_out != NULL)
    *uncompressed_size_out = self->uncompressed_size;
  if (compressed_sha256_out != NULL)
    *compressed_sha256_out = g_strdup (g_checksum_get_string (self->compressed_checksum));
  if (compressed_size_out != NULL)
    *compressed_size_out = self->compressed_size;

  return TRUE;
}

/*************************************************************************/

typedef struct JsonScope JsonScope;

struct JsonScope {
  JsonScope *parent;
  int index;
  char end_char;
};

struct FlatpakJsonWriter
{
  GString *str;
  int depth;
  JsonScope *scope;
};

FlatpakJsonWriter *
flatpak_json_writer_new ()
{
  FlatpakJsonWriter *self = g_new0 (FlatpakJsonWriter, 1);
  self->str = g_string_new ("");

  flatpak_json_writer_open_struct (self);

  return self;
}

GBytes *
flatpak_json_writer_get_result (FlatpakJsonWriter *self)
{
  GBytes *res = NULL;

  if (self->str)
    {
      flatpak_json_writer_close (self);
      res = g_string_free_to_bytes (self->str);
      self->str = NULL;
    }

  return res;
}

void
flatpak_json_writer_free (FlatpakJsonWriter *self)
{
  if (self->str)
    g_string_free (self->str, TRUE);
  g_free (self);
}

static void
flatpak_json_writer_indent (FlatpakJsonWriter *self)
{
  int i;

  for (i = 0; i < self->depth; i++)
    g_string_append (self->str, "    ");
}

static void
flatpak_json_writer_add_bool (FlatpakJsonWriter *self, gboolean val)
{
  if (val)
    g_string_append (self->str, "true");
  else
    g_string_append (self->str, "false");
}

static void
flatpak_json_writer_add_uint64 (FlatpakJsonWriter *self, guint64 val)
{
  g_string_append_printf (self->str, "%"G_GUINT64_FORMAT, val);
}


static void
flatpak_json_writer_add_string (FlatpakJsonWriter *self, const gchar *str)
{
  const gchar *p;

  g_string_append_c (self->str, '"');

  for (p = str; *p != 0; p++)
    {
      if (*p == '\\' || *p == '"')
        {
          g_string_append_c (self->str, '\\');
          g_string_append_c (self->str, *p);
        }
      else if ((*p > 0 && *p < 0x1f) || *p == 0x7f)
        {
          switch (*p)
            {
            case '\b':
              g_string_append (self->str, "\\b");
              break;

            case '\f':
              g_string_append (self->str, "\\f");
              break;

            case '\n':
              g_string_append (self->str, "\\n");
              break;

            case '\r':
              g_string_append (self->str, "\\r");
              break;

            case '\t':
              g_string_append (self->str, "\\t");
              break;

            default:
              g_string_append_printf (self->str, "\\u00%02x", (guint) * p);
              break;
            }
        }
      else
        {
          g_string_append_c (self->str, *p);
        }
    }

  g_string_append_c (self->str, '"');
}

static void
flatpak_json_writer_start_item (FlatpakJsonWriter *self)
{
  int index = self->scope->index;

  if (index != 0)
    g_string_append (self->str, ",\n");
  else
    g_string_append (self->str, "\n");
  flatpak_json_writer_indent (self);
  self->scope->index = index + 1;
}

static void
flatpak_json_writer_open_scope (FlatpakJsonWriter *self,
                                char start_char,
                                char end_char)
{
  JsonScope *scope = g_new0 (JsonScope, 1);

  scope->parent = self->scope;
  scope->end_char = end_char;

  self->scope = scope;
  self->depth += 1;

  g_string_append_c (self->str, start_char);
}

void
flatpak_json_writer_close (FlatpakJsonWriter *self)
{
  JsonScope *scope;

  scope = self->scope;
  self->scope = scope->parent;
  self->depth -= 1;

  g_string_append (self->str, "\n");
  flatpak_json_writer_indent (self);
  g_string_append_c (self->str, scope->end_char);

  g_free (scope);

  /* Last newline in file */
  if (self->scope == NULL)
    g_string_append (self->str, "\n");
}

void
flatpak_json_writer_open_struct (FlatpakJsonWriter *self)
{
  flatpak_json_writer_open_scope (self, '{', '}');
}

void
flatpak_json_writer_open_array (FlatpakJsonWriter *self)
{
  flatpak_json_writer_open_scope (self, '[', ']');
}

static void
flatpak_json_writer_add_property (FlatpakJsonWriter *self, const gchar *name)
{
  flatpak_json_writer_start_item (self);
  flatpak_json_writer_add_string (self, name);
  g_string_append (self->str, ": ");
}

void
flatpak_json_writer_add_struct_property (FlatpakJsonWriter *self, const gchar *name)
{
  flatpak_json_writer_add_property (self, name);
  flatpak_json_writer_open_struct (self);
}

void
flatpak_json_writer_add_array_property (FlatpakJsonWriter *self, const gchar *name)
{
  flatpak_json_writer_add_property (self, name);
  flatpak_json_writer_open_array (self);
}

void
flatpak_json_writer_add_string_property (FlatpakJsonWriter *self, const gchar *name, const char *value)
{
  flatpak_json_writer_add_property (self, name);
  flatpak_json_writer_add_string (self, value);
}

void
flatpak_json_writer_add_uint64_property (FlatpakJsonWriter *self, const gchar *name, guint64 value)
{
  flatpak_json_writer_add_property (self, name);
  flatpak_json_writer_add_uint64 (self, value);
}

void
flatpak_json_writer_add_bool_property (FlatpakJsonWriter *self, const gchar *name, gboolean value)
{
  flatpak_json_writer_add_property (self, name);
  flatpak_json_writer_add_bool (self, value);
}

void
flatpak_json_writer_add_array_string (FlatpakJsonWriter *self, const gchar *string)
{
  flatpak_json_writer_start_item (self);
  flatpak_json_writer_add_string (self, string);
}

void
flatpak_json_writer_add_array_struct (FlatpakJsonWriter *self)
{
  flatpak_json_writer_start_item (self);
  flatpak_json_writer_open_struct (self);
}
