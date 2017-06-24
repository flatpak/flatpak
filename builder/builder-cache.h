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

#ifndef __BUILDER_CACHE_H__
#define __BUILDER_CACHE_H__

#include <gio/gio.h>
#include <libglnx/libglnx.h>

G_BEGIN_DECLS

typedef struct BuilderCache BuilderCache;
typedef struct BuilderContext BuilderContext;

#define BUILDER_TYPE_CACHE (builder_cache_get_type ())
#define BUILDER_CACHE(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), BUILDER_TYPE_CACHE, BuilderCache))
#define BUILDER_IS_CACHE(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), BUILDER_TYPE_CACHE))

GType builder_cache_get_type (void);

BuilderCache *builder_cache_new (BuilderContext *context,
                                 GFile      *app_dir,
                                 const char *branch);
void          builder_cache_disable_lookups (BuilderCache *self);
gboolean      builder_cache_open (BuilderCache *self,
                                  GError      **error);
GChecksum *   builder_cache_get_checksum (BuilderCache *self);
gboolean      builder_cache_lookup (BuilderCache *self,
                                    const char   *stage);
void          builder_cache_ensure_checkout (BuilderCache *self);
gboolean      builder_cache_has_checkout (BuilderCache *self);
gboolean      builder_cache_commit (BuilderCache *self,
                                    const char   *body,
                                    GError      **error);
gboolean      builder_cache_get_outstanding_changes (BuilderCache *self,
                                                     GPtrArray   **changed_out,
                                                     GError      **error);
GPtrArray   *builder_cache_get_files (BuilderCache *self,
                                      GError      **error);
GPtrArray   *builder_cache_get_changes (BuilderCache *self,
                                        GError      **error);
GPtrArray   *builder_cache_get_all_changes (BuilderCache *self,
                                            GError      **error);
gboolean      builder_gc (BuilderCache *self,
                          GError      **error);

void builder_cache_checksum_str (BuilderCache *self,
                                 const char   *str);
void builder_cache_checksum_compat_str (BuilderCache *self,
                                        const char   *str);
void builder_cache_checksum_strv (BuilderCache *self,
                                  char        **strv);
void builder_cache_checksum_compat_strv (BuilderCache *self,
                                         char        **strv);
void builder_cache_checksum_boolean (BuilderCache *self,
                                     gboolean      val);
void builder_cache_checksum_compat_boolean (BuilderCache *self,
                                            gboolean      val);
void builder_cache_checksum_uint32 (BuilderCache *self,
                                    guint32       val);
void builder_cache_checksum_data (BuilderCache *self,
                                  guint8       *data,
                                  gsize         len);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BuilderCache, g_object_unref)

G_END_DECLS

#endif /* __BUILDER_CACHE_H__ */
