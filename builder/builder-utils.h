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

#ifndef __BUILDER_UTILS_H__
#define __BUILDER_UTILS_H__

#include <gio/gio.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

typedef struct BuilderUtils BuilderUtils;

char *builder_uri_to_filename (const char *uri);

const char *path_prefix_match (const char *pattern,
                               const char *string);

gboolean strip (GError **error, ...);
gboolean eu_strip (GError **error, ...);

gboolean is_elf_file (const char *path,
                      gboolean *is_shared,
                      gboolean *is_stripped);

G_END_DECLS

#endif /* __BUILDER_UTILS_H__ */
