/* Copyright (C) 2023 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Based on gzlibcompressor.h:
 *     Author: Alexander Larsson <alexl@redhat.com>
 * Author: Owen Taylor <otaylor@redhat.com>
 */

#ifndef __FLATPAK_ZSTD_COMPRESSOR_H__
#define __FLATPAK_ZSTD_COMPRESSOR_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#define FLATPAK_TYPE_ZSTD_COMPRESSOR flatpak_zstd_compressor_get_type ()
G_DECLARE_FINAL_TYPE (FlatpakZstdCompressor,
                      flatpak_zstd_compressor,
                      FLATPAK, ZSTD_COMPRESSOR,
                      GObject)

FlatpakZstdCompressor *flatpak_zstd_compressor_new (int level);

G_END_DECLS

#endif /* __FLATPAK_ZSTD_COMPRESSOR_H__ */
