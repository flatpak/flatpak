/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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

#include "flatpak-json-oci-private.h"

#include <ostree.h>

#include "flatpak-utils-private.h"
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

FlatpakOciDescriptor *
flatpak_oci_descriptor_new (const char *mediatype,
                            const char *digest,
                            gint64      size)
{
  FlatpakOciDescriptor *desc = g_new0 (FlatpakOciDescriptor, 1);

  desc->mediatype = g_strdup (mediatype);
  desc->digest = g_strdup (digest);
  desc->size = size;
  desc->annotations = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  return desc;
}

void
flatpak_oci_descriptor_copy (FlatpakOciDescriptor *source,
                             FlatpakOciDescriptor *dest)
{
  flatpak_oci_descriptor_destroy (dest);

  dest->mediatype = g_strdup (source->mediatype);
  dest->digest = g_strdup (source->digest);
  dest->size = source->size;
  dest->urls = g_strdupv ((char **) source->urls);
  dest->annotations = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  if (source->annotations)
    flatpak_oci_copy_labels (source->annotations, dest->annotations);
}

void
flatpak_oci_descriptor_destroy (FlatpakOciDescriptor *self)
{
  g_free (self->mediatype);
  g_free (self->digest);
  g_strfreev (self->urls);
  if (self->annotations)
    g_hash_table_destroy (self->annotations);
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
  FLATPAK_JSON_STRMAP_PROP (FlatpakOciDescriptor, annotations, "annotations"),
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

FlatpakOciManifestDescriptor *
flatpak_oci_manifest_descriptor_new (void)
{
  return g_new0 (FlatpakOciManifestDescriptor, 1);
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
  FLATPAK_JSON_OPT_STRUCT_PROP (FlatpakOciManifestDescriptor, platform, "platform", flatpak_oci_manifest_platform_props),
  FLATPAK_JSON_LAST_PROP
};


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
flatpak_oci_versioned_from_json (GBytes *bytes,
                                 const char *content_type,
                                 GError **error)
{
  g_autoptr(JsonParser) parser = NULL;
  JsonNode *root = NULL;
  const gchar *mediatype = NULL;
  JsonObject *object;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser,
                                   g_bytes_get_data (bytes, NULL),
                                   g_bytes_get_size (bytes),
                                   error))
    return NULL;

  root = json_parser_get_root (parser);
  object = json_node_get_object (root);

  if (json_object_has_member (object, "mediaType"))
    mediatype = json_object_get_string_member (object, "mediaType");
  else
    mediatype = content_type;

  if (mediatype == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Versioned object lacks mediatype");
      return NULL;
    }

  /* The docker v2 image manifest is similar enough that we can just load it, it does not have the annotation field though */
  if (strcmp (mediatype, FLATPAK_OCI_MEDIA_TYPE_IMAGE_MANIFEST) == 0 ||
      strcmp (mediatype, FLATPAK_DOCKER_MEDIA_TYPE_IMAGE_MANIFEST2) == 0)
    return (FlatpakOciVersioned *) flatpak_json_from_node (root, FLATPAK_TYPE_OCI_MANIFEST, error);

  if (strcmp (mediatype, FLATPAK_OCI_MEDIA_TYPE_IMAGE_INDEX) == 0)
    return (FlatpakOciVersioned *) flatpak_json_from_node (root, FLATPAK_TYPE_OCI_INDEX, error);

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
               "Unsupported media type %s", mediatype);
  return NULL;
}

