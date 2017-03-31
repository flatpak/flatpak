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

#ifndef __BUILDER_MANIFEST_H__
#define __BUILDER_MANIFEST_H__

#include <json-glib/json-glib.h>

#include "flatpak-run.h"
#include "builder-options.h"
#include "builder-module.h"
#include "builder-cache.h"

G_BEGIN_DECLS

typedef struct BuilderManifest BuilderManifest;

#define BUILDER_TYPE_MANIFEST (builder_manifest_get_type ())
#define BUILDER_MANIFEST(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), BUILDER_TYPE_MANIFEST, BuilderManifest))
#define BUILDER_IS_MANIFEST(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), BUILDER_TYPE_MANIFEST))

/* Bump this if format changes in incompatible ways to force rebuild */
#define BUILDER_MANIFEST_CHECKSUM_VERSION "4"
#define BUILDER_MANIFEST_CHECKSUM_CLEANUP_VERSION "1"
#define BUILDER_MANIFEST_CHECKSUM_FINISH_VERSION "2"
#define BUILDER_MANIFEST_CHECKSUM_PLATFORM_VERSION "1"

GType builder_manifest_get_type (void);

void builder_manifest_set_demarshal_buid_context (BuilderContext *build_context);

const char *    builder_manifest_get_id (BuilderManifest *self);
char *          builder_manifest_get_locale_id (BuilderManifest *self);
char *          builder_manifest_get_debug_id (BuilderManifest *self);
char *          builder_manifest_get_sources_id (BuilderManifest *self);
const char *    builder_manifest_get_id_platform (BuilderManifest *self);
char *          builder_manifest_get_locale_id_platform (BuilderManifest *self);
BuilderOptions *builder_manifest_get_build_options (BuilderManifest *self);
GList *         builder_manifest_get_modules (BuilderManifest *self);
const char *    builder_manifest_get_branch (BuilderManifest *self,
                                             const char *default_branch);

gboolean        builder_manifest_start (BuilderManifest *self,
                                        gboolean         allow_missing_runtimes,
                                        BuilderContext  *context,
                                        GError         **error);
gboolean        builder_manifest_init_app_dir (BuilderManifest *self,
                                               BuilderCache    *cache,
                                               BuilderContext  *context,
                                               GError         **error);
gboolean        builder_manifest_download (BuilderManifest *self,
                                           gboolean         update_vcs,
                                           const char      *only_module,
                                           BuilderContext  *context,
                                           GError         **error);
gboolean        builder_manifest_build_shell (BuilderManifest *self,
                                              BuilderContext  *context,
                                              const char      *modulename,
                                              GError         **error);
gboolean        builder_manifest_build (BuilderManifest *self,
                                        BuilderCache    *cache,
                                        BuilderContext  *context,
                                        GError         **error);
gboolean        builder_manifest_run (BuilderManifest *self,
                                      BuilderContext  *context,
                                      FlatpakContext  *arg_context,
                                      char           **argv,
                                      int              argc,
                                      GError         **error);
gboolean        builder_manifest_show_deps (BuilderManifest *self,
                                            BuilderContext  *context,
                                            GError         **error);
void            builder_manifest_checksum (BuilderManifest *self,
                                           BuilderCache    *cache,
                                           BuilderContext  *context);
gboolean        builder_manifest_cleanup (BuilderManifest *self,
                                          BuilderCache    *cache,
                                          BuilderContext  *context,
                                          GError         **error);
gboolean        builder_manifest_finish (BuilderManifest *self,
                                         BuilderCache    *cache,
                                         BuilderContext  *context,
                                         GError         **error);
gboolean        builder_manifest_create_platform (BuilderManifest *self,
                                                  BuilderCache    *cache,
                                                  BuilderContext  *context,
                                                  GError         **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BuilderManifest, g_object_unref)

G_END_DECLS

#endif /* __BUILDER_MANIFEST_H__ */
