/*
 * Copyright (C) 2015 Red Hat, Inc
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"
#include "string.h"

#include "flatpak-json-oci.h"
#include "flatpak-utils.h"
#include "libglnx.h"

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

void
flatpak_oci_descriptor_destroy (FlatpakOciDescriptor *self)
{
  g_free (self->mediatype);
  g_free (self->digest);
  g_strfreev (self->urls);
}

void
flatpak_oci_descriptor_free (FlatpakOciDescriptor *self)
{
  flatpak_oci_descriptor_destroy (self);
  g_free (self);
}

static FlatpakJsonProp flatpak_oci_descriptor_props[] = {
  FLATPAK_JSON_STRING_PROP (FlatpakOciDescriptor, mediatype, "mediaType"),
  FLATPAK_JSON_STRING_PROP (FlatpakOciDescriptor, digest, "digest"),
  FLATPAK_JSON_INT64_PROP (FlatpakOciDescriptor, size, "size"),
  FLATPAK_JSON_STRV_PROP (FlatpakOciDescriptor, urls, "urls"),
  FLATPAK_JSON_LAST_PROP
};

static void
flatpak_oci_manifest_platform_destroy (FlatpakOciManifestPlatform *self)
{
  g_free (self->architecture);
  g_free (self->os);
  g_free (self->os_version);
  g_strfreev (self->os_features);
  g_free (self->variant);
  g_strfreev (self->features);
}

void
flatpak_oci_manifest_descriptor_destroy (FlatpakOciManifestDescriptor *self)
{
  flatpak_oci_manifest_platform_destroy (&self->platform);
  flatpak_oci_descriptor_destroy (&self->parent);
}

void
flatpak_oci_manifest_descriptor_free (FlatpakOciManifestDescriptor *self)
{
  flatpak_oci_manifest_descriptor_destroy (self);
  g_free (self);
}

static FlatpakJsonProp flatpak_oci_manifest_platform_props[] = {
  FLATPAK_JSON_STRING_PROP (FlatpakOciManifestPlatform, architecture, "architecture"),
  FLATPAK_JSON_STRING_PROP (FlatpakOciManifestPlatform, os, "os"),
  FLATPAK_JSON_STRING_PROP (FlatpakOciManifestPlatform, os_version, "os.version"),
  FLATPAK_JSON_STRING_PROP (FlatpakOciManifestPlatform, variant, "variant"),
  FLATPAK_JSON_STRV_PROP (FlatpakOciManifestPlatform, os_features, "os.features"),
  FLATPAK_JSON_STRV_PROP (FlatpakOciManifestPlatform, features, "features"),
  FLATPAK_JSON_LAST_PROP
};
static FlatpakJsonProp flatpak_oci_manifest_descriptor_props[] = {
  FLATPAK_JSON_PARENT_PROP (FlatpakOciManifestDescriptor, parent, flatpak_oci_descriptor_props),
  FLATPAK_JSON_STRUCT_PROP (FlatpakOciManifestDescriptor, platform, "platform", flatpak_oci_manifest_platform_props),
  FLATPAK_JSON_LAST_PROP
};

G_DEFINE_TYPE (FlatpakOciRef, flatpak_oci_ref, FLATPAK_TYPE_JSON);

static void
flatpak_oci_ref_finalize (GObject *object)
{
  FlatpakOciRef *self = FLATPAK_OCI_REF (object);

  flatpak_oci_descriptor_destroy (&self->descriptor);

  G_OBJECT_CLASS (flatpak_oci_ref_parent_class)->finalize (object);
}

static void
flatpak_oci_ref_class_init (FlatpakOciRefClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FlatpakJsonClass *json_class = FLATPAK_JSON_CLASS (klass);
  static FlatpakJsonProp props[] = {
    FLATPAK_JSON_PARENT_PROP (FlatpakOciRef, descriptor, flatpak_oci_descriptor_props),
    FLATPAK_JSON_LAST_PROP
  };

  object_class->finalize = flatpak_oci_ref_finalize;
  json_class->props = props;
  json_class->mediatype = FLATPAK_OCI_MEDIA_TYPE_DESCRIPTOR;
}

static void
flatpak_oci_ref_init (FlatpakOciRef *self)
{
}

FlatpakOciRef *
flatpak_oci_ref_new (const char *mediatype,
                    const char *digest,
                    gint64 size)
{
  FlatpakOciRef *ref;

  ref = g_object_new (FLATPAK_TYPE_OCI_REF, NULL);
  ref->descriptor.mediatype = g_strdup (mediatype);
  ref->descriptor.digest = g_strdup (digest);
  ref->descriptor.size = size;

  return ref;
}

const char *
flatpak_oci_ref_get_mediatype (FlatpakOciRef *self)
{
  return self->descriptor.mediatype;
}

const char *
flatpak_oci_ref_get_digest (FlatpakOciRef *self)
{
  return self->descriptor.digest;
}

gint64
flatpak_oci_ref_get_size (FlatpakOciRef *self)
{
  return self->descriptor.size;
}

const char **
flatpak_oci_ref_get_urls (FlatpakOciRef *self)
{
  return (const char **)self->descriptor.urls;
}

void
flatpak_oci_ref_set_urls (FlatpakOciRef *self,
                         const char **urls)
{
  g_strfreev (self->descriptor.urls);
  self->descriptor.urls = g_strdupv ((char **)urls);
}

G_DEFINE_TYPE (FlatpakOciVersioned, flatpak_oci_versioned, FLATPAK_TYPE_JSON);

static void
flatpak_oci_versioned_finalize (GObject *object)
{
  FlatpakOciVersioned *self = FLATPAK_OCI_VERSIONED (object);

  g_free (self->mediatype);

  G_OBJECT_CLASS (flatpak_oci_versioned_parent_class)->finalize (object);
}

static void
flatpak_oci_versioned_class_init (FlatpakOciVersionedClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FlatpakJsonClass *json_class = FLATPAK_JSON_CLASS (klass);
  static FlatpakJsonProp props[] = {
    FLATPAK_JSON_INT64_PROP (FlatpakOciVersioned, version, "schemaVersion"),
    FLATPAK_JSON_STRING_PROP (FlatpakOciVersioned, mediatype, "mediaType"),
    FLATPAK_JSON_LAST_PROP
  };

  object_class->finalize = flatpak_oci_versioned_finalize;
  json_class->props = props;
}

static void
flatpak_oci_versioned_init (FlatpakOciVersioned *self)
{
}

FlatpakOciVersioned *
flatpak_oci_versioned_from_json (GBytes *bytes, GError **error)
{
  g_autoptr(JsonParser) parser = NULL;
  JsonNode *root = NULL;
  const gchar *mediatype;
  JsonObject *object;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser,
                                   g_bytes_get_data (bytes, NULL),
                                   g_bytes_get_size (bytes),
                                   error))
    return NULL;

  root = json_parser_get_root (parser);
  object = json_node_get_object (root);

  mediatype = json_object_get_string_member (object, "mediaType");
  if (mediatype == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Versioned object lacks mediatype");
      return NULL;
    }

  if (strcmp (mediatype, FLATPAK_OCI_MEDIA_TYPE_IMAGE_MANIFEST) == 0)
    return (FlatpakOciVersioned *) flatpak_json_from_node (root, FLATPAK_TYPE_OCI_MANIFEST, error);

  if (strcmp (mediatype, FLATPAK_OCI_MEDIA_TYPE_IMAGE_MANIFESTLIST) == 0)
    return (FlatpakOciVersioned *) flatpak_json_from_node (root, FLATPAK_TYPE_OCI_MANIFEST_LIST, error);

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
               "Unsupported media type %s", mediatype);
  return NULL;
}

const char *
flatpak_oci_versioned_get_mediatype (FlatpakOciVersioned *self)
{
  return self->mediatype;
}

gint64
flatpak_oci_versioned_get_version (FlatpakOciVersioned *self)
{
  return self->version;
}

G_DEFINE_TYPE (FlatpakOciManifest, flatpak_oci_manifest, FLATPAK_TYPE_OCI_VERSIONED);

static void
flatpak_oci_manifest_finalize (GObject *object)
{
  FlatpakOciManifest *self = (FlatpakOciManifest *) object;
  int i;

  for (i = 0; self->layers != NULL && self->layers[i] != NULL; i++)
    flatpak_oci_descriptor_free (self->layers[i]);
  g_free (self->layers);
  flatpak_oci_descriptor_destroy (&self->config);
  if (self->annotations)
    g_hash_table_destroy (self->annotations);

  G_OBJECT_CLASS (flatpak_oci_manifest_parent_class)->finalize (object);
}

static void
flatpak_oci_manifest_class_init (FlatpakOciManifestClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FlatpakJsonClass *json_class = FLATPAK_JSON_CLASS (klass);
  static FlatpakJsonProp props[] = {
    FLATPAK_JSON_STRUCT_PROP(FlatpakOciManifest, config, "config", flatpak_oci_descriptor_props),
    FLATPAK_JSON_STRUCTV_PROP(FlatpakOciManifest, layers, "layers", flatpak_oci_descriptor_props),
    FLATPAK_JSON_STRMAP_PROP(FlatpakOciManifest, annotations, "annotations"),
    FLATPAK_JSON_LAST_PROP
  };

  object_class->finalize = flatpak_oci_manifest_finalize;
  json_class->props = props;
  json_class->mediatype = FLATPAK_OCI_MEDIA_TYPE_IMAGE_MANIFEST;
}

static void
flatpak_oci_manifest_init (FlatpakOciManifest *self)
{
}

FlatpakOciManifest *
flatpak_oci_manifest_new (void)
{
  FlatpakOciManifest *manifest;

  manifest = g_object_new (FLATPAK_TYPE_OCI_MANIFEST, NULL);
  manifest->parent.version = 2;
  manifest->parent.mediatype = g_strdup (FLATPAK_OCI_MEDIA_TYPE_IMAGE_MANIFEST);

  manifest->annotations = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  return manifest;
}

void
flatpak_oci_manifest_set_config (FlatpakOciManifest *self,
                                FlatpakOciRef *ref)
{
  g_free (self->config.mediatype);
  self->config.mediatype = g_strdup (ref->descriptor.mediatype);
  g_free (self->config.digest);
  self->config.digest = g_strdup (ref->descriptor.digest);
  self->config.size = ref->descriptor.size;
}

static int
ptrv_count (gpointer *ptrs)
{
  int count;

  for (count = 0; ptrs != NULL && ptrs[count] != NULL; count++)
    ;

  return count;
}

void
flatpak_oci_manifest_set_layer (FlatpakOciManifest  *self,
                                FlatpakOciRef       *ref)
{
  FlatpakOciRef *refs[2] = { ref, NULL };
  flatpak_oci_manifest_set_layers (self, refs);
}

void
flatpak_oci_manifest_set_layers (FlatpakOciManifest *self,
                                FlatpakOciRef **refs)
{
  int i, count;

  for (i = 0; self->layers != NULL && self->layers[i] != NULL; i++)
    flatpak_oci_descriptor_free (self->layers[i]);
  g_free (self->layers);

  count = ptrv_count ((gpointer *)refs);

  self->layers = g_new0 (FlatpakOciDescriptor *, count + 1);
  for (i = 0; i < count; i++)
    {
      self->layers[i] = g_new0 (FlatpakOciDescriptor, 1);
      self->layers[i]->mediatype = g_strdup (refs[i]->descriptor.mediatype);
      self->layers[i]->digest = g_strdup (refs[i]->descriptor.digest);
      self->layers[i]->size = refs[i]->descriptor.size;
    }
}

int
flatpak_oci_manifest_get_n_layers (FlatpakOciManifest *self)
{
  return ptrv_count ((gpointer *)self->layers);
}

const char *
flatpak_oci_manifest_get_layer_digest (FlatpakOciManifest *self,
                                      int i)
{
  return self->layers[i]->digest;
}

GHashTable *
flatpak_oci_manifest_get_annotations (FlatpakOciManifest *self)
{
  return self->annotations;
}

G_DEFINE_TYPE (FlatpakOciManifestList, flatpak_oci_manifest_list, FLATPAK_TYPE_OCI_VERSIONED);

static void
flatpak_oci_manifest_list_finalize (GObject *object)
{
  FlatpakOciManifestList *self = (FlatpakOciManifestList *) object;
  int i;

  for (i = 0; self->manifests != NULL && self->manifests[i] != NULL; i++)
    flatpak_oci_manifest_descriptor_free (self->manifests[i]);
  g_free (self->manifests);

  if (self->annotations)
    g_hash_table_destroy (self->annotations);

  G_OBJECT_CLASS (flatpak_oci_manifest_list_parent_class)->finalize (object);
}


static void
flatpak_oci_manifest_list_class_init (FlatpakOciManifestListClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FlatpakJsonClass *json_class = FLATPAK_JSON_CLASS (klass);
  static FlatpakJsonProp props[] = {
    FLATPAK_JSON_STRUCTV_PROP(FlatpakOciManifestList, manifests, "manifests", flatpak_oci_manifest_descriptor_props),
    FLATPAK_JSON_STRMAP_PROP(FlatpakOciManifestList, annotations, "annotations"),
    FLATPAK_JSON_LAST_PROP
  };

  object_class->finalize = flatpak_oci_manifest_list_finalize;
  json_class->props = props;
  json_class->mediatype = FLATPAK_OCI_MEDIA_TYPE_IMAGE_MANIFESTLIST;
}

static void
flatpak_oci_manifest_list_init (FlatpakOciManifestList *self)
{
}

G_DEFINE_TYPE (FlatpakOciImage, flatpak_oci_image, FLATPAK_TYPE_JSON);

static void
flatpak_oci_image_rootfs_destroy (FlatpakOciImageRootfs *self)
{
  g_free (self->type);
  g_strfreev (self->diff_ids);
}

static void
flatpak_oci_image_config_destroy (FlatpakOciImageConfig *self)
{
  g_free (self->user);
  g_free (self->working_dir);
  g_strfreev (self->env);
  g_strfreev (self->cmd);
  g_strfreev (self->entrypoint);
  g_strfreev (self->exposed_ports);
  g_strfreev (self->volumes);
  if (self->labels)
    g_hash_table_destroy (self->labels);
}

static void
flatpak_oci_image_history_free (FlatpakOciImageHistory *self)
{
  g_free (self->created);
  g_free (self->created_by);
  g_free (self->author);
  g_free (self->comment);
  g_free (self);
}

static void
flatpak_oci_image_finalize (GObject *object)
{
  FlatpakOciImage *self = (FlatpakOciImage *) object;
  int i;

  g_free (self->created);
  g_free (self->author);
  g_free (self->architecture);
  g_free (self->os);
  flatpak_oci_image_rootfs_destroy (&self->rootfs);
  flatpak_oci_image_config_destroy (&self->config);

  for (i = 0; self->history != NULL && self->history[i] != NULL; i++)
    flatpak_oci_image_history_free (self->history[i]);
  g_free (self->history);

  G_OBJECT_CLASS (flatpak_oci_image_parent_class)->finalize (object);
}

static void
flatpak_oci_image_class_init (FlatpakOciImageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FlatpakJsonClass *json_class = FLATPAK_JSON_CLASS (klass);
  static FlatpakJsonProp config_props[] = {
    FLATPAK_JSON_STRING_PROP (FlatpakOciImageConfig, user, "User"),
    FLATPAK_JSON_INT64_PROP (FlatpakOciImageConfig, memory, "Memory"),
    FLATPAK_JSON_INT64_PROP (FlatpakOciImageConfig, memory_swap, "MemorySwap"),
    FLATPAK_JSON_INT64_PROP (FlatpakOciImageConfig, cpu_shares, "CpuShares"),
    FLATPAK_JSON_BOOLMAP_PROP (FlatpakOciImageConfig, exposed_ports, "ExposedPorts"),
    FLATPAK_JSON_STRV_PROP (FlatpakOciImageConfig, env, "Env"),
    FLATPAK_JSON_STRV_PROP (FlatpakOciImageConfig, entrypoint, "Entrypoint"),
    FLATPAK_JSON_STRV_PROP (FlatpakOciImageConfig, cmd, "Cmd"),
    FLATPAK_JSON_BOOLMAP_PROP (FlatpakOciImageConfig, volumes, "Volumes"),
    FLATPAK_JSON_STRING_PROP (FlatpakOciImageConfig, working_dir, "WorkingDir"),
    FLATPAK_JSON_STRMAP_PROP(FlatpakOciImageConfig, labels, "Labels"),
    FLATPAK_JSON_LAST_PROP
  };
  static FlatpakJsonProp rootfs_props[] = {
    FLATPAK_JSON_STRING_PROP (FlatpakOciImageRootfs, type, "type"),
    FLATPAK_JSON_STRV_PROP (FlatpakOciImageRootfs, diff_ids, "diff_ids"),
    FLATPAK_JSON_LAST_PROP
  };
  static FlatpakJsonProp history_props[] = {
    FLATPAK_JSON_STRING_PROP (FlatpakOciImageHistory, created, "created"),
    FLATPAK_JSON_STRING_PROP (FlatpakOciImageHistory, created_by, "created_by"),
    FLATPAK_JSON_STRING_PROP (FlatpakOciImageHistory, author, "author"),
    FLATPAK_JSON_STRING_PROP (FlatpakOciImageHistory, comment, "comment"),
    FLATPAK_JSON_BOOL_PROP (FlatpakOciImageHistory, empty_layer, "empty_layer"),
    FLATPAK_JSON_LAST_PROP
  };
  static FlatpakJsonProp props[] = {
    FLATPAK_JSON_STRING_PROP (FlatpakOciImage, created, "created"),
    FLATPAK_JSON_STRING_PROP (FlatpakOciImage, author, "author"),
    FLATPAK_JSON_STRING_PROP (FlatpakOciImage, architecture, "architecture"),
    FLATPAK_JSON_STRING_PROP (FlatpakOciImage, os, "os"),
    FLATPAK_JSON_STRUCT_PROP (FlatpakOciImage, config, "config", config_props),
    FLATPAK_JSON_STRUCT_PROP (FlatpakOciImage, rootfs, "rootfs", rootfs_props),
    FLATPAK_JSON_STRUCTV_PROP (FlatpakOciImage, history, "history", history_props),
    FLATPAK_JSON_LAST_PROP
  };

  object_class->finalize = flatpak_oci_image_finalize;
  json_class->props = props;
  json_class->mediatype = FLATPAK_OCI_MEDIA_TYPE_IMAGE_CONFIG;
}

static void
flatpak_oci_image_init (FlatpakOciImage *self)
{
}

FlatpakOciImage *
flatpak_oci_image_new (void)
{
  FlatpakOciImage *image;
  GTimeVal stamp;

  stamp.tv_sec = time (NULL);
  stamp.tv_usec = 0;

  image = g_object_new (FLATPAK_TYPE_OCI_IMAGE, NULL);

  /* Some default values */
  image->created = g_time_val_to_iso8601 (&stamp);
  image->architecture = g_strdup ("arm64");
  image->os = g_strdup ("linux");

  image->rootfs.type = g_strdup ("layers");
  image->rootfs.diff_ids = g_new0 (char *, 1);

  return image;
}