FlatpakOciImage *
flatpak_oci_image_from_json (GBytes *bytes,
                             GError **error)
{
  g_autoptr(JsonParser) parser = NULL;
  JsonNode *root = NULL;

  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser,
                                   g_bytes_get_data (bytes, NULL),
                                   g_bytes_get_size (bytes),
                                   error))
    return NULL;

  root = json_parser_get_root (parser);

  return (FlatpakOciImage *) flatpak_json_from_node (root, FLATPAK_TYPE_OCI_IMAGE, error);
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
    FLATPAK_JSON_STRUCT_PROP (FlatpakOciManifest, config, "config", flatpak_oci_descriptor_props),
    FLATPAK_JSON_STRUCTV_PROP (FlatpakOciManifest, layers, "layers", flatpak_oci_descriptor_props),
    FLATPAK_JSON_STRMAP_PROP (FlatpakOciManifest, annotations, "annotations"),
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
flatpak_oci_manifest_set_config (FlatpakOciManifest   *self,
                                 FlatpakOciDescriptor *desc)
{
  g_free (self->config.mediatype);
  self->config.mediatype = g_strdup (desc->mediatype);
  g_free (self->config.digest);
  self->config.digest = g_strdup (desc->digest);
  self->config.size = desc->size;
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
flatpak_oci_manifest_set_layer (FlatpakOciManifest   *self,
                                FlatpakOciDescriptor *desc)
{
  FlatpakOciDescriptor *descs[2] = { desc, NULL };

  flatpak_oci_manifest_set_layers (self, descs);
}

void
flatpak_oci_manifest_set_layers (FlatpakOciManifest    *self,
                                 FlatpakOciDescriptor **descs)
{
  int i, count;

  for (i = 0; self->layers != NULL && self->layers[i] != NULL; i++)
    flatpak_oci_descriptor_free (self->layers[i]);
  g_free (self->layers);

  count = ptrv_count ((gpointer *) descs);

  self->layers = g_new0 (FlatpakOciDescriptor *, count + 1);
  for (i = 0; i < count; i++)
    {
      self->layers[i] = g_new0 (FlatpakOciDescriptor, 1);
      self->layers[i]->mediatype = g_strdup (descs[i]->mediatype);
      self->layers[i]->digest = g_strdup (descs[i]->digest);
      self->layers[i]->size = descs[i]->size;
    }
}

int
flatpak_oci_manifest_get_n_layers (FlatpakOciManifest *self)
{
  return ptrv_count ((gpointer *) self->layers);
}

const char *
flatpak_oci_manifest_get_layer_digest (FlatpakOciManifest *self,
                                       int                 i)
{
  return self->layers[i]->digest;
}

GHashTable *
flatpak_oci_manifest_get_annotations (FlatpakOciManifest *self)
{
  return self->annotations;
}

FlatpakOciDescriptor *
flatpak_oci_manifest_find_delta_for (FlatpakOciManifest *delta_manifest,
                                     const char         *from_diffid,
                                     const char         *to_diffid)
{
  int i;

  if (from_diffid == NULL || to_diffid == NULL)
    return NULL;

  for (i = 0; delta_manifest->layers != NULL && delta_manifest->layers[i] != NULL; i++)
    {
      FlatpakOciDescriptor *layer = delta_manifest->layers[i];
      const char *layer_from = NULL, *layer_to = NULL;

      if (layer->annotations != NULL)
        {
          layer_from = g_hash_table_lookup (layer->annotations, "io.github.containers.delta.from");
          layer_to = g_hash_table_lookup (layer->annotations, "io.github.containers.delta.to");

          if (g_strcmp0 (layer_from, from_diffid) == 0 &&
              g_strcmp0 (layer_to, to_diffid) == 0)
            return layer;
        }
    }

  return NULL;
}


G_DEFINE_TYPE (FlatpakOciIndex, flatpak_oci_index, FLATPAK_TYPE_OCI_VERSIONED);

static void
flatpak_oci_index_finalize (GObject *object)
{
  FlatpakOciIndex *self = (FlatpakOciIndex *) object;
  int i;

  for (i = 0; self->manifests != NULL && self->manifests[i] != NULL; i++)
    flatpak_oci_manifest_descriptor_free (self->manifests[i]);
  g_free (self->manifests);

  if (self->annotations)
    g_hash_table_destroy (self->annotations);

  G_OBJECT_CLASS (flatpak_oci_index_parent_class)->finalize (object);
}


static void
flatpak_oci_index_class_init (FlatpakOciIndexClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FlatpakJsonClass *json_class = FLATPAK_JSON_CLASS (klass);
  static FlatpakJsonProp props[] = {
    FLATPAK_JSON_STRUCTV_PROP (FlatpakOciIndex, manifests, "manifests", flatpak_oci_manifest_descriptor_props),
    FLATPAK_JSON_STRMAP_PROP (FlatpakOciIndex, annotations, "annotations"),
    FLATPAK_JSON_LAST_PROP
  };

  object_class->finalize = flatpak_oci_index_finalize;
  json_class->props = props;
  json_class->mediatype = FLATPAK_OCI_MEDIA_TYPE_IMAGE_INDEX;
}

static void
flatpak_oci_index_init (FlatpakOciIndex *self)
{
}

FlatpakOciIndex *
flatpak_oci_index_new (void)
{
  FlatpakOciIndex *index;

  index = g_object_new (FLATPAK_TYPE_OCI_INDEX, NULL);

  index->parent.version = 2;
  index->parent.mediatype = g_strdup (FLATPAK_OCI_MEDIA_TYPE_IMAGE_INDEX);
  index->annotations = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  return index;
}

static FlatpakOciManifestDescriptor *
manifest_desc_for_desc (FlatpakOciDescriptor *src_descriptor, const char *ref)
{
  FlatpakOciManifestDescriptor *desc;

  desc = flatpak_oci_manifest_descriptor_new ();
  flatpak_oci_descriptor_copy (src_descriptor, &desc->parent);

  g_hash_table_replace (desc->parent.annotations,
                        g_strdup ("org.opencontainers.image.ref.name"),
                        g_strdup (ref));
  return desc;
}

int
flatpak_oci_index_get_n_manifests (FlatpakOciIndex *self)
{
  return ptrv_count ((gpointer *) self->manifests);
}

void
flatpak_oci_index_add_manifest (FlatpakOciIndex      *self,
                                const char           *ref,
                                FlatpakOciDescriptor *desc)
{
  FlatpakOciManifestDescriptor *m;
  int count;

  if (ref != NULL)
    flatpak_oci_index_remove_manifest (self, ref);

  count = flatpak_oci_index_get_n_manifests (self);

  m = manifest_desc_for_desc (desc, ref);
  self->manifests = g_renew (FlatpakOciManifestDescriptor *, self->manifests, count + 2);
  self->manifests[count] = m;
  self->manifests[count + 1] = NULL;
}

const char *
flatpak_oci_manifest_descriptor_get_ref (FlatpakOciManifestDescriptor *m)
{
  if (m->parent.mediatype == NULL ||
      (strcmp (m->parent.mediatype, FLATPAK_OCI_MEDIA_TYPE_IMAGE_MANIFEST) != 0 &&
       strcmp (m->parent.mediatype, FLATPAK_DOCKER_MEDIA_TYPE_IMAGE_MANIFEST2) != 0))
    return NULL;

  if (m->parent.annotations == NULL)
    return NULL;

  return g_hash_table_lookup (m->parent.annotations, "org.opencontainers.image.ref.name");
}

static int
index_find_ref (FlatpakOciIndex *self,
                const char      *ref)
{
  int i;

  if (self->manifests == NULL)
    return -1;

  for (i = 0; self->manifests[i] != NULL; i++)
    {
      const char *m_ref = flatpak_oci_manifest_descriptor_get_ref (self->manifests[i]);

      if (m_ref == NULL)
        continue;

      if (strcmp (ref, m_ref) == 0)
        return i;
    }

  return -1;
}

FlatpakOciManifestDescriptor *
flatpak_oci_index_get_manifest (FlatpakOciIndex *self,
                                const char      *ref)
{
  int i = index_find_ref (self, ref);

  if (i >= 0)
    return self->manifests[i];

  return NULL;
}

FlatpakOciManifestDescriptor *
flatpak_oci_index_get_only_manifest (FlatpakOciIndex *self)
{
  int i, found = -1;

  if (self->manifests == NULL)
    return NULL;

  for (i = 0; self->manifests[i] != NULL; i++)
    {
      const char *m_ref = flatpak_oci_manifest_descriptor_get_ref (self->manifests[i]);

      if (m_ref == NULL)
        continue;

      if (found == -1)
        found = i;
      else
        return NULL;
    }

  if (found >= 0)
    return self->manifests[found];

  return NULL;
}

FlatpakOciManifestDescriptor *
flatpak_oci_index_get_manifest_for_arch (FlatpakOciIndex *self,
                                         const char      *oci_arch)
{
  int i, found = -1;

  if (self->manifests == NULL)
    return NULL;

  for (i = 0; self->manifests[i] != NULL; i++)
    {
      if (strcmp (self->manifests[i]->platform.architecture, oci_arch) == 0)
        return self->manifests[i];
    }

  if (found >= 0)
    return self->manifests[found];

  return NULL;
}

gboolean
flatpak_oci_index_remove_manifest (FlatpakOciIndex *self,
                                   const char      *ref)
{
  int i = index_find_ref (self, ref);

  if (i < 0)
    return FALSE;

  flatpak_oci_manifest_descriptor_free (self->manifests[i]);

  for (; self->manifests[i] != NULL; i++)
    self->manifests[i] = self->manifests[i + 1];

  return TRUE;
}

FlatpakOciDescriptor *
flatpak_oci_index_find_delta_for (FlatpakOciIndex *delta_index,
                                  const char      *for_digest)
{
  int i;

  if (delta_index->manifests == NULL)
    return NULL;

  for (i = 0; delta_index->manifests[i] != NULL; i++)
    {
      FlatpakOciManifestDescriptor *d = delta_index->manifests[i];
      const char *target;

      if (d->parent.annotations == NULL)
        continue;

      target = g_hash_table_lookup (d->parent.annotations, "io.github.containers.delta.target");
      if (g_strcmp0 (target, for_digest) == 0)
        return &d->parent;
    }

  return NULL;
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
    FLATPAK_JSON_STRMAP_PROP (FlatpakOciImageConfig, labels, "Labels"),
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
                               const char      *created)
{
  g_free (image->created);
  image->created = g_strdup (created);
}

void
flatpak_oci_image_set_architecture (FlatpakOciImage *image,
                                    const char      *arch)
{
  g_free (image->architecture);
  image->architecture = g_strdup (arch);
}

void
flatpak_oci_image_set_os (FlatpakOciImage *image,
                          const char      *os)
{
  g_free (image->os);
  image->os = g_strdup (os);
}

void
flatpak_oci_image_set_layers (FlatpakOciImage *image,
                              const char     **layers)
{
  g_strfreev (image->rootfs.diff_ids);
  image->rootfs.diff_ids = g_strdupv ((char **) layers);
}

int
flatpak_oci_image_get_n_layers (FlatpakOciImage *image)
{
  return ptrv_count ((gpointer *) image->rootfs.diff_ids);
}

GHashTable *
flatpak_oci_image_get_labels (FlatpakOciImage *self)
{
  if (self->config.labels == NULL)
    self->config.labels = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  return self->config.labels;
}

void
flatpak_oci_image_set_layer (FlatpakOciImage *image,
                             const char      *layer)
{
  const char *layers[] = {layer, NULL};

  flatpak_oci_image_set_layers (image, layers);
}

void
flatpak_oci_export_labels (GHashTable *source,
                           GHashTable *dest)
{
  const char *keys[] = {
    "org.flatpak.ref",
    "org.flatpak.installed-size",
    "org.flatpak.download-size",
    "org.flatpak.metadata",
  };
  int i;

  if (source == NULL)
    return;

  for (i = 0; i < G_N_ELEMENTS (keys); i++)
    {
      const char *key = keys[i];
      const char *value = g_hash_table_lookup (source, key);
      if (value)
        g_hash_table_replace (dest,
                              g_strdup (key),
                              g_strdup (value));
    }
}

void
flatpak_oci_copy_labels (GHashTable *source,
                         GHashTable *dest)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, source);
  while (g_hash_table_iter_next (&iter, &key, &value))
    g_hash_table_replace (dest,
                          g_strdup ((char *) key),
                          g_strdup ((char *) value));
}

