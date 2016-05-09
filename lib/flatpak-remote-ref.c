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

#include "flatpak-utils.h"
#include "flatpak-remote-ref-private.h"
#include "flatpak-remote-ref.h"
#include "flatpak-enum-types.h"

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
  char *remote_name;
};

G_DEFINE_TYPE_WITH_PRIVATE (FlatpakRemoteRef, flatpak_remote_ref, FLATPAK_TYPE_REF)

enum {
  PROP_0,

  PROP_REMOTE_NAME,
};

static void
flatpak_remote_ref_finalize (GObject *object)
{
  FlatpakRemoteRef *self = FLATPAK_REMOTE_REF (object);
  FlatpakRemoteRefPrivate *priv = flatpak_remote_ref_get_instance_private (self);

  g_free (priv->remote_name);

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
                                                        G_PARAM_READWRITE));
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


FlatpakRemoteRef *
flatpak_remote_ref_new (const char *full_ref,
                        const char *commit,
                        const char *remote_name)
{
  FlatpakRefKind kind = FLATPAK_REF_KIND_APP;

  g_auto(GStrv) parts = NULL;
  FlatpakRemoteRef *ref;

  parts = flatpak_decompose_ref (full_ref, NULL);
  if (parts == NULL)
    return NULL;

  if (strcmp (parts[0], "app") != 0)
    kind = FLATPAK_REF_KIND_RUNTIME;

  ref = g_object_new (FLATPAK_TYPE_REMOTE_REF,
                      "kind", kind,
                      "name", parts[1],
                      "arch", parts[2],
                      "branch", parts[3],
                      "commit", commit,
                      "remote-name", remote_name,
                      NULL);

  return ref;
}
