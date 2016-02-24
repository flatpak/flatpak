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
#error "Only <xdg-app.h> can be included directly."
#endif

#ifndef __XDG_APP_REMOTE_H__
#define __XDG_APP_REMOTE_H__

typedef struct _XdgAppRemote XdgAppRemote;

#include <gio/gio.h>
#include <xdg-app-remote-ref.h>

#define XDG_APP_TYPE_REMOTE xdg_app_remote_get_type()
#define XDG_APP_REMOTE(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), XDG_APP_TYPE_REMOTE, XdgAppRemote))
#define XDG_APP_IS_REMOTE(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), XDG_APP_TYPE_REMOTE))

XDG_APP_EXTERN GType xdg_app_remote_get_type (void);

struct _XdgAppRemote {
  GObject parent;
};

typedef struct {
  GObjectClass parent_class;
} XdgAppRemoteClass;

#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdgAppRemote, g_object_unref)
#endif

XDG_APP_EXTERN const char *  xdg_app_remote_get_name          (XdgAppRemote *self);
XDG_APP_EXTERN GFile *       xdg_app_remote_get_appstream_dir (XdgAppRemote *self,
                                                               const char   *arch);
XDG_APP_EXTERN GFile *       xdg_app_remote_get_appstream_timestamp (XdgAppRemote *self,
                                                                     const char   *arch);
XDG_APP_EXTERN char *        xdg_app_remote_get_url           (XdgAppRemote *self);
XDG_APP_EXTERN char *        xdg_app_remote_get_title         (XdgAppRemote *self);
XDG_APP_EXTERN gboolean      xdg_app_remote_get_gpg_verify    (XdgAppRemote *self);
XDG_APP_EXTERN gboolean      xdg_app_remote_get_noenumerate   (XdgAppRemote *self);
XDG_APP_EXTERN int           xdg_app_remote_get_prio          (XdgAppRemote *self);

#endif /* __XDG_APP_REMOTE_H__ */
