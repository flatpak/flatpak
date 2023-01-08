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

#include <string.h>

#include "flatpak-utils-private.h"
#include "flatpak-bundle-ref.h"
#include "flatpak-enum-types.h"

/**
 * SECTION:flatpak-bundle-ref
 * @Title: FlatpakBundleRef
 * @Short_description: Application bundle reference
 *
 * A FlatpakBundleRef refers to a single-file bundle containing an
 * application or runtime.
 */

typedef struct _FlatpakBundleRefPrivate FlatpakBundleRefPrivate;

struct _FlatpakBundleRefPrivate
{
  GFile  *file;
  char   *origin;
  char   *runtime_repo;
  GBytes *metadata;
  GBytes *appstream;
  GBytes *icon_64;
  GBytes *icon_128;
  guint64 installed_size;
};

G_DEFINE_TYPE_WITH_PRIVATE (FlatpakBundleRef, flatpak_bundle_ref, FLATPAK_TYPE_REF)

enum {
  PROP_0,

  PROP_FILE,
};

static void
flatpak_bundle_ref_finalize (GObject *object)
{
  FlatpakBundleRef *self = FLATPAK_BUNDLE_REF (object);
  FlatpakBundleRefPrivate *priv = flatpak_bundle_ref_get_instance_private (self);

  g_clear_object (&priv->file);

  g_bytes_unref (priv->metadata);
  g_bytes_unref (priv->appstream);
  g_bytes_unref (priv->icon_64);
  g_bytes_unref (priv->icon_128);
  g_free (priv->origin);
  g_free (priv->runtime_repo);

  G_OBJECT_CLASS (flatpak_bundle_ref_parent_class)->finalize (object);
}

