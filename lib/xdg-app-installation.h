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

#if !defined (__XDG_APP_H_INSIDE__) && !defined (XDG_APP_COMPILATION)
#error "Only <xdg-app.h> can be included installationectly."
#endif

#ifndef __XDG_APP_INSTALLATION_H__
#define __XDG_APP_INSTALLATION_H__

typedef struct XdgAppInstallation XdgAppInstallation;

#include <gio/gio.h>
#include <xdg-app-installed-ref.h>
#include <xdg-app-remote.h>

#define XDG_APP_TYPE_INSTALLATION xdg_app_installation_get_type()
#define XDG_APP_INSTALLATION(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), XDG_APP_TYPE_INSTALLATION, XdgAppInstallation))
#define XDG_APP_IS_INSTALLATION(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), XDG_APP_TYPE_INSTALLATION))

XDG_APP_EXTERN GType xdg_app_installation_get_type (void);

struct XdgAppInstallation {
  GObject parent;
};

typedef struct {
  GObjectClass parent_class;
} XdgAppInstallationClass;


#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdgAppInstallation, g_object_unref)
#endif

XDG_APP_EXTERN XdgAppInstallation *xdg_app_installation_new_system (void);
XDG_APP_EXTERN XdgAppInstallation *xdg_app_installation_new_user (void);
XDG_APP_EXTERN XdgAppInstallation *xdg_app_installation_new_for_path (GFile *path,
                                                                      gboolean user);

XDG_APP_EXTERN gboolean             xdg_app_installation_get_is_user               (XdgAppInstallation  *self);
XDG_APP_EXTERN XdgAppInstalledRef **xdg_app_installation_list_installed_refs       (XdgAppInstallation  *self,
                                                                                    XdgAppRefKind        kind,
                                                                                    GCancellable        *cancellable,
                                                                                    GError             **error);
XDG_APP_EXTERN XdgAppInstalledRef * xdg_app_installation_get_installed_ref         (XdgAppInstallation  *self,
                                                                                    XdgAppRefKind        kind,
                                                                                    const char          *name,
                                                                                    const char          *arch,
                                                                                    const char          *version,
                                                                                    GCancellable        *cancellable,
                                                                                    GError             **error);
XDG_APP_EXTERN XdgAppInstalledRef * xdg_app_installation_get_current_installed_app (XdgAppInstallation  *self,
                                                                                    const char          *name,
                                                                                    GCancellable        *cancellable,
                                                                                    GError             **error);
XDG_APP_EXTERN XdgAppRemote **      xdg_app_installation_list_remotes              (XdgAppInstallation  *self,
                                                                                    GCancellable        *cancellable,
                                                                                    GError             **error);
XDG_APP_EXTERN char *              xdg_app_installation_load_app_overrides         (XdgAppInstallation *self,
                                                                                    const char         *app_id,
                                                                                    GCancellable       *cancellable,
                                                                                    GError            **error);

#endif /* __XDG_APP_INSTALLATION_H__ */