int
flatpak_oci_image_add_history (FlatpakOciImage *image)
{
  FlatpakOciImageHistory **old;
  int i, index, old_len;

  old = image->history;

  for (old_len = 0; old != NULL && old[old_len] != NULL; old_len++)
    ;

  image->history = g_new0 (FlatpakOciImageHistory *, old_len + 2);
  for (i = 0; i < old_len; i++)
    image->history[i] = old[i];

  index = i;

  image->history[i++] = g_new0 (FlatpakOciImageHistory, 1);
  image->history[i++] = NULL;

  g_free (old);

  return index;
}

static void
add_label (GHashTable *labels, const char *key, const char *value)
{
  g_hash_table_replace (labels,
                        g_strdup (key),
                        g_strdup (value));
}

void
flatpak_oci_add_labels_for_commit (GHashTable *labels,
                                   const char *ref,
                                   const char *commit,
                                   GVariant   *commit_data)
{
  if (ref)
    add_label (labels, "org.flatpak.ref", ref);

  if (commit)
    add_label (labels, "org.flatpak.commit", commit);

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
        add_label (labels, "org.flatpak.parent-commit", parent);

      metadata = g_variant_get_child_value (commit_data, 0);
      for (i = 0; i < g_variant_n_children (metadata); i++)
        {
          g_autoptr(GVariant) elm = g_variant_get_child_value (metadata, i);
          g_autoptr(GVariant) value = g_variant_get_child_value (elm, 1);
          g_autofree char *key = NULL;
          g_autofree char *full_key = NULL;
          g_autofree char *value_base64 = NULL;

          g_variant_get_child (elm, 0, "s", &key);
          full_key = g_strdup_printf ("org.flatpak.commit-metadata.%s", key);

          value_base64 = g_base64_encode (g_variant_get_data (value), g_variant_get_size (value));
          add_label (labels, full_key, value_base64);
        }

      timestamp = g_strdup_printf ("%"G_GUINT64_FORMAT, ostree_commit_get_timestamp (commit_data));
      add_label (labels, "org.flatpak.timestamp", timestamp);

      g_variant_get_child (commit_data, 3, "s", &subject);
      add_label (labels, "org.flatpak.subject", subject);

      g_variant_get_child (commit_data, 4, "s", &body);
      add_label (labels, "org.flatpak.body", body);
    }
}


