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

#ifndef __BUILDER_SOURCE_H__
#define __BUILDER_SOURCE_H__

#include <json-glib/json-glib.h>

#include "builder-context.h"
#include "builder-cache.h"

G_BEGIN_DECLS

typedef struct BuilderSource BuilderSource;

#define BUILDER_TYPE_SOURCE (builder_source_get_type ())
#define BUILDER_SOURCE(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), BUILDER_TYPE_SOURCE, BuilderSource))
#define BUILDER_SOURCE_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), BUILDER_TYPE_SOURCE, BuilderSourceClass))
#define BUILDER_IS_SOURCE(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), BUILDER_TYPE_SOURCE))
#define BUILDER_IS_SOURCE_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), BUILDER_TYPE_SOURCE))
#define BUILDER_SOURCE_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), BUILDER_TYPE_SOURCE, BuilderSourceClass))

struct BuilderSource
{
  GObject parent;

  char   *dest;
  char  **only_arches;
  char  **skip_arches;
};

typedef struct
{
  GObjectClass parent_class;

  gboolean (* show_deps)(BuilderSource  *self,
                         GError        **error);
  gboolean (* download)(BuilderSource  *self,
                        gboolean        update_vcs,
                        BuilderContext *context,
                        GError        **error);
  gboolean (* extract)(BuilderSource  *self,
                       GFile          *dest,
                       BuilderOptions *build_options,
                       BuilderContext *context,
                       GError        **error);
  gboolean (* bundle)(BuilderSource  *self,
                      BuilderContext *context,
                      GError        **error);
  gboolean (* update)(BuilderSource  *self,
                      BuilderContext *context,
                      GError        **error);
  void (* checksum)(BuilderSource  *self,
                    BuilderCache   *cache,
                    BuilderContext *context);
} BuilderSourceClass;

GType builder_source_get_type (void);

BuilderSource * builder_source_from_json (JsonNode *node);
JsonNode *      builder_source_to_json (BuilderSource *self);

gboolean builder_source_show_deps (BuilderSource  *self,
                                   GError        **error);
gboolean builder_source_download (BuilderSource  *self,
                                  gboolean        update_vcs,
                                  BuilderContext *context,
                                  GError        **error);
gboolean builder_source_extract (BuilderSource  *self,
                                 GFile          *dest,
                                 BuilderOptions *build_options,
                                 BuilderContext *context,
                                 GError        **error);
gboolean builder_source_bundle (BuilderSource  *self,
                                BuilderContext *context,
                                GError        **error);
gboolean builder_source_update (BuilderSource  *self,
                                BuilderContext *context,
                                GError        **error);

void     builder_source_checksum (BuilderSource  *self,
                                  BuilderCache   *cache,
                                  BuilderContext *context);

gboolean builder_source_is_enabled (BuilderSource *self,
                                    BuilderContext *context);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BuilderSource, g_object_unref)

G_END_DECLS

#endif /* __BUILDER_SOURCE_H__ */
