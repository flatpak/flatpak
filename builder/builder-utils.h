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

#ifndef __BUILDER_UTILS_H__
#define __BUILDER_UTILS_H__

#include <gio/gio.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

typedef struct BuilderUtils BuilderUtils;

char *builder_uri_to_filename (const char *uri);

gboolean strip (GError **error,
                ...);
gboolean eu_strip (GError **error,
                   ...);

gboolean is_elf_file (const char *path,
                      gboolean   *is_shared,
                      gboolean   *is_stripped);

char ** builder_get_debuginfo_file_references (const char *filename,
                                               GError    **error);

gboolean directory_is_empty (const char *path);

gboolean flatpak_matches_path_pattern (const char *path,
                                       const char *pattern);
void     flatpak_collect_matches_for_path_pattern (const char *path,
                                                   const char *pattern,
                                                   const char *add_prefix,
                                                   GHashTable *to_remove_ht);
gboolean builder_migrate_locale_dirs (GFile   *root_dir,
                                      GError **error);

gboolean builder_host_spawnv (GFile                *dir,
                              char                **output,
                              GError              **error,
                              const gchar * const  *argv);
gboolean builder_maybe_host_spawnv (GFile                *dir,
                                    char                **output,
                                    GError              **error,
                                    const gchar * const  *argv);


G_END_DECLS

#endif /* __BUILDER_UTILS_H__ */
