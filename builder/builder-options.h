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

#ifndef __BUILDER_OPTIONS_H__
#define __BUILDER_OPTIONS_H__

#include <json-glib/json-glib.h>
#include "builder-cache.h"

G_BEGIN_DECLS

typedef struct BuilderContext BuilderContext;
typedef struct BuilderOptions BuilderOptions;

#define BUILDER_TYPE_OPTIONS (builder_options_get_type ())
#define BUILDER_OPTIONS(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), BUILDER_TYPE_OPTIONS, BuilderOptions))
#define BUILDER_IS_OPTIONS(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), BUILDER_TYPE_OPTIONS))

/* Bump this if format changes in incompatible ways to force rebuild */
#define BUILDER_OPTION_CHECKSUM_VERSION "1"

GType builder_options_get_type (void);

const char *builder_options_get_cflags (BuilderOptions *self,
                                        BuilderContext *context);
const char *builder_options_get_cxxflags (BuilderOptions *self,
                                          BuilderContext *context);
const char *builder_options_get_prefix (BuilderOptions *self,
                                        BuilderContext *context);
char **     builder_options_get_env (BuilderOptions *self,
                                     BuilderContext *context);
char **     builder_options_get_build_args (BuilderOptions *self,
                                            BuilderContext *context,
                                            GError **error);
char **     builder_options_get_config_opts (BuilderOptions *self,
                                             BuilderContext *context,
                                             char          **base_opts);
void        builder_options_checksum (BuilderOptions *self,
                                      BuilderCache   *cache,
                                      BuilderContext *context);
gboolean    builder_options_get_no_debuginfo (BuilderOptions *self,
                                              BuilderContext *context);
gboolean    builder_options_get_strip (BuilderOptions *self,
                                       BuilderContext *context);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BuilderOptions, g_object_unref)

G_END_DECLS

#endif /* __BUILDER_OPTIONS_H__ */
