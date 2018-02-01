/*
 * Copyright © 2014-2018 Red Hat, Inc
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

#ifndef __FLATPAK_EXPORTS_H__
#define __FLATPAK_EXPORTS_H__

#include "libglnx/libglnx.h"
#include "flatpak-utils.h"
#include "flatpak-bwrap.h"
#include "flatpak-context.h"

typedef struct _FlatpakExports FlatpakExports;

void flatpak_exports_free (FlatpakExports *exports);
FlatpakExports *flatpak_exports_new (void);
void flatpak_exports_append_bwrap_args (FlatpakExports *exports,
                                        FlatpakBwrap *bwrap);
void flatpak_export_paths_export_context (FlatpakContext *context,
                                          FlatpakExports *exports,
                                          GFile *app_id_dir,
                                          gboolean do_create,
                                          GString *xdg_dirs_conf,
                                          gboolean *home_access_out);
void flatpak_exports_add_home_expose (FlatpakExports *exports,
                                      FlatpakFilesystemMode mode);
void flatpak_exports_add_path_expose (FlatpakExports *exports,
                                      FlatpakFilesystemMode mode,
                                      const char *path);
void flatpak_exports_add_path_tmpfs (FlatpakExports *exports,
                                 const char *path);
void flatpak_exports_add_path_expose_or_hide (FlatpakExports *exports,
                                          FlatpakFilesystemMode mode,
                                          const char *path);
void flatpak_exports_add_path_dir (FlatpakExports *exports,
                                   const char *path);

gboolean flatpak_exports_path_is_visible (FlatpakExports *exports,
                                          const char *path);
FlatpakExports *flatpak_exports_from_context (FlatpakContext *context,
                                              const char *app_id);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakExports, flatpak_exports_free);


#endif /* __FLATPAK_EXPORTS_H__ */
