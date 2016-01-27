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

typedef struct _XdgAppInstallation XdgAppInstallation;

#include <gio/gio.h>
#include <xdg-app-installed-ref.h>
#include <xdg-app-remote.h>

#define XDG_APP_TYPE_INSTALLATION xdg_app_installation_get_type()
#define XDG_APP_INSTALLATION(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), XDG_APP_TYPE_INSTALLATION, XdgAppInstallation))
#define XDG_APP_IS_INSTALLATION(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), XDG_APP_TYPE_INSTALLATION))

XDG_APP_EXTERN GType xdg_app_installation_get_type (void);

struct _XdgAppInstallation {
  GObject parent;
};

typedef struct {
  GObjectClass parent_class;
} XdgAppInstallationClass;

/**
 * XdgAppUpdateFlags:
 * @XDG_APP_UPDATE_FLAGS_NONE: Fetch remote builds and install the latest one (default)
 * @XDG_APP_UPDATE_FLAGS_NO_DEPLOY: Don't install any new builds that might be fetched
 * @XDG_APP_UPDATE_FLAGS_NO_PULL: Don't try to fetch new builds from the remote repo
 *
 * Flags to alter the behavior of xdg_app_installation_update().
 */
typedef enum {
  XDG_APP_UPDATE_FLAGS_NONE      = 0,
  XDG_APP_UPDATE_FLAGS_NO_DEPLOY = (1<<0),
  XDG_APP_UPDATE_FLAGS_NO_PULL   = (1<<1),
} XdgAppUpdateFlags;


#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdgAppInstallation, g_object_unref)
#endif

XDG_APP_EXTERN XdgAppInstallation *xdg_app_installation_new_system (GCancellable *cancellable,
                                                                    GError **error);
XDG_APP_EXTERN XdgAppInstallation *xdg_app_installation_new_user (GCancellable *cancellable,
                                                                  GError **error);
XDG_APP_EXTERN XdgAppInstallation *xdg_app_installation_new_for_path (GFile *path,
                                                                      gboolean user,
                                                                      GCancellable *cancellable,
                                                                      GError **error);

typedef void (*XdgAppProgressCallback)(const char *status,
                                       guint progress,
                                       gboolean estimating,
                                       gpointer user_data);

XDG_APP_EXTERN gboolean             xdg_app_installation_get_is_user               (XdgAppInstallation  *self);
XDG_APP_EXTERN gboolean             xdg_app_installation_launch                    (XdgAppInstallation  *self,
                                                                                    const char          *name,
                                                                                    const char          *arch,
                                                                                    const char          *branch,
                                                                                    const char          *commit,
                                                                                    GCancellable        *cancellable,
                                                                                    GError             **error);
XDG_APP_EXTERN GFileMonitor        *xdg_app_installation_create_monitor            (XdgAppInstallation  *self,
                                                                                    GCancellable        *cancellable,
                                                                                    GError             **error);
XDG_APP_EXTERN GPtrArray           *xdg_app_installation_list_installed_refs       (XdgAppInstallation  *self,
                                                                                    GCancellable        *cancellable,
                                                                                    GError             **error);
XDG_APP_EXTERN GPtrArray           *xdg_app_installation_list_installed_refs_by_kind (XdgAppInstallation  *self,
                                                                                    XdgAppRefKind        kind,
                                                                                    GCancellable        *cancellable,
                                                                                    GError             **error);
XDG_APP_EXTERN GPtrArray           *xdg_app_installation_list_installed_refs_for_update (XdgAppInstallation  *self,
                                                                                         GCancellable        *cancellable,
                                                                                         GError             **error);
XDG_APP_EXTERN XdgAppInstalledRef * xdg_app_installation_get_installed_ref         (XdgAppInstallation  *self,
                                                                                    XdgAppRefKind        kind,
                                                                                    const char          *name,
                                                                                    const char          *arch,
                                                                                    const char          *branch,
                                                                                    GCancellable        *cancellable,
                                                                                    GError             **error);
