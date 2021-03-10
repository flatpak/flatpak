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

#include <glib/gi18n-lib.h>

#include "flatpak-utils-private.h"
#include "flatpak-remote-private.h"
#include "flatpak-remote-ref-private.h"
#include "flatpak-enum-types.h"

#include <string.h>
#include <ostree.h>

#include <ostree-repo-finder-avahi.h>

/**
 * SECTION:flatpak-remote
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
  char             *name;
  FlatpakDir       *dir;

  char             *local_url;
  char             *local_collection_id;
  char             *local_title;
  char             *local_default_branch;
  char             *local_main_ref;
  char             *local_filter;
  gboolean          local_gpg_verify;
  gboolean          local_sign_verify;
  gboolean          local_noenumerate;
  gboolean          local_nodeps;
  gboolean          local_disabled;
  int               local_prio;
  char             *local_comment;
  char             *local_description;
  char             *local_homepage;
  char             *local_icon;
  FlatpakRemoteType type;

  guint             local_url_set            : 1;
  guint             local_collection_id_set  : 1;
  guint             local_title_set          : 1;
  guint             local_default_branch_set : 1;
  guint             local_main_ref_set       : 1;
  guint             local_filter_set         : 1;
  guint             local_gpg_verify_set     : 1;
  guint             local_sign_verify_set    : 1;
  guint             local_noenumerate_set    : 1;
  guint             local_nodeps_set         : 1;
  guint             local_disabled_set       : 1;
  guint             local_prio_set           : 1;
  guint             local_icon_set           : 1;
  guint             local_comment_set        : 1;
  guint             local_description_set    : 1;
  guint             local_homepage_set       : 1;

  GBytes           *local_gpg_key;
};

G_DEFINE_TYPE_WITH_PRIVATE (FlatpakRemote, flatpak_remote, G_TYPE_OBJECT)

enum {
  PROP_0,

  PROP_NAME,
  PROP_TYPE,
};

static void
flatpak_remote_finalize (GObject *object)
{
  FlatpakRemote *self = FLATPAK_REMOTE (object);
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  g_free (priv->name);
  if (priv->dir)
    g_object_unref (priv->dir);
  if (priv->local_gpg_key)
    g_bytes_unref (priv->local_gpg_key);

  g_free (priv->local_url);
  g_free (priv->local_collection_id);
  g_free (priv->local_title);
  g_free (priv->local_default_branch);
  g_free (priv->local_main_ref);

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

    case PROP_TYPE:
      priv->type = g_value_get_enum (value);
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

    case PROP_TYPE:
      g_value_set_enum (value, priv->type);
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

  /**
   * FlatpakRemote:name:
   *
   * Name of the remote, as used in configuration files and when interfacing
   * with OSTree. This is typically human readable, but could be generated, and
   * must conform to ostree_validate_remote_name(). It should typically not be
   * presented in the UI.
   */
  g_object_class_install_property (object_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Name",
                                                        "The name of the remote",
                                                        NULL,
                                                        G_PARAM_READWRITE));

  /**
   * FlatpakRemote:type:
   *
   * The type of the remote: whether it comes from static configuration files
   * (@FLATPAK_REMOTE_TYPE_STATIC) or has been dynamically found from the local
   * network or a mounted USB drive (@FLATPAK_REMOTE_TYPE_LAN,
   * @FLATPAK_REMOTE_TYPE_USB). Dynamic remotes may be added and removed over
   * time.
   *
   * Since: 0.9.8
   */
  g_object_class_install_property (object_class,
                                   PROP_TYPE,
                                   g_param_spec_enum ("type",
                                                      "Type",
                                                      "The type of the remote",
                                                      FLATPAK_TYPE_REMOTE_TYPE,
                                                      FLATPAK_REMOTE_TYPE_STATIC,
                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
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

  if (priv->dir == NULL)
    return NULL;

  if (arch == NULL)
    arch = flatpak_get_arch ();

  if (flatpak_dir_get_remote_oci (priv->dir, priv->name))
    subdir = g_strdup_printf ("appstream/%s/%s", priv->name, arch);
  else
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

  if (priv->dir == NULL)
    return NULL;

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
  char *url;

  if (priv->local_url_set)
    return g_strdup (priv->local_url);

  if (priv->dir)
    {
      OstreeRepo *repo = flatpak_dir_get_repo (priv->dir);
      if (ostree_repo_remote_get_url (repo, priv->name, &url, NULL))
        return url;
    }

  return NULL;
}

/**
 * flatpak_remote_set_url:
 * @self: a #FlatpakRemote
 * @url: The new url
 *
 * Sets the repository URL of this remote.
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 */
void
flatpak_remote_set_url (FlatpakRemote *self,
                        const char    *url)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  g_free (priv->local_url);
  priv->local_url = g_strdup (url);
  priv->local_url_set = TRUE;
}

