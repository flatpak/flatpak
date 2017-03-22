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

#include "flatpak-utils.h"
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
};

G_DEFINE_TYPE_WITH_PRIVATE (FlatpakInstalledRef, flatpak_installed_ref, FLATPAK_TYPE_REF)

enum {
  PROP_0,

  PROP_IS_CURRENT,
  PROP_ORIGIN,
  PROP_LATEST_COMMIT,
  PROP_DEPLOY_DIR,
  PROP_INSTALLED_SIZE,
  PROP_SUBPATHS
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
                                                       "",
                                                       "",
                                                       G_TYPE_STRV,
                                                       G_PARAM_READWRITE));
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
 * Returns: (transfer none): the latest commit
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

FlatpakInstalledRef *
flatpak_installed_ref_new (const char  *full_ref,
                           const char  *commit,
                           const char  *latest_commit,
                           const char  *origin,
                           const char **subpaths,
                           const char  *deploy_dir,
                           guint64      installed_size,
                           gboolean     is_current)
{
  FlatpakRefKind kind = FLATPAK_REF_KIND_APP;
  FlatpakInstalledRef *ref;

  g_auto(GStrv) parts = NULL;

  parts = g_strsplit (full_ref, "/", -1);

  if (strcmp (parts[0], "app") != 0)
    kind = FLATPAK_REF_KIND_RUNTIME;

  /* Canonicalize the "no subpaths" case */
  if (subpaths && *subpaths == NULL)
    subpaths = NULL;

  ref = g_object_new (FLATPAK_TYPE_INSTALLED_REF,
                      "kind", kind,
                      "name", parts[1],
                      "arch", parts[2],
                      "branch", parts[3],
                      "commit", commit,
                      "latest-commit", latest_commit,
                      "origin", origin,
                      "subpaths", subpaths,
                      "is-current", is_current,
                      "installed-size", installed_size,
                      "deploy-dir", deploy_dir,
                      NULL);

  return ref;
}
