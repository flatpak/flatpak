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
#include "xdg-app-remote-private.h"
#include "xdg-app-remote-ref-private.h"
#include "xdg-app-enum-types.h"

#include <string.h>

/**
 * SECTION:xdg-app-remote
 * @Short_description: Remote repository
 * @Title: XdgAppRemote
 *
 * A #XdgAppRemote object provides information about a remote
 * repository (or short: remote) that has been configured.
 *
 * At its most basic level, a remote has a name and the URL for
 * the repository. In addition, they provide some additional
 * information that can be useful when presenting repositories
 * in a UI, such as a title, a priority or a "don't enumerate"
 * flags.
 *
 * To obtain XdgAppRemote objects for the configured remotes
 * on a system, use xdg_app_installation_list_remotes() or
 * xdg_app_installation_get_remote_by_name().
 */

typedef struct _XdgAppRemotePrivate XdgAppRemotePrivate;

struct _XdgAppRemotePrivate
{
  char *name;
  XdgAppDir *dir;
};

G_DEFINE_TYPE_WITH_PRIVATE (XdgAppRemote, xdg_app_remote, G_TYPE_OBJECT)

enum {
  PROP_0,

  PROP_NAME,
};

static void
xdg_app_remote_finalize (GObject *object)
{
  XdgAppRemote *self = XDG_APP_REMOTE (object);
  XdgAppRemotePrivate *priv = xdg_app_remote_get_instance_private (self);

  g_free (priv->name);
  g_object_unref (priv->dir);

  G_OBJECT_CLASS (xdg_app_remote_parent_class)->finalize (object);
}

