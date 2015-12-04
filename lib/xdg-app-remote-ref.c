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

#include "xdg-app-remote-ref.h"
#include "xdg-app-enum-types.h"
#include "xdg-app-error.h"

typedef struct _XdgAppRemoteRefPrivate XdgAppRemoteRefPrivate;

struct _XdgAppRemoteRefPrivate
{
  int dummy;
};

G_DEFINE_TYPE_WITH_PRIVATE (XdgAppRemoteRef, xdg_app_remote_ref, XDG_APP_TYPE_REF)

enum {
  PROP_0,

};

static void
xdg_app_remote_ref_finalize (GObject *object)
{
  G_OBJECT_CLASS (xdg_app_remote_ref_parent_class)->finalize (object);
}

static void
xdg_app_remote_ref_set_property (GObject         *object,
                                    guint            prop_id,
                                    const GValue    *value,
                                    GParamSpec      *pspec)
{
  switch (prop_id)
    {

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
  switch (prop_id)
    {

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
}

static void
xdg_app_remote_ref_init (XdgAppRemoteRef *self)
{
}

XdgAppRemoteRef *
xdg_app_remote_ref_new (const char *full_ref,
                        const char *commit)
{
  XdgAppRefKind kind = XDG_APP_REF_KIND_APP;
  g_auto(GStrv) parts = NULL;

  parts = g_strsplit (full_ref, "/", -1);

  if (strcmp (parts[0], "app") != 0)
    kind = XDG_APP_REF_KIND_RUNTIME;

  return g_object_new (XDG_APP_TYPE_REMOTE_REF,
                       "kind", kind,
                       "name", parts[1],
                       "arch", parts[2],
                       "version", parts[3],
                       "commit", commit,
                       NULL);
}
