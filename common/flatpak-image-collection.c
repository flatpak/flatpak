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

#include "flatpak-image-collection-private.h"
#include "flatpak-oci-registry-private.h"

struct FlatpakImageCollection
{
  GObject parent;

  GPtrArray *sources;
};

typedef struct
{
  GObjectClass parent_class;
} FlatpakImageCollectionClass;

G_DEFINE_TYPE (FlatpakImageCollection, flatpak_image_collection, G_TYPE_OBJECT)


static void
flatpak_image_collection_finalize (GObject *object)
{
  FlatpakImageCollection *self = FLATPAK_IMAGE_COLLECTION (object);

  g_ptr_array_free (self->sources, TRUE);

  G_OBJECT_CLASS (flatpak_image_collection_parent_class)->finalize (object);
}

static void
flatpak_image_collection_class_init (FlatpakImageCollectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = flatpak_image_collection_finalize;
}

static void
flatpak_image_collection_init (FlatpakImageCollection *self)
{
  self->sources = g_ptr_array_new_with_free_func ((GDestroyNotify)g_object_unref);
}

FlatpakImageCollection *
flatpak_image_collection_new (const char   *location,
                              GCancellable *cancellable,
                              GError      **error)
{
  g_autoptr(FlatpakImageCollection) self = NULL;
  g_autoptr(FlatpakOciRegistry) registry = NULL;
  g_autoptr(FlatpakOciIndex) index = NULL;
  gsize i;

  self = g_object_new (FLATPAK_TYPE_IMAGE_COLLECTION, NULL);

  if (g_str_has_prefix (location, "oci:"))
    {
      g_autoptr(GFile) dir = g_file_new_for_path (location + 4);
      g_autofree char *uri = g_file_get_uri (dir);

      registry = flatpak_oci_registry_new (uri, FALSE, -1, cancellable, error);
      if (registry == NULL)
        return NULL;
    }
  else if (g_str_has_prefix (location, "oci-archive:"))
    {
      g_autoptr(GFile) file = g_file_new_for_path (location + 12);
      registry = flatpak_oci_registry_new_for_archive (file, cancellable, error);
      if (registry == NULL)
        return NULL;
    }
  else
    {
      flatpak_fail (error, "Can't parse image collection location %s", location);
      return NULL;
    }

  index = flatpak_oci_registry_load_index (registry, cancellable, error);
  if (index == NULL)
    return NULL;

  for (i = 0; index->manifests[i] != NULL; i++)
    {
      g_autoptr(GError) local_error = NULL;
      FlatpakOciManifestDescriptor *descriptor = index->manifests[i];
      g_autoptr(FlatpakImageSource) image_source = flatpak_image_source_new (registry, NULL,
                                                                             descriptor->parent.digest,
                                                                             cancellable, &local_error);
      if (image_source == NULL)
        {
          g_info ("Can't load manifest in image collection: %s", local_error->message);
          continue;
        }

      g_ptr_array_add (self->sources, g_steal_pointer (&image_source));
    }

  return g_steal_pointer (&self);
}

FlatpakImageSource *
flatpak_image_collection_lookup_ref (FlatpakImageCollection *self,
                                     const char             *ref)
{
  for (guint i = 0; i < self->sources->len; i++)
    {
      FlatpakImageSource *source = g_ptr_array_index (self->sources, i);
      if (strcmp (flatpak_image_source_get_ref (source), ref) == 0)
        return g_object_ref (source);
    }

  return NULL;
}

FlatpakImageSource *
flatpak_image_collection_lookup_digest (FlatpakImageCollection *self,
                                        const char             *digest)
{
  for (guint i = 0; i < self->sources->len; i++)
    {
      FlatpakImageSource *source = g_ptr_array_index (self->sources, i);
      if (strcmp (flatpak_image_source_get_digest (source), digest) == 0)
        return g_object_ref (source);
    }

  return NULL;
}

GPtrArray *
flatpak_image_collection_get_sources (FlatpakImageCollection *self)
{
  return g_ptr_array_ref (self->sources);
}