G_DEFINE_TYPE (FlatpakOciSignature, flatpak_oci_signature, FLATPAK_TYPE_JSON);

static void
flatpak_oci_signature_critical_destroy (FlatpakOciSignatureCritical *self)
{
  g_free (self->type);
  g_free (self->image.digest);
  g_free (self->identity.ref);
}

static void
flatpak_oci_signature_optional_destroy (FlatpakOciSignatureOptional *self)
{
  g_free (self->creator);
}

static void
flatpak_oci_signature_finalize (GObject *object)
{
  FlatpakOciSignature *self = (FlatpakOciSignature *) object;

  flatpak_oci_signature_critical_destroy (&self->critical);
  flatpak_oci_signature_optional_destroy (&self->optional);

  G_OBJECT_CLASS (flatpak_oci_signature_parent_class)->finalize (object);
}

static void
flatpak_oci_signature_class_init (FlatpakOciSignatureClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FlatpakJsonClass *json_class = FLATPAK_JSON_CLASS (klass);
  static FlatpakJsonProp image_props[] = {
    FLATPAK_JSON_MANDATORY_STRING_PROP (FlatpakOciSignatureCriticalImage, digest, "oci-image-manifest-digest"),
    FLATPAK_JSON_LAST_PROP
  };
  static FlatpakJsonProp identity_props[] = {
    FLATPAK_JSON_MANDATORY_STRING_PROP (FlatpakOciSignatureCriticalIdentity, ref, "oci-image-ref"),
    FLATPAK_JSON_LAST_PROP
  };
  static FlatpakJsonProp critical_props[] = {
    FLATPAK_JSON_MANDATORY_STRING_PROP (FlatpakOciSignatureCritical, type, "type"),
    FLATPAK_JSON_MANDATORY_STRICT_STRUCT_PROP (FlatpakOciSignatureCritical, image, "image", image_props),
    FLATPAK_JSON_MANDATORY_STRICT_STRUCT_PROP (FlatpakOciSignatureCritical, identity, "identity", identity_props),
    FLATPAK_JSON_LAST_PROP
  };
  static FlatpakJsonProp optional_props[] = {
    FLATPAK_JSON_STRING_PROP (FlatpakOciSignatureOptional, creator, "creator"),
    FLATPAK_JSON_INT64_PROP (FlatpakOciSignatureOptional, timestamp, "timestamp"),
    FLATPAK_JSON_LAST_PROP
  };
  static FlatpakJsonProp props[] = {
    FLATPAK_JSON_MANDATORY_STRICT_STRUCT_PROP (FlatpakOciSignature, critical, "critical", critical_props),
    FLATPAK_JSON_STRUCT_PROP (FlatpakOciSignature, optional, "optional", optional_props),
    FLATPAK_JSON_LAST_PROP
  };

  object_class->finalize = flatpak_oci_signature_finalize;
  json_class->props = props;
}

