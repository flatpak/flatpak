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

#include <string.h>

#include "flatpak-utils-private.h"
#include "flatpak-related-ref.h"
#include "flatpak-related-ref-private.h"
#include "flatpak-enum-types.h"

/**
 * SECTION:flatpak-related-ref
 * @Title: FlatpakRelatedRef
 * @Short_description: Related application reference
 *
 * A FlatpakRelatedRef provides information about a ref that is related
 * to another ref. For instance, the local extension ref of an app.
 *
 * Since: 0.6.7
 */

typedef struct _FlatpakRelatedRefPrivate FlatpakRelatedRefPrivate;

struct _FlatpakRelatedRefPrivate
{
  char   **subpaths;
  gboolean download;
  gboolean delete;
  gboolean autoprune;
};

G_DEFINE_TYPE_WITH_PRIVATE (FlatpakRelatedRef, flatpak_related_ref, FLATPAK_TYPE_REF)

enum {
  PROP_0,

  PROP_SUBPATHS,
  PROP_SHOULD_DOWNLOAD,
  PROP_SHOULD_DELETE,
  PROP_SHOULD_AUTOPRUNE,
};

static void
flatpak_related_ref_finalize (GObject *object)
{
  FlatpakRelatedRef *self = FLATPAK_RELATED_REF (object);
  FlatpakRelatedRefPrivate *priv = flatpak_related_ref_get_instance_private (self);

  g_strfreev (priv->subpaths);

  G_OBJECT_CLASS (flatpak_related_ref_parent_class)->finalize (object);
}

static void
flatpak_related_ref_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  FlatpakRelatedRef *self = FLATPAK_RELATED_REF (object);
  FlatpakRelatedRefPrivate *priv = flatpak_related_ref_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_SHOULD_DOWNLOAD:
      priv->download = g_value_get_boolean (value);
      break;

    case PROP_SHOULD_DELETE:
      priv->delete = g_value_get_boolean (value);
      break;

    case PROP_SHOULD_AUTOPRUNE:
      priv->autoprune = g_value_get_boolean (value);
      break;

    case PROP_SUBPATHS:
      g_clear_pointer (&priv->subpaths, g_strfreev);
      priv->subpaths = g_strdupv (g_value_get_boxed (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_related_ref_get_property (GObject    *object,
                                  guint       prop_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  FlatpakRelatedRef *self = FLATPAK_RELATED_REF (object);
  FlatpakRelatedRefPrivate *priv = flatpak_related_ref_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_SHOULD_DOWNLOAD:
      g_value_set_boolean (value, priv->download);
      break;

    case PROP_SHOULD_DELETE:
      g_value_set_boolean (value, priv->delete);
      break;

    case PROP_SHOULD_AUTOPRUNE:
      g_value_set_boolean (value, priv->autoprune);
      break;

    case PROP_SUBPATHS:
      g_value_set_boxed (value, priv->subpaths);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_related_ref_class_init (FlatpakRelatedRefClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = flatpak_related_ref_get_property;
  object_class->set_property = flatpak_related_ref_set_property;
  object_class->finalize = flatpak_related_ref_finalize;

  g_object_class_install_property (object_class,
                                   PROP_SHOULD_DOWNLOAD,
                                   g_param_spec_boolean ("should-download",
                                                         "Should download",
                                                         "Whether to auto-download the ref with the main ref",
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_SHOULD_DELETE,
                                   g_param_spec_boolean ("should-delete",
                                                         "Should delete",
                                                         "Whether to auto-delete the ref with the main ref",
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_SHOULD_AUTOPRUNE,
                                   g_param_spec_boolean ("should-autoprune",
                                                         "Should autoprune",
                                                         "Whether to delete when pruning unused refs",
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_SUBPATHS,
                                   g_param_spec_boxed ("subpaths",
                                                       "Subpaths",
                                                       "The subpaths for a partially installed ref",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
flatpak_related_ref_init (FlatpakRelatedRef *self)
{
}

/**
 * flatpak_related_ref_should_download:
 * @self: a #FlatpakRelatedRef
 *
 * Returns whether to auto-download the ref with the main ref.
 *
 * Returns: %TRUE if the ref should be downloaded with the main ref.
 *
 * Since: 0.6.7
 */
gboolean
flatpak_related_ref_should_download (FlatpakRelatedRef *self)
{
  FlatpakRelatedRefPrivate *priv = flatpak_related_ref_get_instance_private (self);

  return priv->download;
}

/**
 * flatpak_related_ref_should_delete:
 * @self: a #FlatpakRelatedRef
 *
 * Returns whether to auto-delete the ref with the main ref.
 *
 * Returns: %TRUE if the ref should be deleted with the main ref.
 *
 * Since: 0.6.7
 */
gboolean
flatpak_related_ref_should_delete (FlatpakRelatedRef *self)
{
  FlatpakRelatedRefPrivate *priv = flatpak_related_ref_get_instance_private (self);

  return priv->delete;
}

/**
 * flatpak_related_ref_should_autoprune:
 * @self: a #FlatpakRelatedRef
 *
 * Returns whether to delete when pruning unused refs.
 *
 * Returns: %TRUE if the ref should be considered unused when pruning.
 *
 * Since: 0.11.8
 */
gboolean
flatpak_related_ref_should_autoprune (FlatpakRelatedRef *self)
{
  FlatpakRelatedRefPrivate *priv = flatpak_related_ref_get_instance_private (self);

  return priv->autoprune;
}

/**
 * flatpak_related_ref_get_subpaths:
 * @self: a #FlatpakRelatedRef
 *
 * Returns the subpaths that should be installed/updated for the ref.
 * This returns %NULL if all files should be installed.
 *
 * Returns: (transfer none): A strv, or %NULL
 *
 * Since: 0.6.7
 */
const char * const *
flatpak_related_ref_get_subpaths (FlatpakRelatedRef *self)
{
  FlatpakRelatedRefPrivate *priv = flatpak_related_ref_get_instance_private (self);

  return (const char * const *) priv->subpaths;
}

/**
 * flatpak_related_ref_new:
 * @full_ref: a full ref to refer to
 * @commit: (nullable): a commit ID to refer to
 * @subpaths: (nullable): a nul-terminated array of subpaths
 * @download: whether to auto-download the ref with the main ref
 * @delete: whether to auto-delete the ref with the main ref
 *
 * Creates a new FlatpakRelatedRef object.
 *
 * Returns: a new ref
 */
FlatpakRelatedRef *
flatpak_related_ref_new (const char *full_ref,
                         const char *commit,
                         char      **subpaths,
                         gboolean    download,
                         gboolean    delete)
{
  FlatpakRefKind kind = FLATPAK_REF_KIND_APP;
  FlatpakRelatedRef *ref;
  g_auto(GStrv) parts = NULL;

  parts = g_strsplit (full_ref, "/", -1);

  if (strcmp (parts[0], "app") != 0)
    kind = FLATPAK_REF_KIND_RUNTIME;

  /* Canonicalize the "no subpaths" case */
  if (subpaths && *subpaths == NULL)
    subpaths = NULL;

  ref = g_object_new (FLATPAK_TYPE_RELATED_REF,
                      "kind", kind,
                      "name", parts[1],
                      "arch", parts[2],
                      "branch", parts[3],
                      "commit", commit,
                      "subpaths", subpaths,
                      "should-download", download,
                      "should-delete", delete,
                      NULL);

  return ref;
}
