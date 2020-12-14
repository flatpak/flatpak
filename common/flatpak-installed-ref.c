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
#include "flatpak-installed-ref.h"
#include "flatpak-installed-ref-private.h"
#include "flatpak-enum-types.h"

/**
 * SECTION:flatpak-installed-ref
 * @Title: FlatpakInstalledRef
 * @Short_description: Installed application reference
 *
 * A FlatpakInstalledRef provides information about an installed
 * application or runtime (in short: ref), such as the available
 * builds, its size, location, etc.
 */

typedef struct _FlatpakInstalledRefPrivate FlatpakInstalledRefPrivate;

struct _FlatpakInstalledRefPrivate
{
  gboolean is_current;
  char    *origin;
  char    *latest_commit;
  char    *deploy_dir;
  char   **subpaths;
  guint64  installed_size;
  char    *eol;
  char    *eol_rebase;
  char    *appdata_name;
  char    *appdata_summary;
  char    *appdata_version;
  char    *appdata_license;
  char    *appdata_content_rating_type;
  GHashTable *appdata_content_rating;  /* (element-type interned-utf8 interned-utf8) */
};

G_DEFINE_TYPE_WITH_PRIVATE (FlatpakInstalledRef, flatpak_installed_ref, FLATPAK_TYPE_REF)

enum {
  PROP_0,

  PROP_IS_CURRENT,
  PROP_ORIGIN,
  PROP_LATEST_COMMIT,
  PROP_DEPLOY_DIR,
  PROP_INSTALLED_SIZE,
  PROP_SUBPATHS,
  PROP_EOL,
  PROP_EOL_REBASE,
  PROP_APPDATA_NAME,
  PROP_APPDATA_SUMMARY,
  PROP_APPDATA_VERSION,
  PROP_APPDATA_LICENSE,
  PROP_APPDATA_CONTENT_RATING_TYPE,
  PROP_APPDATA_CONTENT_RATING,
};

static void
flatpak_installed_ref_finalize (GObject *object)
{
  FlatpakInstalledRef *self = FLATPAK_INSTALLED_REF (object);
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  g_free (priv->origin);
  g_free (priv->latest_commit);
  g_free (priv->deploy_dir);
  g_strfreev (priv->subpaths);
  g_free (priv->eol);
  g_free (priv->eol_rebase);
  g_free (priv->appdata_name);
  g_free (priv->appdata_summary);
  g_free (priv->appdata_version);
  g_free (priv->appdata_license);
  g_free (priv->appdata_content_rating_type);
  g_clear_pointer (&priv->appdata_content_rating, g_hash_table_unref);

  G_OBJECT_CLASS (flatpak_installed_ref_parent_class)->finalize (object);
}