/**
 * flatpak_remote_get_collection_id:
 * @self: a #FlatpakRemote
 *
 * Returns the repository collection ID of this remote, if set.
 *
 * Returns: (transfer full) (nullable): the collection ID, or %NULL if unset
 */
char *
flatpak_remote_get_collection_id (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  if (priv->local_collection_id_set)
    return g_strdup (priv->local_collection_id);

  if (priv->dir)
    return flatpak_dir_get_remote_collection_id (priv->dir, priv->name);

  return NULL;
}

/**
 * flatpak_remote_set_collection_id:
 * @self: a #FlatpakRemote
 * @collection_id: (nullable): The new collection ID, or %NULL to unset
 *
 * Sets the repository collection ID of this remote.
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 */
void
flatpak_remote_set_collection_id (FlatpakRemote *self,
                                  const char    *collection_id)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  if (collection_id != NULL && *collection_id == '\0')
    collection_id = NULL;

  g_free (priv->local_collection_id);
  priv->local_collection_id = g_strdup (collection_id);
  priv->local_collection_id_set = TRUE;
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

  if (priv->local_title_set)
    return g_strdup (priv->local_title);

  if (priv->dir)
    return flatpak_dir_get_remote_title (priv->dir, priv->name);

  return NULL;
}

/**
 * flatpak_remote_set_title:
 * @self: a #FlatpakRemote
 * @title: The new title, or %NULL to unset
 *
 * Sets the repository title of this remote.
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 */
void
flatpak_remote_set_title (FlatpakRemote *self,
                          const char    *title)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  g_free (priv->local_title);
  priv->local_title = g_strdup (title);
  priv->local_title_set = TRUE;
}

/**
 * flatpak_remote_get_filter:
 * @self: a #FlatpakRemote
 *
 * Returns the filter file of the remote.
 *
 * Returns: (transfer full): a pathname to a filter file
 *
 * Since: 1.4
 */
char *
flatpak_remote_get_filter (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  if (priv->local_filter_set)
    return g_strdup (priv->local_filter);

  if (priv->dir)
    return flatpak_dir_get_remote_filter (priv->dir, priv->name);

  return NULL;
}

/**
 * flatpak_remote_set_filter:
 * @self: a #FlatpakRemote
 * @filter_path: The pathname of the new filter file
 *
 * Sets a filter for this remote.
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 *
 * Since: 1.4
 */
void
flatpak_remote_set_filter (FlatpakRemote *self,
                           const char    *filter_path)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  g_free (priv->local_filter);
  priv->local_filter = g_strdup (filter_path);
  priv->local_filter_set = TRUE;
}

/**
 * flatpak_remote_get_comment:
 * @self: a #FlatpakRemote
 *
 * Returns the comment of the remote.
 *
 * Returns: (transfer full): the comment
 *
 * Since: 1.4
 */
char *
flatpak_remote_get_comment (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  if (priv->local_comment_set)
    return g_strdup (priv->local_comment);

  if (priv->dir)
    return flatpak_dir_get_remote_comment (priv->dir, priv->name);

  return NULL;
}

/**
 * flatpak_remote_set_comment:
 * @self: a #FlatpakRemote
 * @comment: The new comment 
 *
 * Sets the comment of this remote.
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 *
 * Since: 1.4
 */
