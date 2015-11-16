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

#ifndef __BUILDER_CONTEXT_H__
#define __BUILDER_CONTEXT_H__

#include <gio/gio.h>
#include <libsoup/soup.h>
#include "builder-options.h"

G_BEGIN_DECLS

/* BuilderContext defined in builder-options.h to fix include loop */

#define BUILDER_TYPE_CONTEXT (builder_context_get_type())
#define BUILDER_CONTEXT(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), BUILDER_TYPE_CONTEXT, BuilderContext))
#define BUILDER_IS_CONTEXT(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), BUILDER_TYPE_CONTEXT))

GType builder_context_get_type (void);

GFile *         builder_context_get_app_dir      (BuilderContext *self);
GFile *         builder_context_get_base_dir     (BuilderContext *self);
GFile *         builder_context_get_state_dir    (BuilderContext *self);
GFile *         builder_context_get_cache_dir    (BuilderContext *self);
GFile *         builder_context_get_download_dir (BuilderContext *self);
SoupSession *   builder_context_get_soup_session (BuilderContext *self);
const char *    builder_context_get_arch         (BuilderContext *self);
void            builder_context_set_arch         (BuilderContext *self,
                                                  const char     *arch);
int             builder_context_get_n_cpu        (BuilderContext *self);
BuilderOptions *builder_context_get_options      (BuilderContext *self);
void            builder_context_set_options      (BuilderContext *self,
                                                  BuilderOptions *option);

BuilderContext *builder_context_new              (GFile          *base_dir,
                                                  GFile          *app_dir);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(BuilderContext, g_object_unref)

G_END_DECLS

#endif /* __BUILDER_CONTEXT_H__ */
