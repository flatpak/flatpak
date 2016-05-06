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
 * @Title: FlatpakRemote
 *
 * A #FlatpakRemote object provides information about a remote
 * repository (or short: remote) that has been configured.
 *
 * At its most basic level, a remote has a name and the URL for
 * the repository. In addition, they provide some additional
 * information that can be useful when presenting repositories
 * in a UI, such as a title, a priority or a "don't enumerate"
 * flags.
 *
 * To obtain FlatpakRemote objects for the configured remotes
 * on a system, use flatpak_installation_list_remotes() or
 * flatpak_installation_get_remote_by_name().
 */

typedef struct _FlatpakRemotePrivate FlatpakRemotePrivate;

struct _FlatpakRemotePrivate
{
  char       *name;
  FlatpakDir *dir;
};

G_DEFINE_TYPE_WITH_PRIVATE (FlatpakRemote, flatpak_remote, G_TYPE_OBJECT)

enum {
  PROP_0,

  PROP_NAME,
};

static void
flatpak_remote_finalize (GObject *object)
{
  FlatpakRemote *self = FLATPAK_REMOTE (object);
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  g_free (priv->name);
  g_object_unref (priv->dir);

  G_OBJECT_CLASS (flatpak_remote_parent_class)->finalize (object);
}

static void
flatpak_remote_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  FlatpakRemote *self = FLATPAK_REMOTE (object);
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

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
flatpak_remote_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  FlatpakRemote *self = FLATPAK_REMOTE (object);
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

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
flatpak_remote_class_init (FlatpakRemoteClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = flatpak_remote_get_property;
  object_class->set_property = flatpak_remote_set_property;
  object_class->finalize = flatpak_remote_finalize;

  g_object_class_install_property (object_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Name",
                                                        "The name of the remote",
                                                        NULL,
                                                        G_PARAM_READWRITE));
}

static void
flatpak_remote_init (FlatpakRemote *self)
{
}

/**
 * flatpak_remote_get_name:
 * @self: a #FlatpakRemote
 *
 * Returns the name of the remote repository.
 *
 * Returns: (transfer none): the name
 */
const char *
flatpak_remote_get_name (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  return priv->name;
}

/**
 * flatpak_remote_get_appstream_dir:
 * @self: a #FlatpakRemote
 * @arch: (nullable): which architecture to fetch (default: current architecture)
 *
 * Returns the directory where this remote will store locally cached
 * appstream information for the specified @arch.
 *
 * Returns: (transfer full): a #GFile
 **/
GFile *
flatpak_remote_get_appstream_dir (FlatpakRemote *self,
                                  const char    *arch)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);
  g_autofree char *subdir = NULL;

  if (arch == NULL)
    arch = flatpak_get_arch ();

  subdir = g_strdup_printf ("appstream/%s/%s/active", priv->name, arch);
  return g_file_resolve_relative_path (flatpak_dir_get_path (priv->dir),
                                       subdir);
}

/**
 * flatpak_remote_get_appstream_timestamp:
 * @self: a #FlatpakRemote
 * @arch: (nullable): which architecture to fetch (default: current architecture)
 *
 * Returns the timestamp file that will be updated whenever the appstream information
 * has been updated (or tried to update) for the specified @arch.
 *
 * Returns: (transfer full): a #GFile
 **/
GFile *
flatpak_remote_get_appstream_timestamp (FlatpakRemote *self,
                                        const char    *arch)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);
  g_autofree char *subdir = NULL;

  if (arch == NULL)
    arch = flatpak_get_arch ();

  subdir = g_strdup_printf ("appstream/%s/%s/.timestamp", priv->name, arch);
  return g_file_resolve_relative_path (flatpak_dir_get_path (priv->dir),
                                       subdir);
}

/**
 * flatpak_remote_get_url:
 * @self: a #FlatpakRemote
 *
 * Returns the repository URL of this remote.
 *
 * Returns: (transfer full): the URL
 */
char *
flatpak_remote_get_url (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);
  OstreeRepo *repo = flatpak_dir_get_repo (priv->dir);
  char *url;

  if (ostree_repo_remote_get_url (repo, priv->name, &url, NULL))
    return url;

  return NULL;
}

/**
 * flatpak_remote_get_title:
 * @self: a #FlatpakRemote
 *
 * Returns the title of the remote.
 *
 * Returns: (transfer full): the title
 */
char *
flatpak_remote_get_title (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  return flatpak_dir_get_remote_title (priv->dir, priv->name);
}

/**
 * flatpak_remote_get_noenumerate:
 * @self: a #FlatpakRemote
 *
 * Returns whether this remote should be used to list applications.
 *
 * Returns: whether the remote is marked as "don't enumerate"
 */
gboolean
flatpak_remote_get_noenumerate (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  return flatpak_dir_get_remote_noenumerate (priv->dir, priv->name);
}

/**
 * flatpak_remote_get_disable:
 * @self: a #FlatpakRemote
 *
 * Returns whether this remote is disabled.
 *
 * Returns: whether the remote is marked as "don't enumerate"
 */
gboolean
flatpak_remote_get_disabled (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  return flatpak_dir_get_remote_disabled (priv->dir, priv->name);
}

/**
 * flatpak_remote_get_prio:
 * @self: a #FlatpakRemote
 *
 * Returns the priority for the remote.
 *
 * Returns: the priority
 */
int
flatpak_remote_get_prio (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  return flatpak_dir_get_remote_prio (priv->dir, priv->name);
}

/**
 * flatpak_remote_get_gpg_verify:
 * @self: a #FlatpakRemote
 *
 * Returns whether GPG verification is enabled for the remote.
 *
 * Returns: whether GPG verification is enabled
 */
gboolean
flatpak_remote_get_gpg_verify (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);
  OstreeRepo *repo = flatpak_dir_get_repo (priv->dir);
  gboolean res;

  if (ostree_repo_remote_get_gpg_verify (repo, priv->name, &res, NULL))
    return res;

  return FALSE;
}

FlatpakRemote *
flatpak_remote_new (FlatpakDir *dir,
                    const char *name)
{
  FlatpakRemotePrivate *priv;
  FlatpakRemote *self = g_object_new (FLATPAK_TYPE_REMOTE,
                                      "name", name,
                                      NULL);

  priv = flatpak_remote_get_instance_private (self);
  priv->dir = g_object_ref (dir);

  return self;
}
