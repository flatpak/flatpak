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
#include "flatpak-remote-ref-private.h"
#include "flatpak-remote-ref.h"
#include "flatpak-enum-types.h"
#include "flatpak-variant-impl-private.h"

/**
 * SECTION:flatpak-remote-ref
 * @Title: FlatpakRemoteRef
 * @Short_description: Remote application reference
 *
 * A FlatpakRemoteRef provides information about an application or runtime
 * (in short: ref) that is available from a remote repository.
 */
typedef struct _FlatpakRemoteRefPrivate FlatpakRemoteRefPrivate;

struct _FlatpakRemoteRefPrivate
{
  char   *remote_name;
  guint64 installed_size;
  guint64 download_size;
  GBytes *metadata;
  char   *eol;
  char   *eol_rebase;
};

G_DEFINE_TYPE_WITH_PRIVATE (FlatpakRemoteRef, flatpak_remote_ref, FLATPAK_TYPE_REF)

enum {
  PROP_0,

  PROP_REMOTE_NAME,
  PROP_INSTALLED_SIZE,
  PROP_DOWNLOAD_SIZE,
  PROP_METADATA,
  PROP_EOL,
  PROP_EOL_REBASE,
};

static void
flatpak_remote_ref_finalize (GObject *object)
{
  FlatpakRemoteRef *self = FLATPAK_REMOTE_REF (object);
  FlatpakRemoteRefPrivate *priv = flatpak_remote_ref_get_instance_private (self);

  g_free (priv->remote_name);
  g_free (priv->eol);
  g_free (priv->eol_rebase);
  g_clear_pointer (&priv->metadata, g_bytes_unref);

  G_OBJECT_CLASS (flatpak_remote_ref_parent_class)->finalize (object);
}

