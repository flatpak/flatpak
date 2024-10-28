/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2024 Red Hat, Inc
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
 *       Owen Taylor <otaylor@redhat.com>
 */

#include <glib/gi18n-lib.h>

#include "flatpak-image-source-private.h"
#include "flatpak-oci-registry-private.h"

struct _FlatpakImageSource
{
  GObject parent;

  FlatpakOciRegistry *registry;
  char *repository;
  char *digest;

  FlatpakOciManifest *manifest;
  size_t manifest_size;
  FlatpakOciImage *image_config;
};

G_DEFINE_TYPE (FlatpakImageSource, flatpak_image_source, G_TYPE_OBJECT)

static void
flatpak_image_source_finalize (GObject *object)
{
  FlatpakImageSource *self = FLATPAK_IMAGE_SOURCE (object);

  g_clear_object (&self->registry);
  g_clear_pointer (&self->repository, g_free);
  g_clear_pointer (&self->digest, g_free);
  g_clear_object (&self->manifest);
  g_clear_object (&self->image_config);

  G_OBJECT_CLASS (flatpak_image_source_parent_class)->finalize (object);
}

static void
flatpak_image_source_class_init (FlatpakImageSourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = flatpak_image_source_finalize;
}

static void
flatpak_image_source_init (FlatpakImageSource *self)
{
}

static FlatpakImageSource *
flatpak_image_source_new (FlatpakOciRegistry *registry,
                          const char         *repository,
                          const char         *digest,
                          GCancellable       *cancellable,
                          GError            **error)
{
  g_autoptr(FlatpakImageSource) self = NULL;
  g_autoptr(FlatpakOciVersioned) versioned = NULL;

  self = g_object_new (FLATPAK_TYPE_IMAGE_SOURCE, NULL);
  self->registry = g_object_ref (registry);
  self->repository = g_strdup (repository);
  self->digest = g_strdup (digest);

  versioned = flatpak_oci_registry_load_versioned (self->registry, self->repository,
                                                   self->digest, NULL, &self->manifest_size,
                                                   cancellable, error);
  if (versioned == NULL)
    return NULL;

  if (!FLATPAK_IS_OCI_MANIFEST (versioned))
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Image is not a manifest"));
      return NULL;
    }

  self->manifest = FLATPAK_OCI_MANIFEST (g_steal_pointer (&versioned));

  if (self->manifest->config.digest == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Image is not a manifest"));
      return NULL;
    }

  self->image_config =
    flatpak_oci_registry_load_image_config (self->registry,
                                            self->repository,
                                            self->manifest->config.digest,
                                            (const char **) self->manifest->config.urls,
                                            NULL, cancellable, error);
  if (self->image_config == NULL)
    return NULL;

  if (flatpak_image_source_get_ref (self) == NULL)
    {
      flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("No org.flatpak.ref found in image"));
      return NULL;
    }

  return g_steal_pointer (&self);
}