static void
flatpak_installed_ref_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  FlatpakInstalledRef *self = FLATPAK_INSTALLED_REF (object);
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_IS_CURRENT:
      priv->is_current = g_value_get_boolean (value);
      break;

    case PROP_INSTALLED_SIZE:
      priv->installed_size = g_value_get_uint64 (value);
      break;

    case PROP_ORIGIN:
      g_clear_pointer (&priv->origin, g_free);
      priv->origin = g_value_dup_string (value);
      break;

    case PROP_LATEST_COMMIT:
      g_clear_pointer (&priv->latest_commit, g_free);
      priv->latest_commit = g_value_dup_string (value);
      break;

    case PROP_DEPLOY_DIR:
      g_clear_pointer (&priv->deploy_dir, g_free);
      priv->deploy_dir = g_value_dup_string (value);
      break;

    case PROP_SUBPATHS:
      g_clear_pointer (&priv->subpaths, g_strfreev);
      priv->subpaths = g_strdupv (g_value_get_boxed (value));
      break;

    case PROP_EOL:
      g_clear_pointer (&priv->eol, g_free);
      priv->eol = g_value_dup_string (value);
      break;

    case PROP_EOL_REBASE:
      g_clear_pointer (&priv->eol_rebase, g_free);
      priv->eol_rebase = g_value_dup_string (value);
      break;

    case PROP_APPDATA_NAME:
      g_clear_pointer (&priv->appdata_name, g_free);
      priv->appdata_name = g_value_dup_string (value);
      break;

    case PROP_APPDATA_SUMMARY:
      g_clear_pointer (&priv->appdata_summary, g_free);
      priv->appdata_summary = g_value_dup_string (value);
      break;

    case PROP_APPDATA_VERSION:
      g_clear_pointer (&priv->appdata_version, g_free);
      priv->appdata_version = g_value_dup_string (value);
      break;

    case PROP_APPDATA_LICENSE:
      g_clear_pointer (&priv->appdata_license, g_free);
      priv->appdata_license = g_value_dup_string (value);
      break;

    case PROP_APPDATA_CONTENT_RATING_TYPE:
      g_clear_pointer (&priv->appdata_content_rating_type, g_free);
      priv->appdata_content_rating_type = g_value_dup_string (value);
      break;

    case PROP_APPDATA_CONTENT_RATING:
      g_clear_pointer (&priv->appdata_content_rating, g_hash_table_unref);
      priv->appdata_content_rating = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_installed_ref_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  FlatpakInstalledRef *self = FLATPAK_INSTALLED_REF (object);
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_IS_CURRENT:
      g_value_set_boolean (value, priv->is_current);
      break;

    case PROP_INSTALLED_SIZE:
      g_value_set_uint64 (value, priv->installed_size);
      break;

    case PROP_ORIGIN:
      g_value_set_string (value, priv->origin);
      break;

    case PROP_LATEST_COMMIT:
      g_value_set_string (value, priv->latest_commit);
      break;

    case PROP_DEPLOY_DIR:
      g_value_set_string (value, priv->deploy_dir);
      break;

    case PROP_SUBPATHS:
      g_value_set_boxed (value, priv->subpaths);
      break;

    case PROP_EOL:
      g_value_set_string (value, priv->eol);
      break;

    case PROP_EOL_REBASE:
      g_value_set_string (value, priv->eol_rebase);
      break;

    case PROP_APPDATA_NAME:
      g_value_set_string (value, priv->appdata_name);
      break;

    case PROP_APPDATA_SUMMARY:
      g_value_set_string (value, priv->appdata_summary);
      break;

    case PROP_APPDATA_VERSION:
      g_value_set_string (value, priv->appdata_version);
      break;

    case PROP_APPDATA_LICENSE:
      g_value_set_string (value, priv->appdata_license);
      break;

    case PROP_APPDATA_CONTENT_RATING_TYPE:
      g_value_set_string (value, priv->appdata_content_rating_type);
      break;

    case PROP_APPDATA_CONTENT_RATING:
      g_value_set_boxed (value, priv->appdata_content_rating);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_installed_ref_class_init (FlatpakInstalledRefClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = flatpak_installed_ref_get_property;
  object_class->set_property = flatpak_installed_ref_set_property;
  object_class->finalize = flatpak_installed_ref_finalize;

  g_object_class_install_property (object_class,
                                   PROP_IS_CURRENT,
                                   g_param_spec_boolean ("is-current",
                                                         "Is Current",
                                                         "Whether the application is current",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_INSTALLED_SIZE,
                                   g_param_spec_uint64 ("installed-size",
                                                        "Installed Size",
                                                        "The installed size of the application",
                                                        0, G_MAXUINT64, 0,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ORIGIN,
                                   g_param_spec_string ("origin",
                                                        "Origin",
                                                        "The origin",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_LATEST_COMMIT,
                                   g_param_spec_string ("latest-commit",
                                                        "Latest Commit",
                                                        "The latest commit",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_DEPLOY_DIR,
                                   g_param_spec_string ("deploy-dir",
                                                        "Deploy Dir",
                                                        "Where the application is installed",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_SUBPATHS,
                                   g_param_spec_boxed ("subpaths",
                                                       "Subpaths",
                                                       "The subpaths for a partially installed ref",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
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
                                                        "The new ref for the end-of-lifed ref",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_APPDATA_NAME,
                                   g_param_spec_string ("appdata-name",
                                                        "Appdata Name",
                                                        "The localized name field from the appdata",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_APPDATA_SUMMARY,
                                   g_param_spec_string ("appdata-summary",
                                                        "Appdata Summary",
                                                        "The localized summary field from the appdata",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_APPDATA_VERSION,
                                   g_param_spec_string ("appdata-version",
                                                        "Appdata Version",
                                                        "The default version field from the appdata",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_APPDATA_LICENSE,
                                   g_param_spec_string ("appdata-license",
                                                        "Appdata License",
                                                        "The license from the appdata",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_APPDATA_CONTENT_RATING_TYPE,
                                   g_param_spec_string ("appdata-content-rating-type",
                                                        "Appdata Content Rating Type",
                                                        "The type of the content rating data from the appdata",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class,
                                   PROP_APPDATA_CONTENT_RATING,
                                   g_param_spec_boxed ("appdata-content-rating",
                                                       "Appdata Content Rating",
                                                       "The content rating data from the appdata",
                                                       G_TYPE_HASH_TABLE,
                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
flatpak_installed_ref_init (FlatpakInstalledRef *self)
{
}

/**
 * flatpak_installed_ref_get_origin:
 * @self: a #FlatpakInstalledRef
 *
 * Gets the origin of the ref.
 *
 * Returns: (transfer none): the origin
 */
const char *
flatpak_installed_ref_get_origin (FlatpakInstalledRef *self)
{
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  return priv->origin;
}

/**
 * flatpak_installed_ref_get_latest_commit:
 * @self: a #FlatpakInstalledRef
 *
 * Gets the latest commit of the ref.
 *
 * Returns: (transfer none) (nullable): the latest commit
 */
const char *
flatpak_installed_ref_get_latest_commit (FlatpakInstalledRef *self)
{
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  return priv->latest_commit;
}

/**
 * flatpak_installed_ref_get_deploy_dir:
 * @self: a #FlatpakInstalledRef
 *
 * Gets the deploy dir of the ref.
 *
 * Returns: (transfer none): the deploy dir
 */
const char *
flatpak_installed_ref_get_deploy_dir (FlatpakInstalledRef *self)
{
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  return priv->deploy_dir;
}

/**
 * flatpak_installed_ref_get_is_current:
 * @self: a #FlatpakInstalledRef
 *
 * Returns whether the ref is current.
 *
 * Returns: %TRUE if the ref is current
 */
gboolean
flatpak_installed_ref_get_is_current (FlatpakInstalledRef *self)
{
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  return priv->is_current;
}

/**
 * flatpak_installed_ref_get_subpaths:
 * @self: a #FlatpakInstalledRef
 *
 * Returns the subpaths that are installed, or %NULL if all files installed.
 *
 * Returns: (transfer none): A strv, or %NULL
 */
const char * const *
flatpak_installed_ref_get_subpaths (FlatpakInstalledRef *self)
{
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  return (const char * const *) priv->subpaths;
}

/**
 * flatpak_installed_ref_get_installed_size:
 * @self: a #FlatpakInstalledRef
 *
 * Returns the installed size of the ref.
 *
 * Returns: the installed size
 */
guint64
flatpak_installed_ref_get_installed_size (FlatpakInstalledRef *self)
{
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  return priv->installed_size;
}

/**
 * flatpak_installed_ref_load_metadata:
 * @self: a #FlatpakInstalledRef
 * @cancellable: (nullable): a #GCancellable
 * @error: a return location for a #GError
 *
 * Loads the metadata file for this ref.
 *
 * Returns: (transfer full): a #GBytes containing the metadata file,
 *     or %NULL if an error occurred
 */
GBytes *
flatpak_installed_ref_load_metadata (FlatpakInstalledRef *self,
                                     GCancellable        *cancellable,
                                     GError             **error)
{
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);
  g_autofree char *path = NULL;
  char *metadata;
  gsize length;

  if (priv->deploy_dir == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Unknown deploy directory");
      return NULL;
    }

  path = g_build_filename (priv->deploy_dir, "metadata", NULL);
  if (!g_file_get_contents (path, &metadata, &length, error))
    return NULL;

  return g_bytes_new_take (metadata, length);
}

/**
 * flatpak_installed_ref_load_appdata:
 * @self: a #FlatpakInstalledRef
 * @cancellable: (nullable): a #GCancellable
 * @error: a return location for a #GError
 *
 * Loads the compressed xml appdata for this ref (if it exists).
 *
 * Returns: (transfer full): a #GBytes containing the compressed appdata file,
 *     or %NULL if an error occurred
 *
 * Since: 1.1.2
 */
GBytes *
flatpak_installed_ref_load_appdata (FlatpakInstalledRef *self,
                                    GCancellable        *cancellable,
                                    GError             **error)
{
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);
  char *data;
  gsize length;
  g_autofree char *path = NULL;
  g_autofree char *appdata_name = NULL;

  if (priv->deploy_dir == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Unknown deploy directory");
      return NULL;
    }

  appdata_name = g_strconcat (flatpak_ref_get_name (FLATPAK_REF (self)), ".xml.gz", NULL);
  path = g_build_filename (priv->deploy_dir, "files/share/app-info/xmls", appdata_name, NULL);

  if (!g_file_get_contents (path, &data, &length, error))
    return NULL;

  return g_bytes_new_take (data, length);
}

/**
 * flatpak_installed_ref_get_eol:
 * @self: a #FlatpakInstalledRef
 *
 * Returns the end-of-life reason string, or %NULL if the
 * ref is not end-of-lifed.
 *
 * Returns: (transfer none): the end-of-life reason or %NULL
 */
const char *
flatpak_installed_ref_get_eol (FlatpakInstalledRef *self)
{
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  return priv->eol;
}

/**
 * flatpak_installed_ref_get_eol_rebase:
 * @self: a #FlatpakInstalledRef
 *
 * Returns the end-of-life rebased ref, or %NULL if the
 * ref is not end-of-lifed.
 *
 * Returns: (transfer none): the end-of-life rebased ref or %NULL
 */
const char *
flatpak_installed_ref_get_eol_rebase (FlatpakInstalledRef *self)
{
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  return priv->eol_rebase;
}

/**
 * flatpak_installed_ref_get_appdata_name:
 * @self: a #FlatpakInstalledRef
 *
 * Returns the name field from the appdata.
 *
 * The returned string is localized.
 *
 * Returns: (transfer none): the name or %NULL
 *
 * Since: 1.1.2
 */
const char *
flatpak_installed_ref_get_appdata_name (FlatpakInstalledRef *self)
{
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  return priv->appdata_name;
}

/**
 * flatpak_installed_ref_get_appdata_summary:
 * @self: a #FlatpakInstalledRef
 *
 * Returns the summary field from the appdata.
 *
 * The returned string is localized.
 *
 * Returns: (transfer none): the summary or %NULL
 *
 * Since: 1.1.2
 */
const char *
flatpak_installed_ref_get_appdata_summary (FlatpakInstalledRef *self)
{
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  return priv->appdata_summary;
}

/**
 * flatpak_installed_ref_get_appdata_version:
 * @self: a #FlatpakInstalledRef
 *
 * Returns the default version field from the appdata.
 *
 * Returns: (transfer none): the version or %NULL
 *
 * Since: 1.1.2
 */
const char *
flatpak_installed_ref_get_appdata_version (FlatpakInstalledRef *self)
{
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  return priv->appdata_version;
}

/**
 * flatpak_installed_ref_get_appdata_license:
 * @self: a #FlatpakInstalledRef
 *
 * Returns the license field from the appdata.
 *
 * Returns: (transfer none): the license or %NULL
 *
 * Since: 1.1.2
 */
const char *
flatpak_installed_ref_get_appdata_license (FlatpakInstalledRef *self)
{
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  return priv->appdata_license;
}

/**
 * flatpak_installed_ref_get_appdata_content_rating_type:
 * @self: a #FlatpakInstalledRef
 *
 * Returns the content rating type from the appdata. For example, `oars-1.0` or
 * `oars-1.1`.
 *
 * Returns: (transfer none) (nullable): the content rating type or %NULL
 *
 * Since: 1.4.2
 */
const char *
flatpak_installed_ref_get_appdata_content_rating_type (FlatpakInstalledRef *self)
{
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  return priv->appdata_content_rating_type;
}

/**
 * flatpak_installed_ref_get_appdata_content_rating:
 * @self: a #FlatpakInstalledRef
 *
 * Returns the content rating field from the appdata. This is a potentially
 * empty mapping of content rating attribute IDs to values, to be interpreted
 * by the semantics of the content rating type (see
 * flatpak_installed_ref_get_appdata_content_rating_type()).
 *
 * Returns: (transfer none) (nullable) (element-type utf8 utf8): the content
 * rating or %NULL
 *
 * Since: 1.4.2
 */
GHashTable *
flatpak_installed_ref_get_appdata_content_rating (FlatpakInstalledRef *self)
{
  FlatpakInstalledRefPrivate *priv = flatpak_installed_ref_get_instance_private (self);

  return priv->appdata_content_rating;
}

FlatpakInstalledRef *
flatpak_installed_ref_new (FlatpakDecomposed *decomposed,
                           const char  *commit,
                           const char  *latest_commit,
                           const char  *origin,
                           const char  *collection_id,
                           const char **subpaths,
                           const char  *deploy_dir,
                           guint64      installed_size,
                           gboolean     is_current,
                           const char  *eol,
                           const char  *eol_rebase,
                           const char  *appdata_name,
                           const char  *appdata_summary,
                           const char  *appdata_version,
                           const char  *appdata_license,
                           const char  *appdata_content_rating_type,
                           GHashTable  *appdata_content_rating)
{
  FlatpakInstalledRef *ref;

  /* Canonicalize the "no subpaths" case */
  if (subpaths && *subpaths == NULL)
    subpaths = NULL;

  ref = g_object_new (FLATPAK_TYPE_INSTALLED_REF,
                      "kind", flatpak_decomposed_get_kind (decomposed),
                      "name", flatpak_decomposed_peek_id (decomposed, NULL),
                      "arch", flatpak_decomposed_peek_arch (decomposed, NULL),
                      "branch", flatpak_decomposed_peek_branch (decomposed, NULL),
                      "commit", commit,
                      "latest-commit", latest_commit,
                      "origin", origin,
                      "collection-id", collection_id,
                      "subpaths", subpaths,
                      "is-current", is_current,
                      "installed-size", installed_size,
                      "deploy-dir", deploy_dir,
                      "end-of-life", eol,
                      "end-of-life-rebase", eol_rebase,
                      "appdata-name", appdata_name,
                      "appdata-summary", appdata_summary,
                      "appdata-version", appdata_version,
                      "appdata-license", appdata_license,
                      "appdata-content-rating-type", appdata_content_rating_type,
                      "appdata-content-rating", appdata_content_rating,
                      NULL);

  return ref;
}
