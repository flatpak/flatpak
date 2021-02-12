/*
 * Copyright © 2018 Red Hat, Inc
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#if !defined(__FLATPAK_H_INSIDE__) && !defined(FLATPAK_COMPILATION)
#error "Only <flatpak.h> can be included directly."
#endif

#ifndef __FLATPAK_INSTANCE_H__
#define __FLATPAK_INSTANCE_H__

typedef struct _FlatpakInstance FlatpakInstance;

#include <glib-object.h>

G_BEGIN_DECLS

#define FLATPAK_TYPE_INSTANCE flatpak_instance_get_type ()
#define FLATPAK_INSTANCE(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_INSTANCE, FlatpakInstance))
#define FLATPAK_IS_INSTANCE(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_INSTANCE))

FLATPAK_EXTERN GType flatpak_instance_get_type (void);

struct _FlatpakInstance
{
  GObject parent;
};

typedef struct
{
  GObjectClass parent_class;
} FlatpakInstanceClass;


#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakInstance, g_object_unref)
#endif

FLATPAK_EXTERN GPtrArray *  flatpak_instance_get_all (void);

FLATPAK_EXTERN const char * flatpak_instance_get_id (FlatpakInstance *self);
FLATPAK_EXTERN const char * flatpak_instance_get_app (FlatpakInstance *self);
FLATPAK_EXTERN const char * flatpak_instance_get_arch (FlatpakInstance *self);
FLATPAK_EXTERN const char * flatpak_instance_get_branch (FlatpakInstance *self);
FLATPAK_EXTERN const char * flatpak_instance_get_commit (FlatpakInstance *self);
FLATPAK_EXTERN const char * flatpak_instance_get_runtime (FlatpakInstance *self);
FLATPAK_EXTERN const char * flatpak_instance_get_runtime_commit (FlatpakInstance *self);
FLATPAK_EXTERN int          flatpak_instance_get_pid (FlatpakInstance *self);
FLATPAK_EXTERN int          flatpak_instance_get_child_pid (FlatpakInstance *self);
FLATPAK_EXTERN GKeyFile *   flatpak_instance_get_info (FlatpakInstance *self);

FLATPAK_EXTERN gboolean     flatpak_instance_is_running (FlatpakInstance *self);

G_END_DECLS

#endif /* __FLATPAK_INSTANCE_H__ */