static void
flatpak_bundle_ref_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  FlatpakBundleRef *self = FLATPAK_BUNDLE_REF (object);
  FlatpakBundleRefPrivate *priv = flatpak_bundle_ref_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_FILE:
      g_set_object (&priv->file, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_bundle_ref_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  FlatpakBundleRef *self = FLATPAK_BUNDLE_REF (object);
  FlatpakBundleRefPrivate *priv = flatpak_bundle_ref_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_FILE:
      g_value_set_object (value, priv->file);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_bundle_ref_class_init (FlatpakBundleRefClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = flatpak_bundle_ref_get_property;
  object_class->set_property = flatpak_bundle_ref_set_property;
  object_class->finalize = flatpak_bundle_ref_finalize;

  /**
   * FlatpakBundleRef:file:
   *
   * The bundle file that this ref refers to.
   */
  g_object_class_install_property (object_class,
                                   PROP_FILE,
                                   g_param_spec_object ("file",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
flatpak_bundle_ref_init (FlatpakBundleRef *self)
{
}

/**
 * flatpak_bundle_ref_get_file:
 * @self: a #FlatpakBundleRef
 *
 * Get the file this bundle is stored in.
 *
 * Returns: (transfer full) : an #GFile
 */
GFile *
flatpak_bundle_ref_get_file (FlatpakBundleRef *self)
{
  FlatpakBundleRefPrivate *priv = flatpak_bundle_ref_get_instance_private (self);

  return g_object_ref (priv->file);
}

/**
 * flatpak_bundle_ref_get_metadata:
 * @self: a #FlatpakBundleRef
 *
 * Get the metadata for the app/runtime
 *
 * Returns: (transfer full) : an #GBytes with the metadata contents, or %NULL
 */
GBytes *
flatpak_bundle_ref_get_metadata (FlatpakBundleRef *self)
{
  FlatpakBundleRefPrivate *priv = flatpak_bundle_ref_get_instance_private (self);

  if (priv->metadata)
    return g_bytes_ref (priv->metadata);
  return NULL;
}

/**
 * flatpak_bundle_ref_get_appstream:
 * @self: a #FlatpakBundleRef
 *
 * Get the compressed appstream for the app/runtime
 *
 * Returns: (transfer full) : an #GBytes with the appstream contents, or %NULL
 */
GBytes *
flatpak_bundle_ref_get_appstream (FlatpakBundleRef *self)
{
  FlatpakBundleRefPrivate *priv = flatpak_bundle_ref_get_instance_private (self);

  if (priv->appstream)
    return g_bytes_ref (priv->appstream);
  return NULL;
}

/**
 * flatpak_bundle_ref_get_icon:
 * @self: a #FlatpakBundleRef
 * @size: 64 or 128
 *
 * Get the icon png data for the app/runtime
 *
 * Returns: (transfer full) : an #GBytes with png contents
 */
GBytes *
flatpak_bundle_ref_get_icon (FlatpakBundleRef *self,
                             int               size)
{
  FlatpakBundleRefPrivate *priv = flatpak_bundle_ref_get_instance_private (self);

  if (size == 64 && priv->icon_64)
    return g_bytes_ref (priv->icon_64);

  if (size == 128 && priv->icon_128)
    return g_bytes_ref (priv->icon_128);

  return NULL;
}

/**
 * flatpak_bundle_ref_get_origin:
 * @self: a #FlatpakBundleRef
 *
 * Get the origin url stored in the bundle
 *
 * Returns: (transfer full) : an url string, or %NULL
 */
char *
flatpak_bundle_ref_get_origin (FlatpakBundleRef *self)
{
  FlatpakBundleRefPrivate *priv = flatpak_bundle_ref_get_instance_private (self);

  return g_strdup (priv->origin);
}


/**
 * flatpak_bundle_ref_get_runtime_repo_url:
 * @self: a #FlatpakBundleRef
 *
 * Get the runtime flatpakrepo url stored in the bundle (if any)
 *
 * Returns: (transfer full) : an url string, or %NULL
 *
 * Since: 0.8.0
 */
char *
flatpak_bundle_ref_get_runtime_repo_url (FlatpakBundleRef *self)
{
  FlatpakBundleRefPrivate *priv = flatpak_bundle_ref_get_instance_private (self);

  return g_strdup (priv->runtime_repo);
}

/**
 * flatpak_bundle_ref_get_installed_size:
 * @self: a FlatpakBundleRef
 *
 * Returns the installed size for the bundle.
 *
 * Returns: the installed size
 */
guint64
flatpak_bundle_ref_get_installed_size (FlatpakBundleRef *self)
{
  FlatpakBundleRefPrivate *priv = flatpak_bundle_ref_get_instance_private (self);

  return priv->installed_size;
}

/**
 * flatpak_bundle_ref_new:
 * @file: a #GFile
 * @error: (allow-none): return location for an error
 *
 * Creates a new bundle ref for the given file.
 *
 * Returns: a new bundle ref.
 */
FlatpakBundleRef *
flatpak_bundle_ref_new (GFile   *file,
                        GError **error)
{
  FlatpakRefKind kind;
  FlatpakBundleRefPrivate *priv;
  FlatpakBundleRef *ref;
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *commit = NULL;
  g_autofree char *id = NULL;
  g_autofree char *arch = NULL;
  g_autofree char *branch = NULL;
  g_autoptr(FlatpakDecomposed) full_ref = NULL;
  g_autofree char *origin = NULL;
  g_autofree char *runtime_repo = NULL;
  g_autofree char *metadata_contents = NULL;
  g_autoptr(GVariant) appstream = NULL;
  g_autoptr(GVariant) icon_64 = NULL;
  g_autoptr(GVariant) icon_128 = NULL;
  guint64 installed_size;
  g_autofree char *collection_id = NULL;

  metadata = flatpak_bundle_load (file, &commit, &full_ref, &origin, &runtime_repo, &metadata_contents, &installed_size,
                                  NULL, &collection_id, error);
  if (metadata == NULL)
    return NULL;

  kind = flatpak_decomposed_get_kind (full_ref);
  id = flatpak_decomposed_dup_id (full_ref);
  arch = flatpak_decomposed_dup_arch (full_ref);
  branch = flatpak_decomposed_dup_branch (full_ref);

  ref = g_object_new (FLATPAK_TYPE_BUNDLE_REF,
                      "kind", kind,
                      "name", id,
                      "arch", arch,
                      "branch", branch,
                      "commit", commit,
                      "file", file,
                      "collection-id", collection_id,
                      NULL);
  priv = flatpak_bundle_ref_get_instance_private (ref);

  if (metadata_contents)
    priv->metadata = g_bytes_new_take (metadata_contents,
                                       strlen (metadata_contents));
  metadata_contents = NULL; /* Stolen */

  appstream = g_variant_lookup_value (metadata, "appdata", G_VARIANT_TYPE_BYTESTRING);
  if (appstream)
    priv->appstream = g_variant_get_data_as_bytes (appstream);

  icon_64 = g_variant_lookup_value (metadata, "icon-64", G_VARIANT_TYPE_BYTESTRING);
  if (icon_64)
    priv->icon_64 = g_variant_get_data_as_bytes (icon_64);

  icon_128 = g_variant_lookup_value (metadata, "icon-128", G_VARIANT_TYPE_BYTESTRING);
  if (icon_128)
    priv->icon_128 = g_variant_get_data_as_bytes (icon_128);

  priv->installed_size = installed_size;

  priv->origin = g_steal_pointer (&origin);
  priv->runtime_repo = g_steal_pointer (&runtime_repo);

  return ref;
}
