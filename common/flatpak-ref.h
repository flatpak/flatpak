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

#ifndef __FLATPAK_REF_H__
#define __FLATPAK_REF_H__

typedef struct _FlatpakRef FlatpakRef;

#include <glib-object.h>

#define FLATPAK_TYPE_REF flatpak_ref_get_type ()
#define FLATPAK_REF(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_REF, FlatpakRef))
#define FLATPAK_IS_REF(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_REF))

FLATPAK_EXTERN GType flatpak_ref_get_type (void);

struct _FlatpakRef
{
  GObject parent;
};

typedef struct
{
  GObjectClass parent_class;
} FlatpakRefClass;


#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakRef, g_object_unref)
#endif

/**
 * FlatpakRefKind:
 * @FLATPAK_REF_KIND_APP: An application
 * @FLATPAK_REF_KIND_RUNTIME: A runtime that applications can use.
 *
 * The kind of artifact that a FlatpakRef refers to.
 */
typedef enum {
  FLATPAK_REF_KIND_APP,
  FLATPAK_REF_KIND_RUNTIME,
} FlatpakRefKind;

FLATPAK_EXTERN const char *  flatpak_ref_get_name (FlatpakRef *self);
FLATPAK_EXTERN const char *  flatpak_ref_get_arch (FlatpakRef *self);
FLATPAK_EXTERN const char *  flatpak_ref_get_branch (FlatpakRef *self);
FLATPAK_EXTERN const char *  flatpak_ref_get_commit (FlatpakRef *self);
FLATPAK_EXTERN FlatpakRefKind flatpak_ref_get_kind (FlatpakRef *self);
FLATPAK_EXTERN char *        flatpak_ref_format_ref (FlatpakRef *self);
FLATPAK_EXTERN const char *   flatpak_ref_format_ref_cached (FlatpakRef *self);
FLATPAK_EXTERN FlatpakRef *   flatpak_ref_parse (const char *ref,
                                                 GError    **error);
FLATPAK_EXTERN const char *   flatpak_ref_get_collection_id (FlatpakRef *self);

#endif /* __FLATPAK_REF_H__ */
