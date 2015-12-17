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
#include "xdg-app-run.h"
#include "xdg-app-dir.h"

typedef struct _XdgAppInstalledRefPrivate XdgAppInstalledRefPrivate;

struct _XdgAppInstalledRefPrivate
{
  gboolean current;
  char *origin;
  char *deploy_dir;

  XdgAppDir *dir;
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
  g_object_unref (priv->dir);

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

char *
xdg_app_installed_ref_load_metadata  (XdgAppInstalledRef *self,
                                      GCancellable *cancellable,
                                      GError **error)
{
  XdgAppInstalledRefPrivate *priv = xdg_app_installed_ref_get_instance_private (self);
  g_autofree char *path = NULL;
  char *metadata;

  if (priv->deploy_dir == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Unknown deploy directory");
      return NULL;
    }

  path = g_build_filename (priv->deploy_dir, "metadata", NULL);
  if (!g_file_get_contents (path, &metadata, NULL, error))
    return NULL;

  return metadata;
}

gboolean
xdg_app_installed_ref_launch (XdgAppInstalledRef *self,
                              GCancellable *cancellable,
                              GError **error)
{
  XdgAppInstalledRefPrivate *priv = xdg_app_installed_ref_get_instance_private (self);
  g_autofree char *app_ref = NULL;
  g_autoptr(XdgAppDeploy) app_deploy = NULL;

  app_ref =
    xdg_app_build_app_ref (xdg_app_ref_get_name (XDG_APP_REF (self)),
                           xdg_app_ref_get_branch (XDG_APP_REF (self)),
                           xdg_app_ref_get_arch (XDG_APP_REF (self)));


  app_deploy =
    xdg_app_dir_load_deployed (priv->dir, app_ref,
                               xdg_app_ref_get_commit (XDG_APP_REF (self)),
                               cancellable, error);
  if (app_deploy == NULL)
    return FALSE;

  return xdg_app_run_app (app_ref,
                          app_deploy,
                          NULL,
                          NULL,
                          XDG_APP_RUN_FLAG_BACKGROUND,
                          NULL,
                          NULL, 0,
                          cancellable, error);

}

XdgAppInstalledRef *
xdg_app_installed_ref_new (const char *full_ref,
                           const char *commit,
                           const char *origin,
                           const char *deploy_dir,
                           XdgAppDir *dir,
                           gboolean current)
{
  XdgAppRefKind kind = XDG_APP_REF_KIND_APP;
  XdgAppInstalledRefPrivate *priv;
  XdgAppInstalledRef *ref;
  g_auto(GStrv) parts = NULL;

  parts = g_strsplit (full_ref, "/", -1);

  if (strcmp (parts[0], "app") != 0)
    kind = XDG_APP_REF_KIND_RUNTIME;

  ref = g_object_new (XDG_APP_TYPE_INSTALLED_REF,
                      "kind", kind,
                      "name", parts[1],
                      "arch", parts[2],
                      "branch", parts[3],
                      "commit", commit,
                      "origin", origin,
                      "current", current,
                      "deploy-dir", deploy_dir,
                      NULL);

  priv = xdg_app_installed_ref_get_instance_private (ref);
  priv->dir = g_object_ref (dir);

  return ref;
}
