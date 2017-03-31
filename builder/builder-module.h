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

#ifndef __BUILDER_MODULE_H__
#define __BUILDER_MODULE_H__

#include <json-glib/json-glib.h>

#include "builder-source.h"
#include "builder-options.h"

G_BEGIN_DECLS

typedef struct BuilderModule BuilderModule;

#define BUILDER_TYPE_MODULE (builder_module_get_type ())
#define BUILDER_MODULE(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), BUILDER_TYPE_MODULE, BuilderModule))
#define BUILDER_IS_MODULE(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), BUILDER_TYPE_MODULE))

/* Bump this if format changes in incompatible ways to force rebuild */
#define BUILDER_MODULE_CHECKSUM_VERSION "1"

GType builder_module_get_type (void);

const char * builder_module_get_name (BuilderModule *self);
gboolean     builder_module_is_enabled (BuilderModule *self,
                                        BuilderContext *context);
gboolean     builder_module_get_disabled (BuilderModule *self);
GList *      builder_module_get_sources (BuilderModule *self);
GList *      builder_module_get_modules (BuilderModule *self);
void         builder_module_set_json_path (BuilderModule *self,
                                           const char *json_path);
GPtrArray *  builder_module_get_changes (BuilderModule *self);
void         builder_module_set_changes (BuilderModule *self,
                                         GPtrArray     *changes);

gboolean     builder_module_show_deps (BuilderModule *self,
                                       BuilderContext *context,
                                       GError         **error);
gboolean builder_module_download_sources (BuilderModule  *self,
                                          gboolean        update_vcs,
                                          BuilderContext *context,
                                          GError        **error);
gboolean builder_module_extract_sources (BuilderModule  *self,
                                         GFile          *dest,
                                         BuilderContext *context,
                                         GError        **error);
gboolean builder_module_bundle_sources (BuilderModule  *self,
                                        BuilderContext *context,
                                        GError        **error);
gboolean builder_module_ensure_writable (BuilderModule  *self,
                                         BuilderCache   *cache,
                                         BuilderContext *context,
                                         GError        **error);
gboolean builder_module_build (BuilderModule  *self,
                               BuilderCache   *cache,
                               BuilderContext *context,
                               gboolean        run_shell,
                               GError        **error);
gboolean builder_module_update (BuilderModule  *self,
                                BuilderContext *context,
                                GError        **error);
void     builder_module_checksum (BuilderModule  *self,
                                  BuilderCache   *cache,
                                  BuilderContext *context);
void     builder_module_checksum_for_cleanup (BuilderModule  *self,
                                              BuilderCache   *cache,
                                              BuilderContext *context);
void     builder_module_checksum_for_platform (BuilderModule  *self,
                                               BuilderCache   *cache,
                                               BuilderContext *context);
void     builder_module_cleanup_collect (BuilderModule  *self,
                                         gboolean        platform,
                                         BuilderContext *context,
                                         GHashTable     *to_remove_ht);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BuilderModule, g_object_unref)

G_END_DECLS

#endif /* __BUILDER_MODULE_H__ */