static void
flatpak_oci_signature_init (FlatpakOciSignature *self)
{
}

FlatpakOciSignature *
flatpak_oci_signature_new (const char *digest, const char *ref)
{
  FlatpakOciSignature *signature;

  signature = g_object_new (FLATPAK_TYPE_OCI_SIGNATURE, NULL);

  /* Some default values */
  signature->critical.type = g_strdup (FLATPAK_OCI_SIGNATURE_TYPE_FLATPAK);
  signature->critical.image.digest = g_strdup (digest);
  signature->critical.identity.ref = g_strdup (ref);
  signature->optional.creator = g_strdup ("flatpak " PACKAGE_VERSION);
  signature->optional.timestamp = time (NULL);

  return signature;
}

G_DEFINE_TYPE (FlatpakOciIndexResponse, flatpak_oci_index_response, FLATPAK_TYPE_JSON);

static void
flatpak_oci_index_image_free (FlatpakOciIndexImage *self)
{
  g_free (self->digest);
  g_free (self->mediatype);
  g_free (self->os);
  g_free (self->architecture);
  g_strfreev (self->tags);
  if (self->annotations)
    g_hash_table_destroy (self->annotations);
  if (self->labels)
    g_hash_table_destroy (self->labels);
  g_free (self);
}

static void
flatpak_oci_index_image_list_free (FlatpakOciIndexImageList *self)
{
  int i;

  g_free (self->digest);
  g_free (self->mediatype);
  g_strfreev (self->tags);
  for (i = 0; self->images != NULL && self->images[i] != NULL; i++)
    flatpak_oci_index_image_free (self->images[i]);
  g_free (self->images);
  g_free (self);
}

