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

#include "xdg-app-installed-ref.h"
#include "xdg-app-enum-types.h"
#include "xdg-app-error.h"

typedef struct _XdgAppInstalledRefPrivate XdgAppInstalledRefPrivate;

struct _XdgAppInstalledRefPrivate
{
  gboolean current;
  char *origin;
  char *deploy_dir;

  char *metadata;
};

G_DEFINE_TYPE_WITH_PRIVATE (XdgAppInstalledRef, xdg_app_installed_ref, XDG_APP_TYPE_REF)

enum {
  PROP_0,

  PROP_CURRENT,
  PROP_ORIGIN,
  PROP_DEPLOY_DIR
};

static void
xdg_app_installed_ref_finalize (GObject *object)
{
  XdgAppInstalledRef *self = XDG_APP_INSTALLED_REF (object);
  XdgAppInstalledRefPrivate *priv = xdg_app_installed_ref_get_instance_private (self);

  g_free (priv->origin);
  g_free (priv->deploy_dir);
  g_free (priv->metadata);

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
    case PROP_CURRENT:
      priv->current = g_value_get_boolean (value);
      break;

    case PROP_ORIGIN:
      g_clear_pointer (&priv->origin, g_free);
      priv->origin = g_value_dup_string (value);
      break;

    case PROP_DEPLOY_DIR:
      g_clear_pointer (&priv->deploy_dir, g_free);
      priv->deploy_dir = g_value_dup_string (value);
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
    case PROP_CURRENT:
      g_value_set_boolean (value, priv->current);
      break;

    case PROP_ORIGIN:
      g_value_set_string (value, priv->origin);
      break;

    case PROP_DEPLOY_DIR:
      g_value_set_string (value, priv->deploy_dir);
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
                                   PROP_CURRENT,
                                   g_param_spec_boolean ("current",
                                                         "",
                                                         "",
                                                         FALSE,
                                                         G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ORIGIN,
                                   g_param_spec_string ("origin",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_DEPLOY_DIR,
                                   g_param_spec_string ("deploy-dir",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
}

static void
xdg_app_installed_ref_init (XdgAppInstalledRef *self)
{
}

const char *
xdg_app_installed_ref_get_origin (XdgAppInstalledRef *self)
{
  XdgAppInstalledRefPrivate *priv = xdg_app_installed_ref_get_instance_private (self);

  return priv->origin;
}

const char *
xdg_app_installed_ref_get_deploy_dir (XdgAppInstalledRef *self)
{
  XdgAppInstalledRefPrivate *priv = xdg_app_installed_ref_get_instance_private (self);

  return priv->deploy_dir;
}

gboolean
xdg_app_installed_ref_get_current (XdgAppInstalledRef *self)
{
  XdgAppInstalledRefPrivate *priv = xdg_app_installed_ref_get_instance_private (self);

  return priv->current;
}

const char *
xdg_app_installed_ref_load_metadata  (XdgAppInstalledRef *self,
                                      GCancellable *cancellable,
                                      GError **error)
{
  XdgAppInstalledRefPrivate *priv = xdg_app_installed_ref_get_instance_private (self);

  if (priv->metadata == NULL)
    {
      g_autofree char *path = NULL;
      if (priv->deploy_dir == NULL)
        {
          g_set_error (error, XDG_APP_ERROR, XDG_APP_ERROR_NOT_FOUND,
                       "Unknown deploy directory");
          return NULL;
        }

      path = g_build_filename (priv->deploy_dir, "metadata", NULL);
      if (!g_file_get_contents (path, &priv->metadata, NULL, error))
        return NULL;
    }

  return priv->metadata;
}

XdgAppInstalledRef *
xdg_app_installed_ref_new (const char *full_ref,
                           const char *commit,
                           const char *origin,
                           const char *deploy_dir,
                           gboolean current)
{
  XdgAppRefKind kind = XDG_APP_REF_KIND_APP;
  g_auto(GStrv) parts = NULL;

  parts = g_strsplit (full_ref, "/", -1);

  if (strcmp (parts[0], "app") != 0)
    kind = XDG_APP_REF_KIND_RUNTIME;

  return g_object_new (XDG_APP_TYPE_INSTALLED_REF,
                       "kind", kind,
                       "name", parts[1],
                       "arch", parts[2],
                       "version", parts[3],
                       "commit", commit,
                       "origin", origin,
                       "current", current,
                       "deploy-dir", deploy_dir,
                       NULL);
}
