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

#if !defined(__FLATPAK_H_INSIDE__) && !defined(FLATPAK_COMPILATION)
#error "Only <flatpak.h> can be included directly."
#endif

#ifndef __FLATPAK_BUNDLE_REF_H__
#define __FLATPAK_BUNDLE_REF_H__

typedef struct _FlatpakBundleRef FlatpakBundleRef;

#include <gio/gio.h>
#include <flatpak-ref.h>

#define FLATPAK_TYPE_BUNDLE_REF flatpak_bundle_ref_get_type ()
#define FLATPAK_BUNDLE_REF(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_BUNDLE_REF, FlatpakBundleRef))
#define FLATPAK_IS_BUNDLE_REF(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_BUNDLE_REF))

FLATPAK_EXTERN GType flatpak_bundle_ref_get_type (void);

struct _FlatpakBundleRef
{
  FlatpakRef parent;
};

typedef struct
{
  FlatpakRefClass parent_class;
} FlatpakBundleRefClass;

FLATPAK_EXTERN FlatpakBundleRef *flatpak_bundle_ref_new (GFile    *file,
                                                         GVariant *sign_data,
                                                         GError  **error);
FLATPAK_EXTERN GFile           *flatpak_bundle_ref_get_file (FlatpakBundleRef *self);
FLATPAK_EXTERN GBytes          *flatpak_bundle_ref_get_metadata (FlatpakBundleRef *self);
FLATPAK_EXTERN GBytes          *flatpak_bundle_ref_get_appstream (FlatpakBundleRef *self);
FLATPAK_EXTERN GBytes          *flatpak_bundle_ref_get_icon (FlatpakBundleRef *self,
                                                             int               size);
FLATPAK_EXTERN char            *flatpak_bundle_ref_get_origin (FlatpakBundleRef *self);
FLATPAK_EXTERN guint64          flatpak_bundle_ref_get_installed_size (FlatpakBundleRef *self);
FLATPAK_EXTERN char            *flatpak_bundle_ref_get_runtime_repo_url (FlatpakBundleRef *self);


#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakBundleRef, g_object_unref)
#endif

#endif /* __FLATPAK_BUNDLE_REF_H__ */
