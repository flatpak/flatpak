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

#include "xdg-app-utils.h"
#include "xdg-app-ref.h"
#include "xdg-app-enum-types.h"

typedef struct _XdgAppRefPrivate XdgAppRefPrivate;

struct _XdgAppRefPrivate
{
  char *name;
  char *arch;
  char *version;
  char *commit;
  XdgAppRefKind kind;
};

G_DEFINE_TYPE_WITH_PRIVATE (XdgAppRef, xdg_app_ref, G_TYPE_OBJECT)

enum {
  PROP_0,

  PROP_NAME,
  PROP_ARCH,
  PROP_VERSION,
  PROP_COMMIT,
  PROP_KIND
};

static void
xdg_app_ref_finalize (GObject *object)
{
  XdgAppRef *self = XDG_APP_REF (object);
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  g_free (priv->name);
  g_free (priv->arch);
  g_free (priv->version);
  g_free (priv->commit);

  G_OBJECT_CLASS (xdg_app_ref_parent_class)->finalize (object);
}

static void
xdg_app_ref_set_property (GObject         *object,
                          guint            prop_id,
                          const GValue    *value,
                          GParamSpec      *pspec)
{
  XdgAppRef *self = XDG_APP_REF (object);
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_NAME:
      g_clear_pointer (&priv->name, g_free);
      priv->name = g_value_dup_string (value);
      break;

    case PROP_ARCH:
      g_clear_pointer (&priv->arch, g_free);
      priv->arch = g_value_dup_string (value);
      break;

    case PROP_VERSION:
      g_clear_pointer (&priv->version, g_free);
      priv->version = g_value_dup_string (value);
      break;

    case PROP_COMMIT:
      g_clear_pointer (&priv->commit, g_free);
      priv->commit = g_value_dup_string (value);
      break;

    case PROP_KIND:
      priv->kind = g_value_get_enum (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_ref_get_property (GObject         *object,
                          guint            prop_id,
                          GValue          *value,
                          GParamSpec      *pspec)
{
  XdgAppRef *self = XDG_APP_REF (object);
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    case PROP_ARCH:
      g_value_set_string (value, priv->arch);
      break;

    case PROP_VERSION:
      g_value_set_string (value, priv->version);
      break;

    case PROP_COMMIT:
      g_value_set_string (value, priv->commit);
      break;

    case PROP_KIND:
      g_value_set_enum (value, priv->kind);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_ref_class_init (XdgAppRefClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = xdg_app_ref_get_property;
  object_class->set_property = xdg_app_ref_set_property;
  object_class->finalize = xdg_app_ref_finalize;

  g_object_class_install_property (object_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_ARCH,
                                   g_param_spec_string ("arch",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_VERSION,
                                   g_param_spec_string ("version",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_COMMIT,
                                   g_param_spec_string ("commit",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
                                   PROP_KIND,
                                   g_param_spec_enum ("kind",
                                                      "",
                                                      "",
                                                      XDG_TYPE_APP_REF_KIND,
                                                      XDG_APP_REF_KIND_APP,
                                                      G_PARAM_READWRITE));
}

static void
xdg_app_ref_init (XdgAppRef *self)
{
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  priv->kind = XDG_APP_REF_KIND_APP;
}

const char *
xdg_app_ref_get_name (XdgAppRef *self)
{
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  return priv->name;
}

const char *
xdg_app_ref_get_arch (XdgAppRef *self)
{
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  return priv->arch;
}

const char *
xdg_app_ref_get_version (XdgAppRef *self)
{
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  return priv->version;
}

const char *
xdg_app_ref_get_commit (XdgAppRef *self)
{
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  return priv->commit;
}

XdgAppRefKind
xdg_app_ref_get_kind (XdgAppRef *self)
{
  XdgAppRefPrivate *priv = xdg_app_ref_get_instance_private (self);

  return priv->kind;
}