static void
flatpak_oci_index_repository_free (FlatpakOciIndexRepository *self)
{
  int i;

  g_free (self->name);
  for (i = 0; self->images != NULL && self->images[i] != NULL; i++)
    flatpak_oci_index_image_free (self->images[i]);
  g_free (self->images);

  for (i = 0; self->lists != NULL && self->lists[i] != NULL; i++)
    flatpak_oci_index_image_list_free (self->lists[i]);
  g_free (self->lists);

  g_free (self);
}

static void
flatpak_oci_index_response_finalize (GObject *object)
{
  FlatpakOciIndexResponse *self = (FlatpakOciIndexResponse *) object;
  int i;

  g_free (self->registry);
  for (i = 0; self->results != NULL && self->results[i] != NULL; i++)
    flatpak_oci_index_repository_free (self->results[i]);
  g_free (self->results);

  G_OBJECT_CLASS (flatpak_oci_index_response_parent_class)->finalize (object);
}

static void
flatpak_oci_index_response_class_init (FlatpakOciIndexResponseClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FlatpakJsonClass *json_class = FLATPAK_JSON_CLASS (klass);
  static FlatpakJsonProp image_props[] = {
    FLATPAK_JSON_STRING_PROP (FlatpakOciIndexImage, digest, "Digest"),
    FLATPAK_JSON_STRING_PROP (FlatpakOciIndexImage, mediatype, "MediaType"),
    FLATPAK_JSON_STRING_PROP (FlatpakOciIndexImage, os, "OS"),
    FLATPAK_JSON_STRING_PROP (FlatpakOciIndexImage, architecture, "Architecture"),
    FLATPAK_JSON_STRMAP_PROP (FlatpakOciIndexImage, annotations, "Annotations"),
    FLATPAK_JSON_STRMAP_PROP (FlatpakOciIndexImage, labels, "Labels"),
    FLATPAK_JSON_STRV_PROP (FlatpakOciIndexImage, tags, "Tags"),
    FLATPAK_JSON_LAST_PROP
  };
  static FlatpakJsonProp lists_props[] = {
    FLATPAK_JSON_STRING_PROP (FlatpakOciIndexImageList, digest, "Digest"),
    FLATPAK_JSON_STRUCTV_PROP (FlatpakOciIndexImageList, images, "Images", image_props),
    FLATPAK_JSON_STRING_PROP (FlatpakOciIndexImageList, mediatype, "MediaType"),
    FLATPAK_JSON_STRV_PROP (FlatpakOciIndexImageList, tags, "Tags"),
    FLATPAK_JSON_LAST_PROP
  };
  static FlatpakJsonProp results_props[] = {
    FLATPAK_JSON_STRING_PROP (FlatpakOciIndexRepository, name, "Name"),
    FLATPAK_JSON_STRUCTV_PROP (FlatpakOciIndexRepository, images, "Images", image_props),
    FLATPAK_JSON_STRUCTV_PROP (FlatpakOciIndexRepository, lists, "Lists", lists_props),
    FLATPAK_JSON_LAST_PROP
  };
  static FlatpakJsonProp props[] = {
    FLATPAK_JSON_STRING_PROP (FlatpakOciIndexResponse, registry, "Registry"),
    FLATPAK_JSON_STRUCTV_PROP (FlatpakOciIndexResponse, results, "Results", results_props),
    FLATPAK_JSON_LAST_PROP
  };

  object_class->finalize = flatpak_oci_index_response_finalize;
  json_class->props = props;
}

static void
flatpak_oci_index_response_init (FlatpakOciIndexResponse *self)
{
}
