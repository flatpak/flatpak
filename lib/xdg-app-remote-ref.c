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
#include "xdg-app-remote-ref-private.h"
#include "xdg-app-remote-ref.h"
#include "xdg-app-enum-types.h"

/**
 * SECTION:xdg-app-remote-ref
 * @Title: XdgAppRemoteRef
 * @Short_description: Remote application reference
 *
 * A XdgAppRemoteRef provides information about an application or runtime
 * (in short: ref) that is available from a remote repository.
 */
typedef struct _XdgAppRemoteRefPrivate XdgAppRemoteRefPrivate;

struct _XdgAppRemoteRefPrivate
{
  char *remote_name;
};

G_DEFINE_TYPE_WITH_PRIVATE (XdgAppRemoteRef, xdg_app_remote_ref, XDG_APP_TYPE_REF)

enum {
  PROP_0,

  PROP_REMOTE_NAME,
};

static void
xdg_app_remote_ref_finalize (GObject *object)
{
  XdgAppRemoteRef *self = XDG_APP_REMOTE_REF (object);
  XdgAppRemoteRefPrivate *priv = xdg_app_remote_ref_get_instance_private (self);

  g_free (priv->remote_name);

  G_OBJECT_CLASS (xdg_app_remote_ref_parent_class)->finalize (object);
}

static void
xdg_app_remote_ref_set_property (GObject         *object,
                                 guint            prop_id,
                                 const GValue    *value,
                                 GParamSpec      *pspec)
{
  XdgAppRemoteRef *self = XDG_APP_REMOTE_REF (object);
  XdgAppRemoteRefPrivate *priv = xdg_app_remote_ref_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_REMOTE_NAME:
      g_clear_pointer (&priv->remote_name, g_free);
      priv->remote_name = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_remote_ref_get_property (GObject         *object,
                                 guint            prop_id,
                                 GValue          *value,
                                 GParamSpec      *pspec)
{
  XdgAppRemoteRef *self = XDG_APP_REMOTE_REF (object);
  XdgAppRemoteRefPrivate *priv = xdg_app_remote_ref_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_REMOTE_NAME:
      g_value_set_string (value, priv->remote_name);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_remote_ref_class_init (XdgAppRemoteRefClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = xdg_app_remote_ref_get_property;
  object_class->set_property = xdg_app_remote_ref_set_property;
  object_class->finalize = xdg_app_remote_ref_finalize;

  g_object_class_install_property (object_class,
                                   PROP_REMOTE_NAME,
                                   g_param_spec_string ("remote-name",
                                                        "Remote Name",
                                                        "The name of the remote",
                                                        NULL,
                                                        G_PARAM_READWRITE));
}

static void
xdg_app_remote_ref_init (XdgAppRemoteRef *self)
{
}

/**
 * xdg_app_remote_ref_get_remote_name:
 * @self: a #XdgAppRemoteRef
 *
 * Gets the remote name of the ref.
 *
 * Returns: (transfer none): the remote name
 */
const char *
xdg_app_remote_ref_get_remote_name (XdgAppRemoteRef *self)
{
  XdgAppRemoteRefPrivate *priv = xdg_app_remote_ref_get_instance_private (self);

  return priv->remote_name;
}


XdgAppRemoteRef *
xdg_app_remote_ref_new (const char *full_ref,
                        const char *commit,
                        const char *remote_name)
{
  XdgAppRefKind kind = XDG_APP_REF_KIND_APP;
  g_auto(GStrv) parts = NULL;
  XdgAppRemoteRef *ref;

  parts = xdg_app_decompose_ref (full_ref, NULL);
  if (parts == NULL)
    return NULL;

  if (strcmp (parts[0], "app") != 0)
    kind = XDG_APP_REF_KIND_RUNTIME;

  ref = g_object_new (XDG_APP_TYPE_REMOTE_REF,
                      "kind", kind,
                      "name", parts[1],
                      "arch", parts[2],
                      "branch", parts[3],
                      "commit", commit,
                      "remote-name", remote_name,
                      NULL);

  return ref;
}
