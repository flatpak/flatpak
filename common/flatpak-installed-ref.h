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

#ifndef __FLATPAK_INSTALLED_REF_H__
#define __FLATPAK_INSTALLED_REF_H__

typedef struct _FlatpakInstalledRef FlatpakInstalledRef;

#include <gio/gio.h>
#include <flatpak-ref.h>

G_BEGIN_DECLS

#define FLATPAK_TYPE_INSTALLED_REF flatpak_installed_ref_get_type ()
#define FLATPAK_INSTALLED_REF(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_INSTALLED_REF, FlatpakInstalledRef))
#define FLATPAK_IS_INSTALLED_REF(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_INSTALLED_REF))

FLATPAK_EXTERN GType flatpak_installed_ref_get_type (void);

struct _FlatpakInstalledRef
{
  FlatpakRef parent;
};

typedef struct
{
  FlatpakRefClass parent_class;
} FlatpakInstalledRefClass;

#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakInstalledRef, g_object_unref)
#endif

FLATPAK_EXTERN const char  *flatpak_installed_ref_get_origin (FlatpakInstalledRef  * self);
FLATPAK_EXTERN const char * const *flatpak_installed_ref_get_subpaths (FlatpakInstalledRef *self);
FLATPAK_EXTERN guint64      flatpak_installed_ref_get_installed_size (FlatpakInstalledRef *self);
FLATPAK_EXTERN const char  *flatpak_installed_ref_get_deploy_dir (FlatpakInstalledRef *self);
FLATPAK_EXTERN const char  *flatpak_installed_ref_get_latest_commit (FlatpakInstalledRef *self);
FLATPAK_EXTERN const char  *flatpak_installed_ref_get_appdata_name (FlatpakInstalledRef *self);
FLATPAK_EXTERN const char  *flatpak_installed_ref_get_appdata_summary (FlatpakInstalledRef *self);
FLATPAK_EXTERN const char  *flatpak_installed_ref_get_appdata_version (FlatpakInstalledRef *self);
FLATPAK_EXTERN const char  *flatpak_installed_ref_get_appdata_license (FlatpakInstalledRef *self);
FLATPAK_EXTERN const char  *flatpak_installed_ref_get_appdata_content_rating_type (FlatpakInstalledRef *self);
FLATPAK_EXTERN GHashTable  *flatpak_installed_ref_get_appdata_content_rating (FlatpakInstalledRef *self);
FLATPAK_EXTERN gboolean     flatpak_installed_ref_get_is_current (FlatpakInstalledRef *self);
FLATPAK_EXTERN GBytes      *flatpak_installed_ref_load_metadata (FlatpakInstalledRef *self,
                                                                 GCancellable        *cancellable,
                                                                 GError             **error);
FLATPAK_EXTERN GBytes      *flatpak_installed_ref_load_appdata (FlatpakInstalledRef *self,
                                                                GCancellable        *cancellable,
                                                                GError             **error);
FLATPAK_EXTERN const char * flatpak_installed_ref_get_eol (FlatpakInstalledRef *self);
FLATPAK_EXTERN const char * flatpak_installed_ref_get_eol_rebase (FlatpakInstalledRef *self);

G_END_DECLS

#endif /* __FLATPAK_INSTALLED_REF_H__ */