void
flatpak_remote_set_comment (FlatpakRemote *self,
                            const char    *comment)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  g_free (priv->local_comment);
  priv->local_comment = g_strdup (comment);
  priv->local_comment_set = TRUE;
}

/**
 * flatpak_remote_get_description:
 * @self: a #FlatpakRemote
 *
 * Returns the description of the remote.
 *
 * Returns: (transfer full): the description 
 *
 * Since: 1.4
 */
char *
flatpak_remote_get_description (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  if (priv->local_description_set)
    return g_strdup (priv->local_description);

  if (priv->dir)
    return flatpak_dir_get_remote_description (priv->dir, priv->name);

  return NULL;
}

/**
 * flatpak_remote_set_description:
 * @self: a #FlatpakRemote
 * @description: The new description
 *
 * Sets the description of this remote.
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 *
 * Since: 1.4
 */
void
flatpak_remote_set_description (FlatpakRemote *self,
                                const char    *description)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  g_free (priv->local_description);
  priv->local_description = g_strdup (description);
  priv->local_description_set = TRUE;
}

/**
 * flatpak_remote_get_homepage:
 * @self: a #FlatpakRemote
 *
 * Returns the homepage url of the remote.
 *
 * Returns: (transfer full): the homepage url
 *
 * Since: 1.4
 */
char *
flatpak_remote_get_homepage (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  if (priv->local_homepage_set)
    return g_strdup (priv->local_homepage);

  if (priv->dir)
    return flatpak_dir_get_remote_homepage (priv->dir, priv->name);

  return NULL;
}

/**
 * flatpak_remote_set_homepage:
 * @self: a #FlatpakRemote
 * @homepage: The new homepage
 *
 * Sets the homepage of this remote.
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 *
 * Since: 1.4
 */
void
flatpak_remote_set_homepage (FlatpakRemote *self,
                             const char    *homepage)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  g_free (priv->local_homepage);
  priv->local_homepage = g_strdup (homepage);
  priv->local_homepage_set = TRUE;
}

/**
 * flatpak_remote_get_icon:
 * @self: a #FlatpakRemote
 *
 * Returns the icon url of the remote.
 *
 * Returns: (transfer full): the icon url
 *
 * Since: 1.4
 */
char *
flatpak_remote_get_icon (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  if (priv->local_icon_set)
    return g_strdup (priv->local_icon);

  if (priv->dir)
    return flatpak_dir_get_remote_icon (priv->dir, priv->name);

  return NULL;
}

/**
 * flatpak_remote_set_icon:
 * @self: a #FlatpakRemote
 * @icon: The new homepage
 *
 * Sets the homepage of this remote.
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 *
 * Since: 1.4
 */
void
flatpak_remote_set_icon (FlatpakRemote *self,
                         const char    *icon)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  g_free (priv->local_icon);
  priv->local_icon = g_strdup (icon);
  priv->local_icon_set = TRUE;
}

/**
 * flatpak_remote_get_default_branch:
 * @self: a #FlatpakRemote
 *
 * Returns the default branch configured for the remote.
 *
 * Returns: (transfer full): the default branch, or %NULL
 *
 * Since: 0.6.12
 */
char *
flatpak_remote_get_default_branch (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  if (priv->local_default_branch_set)
    return g_strdup (priv->local_default_branch);

  if (priv->dir)
    return flatpak_dir_get_remote_default_branch (priv->dir, priv->name);

  return NULL;
}

/**
 * flatpak_remote_set_default_branch:
 * @self: a #FlatpakRemote
 * @default_branch: The new default_branch, or %NULL to unset
 *
 * Sets the default branch configured for this remote.
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 *
 * Since: 0.6.12
 */
void
flatpak_remote_set_default_branch (FlatpakRemote *self,
                                   const char    *default_branch)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  g_free (priv->local_default_branch);
  priv->local_default_branch = g_strdup (default_branch);
  priv->local_default_branch_set = TRUE;
}

/**
 * flatpak_remote_get_main_ref:
 * @self: a #FlatpakRemote
 *
 * Returns the main ref of this remote, if set. The main ref is the ref that an
 * origin remote is created for.
 *
 * Returns: (transfer full): the main ref, or %NULL
 *
 * Since: 1.1.1
 */
