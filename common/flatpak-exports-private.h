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
#include "flatpak-bwrap-private.h"

/* In numerical order of more privs */
typedef enum {
  FLATPAK_FILESYSTEM_MODE_NONE         = 0,
  FLATPAK_FILESYSTEM_MODE_READ_ONLY    = 1,
  FLATPAK_FILESYSTEM_MODE_READ_WRITE   = 2,
  FLATPAK_FILESYSTEM_MODE_CREATE       = 3,
  FLATPAK_FILESYSTEM_MODE_LAST         = FLATPAK_FILESYSTEM_MODE_CREATE
} FlatpakFilesystemMode;

typedef struct _FlatpakExports FlatpakExports;

void flatpak_exports_free (FlatpakExports *exports);
FlatpakExports *flatpak_exports_new (void);
void flatpak_exports_append_bwrap_args (FlatpakExports *exports,
                                        FlatpakBwrap   *bwrap);
void flatpak_exports_add_host_etc_expose (FlatpakExports       *exports,
                                          FlatpakFilesystemMode mode);
void flatpak_exports_add_host_os_expose (FlatpakExports       *exports,
                                         FlatpakFilesystemMode mode);
void flatpak_exports_add_path_expose (FlatpakExports       *exports,
                                      FlatpakFilesystemMode mode,
                                      const char           *path);
void flatpak_exports_add_path_tmpfs (FlatpakExports *exports,
                                     const char     *path);
void flatpak_exports_add_path_expose_or_hide (FlatpakExports       *exports,
                                              FlatpakFilesystemMode mode,
                                              const char           *path);
void flatpak_exports_add_path_dir (FlatpakExports *exports,
                                   const char     *path);

gboolean flatpak_exports_path_is_visible (FlatpakExports *exports,
                                          const char     *path);
FlatpakFilesystemMode flatpak_exports_path_get_mode (FlatpakExports *exports,
                                                     const char     *path);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakExports, flatpak_exports_free);

/*
 * FlatpakExportsTestFlags:
 * @FLATPAK_EXPORTS_TEST_FLAGS_AUTOFS: Pretend everything is an autofs.
 *
 * Flags used to provide mock behaviour during unit testing.
 */
typedef enum
{
  FLATPAK_EXPORTS_TEST_FLAGS_AUTOFS = (1 << 0),
  FLATPAK_EXPORTS_TEST_FLAGS_NONE = 0
} FlatpakExportsTestFlags;

void flatpak_exports_take_host_fd (FlatpakExports *exports,
                                   int             fd);
void flatpak_exports_set_test_flags (FlatpakExports *exports,
                                     FlatpakExportsTestFlags flags);

#endif /* __FLATPAK_EXPORTS_H__ */