void
flatpak_oci_image_set_created (FlatpakOciImage *image,
                              const char *created)
{
  g_free (image->created);
  image->created = g_strdup (created);
}

void
flatpak_oci_image_set_architecture (FlatpakOciImage *image,
                                   const char *arch)
{
  g_free (image->architecture);
  image->architecture = g_strdup (arch);
}

void
flatpak_oci_image_set_os (FlatpakOciImage *image,
                         const char *os)
{
  g_free (image->os);
  image->os = g_strdup (os);
}

void
flatpak_oci_image_set_layers (FlatpakOciImage *image,
                             const char **layers)
{
  g_strfreev (image->rootfs.diff_ids);
  image->rootfs.diff_ids = g_strdupv ((char **)layers);
}

void
flatpak_oci_image_set_layer (FlatpakOciImage *image,
                             const char *layer)
{
  const char *layers[] = {layer, NULL};

  flatpak_oci_image_set_layers (image, layers);
}

static void
add_annotation (GHashTable *annotations, const char *key, const char *value)
{
    g_hash_table_replace (annotations,
                          g_strdup (key),
                          g_strdup (value));
}

void
flatpak_oci_add_annotations_for_commit (GHashTable *annotations,
                                       const char *ref,
                                       const char *commit,
                                       GVariant *commit_data)
{
  if (ref)
    add_annotation (annotations,"org.flatpak.Ostree.Ref", ref);

  if (commit)
    add_annotation (annotations,"org.flatpak.Ostree.Commit", commit);

  if (commit_data)
    {
      g_autofree char *parent = NULL;
      g_autofree char *subject = NULL;
      g_autofree char *body = NULL;
      g_autofree char *timestamp = NULL;
      g_autoptr(GVariant) metadata = NULL;
      int i;

      parent = ostree_commit_get_parent (commit_data);
      if (parent)
        add_annotation (annotations, "org.flatpak.Ostree.ParentCommit", parent);

      metadata = g_variant_get_child_value (commit_data, 0);
      for (i = 0; i < g_variant_n_children (metadata); i++)
        {
          g_autoptr(GVariant) elm = g_variant_get_child_value (metadata, i);
          g_autoptr(GVariant) value = g_variant_get_child_value (elm, 1);
          g_autofree char *key = NULL;
          g_autofree char *full_key = NULL;
          g_autofree char *value_base64 = NULL;

          g_variant_get_child (elm, 0, "s", &key);
          full_key = g_strdup_printf ("org.flatpak.Ostree.Metadata.%s", key);

          value_base64 = g_base64_encode (g_variant_get_data (value), g_variant_get_size (value));
          add_annotation (annotations, full_key, value_base64);
        }

      timestamp = g_strdup_printf ("%"G_GUINT64_FORMAT, ostree_commit_get_timestamp (commit_data));
      add_annotation (annotations, "org.flatpak.Ostree.Timestamp", timestamp);

      g_variant_get_child (commit_data, 3, "s", &subject);
      add_annotation (annotations, "org.flatpak.Ostree.Subject", subject);

      g_variant_get_child (commit_data, 4, "s", &body);
      add_annotation (annotations, "org.flatpak.Ostree.Body", body);
   }
}