char *
flatpak_remote_get_main_ref (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  if (priv->local_main_ref_set)
    return g_strdup (priv->local_main_ref);

  if (priv->dir)
    return flatpak_dir_get_remote_main_ref (priv->dir, priv->name);

  return NULL;
}

/**
 * flatpak_remote_set_main_ref:
 * @self: a #FlatpakRemote
 * @main_ref: The new main ref
 *
 * Sets the main ref of this remote. The main ref is the ref that an origin
 * remote is created for.
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 *
 * Since: 1.1.1
 */
void
flatpak_remote_set_main_ref (FlatpakRemote *self,
                             const char    *main_ref)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  g_free (priv->local_main_ref);
  priv->local_main_ref = g_strdup (main_ref);
  priv->local_main_ref_set = TRUE;
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

  if (priv->local_noenumerate_set)
    return priv->local_noenumerate;

  if (priv->dir)
    return flatpak_dir_get_remote_noenumerate (priv->dir, priv->name);

  return FALSE;
}

/**
 * flatpak_remote_set_noenumerate:
 * @self: a #FlatpakRemote
 * @noenumerate: a bool
 *
 * Sets the noenumeration config of this remote. See flatpak_remote_get_noenumerate().
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 */
void
flatpak_remote_set_noenumerate (FlatpakRemote *self,
                                gboolean       noenumerate)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  priv->local_noenumerate = noenumerate;
  priv->local_noenumerate_set = TRUE;
}

/**
 * flatpak_remote_get_nodeps:
 * @self: a #FlatpakRemote
 *
 * Returns whether this remote should be used to find dependencies.
 *
 * Returns: whether the remote is marked as "don't use for dependencies"
 */
gboolean
flatpak_remote_get_nodeps (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  if (priv->local_nodeps_set)
    return priv->local_nodeps;

  if (priv->dir)
    return flatpak_dir_get_remote_nodeps (priv->dir, priv->name);

  return FALSE;
}

/**
 * flatpak_remote_set_nodeps:
 * @self: a #FlatpakRemote
 * @nodeps: a bool
 *
 * Sets the nodeps config of this remote. See flatpak_remote_get_nodeps().
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 */
void
flatpak_remote_set_nodeps (FlatpakRemote *self,
                           gboolean       nodeps)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  priv->local_nodeps = nodeps;
  priv->local_nodeps_set = TRUE;
}

/**
 * flatpak_remote_get_disabled:
 * @self: a #FlatpakRemote
 *
 * Returns whether this remote is disabled.
 *
 * Returns: whether the remote is marked as disabled
 */
gboolean
flatpak_remote_get_disabled (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  if (priv->local_disabled_set)
    return priv->local_disabled;

  if (priv->dir)
    return flatpak_dir_get_remote_disabled (priv->dir, priv->name);

  return FALSE;
}
/**
 * flatpak_remote_set_disabled:
 * @self: a #FlatpakRemote
 * @disabled: a bool
 *
 * Sets the disabled config of this remote. See flatpak_remote_get_disabled().
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 */
void
flatpak_remote_set_disabled (FlatpakRemote *self,
                             gboolean       disabled)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  priv->local_disabled = disabled;
  priv->local_disabled_set = TRUE;
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

  if (priv->local_prio_set)
    return priv->local_prio;

  if (priv->dir)
    return flatpak_dir_get_remote_prio (priv->dir, priv->name);

  return 1;
}

/**
 * flatpak_remote_set_prio:
 * @self: a #FlatpakRemote
 * @prio: a bool
 *
 * Sets the prio config of this remote. See flatpak_remote_get_prio().
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 */
void
flatpak_remote_set_prio (FlatpakRemote *self,
                         int            prio)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  priv->local_prio = prio;
  priv->local_prio_set = TRUE;
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
  gboolean res;

  if (priv->local_gpg_verify_set)
    return priv->local_gpg_verify;

  if (priv->dir)
    {
      OstreeRepo *repo = flatpak_dir_get_repo (priv->dir);
      if (ostree_repo_remote_get_gpg_verify (repo, priv->name, &res, NULL))
        return res;
    }

  return FALSE;
}

