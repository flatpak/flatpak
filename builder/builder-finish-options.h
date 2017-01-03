/*
 * Copyright © 2016 Kinvolk GmbH
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
 *       Michal Rostecki <michal@kinvolk.io>
 */

#ifndef __BUILDER_FINISH_OPTIONS_H__
#define __BUILDER_FINISH_OPTIONS_H__

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct BuilderContext BuilderContext;
typedef struct BuilderFinishOptions BuilderFinishOptions;

#define BUILDER_TYPE_FINISH_OPTIONS (builder_finish_options_get_type ())
#define BUILDER_FINISH_OPTIONS(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), BUILDER_TYPE_FINISH_OPTIONS, BuilderFinishOptions))
#define BUILDER_IS_FINISH_OPTIONS(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), BUILDER_TYPE_FINISH_OPTIONS))

GType builder_finish_options_get_type (void);

char ** builder_finish_options_get_finish_args (BuilderFinishOptions *self,
                                                BuilderContext *context);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BuilderFinishOptions, g_object_unref)

G_END_DECLS

#endif /* __BUILDER_FINISH_OPTIONS_H__ */
