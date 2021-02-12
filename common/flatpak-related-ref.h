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

#ifndef __FLATPAK_RELATED_REF_H__
#define __FLATPAK_RELATED_REF_H__

typedef struct _FlatpakRelatedRef FlatpakRelatedRef;

#include <gio/gio.h>
#include <flatpak-ref.h>

G_BEGIN_DECLS

#define FLATPAK_TYPE_RELATED_REF flatpak_related_ref_get_type ()
#define FLATPAK_RELATED_REF(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_RELATED_REF, FlatpakRelatedRef))
#define FLATPAK_IS_RELATED_REF(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_RELATED_REF))

FLATPAK_EXTERN GType flatpak_related_ref_get_type (void);

struct _FlatpakRelatedRef
{
  FlatpakRef parent;
};

typedef struct
{
  FlatpakRefClass parent_class;
} FlatpakRelatedRefClass;

#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakRelatedRef, g_object_unref)
#endif

FLATPAK_EXTERN const char * const *flatpak_related_ref_get_subpaths (FlatpakRelatedRef * self);
FLATPAK_EXTERN gboolean     flatpak_related_ref_should_download (FlatpakRelatedRef *self);
FLATPAK_EXTERN gboolean     flatpak_related_ref_should_delete (FlatpakRelatedRef *self);
FLATPAK_EXTERN gboolean     flatpak_related_ref_should_autoprune (FlatpakRelatedRef *self);

G_END_DECLS

#endif /* __FLATPAK_RELATED_REF_H__ */