void
flatpak_oci_parse_commit_annotations (GHashTable *annotations,
                                      guint64 *out_timestamp,
                                      char **out_subject,
                                      char **out_body,
                                      char **out_ref,
                                      char **out_commit,
                                      char **out_parent_commit,
                                      GVariantBuilder *metadata_builder)
{
  const char *oci_timestamp, *oci_subject, *oci_body, *oci_parent_commit, *oci_commit, *oci_ref;
  GHashTableIter iter;
  gpointer _key, _value;

  oci_ref = g_hash_table_lookup (annotations, "org.flatpak.Ostree.Ref");
  if (oci_ref != NULL && out_ref != NULL && *out_ref == NULL)
    *out_ref = g_strdup (oci_ref);

  oci_commit = g_hash_table_lookup (annotations, "org.flatpak.Ostree.Commit");
  if (oci_commit != NULL && out_commit != NULL && *out_commit == NULL)
    *out_commit = g_strdup (oci_commit);

  oci_parent_commit = g_hash_table_lookup (annotations, "org.flatpak.Ostree.ParentCommit");
  if (oci_parent_commit != NULL && out_parent_commit != NULL && *out_parent_commit == NULL)
    *out_parent_commit = g_strdup (oci_parent_commit);

  oci_timestamp = g_hash_table_lookup (annotations, "org.flatpak.Ostree.Timestamp");
  if (oci_timestamp != NULL && out_timestamp != NULL && *out_timestamp == 0)
    *out_timestamp = g_ascii_strtoull (oci_timestamp, NULL, 10);

  oci_subject = g_hash_table_lookup (annotations, "org.flatpak.Ostree.Subject");
  if (oci_subject != NULL && out_subject != NULL && *out_subject == NULL)
    *out_subject = g_strdup (oci_subject);

  oci_body = g_hash_table_lookup (annotations, "org.flatpak.Ostree.Body");
  if (oci_body != NULL && out_body != NULL && *out_body == NULL)
    *out_body = g_strdup (oci_body);

  if (metadata_builder)
    {
      g_hash_table_iter_init (&iter, annotations);
      while (g_hash_table_iter_next (&iter, &_key, &_value))
        {
          const char *key = _key;
          const char *value = _value;
          guchar *bin;
          gsize bin_len;
          g_autoptr(GVariant) data = NULL;

          if (!g_str_has_prefix (key, "org.flatpak.Ostree.Metadata."))
            continue;
          key += strlen ("org.flatpak.Ostree.Metadata.");

          bin = g_base64_decode (value, &bin_len);
          data = g_variant_ref_sink (g_variant_new_from_data (G_VARIANT_TYPE("v"), bin, bin_len, FALSE,
                                                              g_free, bin));
          g_variant_builder_add (metadata_builder, "{s@v}", key, data);
        }
    }
}