FlatpakImageSource *
flatpak_image_source_new_local (GFile        *file,
                                const char   *reference,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_autofree char *dir_uri = NULL;
  g_autofree char *target_ref = NULL;
  g_autoptr(FlatpakImageSource) image_source = NULL;
  g_autoptr(FlatpakOciRegistry) registry = NULL;
  g_autoptr(FlatpakOciIndex) index = NULL;
  const FlatpakOciManifestDescriptor *desc;

  dir_uri = g_file_get_uri (file);
  registry = flatpak_oci_registry_new (dir_uri, FALSE, -1, cancellable, error);
  if (registry == NULL)
    return NULL;

  index = flatpak_oci_registry_load_index (registry, cancellable, error);
  if (index == NULL)
    return NULL;

  if (reference)
    {
      desc = flatpak_oci_index_get_manifest (index, reference);
      if (desc == NULL)
        {
          flatpak_fail (error, _("Ref '%s' not found in registry"), reference);
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

  return flatpak_image_source_new (registry, NULL, desc->parent.digest, cancellable, error);
}

FlatpakImageSource *
flatpak_image_source_new_remote (const char   *uri,
                                 const char   *oci_repository,
                                 const char   *digest,
                                 GCancellable *cancellable,
                                 GError      **error)
{
  g_autoptr(FlatpakOciRegistry) registry = NULL;

  registry = flatpak_oci_registry_new (uri, FALSE, -1, cancellable, error);
  if (!registry)
    return NULL;

  return flatpak_image_source_new (registry, oci_repository, digest, cancellable, error);
}

void
flatpak_image_source_set_token (FlatpakImageSource *self,
                                const char            *token)
{
  flatpak_oci_registry_set_token (self->registry, token);
}

FlatpakOciRegistry *
flatpak_image_source_get_registry (FlatpakImageSource *self)
{
  return self->registry;
}

const char *
flatpak_image_source_get_oci_repository (FlatpakImageSource *self)
{
  return self->repository;
}

const char *
flatpak_image_source_get_digest (FlatpakImageSource *self)
{
  return self->digest;
}

FlatpakOciManifest *
flatpak_image_source_get_manifest (FlatpakImageSource *self)
{
  return self->manifest;
}

size_t
flatpak_image_source_get_manifest_size (FlatpakImageSource *self)
{
  return self->manifest_size;
}

FlatpakOciImage *
flatpak_image_source_get_image_config (FlatpakImageSource *self)
{
  return self->image_config;
}

const char *
flatpak_image_source_get_ref (FlatpakImageSource *self)
{
  GHashTable *labels = flatpak_oci_image_get_labels (self->image_config);

  return g_hash_table_lookup (labels, "org.flatpak.ref");
}

const char *
flatpak_image_source_get_metadata (FlatpakImageSource *self)
{
  GHashTable *labels = flatpak_oci_image_get_labels (self->image_config);

  return g_hash_table_lookup (labels, "org.flatpak.metadata");
}

const char *
flatpak_image_source_get_commit (FlatpakImageSource *self)
{
  GHashTable *labels = flatpak_oci_image_get_labels (self->image_config);

  return g_hash_table_lookup (labels, "org.flatpak.commit");
}

const char *
flatpak_image_source_get_parent_commit (FlatpakImageSource *self)
{
  GHashTable *labels = flatpak_oci_image_get_labels (self->image_config);

  return g_hash_table_lookup (labels, "org.flatpak.parent-commit");
}

guint64
flatpak_image_source_get_commit_timestamp (FlatpakImageSource *self)
{
  GHashTable *labels = flatpak_oci_image_get_labels (self->image_config);
  const char *oci_timestamp = g_hash_table_lookup (labels, "org.flatpak.timestamp");

  if (oci_timestamp != NULL)
    return g_ascii_strtoull (oci_timestamp, NULL, 10);
  else
    return 0;
}

const char *
flatpak_image_source_get_commit_subject (FlatpakImageSource *self)
{
  GHashTable *labels = flatpak_oci_image_get_labels (self->image_config);

  return g_hash_table_lookup (labels, "org.flatpak.subject");
}

const char *
flatpak_image_source_get_commit_body (FlatpakImageSource *self)
{
  GHashTable *labels = flatpak_oci_image_get_labels (self->image_config);

  return g_hash_table_lookup (labels, "org.flatpak.body");
}

void
flatpak_image_source_build_commit_metadata (FlatpakImageSource *self,
                                            GVariantBuilder    *metadata_builder)
{
  GHashTable *labels = flatpak_oci_image_get_labels (self->image_config);
  GHashTableIter iter;
  const char *key;
  const char *value;

  g_hash_table_iter_init (&iter, labels);
  while (g_hash_table_iter_next (&iter, (gpointer *)&key, (gpointer *)&value))
    {
      g_autoptr(GVariant) data = NULL;
      uint8_t *bin;
      size_t bin_len;

      if (!g_str_has_prefix (key, "org.flatpak.commit-metadata."))
        continue;

      key += strlen ("org.flatpak.commit-metadata.");

      bin = g_base64_decode (value, &bin_len);
      data = g_variant_ref_sink (g_variant_new_from_data (G_VARIANT_TYPE ("v"),
                                                          bin,
                                                          bin_len,
                                                          FALSE,
                                                          g_free,
                                                          bin));
      g_variant_builder_add (metadata_builder, "{s@v}", key, data);
    }
}
