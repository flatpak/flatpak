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

#include "xdg-app-installation.h"
#include "xdg-app-installed-ref-private.h"
#include "xdg-app-remote-private.h"
#include "xdg-app-enum-types.h"
#include "xdg-app-dir.h"
#include "xdg-app-utils.h"

typedef struct _XdgAppInstallationPrivate XdgAppInstallationPrivate;

struct _XdgAppInstallationPrivate
{
  XdgAppDir *dir;
};

G_DEFINE_TYPE_WITH_PRIVATE (XdgAppInstallation, xdg_app_installation, G_TYPE_OBJECT)

enum {
  PROP_0,
};

static void
xdg_app_installation_finalize (GObject *object)
{
  XdgAppInstallation *self = XDG_APP_INSTALLATION (object);
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);

  g_object_unref (priv->dir);

  G_OBJECT_CLASS (xdg_app_installation_parent_class)->finalize (object);
}

static void
xdg_app_installation_set_property (GObject         *object,
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
xdg_app_installation_get_property (GObject         *object,
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
xdg_app_installation_class_init (XdgAppInstallationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = xdg_app_installation_get_property;
  object_class->set_property = xdg_app_installation_set_property;
  object_class->finalize = xdg_app_installation_finalize;

}

static void
xdg_app_installation_init (XdgAppInstallation *self)
{
}

static XdgAppInstallation *
xdg_app_installation_new_for_dir (XdgAppDir *dir)
{
  XdgAppInstallation *self = g_object_new (XDG_APP_TYPE_INSTALLATION, NULL);
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);

  priv->dir = dir;
  return self;
}

XdgAppInstallation *
xdg_app_installation_new_system (void)
{
  return xdg_app_installation_new_for_dir (xdg_app_dir_get_system ());
}

XdgAppInstallation *
xdg_app_installation_new_user (void)
{
  return xdg_app_installation_new_for_dir (xdg_app_dir_get_user ());
}

XdgAppInstallation *
xdg_app_installation_new_for_path (GFile *path, gboolean user)
{
  return xdg_app_installation_new_for_dir (xdg_app_dir_new (path, user));
}

gboolean
xdg_app_installation_get_is_user (XdgAppInstallation *self)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);

  return xdg_app_dir_is_user (priv->dir);
}

static XdgAppInstalledRef *
get_ref (XdgAppInstallation *self,
         const char *full_ref,
         GCancellable *cancellable)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_auto(GStrv) parts = NULL;
  g_autofree char *origin = NULL;
  g_autofree char *commit = NULL;
  g_autoptr(XdgAppDir) dir = NULL;
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(GFile) deploy_subdir = NULL;
  g_autofree char *deploy_path = NULL;
  gboolean is_current = FALSE;

  parts = g_strsplit (full_ref, "/", -1);

  origin = xdg_app_dir_get_origin (priv->dir, full_ref, NULL, NULL);
  commit = xdg_app_dir_read_active (priv->dir, full_ref, cancellable);
  deploy_dir = xdg_app_dir_get_deploy_dir  (priv->dir, full_ref);
  if (deploy_dir && commit)
    {
      deploy_subdir = g_file_get_child (deploy_dir, commit);
      deploy_path = g_file_get_path (deploy_subdir);
    }

  if (strcmp (parts[0], "app") == 0)
    {
      g_autofree char *current =
        xdg_app_dir_current_ref (priv->dir, parts[1], cancellable);
      if (current && strcmp (full_ref, current) == 0)
        is_current = TRUE;
    }

  return xdg_app_installed_ref_new (full_ref,
                                    commit,
                                    origin,
                                    deploy_path,
                                    is_current);
}

XdgAppInstalledRef *
xdg_app_installation_get_installed_ref (XdgAppInstallation *self,
                                        XdgAppRefKind kind,
                                        const char *name,
                                        const char *arch,
                                        const char *version,
                                        GCancellable *cancellable,
                                        GError **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autoptr(GFile) deploy = NULL;
  g_autofree char *ref = NULL;

  if (arch == NULL)
    arch = xdg_app_get_arch ();

  if (kind == XDG_APP_REF_KIND_APP)
    ref = xdg_app_build_app_ref (name, version, arch);
  else
    ref = xdg_app_build_runtime_ref (name, version, arch);


  deploy = xdg_app_dir_get_if_deployed (priv->dir,
                                        ref, NULL, cancellable);
  if (deploy == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Ref %s no installed", ref);
      return NULL;
    }

  return get_ref (self, ref, cancellable);
}

XdgAppInstalledRef *
xdg_app_installation_get_current_installed_app (XdgAppInstallation *self,
                                                const char *name,
                                                GCancellable *cancellable,
                                                GError **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_autoptr(GFile) deploy = NULL;
  g_autofree char *current =
    xdg_app_dir_current_ref (priv->dir, name, cancellable);

  if (current)
    deploy = xdg_app_dir_get_if_deployed (priv->dir,
                                          current, NULL, cancellable);

  if (deploy == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "App %s no installed", name);
      return NULL;
    }

  return get_ref (self, current, cancellable);
}

XdgAppInstalledRef **
xdg_app_installation_list_installed_refs (XdgAppInstallation *self,
                                          XdgAppRefKind kind,
                                          GCancellable *cancellable,
                                          GError **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_auto(GStrv) raw_refs = NULL;
  g_autoptr(GPtrArray) refs = g_ptr_array_new_with_free_func (g_object_unref);
  int i;

  if (!xdg_app_dir_list_refs (priv->dir,
                              kind == XDG_APP_REF_KIND_APP ? "app" : "runtime",
&raw_refs,
                              cancellable, error))
    return NULL;

  for (i = 0; raw_refs[i] != NULL; i++)
    g_ptr_array_add (refs,
                     get_ref (self, raw_refs[i], cancellable));

  g_ptr_array_add (refs, NULL);
  return (XdgAppInstalledRef **)g_ptr_array_free (g_steal_pointer (&refs), FALSE);
}

XdgAppRemote **
xdg_app_installation_list_remotes (XdgAppInstallation  *self,
                                   GCancellable        *cancellable,
                                   GError             **error)
{
  XdgAppInstallationPrivate *priv = xdg_app_installation_get_instance_private (self);
  g_auto(GStrv) remote_names = NULL;
  g_autoptr(GPtrArray) remotes = g_ptr_array_new_with_free_func (g_object_unref);
  int i;

  remote_names = xdg_app_dir_list_remotes (priv->dir, cancellable, error);
  if (remote_names == NULL)
    return NULL;

  for (i = 0; remote_names[i] != NULL; i++)
    g_ptr_array_add (remotes,
                     xdg_app_remote_new (priv->dir, remote_names[i]));

  g_ptr_array_add (remotes, NULL);
  return (XdgAppRemote **)g_ptr_array_free (g_steal_pointer (&remotes), FALSE);
}