/**
 * flatpak_remote_set_gpg_verify:
 * @self: a #FlatpakRemote
 * @gpg_verify: a bool
 *
 * Sets the gpg_verify config of this remote. See flatpak_remote_get_gpg_verify().
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 */
void
flatpak_remote_set_gpg_verify (FlatpakRemote *self,
                               gboolean       gpg_verify)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  priv->local_gpg_verify = gpg_verify;
  priv->local_gpg_verify_set = TRUE;
}

/**
 * flatpak_remote_set_gpg_key:
 * @self: a #FlatpakRemote
 * @gpg_key: a #GBytes with gpg binary key data
 *
 * Sets the trusted gpg key for this remote.
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 */
void
flatpak_remote_set_gpg_key (FlatpakRemote *self,
                            GBytes        *gpg_key)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  if (priv->local_gpg_key != NULL)
    g_bytes_unref (priv->local_gpg_key);
  priv->local_gpg_key = g_bytes_ref (gpg_key);
}

FlatpakRemote *
flatpak_remote_new_with_dir (const char *name,
                             FlatpakDir *dir)
{
  FlatpakRemotePrivate *priv;
  FlatpakRemote *self = g_object_new (FLATPAK_TYPE_REMOTE,
                                      "name", name,
                                      NULL);

  priv = flatpak_remote_get_instance_private (self);
  if (dir)
    priv->dir = g_object_ref (dir);

  return self;
}

/**
 * flatpak_remote_get_sign_verify:
 * @self: a #FlatpakRemote
 *
 * Returns whether signature verification is enabled for the remote.
 *
 * Returns: whether signature verification is enabled
 */
gboolean
flatpak_remote_get_sign_verify (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);
  gboolean res;

  if (priv->local_sign_verify_set)
    return priv->local_sign_verify;

  if (priv->dir)
    {
      OstreeRepo *repo = flatpak_dir_get_repo (priv->dir);
      if (flatpak_dir_get_sign_verify(repo, priv->name, &res, NULL))
        return res;
    }

  return FALSE;
}

/**
 * flatpak_remote_set_sign_verify:
 * @self: a #FlatpakRemote
 * @sign_verify: a bool
 *
 * Sets the sign_verify config of this remote. See flatpak_remote_get_sign_verify().
 *
 * Note: This is a local modification of this object, you must commit changes
 * using flatpak_installation_modify_remote() for the changes to take
 * effect.
 */
void
flatpak_remote_set_sign_verify (FlatpakRemote *self,
                               gboolean       sign_verify)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  priv->local_sign_verify = sign_verify;
  priv->local_sign_verify_set = TRUE;
}

/**
 * flatpak_remote_new:
 * @name: a name
 *
 * Returns a new remote object which can be used to configure a new remote.
 *
 * Note: This is a local configuration object, you must commit changes
 * using flatpak_installation_modify_remote() or flatpak_installation_add_remote() for the changes to take
 * effect.
 *
 * Returns: (transfer full): a new #FlatpakRemote
 **/
FlatpakRemote *
flatpak_remote_new (const char *name)
{
  return flatpak_remote_new_with_dir (name, NULL);
}


#define read_str_option(_key, _field) \
  {                                                                     \
    char *val = g_key_file_get_string (config, group, _key, NULL);      \
    if (val != NULL) {                                                  \
      priv->local_ ## _field = val;                                     \
      priv->local_ ## _field ## _set = TRUE;                            \
    }                                                                   \
  }

#define read_bool_option(_key, _field)                                  \
  if (g_key_file_has_key (config, group, _key, NULL)) {                 \
    priv->local_ ## _field = g_key_file_get_boolean (config, group, _key, NULL); \
    priv->local_ ## _field ## _set = TRUE;                              \
  }

#define read_int_option(_key, _field)                                   \
  if (g_key_file_has_key (config, group, _key, NULL)) {                 \
    priv->local_ ## _field = g_key_file_get_integer (config, group, _key, NULL); \
    priv->local_ ## _field ## _set = TRUE;                              \
  }