XDG_APP_EXTERN XdgAppInstalledRef * xdg_app_installation_get_current_installed_app (XdgAppInstallation  *self,
                                                                                    const char          *name,
                                                                                    GCancellable        *cancellable,
                                                                                    GError             **error);
XDG_APP_EXTERN GPtrArray           *xdg_app_installation_list_remotes              (XdgAppInstallation  *self,
                                                                                    GCancellable        *cancellable,
                                                                                    GError             **error);
XDG_APP_EXTERN char *              xdg_app_installation_load_app_overrides         (XdgAppInstallation *self,
                                                                                    const char         *app_id,
                                                                                    GCancellable       *cancellable,
                                                                                    GError            **error);
XDG_APP_EXTERN XdgAppInstalledRef * xdg_app_installation_install                   (XdgAppInstallation  *self,
                                                                                    const char          *remote_name,
                                                                                    XdgAppRefKind        kind,
                                                                                    const char          *name,
                                                                                    const char          *arch,
                                                                                    const char          *branch,
                                                                                    XdgAppProgressCallback  progress,
                                                                                    gpointer             progress_data,
                                                                                    GCancellable        *cancellable,
                                                                                    GError             **error);
XDG_APP_EXTERN XdgAppInstalledRef * xdg_app_installation_update                    (XdgAppInstallation  *self,
                                                                                    XdgAppUpdateFlags    flags,
                                                                                    XdgAppRefKind        kind,
                                                                                    const char          *name,
                                                                                    const char          *arch,
                                                                                    const char          *branch,
                                                                                    XdgAppProgressCallback  progress,
                                                                                    gpointer             progress_data,
                                                                                    GCancellable        *cancellable,
                                                                                    GError             **error);
XDG_APP_EXTERN gboolean             xdg_app_installation_uninstall                 (XdgAppInstallation  *self,
                                                                                    XdgAppRefKind        kind,
                                                                                    const char          *name,
                                                                                    const char          *arch,
                                                                                    const char          *branch,
                                                                                    XdgAppProgressCallback  progress,
                                                                                    gpointer             progress_data,
                                                                                    GCancellable        *cancellable,
                                                                                    GError             **error);

XDG_APP_EXTERN gboolean          xdg_app_installation_fetch_remote_size_sync     (XdgAppInstallation  *self,
                                                                                  const char          *remote_name,
                                                                                  const char          *commit,
                                                                                  guint64             *download_size,
                                                                                  guint64             *installed_size,
                                                                                  GCancellable        *cancellable,
                                                                                  GError             **error);
XDG_APP_EXTERN GBytes        *   xdg_app_installation_fetch_remote_metadata_sync (XdgAppInstallation  *self,
                                                                                  const char          *remote_name,
                                                                                  const char          *commit,
                                                                                  GCancellable        *cancellable,
                                                                                  GError             **error);
XDG_APP_EXTERN GPtrArray    *    xdg_app_installation_list_remote_refs_sync      (XdgAppInstallation  *self,
                                                                                  const char          *remote_name,
                                                                                  GCancellable        *cancellable,
                                                                                  GError             **error);
XDG_APP_EXTERN XdgAppRemoteRef  *xdg_app_installation_fetch_remote_ref_sync      (XdgAppInstallation  *self,
                                                                                  const char          *remote_name,
                                                                                  XdgAppRefKind        kind,
                                                                                  const char          *name,
                                                                                  const char          *arch,
                                                                                  const char          *branch,
                                                                                  GCancellable        *cancellable,
                                                                                  GError             **error);
XDG_APP_EXTERN gboolean          xdg_app_installation_update_appstream_sync      (XdgAppInstallation  *self,
                                                                                  const char          *remote_name,
                                                                                  const char          *arch,
                                                                                  gboolean            *out_changed,
                                                                                  GCancellable        *cancellable,
                                                                                  GError             **error);

#endif /* __XDG_APP_INSTALLATION_H__ */
