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

#ifndef __XDG_APP_REF_H__
#define __XDG_APP_REF_H__

typedef struct _XdgAppRef XdgAppRef;

#include <glib-object.h>

#define XDG_APP_TYPE_REF xdg_app_ref_get_type()
#define XDG_APP_REF(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), XDG_APP_TYPE_REF, XdgAppRef))
#define XDG_APP_IS_REF(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), XDG_APP_TYPE_REF))

XDG_APP_EXTERN GType xdg_app_ref_get_type (void);

struct _XdgAppRef {
  GObject parent;
};

typedef struct {
  GObjectClass parent_class;
} XdgAppRefClass;


#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdgAppRef, g_object_unref)
#endif

/**
 * XdgAppRefKind:
 * @XDG_APP_REF_KIND_APP: An application
 * @XDG_APP_REF_KIND_RUNTIME: A runtime that applications can use.
 *
 * Currently xdg-app manages two types of binary artifacts: applications, and
 * runtimes. Applications contain a program that desktop users can run, while
 * runtimes contain only libraries and data.
 */
typedef enum {
  XDG_APP_REF_KIND_APP,
  XDG_APP_REF_KIND_RUNTIME,
} XdgAppRefKind;

XDG_APP_EXTERN const char *  xdg_app_ref_get_name    (XdgAppRef      *self);
XDG_APP_EXTERN const char *  xdg_app_ref_get_arch    (XdgAppRef      *self);
XDG_APP_EXTERN const char *  xdg_app_ref_get_branch (XdgAppRef      *self);
XDG_APP_EXTERN const char *  xdg_app_ref_get_commit  (XdgAppRef      *self);
XDG_APP_EXTERN XdgAppRefKind xdg_app_ref_get_kind    (XdgAppRef      *self);
XDG_APP_EXTERN char *        xdg_app_ref_format_ref  (XdgAppRef      *self);
XDG_APP_EXTERN XdgAppRef *   xdg_app_ref_parse       (const char     *ref,
                                                      GError        **error);

#endif /* __XDG_APP_REF_H__ */