static void
flatpak_remote_ref_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  FlatpakRemoteRef *self = FLATPAK_REMOTE_REF (object);
  FlatpakRemoteRefPrivate *priv = flatpak_remote_ref_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_REMOTE_NAME:
      g_clear_pointer (&priv->remote_name, g_free);
      priv->remote_name = g_value_dup_string (value);
      break;

    case PROP_INSTALLED_SIZE:
      priv->installed_size = g_value_get_uint64 (value);
      break;

    case PROP_DOWNLOAD_SIZE:
      priv->download_size = g_value_get_uint64 (value);
      break;

    case PROP_METADATA:
      g_clear_pointer (&priv->metadata, g_bytes_unref);
      priv->metadata = g_value_get_boxed (value) ? g_bytes_ref (g_value_get_boxed (value)) : NULL;
      break;

    case PROP_EOL:
      g_clear_pointer (&priv->eol, g_free);
      priv->eol = g_value_dup_string (value);
      break;

    case PROP_EOL_REBASE:
      g_clear_pointer (&priv->eol_rebase, g_free);
      priv->eol_rebase = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_remote_ref_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  FlatpakRemoteRef *self = FLATPAK_REMOTE_REF (object);
  FlatpakRemoteRefPrivate *priv = flatpak_remote_ref_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_REMOTE_NAME:
      g_value_set_string (value, priv->remote_name);
      break;

    case PROP_INSTALLED_SIZE:
      g_value_set_uint64 (value, priv->installed_size);
      break;

    case PROP_DOWNLOAD_SIZE:
      g_value_set_uint64 (value, priv->installed_size);
      break;

    case PROP_METADATA:
      g_value_set_boxed (value, priv->metadata);
      break;

    case PROP_EOL:
      g_value_set_string (value, priv->eol);
      break;

    case PROP_EOL_REBASE:
      g_value_set_string (value, priv->eol_rebase);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_remote_ref_class_init (FlatpakRemoteRefClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = flatpak_remote_ref_get_property;
  object_class->set_property = flatpak_remote_ref_set_property;
  object_class->finalize = flatpak_remote_ref_finalize;

  g_object_class_install_property (object_class,
                                   PROP_REMOTE_NAME,
                                   g_param_spec_string ("remote-name",
                                                        "Remote Name",
                                                        "The name of the remote",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_INSTALLED_SIZE,
                                   g_param_spec_uint64 ("installed-size",
                                                        "Installed Size",
                                                        "The installed size of the application",
                                                        0, G_MAXUINT64, 0,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_DOWNLOAD_SIZE,
                                   g_param_spec_uint64 ("download-size",
                                                        "Download Size",
                                                        "The download size of the application",
                                                        0, G_MAXUINT64, 0,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_METADATA,
                                   g_param_spec_boxed ("metadata",
                                                       "Metadata",
                                                       "The metadata info for the application",
                                                       G_TYPE_BYTES,
                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_EOL,
                                   g_param_spec_string ("end-of-life",
                                                        "End of life",
                                                        "The reason for the ref to be end of life",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_EOL_REBASE,
                                   g_param_spec_string ("end-of-life-rebase",
                                                        "End of life rebase",
                                                        "The new ref for the end of lifeed ref",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
flatpak_remote_ref_init (FlatpakRemoteRef *self)
{
}

/**
 * flatpak_remote_ref_get_remote_name:
 * @self: a #FlatpakRemoteRef
 *
 * Gets the remote name of the ref.
 *
 * Returns: (transfer none): the remote name
 */
const char *
flatpak_remote_ref_get_remote_name (FlatpakRemoteRef *self)
{
  FlatpakRemoteRefPrivate *priv = flatpak_remote_ref_get_instance_private (self);

  return priv->remote_name;
}

/**
 * flatpak_remote_ref_get_installed_size:
 * @self: a #FlatpakRemoteRef
 *
 * Returns the installed size of the ref.
 *
 * Returns: the installed size
 */
guint64
flatpak_remote_ref_get_installed_size (FlatpakRemoteRef *self)
{
  FlatpakRemoteRefPrivate *priv = flatpak_remote_ref_get_instance_private (self);

  return priv->installed_size;
}

/**
 * flatpak_remote_ref_get_download_size:
 * @self: a #FlatpakRemoteRef
 *
 * Returns the download size of the ref.
 *
 * Returns: the download size
 */
guint64
flatpak_remote_ref_get_download_size (FlatpakRemoteRef *self)
{
  FlatpakRemoteRefPrivate *priv = flatpak_remote_ref_get_instance_private (self);

  return priv->download_size;
}

/**
 * flatpak_remote_ref_get_metadata:
 * @self: a #FlatpakRemoteRef
 *
 * Returns the app metadata from the metadata cache of the ref.
 *
 * Returns: (transfer none) (nullable): a #GBytes with the metadata file
 * contents or %NULL
 */
GBytes *
flatpak_remote_ref_get_metadata (FlatpakRemoteRef *self)
{
  FlatpakRemoteRefPrivate *priv = flatpak_remote_ref_get_instance_private (self);

  return priv->metadata;
}

/**
 * flatpak_remote_ref_get_eol:
 * @self: a #FlatpakRemoteRef
 *
 * Returns the end-of-life reason string, or %NULL if the
 * ref is not end-of-lifed.
 *
 * Returns: (transfer none): the end-of-life reason or %NULL
 */
const char *
flatpak_remote_ref_get_eol (FlatpakRemoteRef *self)
{
  FlatpakRemoteRefPrivate *priv = flatpak_remote_ref_get_instance_private (self);

  return priv->eol;
}

/**
 * flatpak_remote_ref_get_eol_rebase:
 * @self: a #FlatpakRemoteRef
 *
 * Returns the end-of-life rebased ref, or %NULL if the
 * ref is not end-of-lifed.
 *
 * Returns: (transfer none): the end-of-life rebased ref or %NULL
 */
const char *
flatpak_remote_ref_get_eol_rebase (FlatpakRemoteRef *self)
{
  FlatpakRemoteRefPrivate *priv = flatpak_remote_ref_get_instance_private (self);

  return priv->eol_rebase;
}

FlatpakRemoteRef *
flatpak_remote_ref_new (FlatpakDecomposed   *decomposed,
                        const char          *commit,
                        const char          *remote_name,
                        const char          *collection_id,
                        FlatpakRemoteState  *state)
{
  guint64 download_size = 0, installed_size = 0;
  g_autofree char *metadata = NULL;
  g_autoptr(GBytes) metadata_bytes = NULL;
  VarMetadataRef sparse_cache;
  const char *eol = NULL;
  const char *eol_rebase = NULL;
  FlatpakRemoteRef *ref;

  if (collection_id == NULL)
    collection_id = flatpak_decomposed_get_collection_id (decomposed);

  if (state &&
      !flatpak_remote_state_load_data (state, flatpak_decomposed_get_ref (decomposed),
                                       &download_size, &installed_size, &metadata,
                                       NULL))
    {
      g_info ("Can't find metadata for ref %s", flatpak_decomposed_get_ref (decomposed));
    }

  if (metadata)
    {
      metadata_bytes = g_bytes_new_take (metadata, strlen (metadata));
      metadata = NULL; /* steal */
    }

  if (state &&
      flatpak_remote_state_lookup_sparse_cache (state, flatpak_decomposed_get_ref (decomposed), &sparse_cache, NULL))
    {
      eol = var_metadata_lookup_string (sparse_cache, FLATPAK_SPARSE_CACHE_KEY_ENDOFLIFE, NULL);
      eol_rebase = var_metadata_lookup_string (sparse_cache, FLATPAK_SPARSE_CACHE_KEY_ENDOFLIFE_REBASE, NULL);
    }

  ref = g_object_new (FLATPAK_TYPE_REMOTE_REF,
                      "kind", flatpak_decomposed_get_kind (decomposed),
                      "name", flatpak_decomposed_peek_id (decomposed, NULL),
                      "arch", flatpak_decomposed_peek_arch (decomposed, NULL),
                      "branch", flatpak_decomposed_peek_branch (decomposed, NULL),
                      "commit", commit,
                      "remote-name", remote_name,
                      "collection-id", collection_id,
                      "installed-size", installed_size,
                      "download-size", download_size,
                      "metadata", metadata_bytes,
                      "end-of-life", eol,
                      "end-of-life-rebase", eol_rebase,
                      NULL);

  return ref;
}
