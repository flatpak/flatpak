/*
 * Copyright © 2017 Red Hat, Inc
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

#ifndef __BUILDER_EXTENSION_H__
#define __BUILDER_EXTENSION_H__

#include <json-glib/json-glib.h>

#include "builder-context.h"
#include "builder-cache.h"

G_BEGIN_DECLS

typedef struct BuilderExtension BuilderExtension;

#define BUILDER_TYPE_EXTENSION (builder_extension_get_type ())
#define BUILDER_EXTENSION(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), BUILDER_TYPE_EXTENSION, BuilderExtension))
#define BUILDER_IS_EXTENSION(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), BUILDER_TYPE_EXTENSION))

/* Bump this if format changes in incompatible ways to force rebuild */
#define BUILDER_EXTENSION_CHECKSUM_VERSION "1"

GType builder_extension_get_type (void);

void   builder_extension_set_name (BuilderExtension *self,
                                   const char *name);
const char * builder_extension_get_name (BuilderExtension *self);

gboolean builder_extension_is_bundled (BuilderExtension *self);
const char * builder_extension_get_directory (BuilderExtension *self);

void    builder_extension_add_finish_args (BuilderExtension  *self,
                                           GPtrArray *args);
void     builder_extension_checksum (BuilderExtension  *self,
                                     BuilderCache   *cache,
                                     BuilderContext *context);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BuilderExtension, g_object_unref)

G_END_DECLS

#endif /* __BUILDER_EXTENSION_H__ */