/**
 * flatpak_remote_new_from_file:
 * @name: a name
 * @data: The content of a flatpakrepo file
 * @error: return location for a #GError
 *
 * Returns a new pre-filled remote object which can be used to configure a new remote.
 * The fields in the remote are filled in according to the values in the
 * passed in flatpakrepo file.
 *
 * Note: This is a local configuration object, you must commit changes
 * using flatpak_installation_modify_remote()  or flatpak_installation_add_remote() for the changes to take
 * effect.
 *
 * Returns: (transfer full): a new #FlatpakRemote, or %NULL on error
 *
 * Since: 1.3.4
 **/
FlatpakRemote *
flatpak_remote_new_from_file (const char *name, GBytes *data, GError **error)
{
  FlatpakRemote *remote = flatpak_remote_new (name);
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (remote);
  g_autofree char *group = g_strdup_printf ("remote \"%s\"", name);
  g_autoptr(GKeyFile) keyfile = g_key_file_new ();
  g_autoptr(GKeyFile) config = NULL;
  g_autoptr(GBytes) gpg_data = NULL;

  if (!g_key_file_load_from_data (keyfile, g_bytes_get_data (data, NULL), g_bytes_get_size (data), 0, error))
    return NULL;

  config = flatpak_parse_repofile (name, FALSE, keyfile, &gpg_data, NULL, error);
  if (config == NULL)
    return NULL;

  priv->local_gpg_key = g_steal_pointer (&gpg_data);

  read_str_option("url", url);
  read_str_option("collection-id", collection_id);
  read_str_option("xa.title", title);
  read_str_option("xa.filter", filter);
  /* Canonicalize empty to null-but-is-set */
  if (priv->local_filter && priv->local_filter[0] == 0)
    g_free (g_steal_pointer (&priv->local_filter));
  read_str_option("xa.comment", comment);
  read_str_option("xa.description", description);
  read_str_option("xa.homepage", homepage);
  read_str_option("xa.icon", icon);
  read_str_option("xa.default-branch", default_branch);
  read_str_option("xa.main-ref", main_ref);

  read_bool_option("xa.gpg-verify", gpg_verify);
  read_bool_option("xa.noenumerate", noenumerate);
  read_bool_option("xa.disable", disabled);
  read_bool_option("xa.nodeps", nodeps);

  read_int_option("xa.prio", prio);

  return remote;
}

/* copied from GLib */
static gboolean
g_key_file_is_group_name (const gchar *name)
{
  gchar *p, *q;

  if (name == NULL)
    return FALSE;

  p = q = (gchar *) name;
  while (*q && *q != ']' && *q != '[' && !g_ascii_iscntrl (*q))
    q = g_utf8_find_next_char (q, NULL);

  if (*q != '\0' || q == p)
    return FALSE;

  return TRUE;
}

