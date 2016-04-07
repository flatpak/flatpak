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
#include "xdg-app-installed-ref.h"
#include "xdg-app-enum-types.h"

/**
 * SECTION:xdg-app-installed-ref
 * @Title: XdgAppInstalledRef
 * @Short_description: Installed application reference
 *
 * A XdgAppInstalledRef provides information about an installed
 * application or runtime (in short: ref), such as the available
 * builds, its size, location, etc.
 */

typedef struct _XdgAppInstalledRefPrivate XdgAppInstalledRefPrivate;

struct _XdgAppInstalledRefPrivate
{
  gboolean is_current;
  char *origin;
  char *latest_commit;
  char *deploy_dir;
  char **subpaths;
  guint64 installed_size;
};

G_DEFINE_TYPE_WITH_PRIVATE (XdgAppInstalledRef, xdg_app_installed_ref, XDG_APP_TYPE_REF)

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
xdg_app_installed_ref_finalize (GObject *object)
{
  XdgAppInstalledRef *self = XDG_APP_INSTALLED_REF (object);
  XdgAppInstalledRefPrivate *priv = xdg_app_installed_ref_get_instance_private (self);

  g_free (priv->origin);
  g_free (priv->deploy_dir);
  g_strfreev (priv->subpaths);

  G_OBJECT_CLASS (xdg_app_installed_ref_parent_class)->finalize (object);
}

static void
xdg_app_installed_ref_set_property (GObject         *object,
                                    guint            prop_id,
                                    const GValue    *value,
                                    GParamSpec      *pspec)
{
  XdgAppInstalledRef *self = XDG_APP_INSTALLED_REF (object);
  XdgAppInstalledRefPrivate *priv = xdg_app_installed_ref_get_instance_private (self);

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
xdg_app_installed_ref_get_property (GObject         *object,
                                    guint            prop_id,
                                    GValue          *value,
                                    GParamSpec      *pspec)
{
  XdgAppInstalledRef *self = XDG_APP_INSTALLED_REF (object);
  XdgAppInstalledRefPrivate *priv = xdg_app_installed_ref_get_instance_private (self);

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
xdg_app_installed_ref_class_init (XdgAppInstalledRefClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = xdg_app_installed_ref_get_property;
  object_class->set_property = xdg_app_installed_ref_set_property;
  object_class->finalize = xdg_app_installed_ref_finalize;

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
xdg_app_installed_ref_init (XdgAppInstalledRef *self)
{
}

/**
 * xdg_app_installed_ref_get_origin:
 * @self: a #XdgAppInstalledRef
 *
 * Gets the origin of the ref.
 *
 * Returns: (transfer none): the origin
 */
const char *
xdg_app_installed_ref_get_origin (XdgAppInstalledRef *self)
{
  XdgAppInstalledRefPrivate *priv = xdg_app_installed_ref_get_instance_private (self);

  return priv->origin;
}

/**
 * xdg_app_installed_ref_get_latest_commit:
 * @self: a #XdgAppInstalledRef
 *
 * Gets the latest commit of the ref.
 *
 * Returns: (transfer none): the latest commit
 */
const char *
xdg_app_installed_ref_get_latest_commit (XdgAppInstalledRef *self)
{
  XdgAppInstalledRefPrivate *priv = xdg_app_installed_ref_get_instance_private (self);

  return priv->latest_commit;
}

/**
 * xdg_app_installed_ref_get_deploy_dir:
 * @self: a #XdgAppInstalledRef
 *
 * Gets the deploy dir of the ref.
 *
 * Returns: (transfer none): the deploy dir
 */
const char *
xdg_app_installed_ref_get_deploy_dir (XdgAppInstalledRef *self)
{
  XdgAppInstalledRefPrivate *priv = xdg_app_installed_ref_get_instance_private (self);

  return priv->deploy_dir;
}

/**
 * xdg_app_installed_ref_get_is_current:
 * @self: a #XdgAppInstalledRef
 *
 * Returns whether the ref is current.
 *
 * Returns: %TRUE if the ref is current
 */
gboolean
xdg_app_installed_ref_get_is_current (XdgAppInstalledRef *self)
{
  XdgAppInstalledRefPrivate *priv = xdg_app_installed_ref_get_instance_private (self);

  return priv->is_current;
}

/**
 * xdg_app_installed_ref_get_installed_size:
 * @self: a #XdgAppInstalledRef
 *
 * Returns the installed size of the ref.
 *
 * Returns: the installed size
 */
guint64
xdg_app_installed_ref_get_installed_size (XdgAppInstalledRef *self)
{
  XdgAppInstalledRefPrivate *priv = xdg_app_installed_ref_get_instance_private (self);

  return priv->installed_size;
}

/**
 * xdg_app_installed_ref_load_metadata:
 * @self: a #XdgAppInstalledRef
 * @cancellable: (nullable): a #GCancellable
 * @error: a return location for a #GError
 *
 * Loads the metadata file for this ref.
 *
 * Returns: (transfer full): a #GBytes containing the metadata file,
 *     or %NULL if an error occurred
 */
GBytes *
xdg_app_installed_ref_load_metadata  (XdgAppInstalledRef *self,
                                      GCancellable *cancellable,
                                      GError **error)
{
  XdgAppInstalledRefPrivate *priv = xdg_app_installed_ref_get_instance_private (self);
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

XdgAppInstalledRef *
xdg_app_installed_ref_new (const char *full_ref,
                           const char *commit,
                           const char *latest_commit,
                           const char *origin,
                           char      **subpaths,
                           const char *deploy_dir,
                           guint64     installed_size,
                           gboolean    is_current)
{
  XdgAppRefKind kind = XDG_APP_REF_KIND_APP;
  XdgAppInstalledRef *ref;
  g_auto(GStrv) parts = NULL;

  parts = g_strsplit (full_ref, "/", -1);

  if (strcmp (parts[0], "app") != 0)
    kind = XDG_APP_REF_KIND_RUNTIME;

  /* Canonicalize the "no subpaths" case */
  if (subpaths && *subpaths == NULL)
    subpaths = NULL;

  ref = g_object_new (XDG_APP_TYPE_INSTALLED_REF,
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
