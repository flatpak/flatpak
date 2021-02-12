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

#ifndef __FLATPAK_REMOTE_REF_H__
#define __FLATPAK_REMOTE_REF_H__

typedef struct _FlatpakRemoteRef FlatpakRemoteRef;

#include <gio/gio.h>
#include <flatpak-ref.h>

G_BEGIN_DECLS

#define FLATPAK_TYPE_REMOTE_REF flatpak_remote_ref_get_type ()
#define FLATPAK_REMOTE_REF(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_REMOTE_REF, FlatpakRemoteRef))
#define FLATPAK_IS_REMOTE_REF(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_REMOTE_REF))

FLATPAK_EXTERN GType flatpak_remote_ref_get_type (void);

struct _FlatpakRemoteRef
{
  FlatpakRef parent;
};

typedef struct
{
  FlatpakRefClass parent_class;
} FlatpakRemoteRefClass;

FLATPAK_EXTERN const char * flatpak_remote_ref_get_remote_name (FlatpakRemoteRef *self);
FLATPAK_EXTERN guint64      flatpak_remote_ref_get_installed_size (FlatpakRemoteRef *self);
FLATPAK_EXTERN guint64      flatpak_remote_ref_get_download_size (FlatpakRemoteRef *self);
FLATPAK_EXTERN GBytes *     flatpak_remote_ref_get_metadata (FlatpakRemoteRef *self);
FLATPAK_EXTERN const char * flatpak_remote_ref_get_eol (FlatpakRemoteRef *self);
FLATPAK_EXTERN const char * flatpak_remote_ref_get_eol_rebase (FlatpakRemoteRef *self);

#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakRemoteRef, g_object_unref)
#endif

G_END_DECLS

#endif /* __FLATPAK_REMOTE_REF_H__ */
