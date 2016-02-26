/*
 * Copyright © 2015 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#include "xdg-app-utils.h"
#include "xdg-app-bundle-ref.h"
#include "xdg-app-enum-types.h"

typedef struct _XdgAppBundleRefPrivate XdgAppBundleRefPrivate;

struct _XdgAppBundleRefPrivate
{
  GFile *file;
  char *origin;
  GBytes *metadata;
  GBytes *appstream;
  GBytes *icon_64;
  GBytes *icon_128;
  guint64 installed_size;
};

G_DEFINE_TYPE_WITH_PRIVATE (XdgAppBundleRef, xdg_app_bundle_ref, XDG_APP_TYPE_REF)

enum {
  PROP_0,

  PROP_FILE,
};

static void
xdg_app_bundle_ref_finalize (GObject *object)
{
  XdgAppBundleRef *self = XDG_APP_BUNDLE_REF (object);
  XdgAppBundleRefPrivate *priv = xdg_app_bundle_ref_get_instance_private (self);

  g_clear_object (&priv->file);

  g_bytes_unref (priv->metadata);
  g_bytes_unref (priv->appstream);
  g_bytes_unref (priv->icon_64);
  g_bytes_unref (priv->icon_128);
  g_free (priv->origin);

  G_OBJECT_CLASS (xdg_app_bundle_ref_parent_class)->finalize (object);
}

static void
xdg_app_bundle_ref_set_property (GObject         *object,
                                 guint            prop_id,
                                 const GValue    *value,
                                 GParamSpec      *pspec)
{
  XdgAppBundleRef *self = XDG_APP_BUNDLE_REF (object);
  XdgAppBundleRefPrivate *priv = xdg_app_bundle_ref_get_instance_private (self);

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
xdg_app_bundle_ref_get_property (GObject         *object,
                                 guint            prop_id,
                                 GValue          *value,
                                 GParamSpec      *pspec)
{
  XdgAppBundleRef *self = XDG_APP_BUNDLE_REF (object);
  XdgAppBundleRefPrivate *priv = xdg_app_bundle_ref_get_instance_private (self);

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
xdg_app_bundle_ref_class_init (XdgAppBundleRefClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = xdg_app_bundle_ref_get_property;
  object_class->set_property = xdg_app_bundle_ref_set_property;
  object_class->finalize = xdg_app_bundle_ref_finalize;

  g_object_class_install_property (object_class,
                                   PROP_FILE,
                                   g_param_spec_object ("file",
                                                        "",
                                                        "",
                                                        G_TYPE_FILE,
                                                        G_PARAM_READWRITE));
}

static void
xdg_app_bundle_ref_init (XdgAppBundleRef *self)
{
}

/**
 * xdg_app_bundle_ref_get_file:
 * @self: a #XdgAppInstallation
 *
 * Get the file this bundle is stored in.
 *
 * Returns: (transfer full) : an #GFile
 */
GFile *
xdg_app_bundle_ref_get_file (XdgAppBundleRef *self)
{
  XdgAppBundleRefPrivate *priv = xdg_app_bundle_ref_get_instance_private (self);

  return g_object_ref (priv->file);
}

/**
 * xdg_app_bundle_ref_get_metadata:
 * @self: a #XdgAppInstallation
 *
 * Get the metadata for the app/runtime
 *
 * Returns: (transfer full) : an #GBytes with the metadata contents, or %NULL
 */
GBytes *
xdg_app_bundle_ref_get_metadata (XdgAppBundleRef  *self)
{
  XdgAppBundleRefPrivate *priv = xdg_app_bundle_ref_get_instance_private (self);

  if (priv->metadata)
    return g_bytes_ref (priv->metadata);
  return NULL;
}

/**
 * xdg_app_bundle_ref_get_appstream:
 * @self: a #XdgAppInstallation
 *
 * Get the compressed appstream for the app/runtime
 *
 * Returns: (transfer full) : an #GBytes with the appstream contents, or %NULL
 */
GBytes *
xdg_app_bundle_ref_get_appstream (XdgAppBundleRef  *self)
{
  XdgAppBundleRefPrivate *priv = xdg_app_bundle_ref_get_instance_private (self);

  if (priv->appstream)
    return g_bytes_ref (priv->appstream);
  return NULL;
}

/**
 * xdg_app_bundle_ref_get_icon:
 * @self: a #XdgAppInstallation
 * @size: 64 or 128
 *
 * Get the icon png data for the app/runtime
 *
 * Returns: (transfer full) : an #GBytes with png contents
 */
GBytes *
xdg_app_bundle_ref_get_icon (XdgAppBundleRef  *self,
                             int               size)
{
  XdgAppBundleRefPrivate *priv = xdg_app_bundle_ref_get_instance_private (self);

  if (size == 64 && priv->icon_64)
    return g_bytes_ref (priv->icon_64);

  if (size == 128 && priv->icon_128)
    return g_bytes_ref (priv->icon_128);

  return NULL;
}

/**
 * xdg_app_bundle_ref_get_origin:
 * @self: a #XdgAppInstallation
 *
 * Get the origin url stored in the bundle
 *
 * Returns: (transfer full) : an url string, or %NULL
 */
char *
xdg_app_bundle_ref_get_origin (XdgAppBundleRef  *self)
{
  XdgAppBundleRefPrivate *priv = xdg_app_bundle_ref_get_instance_private (self);

  return g_strdup (priv->origin);
}

guint64
xdg_app_bundle_ref_get_installed_size (XdgAppBundleRef  *self)
{
  XdgAppBundleRefPrivate *priv = xdg_app_bundle_ref_get_instance_private (self);

  return priv->installed_size;
}


XdgAppBundleRef *
xdg_app_bundle_ref_new (GFile *file,
                        GError **error)
{
  XdgAppRefKind kind = XDG_APP_REF_KIND_APP;
  XdgAppBundleRefPrivate *priv;
  g_auto(GStrv) parts = NULL;
  XdgAppBundleRef *ref;
  g_autoptr(GVariant) metadata = NULL;
  g_autofree char *commit = NULL;
  g_autofree char *full_ref = NULL;
  g_autofree char *origin = NULL;
  g_autofree char *metadata_contents = NULL;
  g_autoptr(GVariant) appstream = NULL;
  g_autoptr(GVariant) icon_64 = NULL;
  g_autoptr(GVariant) icon_128 = NULL;
  guint64 installed_size;

  metadata = xdg_app_bundle_load (file, &commit, &full_ref, &origin, &installed_size,
                                  NULL, error);
  if (metadata == NULL)
    return NULL;

  parts = xdg_app_decompose_ref (full_ref, error);
  if (parts == NULL)
    return NULL;

  if (!g_variant_lookup (metadata, "metadata", "s", &metadata_contents))
    metadata_contents = NULL;

  if (strcmp (parts[0], "app") != 0)
    kind = XDG_APP_REF_KIND_RUNTIME;

  ref = g_object_new (XDG_APP_TYPE_BUNDLE_REF,
                      "kind", kind,
                      "name", parts[1],
                      "arch", parts[2],
                      "branch", parts[3],
                      "commit", commit,
                      "file", file,
                      NULL);
  priv = xdg_app_bundle_ref_get_instance_private (ref);

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

  return ref;
}