gboolean
flatpak_remote_commit_filter (FlatpakRemote *self,
                              FlatpakDir    *dir,
                              GCancellable  *cancellable,
                              GError       **error)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);
  g_autofree char *group = g_strdup_printf ("remote \"%s\"", priv->name);

  if (priv->local_filter_set &&
      !flatpak_dir_compare_remote_filter (dir, priv->name, priv->local_filter))
    {
      GKeyFile *config = ostree_repo_copy_config (flatpak_dir_get_repo (dir));

      g_key_file_set_string (config, group, "xa.filter", priv->local_filter ? priv->local_filter : "");

      if (!flatpak_dir_modify_remote (dir, priv->name, config, NULL, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static void
_key_file_set_or_unset_string (GKeyFile   *config,
                               const char *group,
                               const char *key,
                               const char *value)
{
  if (value != NULL)
    g_key_file_set_string (config, group, key, value);
  else
    g_key_file_remove_key (config, group, key, NULL);
}

gboolean
flatpak_remote_commit (FlatpakRemote *self,
                       FlatpakDir    *dir,
                       GCancellable  *cancellable,
                       GError       **error)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);
  OstreeRepo *repo;
  g_autofree char *url = NULL;
  g_autoptr(GKeyFile) config = NULL;
  g_autofree char *group = g_strdup_printf ("remote \"%s\"", priv->name);

  if (priv->name[0] == '\0' ||
      !g_key_file_is_group_name (group))
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("Bad remote name: %s"), priv->name);

  url = flatpak_remote_get_url (self);
  if (url == NULL || *url == 0)
    return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA, _("No url specified"));

  if (priv->type != FLATPAK_REMOTE_TYPE_STATIC)
    return flatpak_fail (error, "Dynamic remote cannot be committed");

  repo = flatpak_dir_get_repo (dir);
  if (repo == NULL)
    config = g_key_file_new ();
  else
    config = ostree_repo_copy_config (repo);

  if (priv->local_url_set)
    g_key_file_set_string (config, group, "url", priv->local_url);

  if (priv->local_collection_id_set)
    _key_file_set_or_unset_string (config, group, "collection-id", priv->local_collection_id);

  if (priv->local_title_set)
    _key_file_set_or_unset_string (config, group, "xa.title", priv->local_title);

  if (priv->local_filter_set)
    g_key_file_set_string (config, group, "xa.filter", priv->local_filter ? priv->local_filter : "");

  if (priv->local_comment_set)
    g_key_file_set_string (config, group, "xa.comment", priv->local_comment);

  if (priv->local_description_set)
    g_key_file_set_string (config, group, "xa.description", priv->local_description);

  if (priv->local_homepage_set)
    g_key_file_set_string (config, group, "xa.homepage", priv->local_homepage);

  if (priv->local_icon_set)
    g_key_file_set_string (config, group, "xa.icon", priv->local_icon);

  if (priv->local_default_branch_set)
    _key_file_set_or_unset_string (config, group, "xa.default-branch", priv->local_default_branch);

  if (priv->local_main_ref_set)
    g_key_file_set_string (config, group, "xa.main-ref", priv->local_main_ref);

  if (priv->local_gpg_verify_set || priv->local_sign_verify_set)
    {
      if (!priv->local_gpg_verify && !priv->local_sign_verify &&
           priv->local_collection_id_set && priv->local_collection_id != NULL)
        return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_DATA,
                                   _("signature verification must be enabled when a collection ID is set"));

      if (priv->local_gpg_verify_set)
        {
          g_key_file_set_boolean (config, group, "gpg-verify", priv->local_gpg_verify);

          if (!priv->local_collection_id_set || priv->local_collection_id == NULL)
            g_key_file_set_boolean (config, group, "gpg-verify-summary", priv->local_gpg_verify);
        }

      if (priv->local_sign_verify_set)
        {
          g_key_file_set_boolean (config, group, "sign-verify", priv->local_sign_verify);

          if (!priv->local_collection_id_set || priv->local_collection_id == NULL)
            g_key_file_set_boolean (config, group, "sign-verify-summary", priv->local_sign_verify);
        }
    }

  if (priv->local_noenumerate_set)
    g_key_file_set_boolean (config, group, "xa.noenumerate", priv->local_noenumerate);

  if (priv->local_disabled_set)
    g_key_file_set_boolean (config, group, "xa.disable", priv->local_disabled);

  if (priv->local_nodeps_set)
    g_key_file_set_boolean (config, group, "xa.nodeps", priv->local_nodeps);

  if (priv->local_prio_set)
    {
      g_autofree char *prio_as_string = g_strdup_printf ("%d", priv->local_prio);
      g_key_file_set_string (config, group, "xa.prio", prio_as_string);
    }

  return flatpak_dir_modify_remote (dir, priv->name, config, priv->local_gpg_key, cancellable, error);
}

/**
 * flatpak_remote_get_remote_type:
 * @self: a #FlatpakRemote
 *
 * Get the value of #FlatpakRemote:type.
 *
 * Returns: the type of remote this is
 * Since: 0.9.8
 */
FlatpakRemoteType
flatpak_remote_get_remote_type (FlatpakRemote *self)
{
  FlatpakRemotePrivate *priv = flatpak_remote_get_instance_private (self);

  g_return_val_if_fail (FLATPAK_IS_REMOTE (self), FLATPAK_REMOTE_TYPE_STATIC);

  return priv->type;
}
