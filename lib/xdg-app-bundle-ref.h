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

#ifndef __XDG_APP_BUNDLE_REF_H__
#define __XDG_APP_BUNDLE_REF_H__

typedef struct _XdgAppBundleRef XdgAppBundleRef;

#include <gio/gio.h>
#include <xdg-app-ref.h>

#define XDG_APP_TYPE_BUNDLE_REF xdg_app_bundle_ref_get_type()
#define XDG_APP_BUNDLE_REF(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), XDG_APP_TYPE_BUNDLE_REF, XdgAppBundleRef))
#define XDG_APP_IS_BUNDLE_REF(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), XDG_APP_TYPE_BUNDLE_REF))

XDG_APP_EXTERN GType xdg_app_bundle_ref_get_type (void);

struct _XdgAppBundleRef {
  XdgAppRef parent;
};

typedef struct {
  XdgAppRefClass parent_class;
} XdgAppBundleRefClass;

XDG_APP_EXTERN XdgAppBundleRef *xdg_app_bundle_ref_new            (GFile *file,
                                                                   GError **error);
XDG_APP_EXTERN GFile           *xdg_app_bundle_ref_get_file       (XdgAppBundleRef *self);
XDG_APP_EXTERN GBytes          *xdg_app_bundle_ref_get_metadata   (XdgAppBundleRef  *self);
XDG_APP_EXTERN GBytes          *xdg_app_bundle_ref_get_appdata    (XdgAppBundleRef  *self);
XDG_APP_EXTERN GBytes          *xdg_app_bundle_ref_get_icon       (XdgAppBundleRef  *self,
                                                                   int               size);
XDG_APP_EXTERN guint64          xdg_app_bundle_ref_get_installed_size (XdgAppBundleRef  *self);


#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdgAppBundleRef, g_object_unref)
#endif

#endif /* __XDG_APP_BUNDLE_REF_H__ */