static void
xdg_app_remote_set_property (GObject         *object,
                             guint            prop_id,
                             const GValue    *value,
                             GParamSpec      *pspec)
{
  XdgAppRemote *self = XDG_APP_REMOTE (object);
  XdgAppRemotePrivate *priv = xdg_app_remote_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_NAME:
      g_clear_pointer (&priv->name, g_free);
      priv->name = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_remote_get_property (GObject         *object,
                             guint            prop_id,
                             GValue          *value,
                             GParamSpec      *pspec)
{
  XdgAppRemote *self = XDG_APP_REMOTE (object);
  XdgAppRemotePrivate *priv = xdg_app_remote_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
xdg_app_remote_class_init (XdgAppRemoteClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = xdg_app_remote_get_property;
  object_class->set_property = xdg_app_remote_set_property;
  object_class->finalize = xdg_app_remote_finalize;

  g_object_class_install_property (object_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Name",
                                                        "The name of the remote",
                                                        NULL,
                                                        G_PARAM_READWRITE));
}

static void
xdg_app_remote_init (XdgAppRemote *self)
{
}

/**
 * xdg_app_remote_get_name:
 * @self: a #XdgAppRemote
 *
 * Returns the name of the remote repository.
 *
 * Returns: (transfer none): the name
 */
const char *
xdg_app_remote_get_name (XdgAppRemote *self)
{
  XdgAppRemotePrivate *priv = xdg_app_remote_get_instance_private (self);

  return priv->name;
}

/**
 * xdg_app_remote_get_appstream_dir:
 * @self: a #XdgAppRemote
 * @arch: (nullable): which architecture to fetch (default: current architecture)
 *
 * Returns the directory where this remote will store locally cached
 * appstream information for the specified @arch.
 *
 * Returns: (transfer full): a #GFile
 **/
GFile *
xdg_app_remote_get_appstream_dir (XdgAppRemote *self,
                                  const char *arch)
{
  XdgAppRemotePrivate *priv = xdg_app_remote_get_instance_private (self);
  g_autofree char *subdir = NULL;

  if (arch == NULL)
    arch = xdg_app_get_arch ();

  subdir = g_strdup_printf ("appstream/%s/%s/active", priv->name, arch);
  return g_file_resolve_relative_path (xdg_app_dir_get_path (priv->dir),
                                       subdir);
}

/**
 * xdg_app_remote_get_appstream_timestamp:
 * @self: a #XdgAppRemote
 * @arch: (nullable): which architecture to fetch (default: current architecture)
 *
 * Returns the timestamp file that will be updated whenever the appstream information
 * has been updated (or tried to update) for the specified @arch.
 *
 * Returns: (transfer full): a #GFile
 **/
GFile *
xdg_app_remote_get_appstream_timestamp (XdgAppRemote *self,
                                        const char *arch)
{
  XdgAppRemotePrivate *priv = xdg_app_remote_get_instance_private (self);
  g_autofree char *subdir = NULL;

  if (arch == NULL)
    arch = xdg_app_get_arch ();

  subdir = g_strdup_printf ("appstream/%s/%s/.timestamp", priv->name, arch);
  return g_file_resolve_relative_path (xdg_app_dir_get_path (priv->dir),
                                       subdir);
}

/**
 * xdg_app_remote_get_url:
 * @self: a #XdgAppRemote
 *
 * Returns the repository URL of this remote.
 *
 * Returns: (transfer full): the URL
 */
char *
xdg_app_remote_get_url (XdgAppRemote *self)
{
  XdgAppRemotePrivate *priv = xdg_app_remote_get_instance_private (self);
  OstreeRepo *repo = xdg_app_dir_get_repo (priv->dir);
  char *url;

  if (ostree_repo_remote_get_url (repo, priv->name, &url, NULL))
    return url;

  return NULL;
}

/**
 * xdg_app_remote_get_title:
 * @self: a #XdgAppRemote
 *
 * Returns the title of the remote.
 *
 * Returns: (transfer full): the title
 */
char *
xdg_app_remote_get_title (XdgAppRemote *self)
{
  XdgAppRemotePrivate *priv = xdg_app_remote_get_instance_private (self);

  return xdg_app_dir_get_remote_title (priv->dir, priv->name);
}

/**
 * xdg_app_remote_get_noenumerate:
 * @self: a #XdgAppRemote
 *
 * Returns whether this remote should be used to list applications.
 *
 * Returns: whether the remote is marked as "don't enumerate"
 */
gboolean
xdg_app_remote_get_noenumerate (XdgAppRemote *self)
{
  XdgAppRemotePrivate *priv = xdg_app_remote_get_instance_private (self);

  return xdg_app_dir_get_remote_noenumerate (priv->dir, priv->name);
}

/**
 * xdg_app_remote_get_prio:
 * @self: a #XdgAppRemote
 *
 * Returns the priority for the remote.
 *
 * Returns: the priority
 */
int
xdg_app_remote_get_prio (XdgAppRemote *self)
{
  XdgAppRemotePrivate *priv = xdg_app_remote_get_instance_private (self);

  return xdg_app_dir_get_remote_prio (priv->dir, priv->name);
}

/**
 * xdg_app_remote_get_gpg_verify:
 * @self: a #XdgAppRemote
 *
 * Returns whether GPG verification is enabled for the remote.
 *
 * Returns: whether GPG verification is enabled
 */
gboolean
xdg_app_remote_get_gpg_verify (XdgAppRemote *self)
{
  XdgAppRemotePrivate *priv = xdg_app_remote_get_instance_private (self);
  OstreeRepo *repo = xdg_app_dir_get_repo (priv->dir);
  gboolean res;

  if (ostree_repo_remote_get_gpg_verify (repo, priv->name, &res, NULL))
    return res;

  return FALSE;
}

XdgAppRemote *
xdg_app_remote_new (XdgAppDir *dir,
                    const char *name)
{
  XdgAppRemotePrivate *priv;
  XdgAppRemote *self = g_object_new (XDG_APP_TYPE_REMOTE,
                                     "name", name,
                                     NULL);

  priv = xdg_app_remote_get_instance_private (self);
  priv->dir = g_object_ref (dir);

  return self;
}
