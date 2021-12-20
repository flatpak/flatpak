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

#ifndef __FLATPAK_COMMON_TYPES_H__
#define __FLATPAK_COMMON_TYPES_H__

typedef enum {
  FLATPAK_KINDS_APP = 1 << 0,
  FLATPAK_KINDS_RUNTIME = 1 << 1,
} FlatpakKinds;

typedef enum {
  FLATPAK_RUN_FLAG_DEVEL              = (1 << 0),
  FLATPAK_RUN_FLAG_BACKGROUND         = (1 << 1),
  FLATPAK_RUN_FLAG_LOG_SESSION_BUS    = (1 << 2),
  FLATPAK_RUN_FLAG_LOG_SYSTEM_BUS     = (1 << 3),
  FLATPAK_RUN_FLAG_NO_SESSION_HELPER  = (1 << 4),
  FLATPAK_RUN_FLAG_MULTIARCH          = (1 << 5),
  FLATPAK_RUN_FLAG_WRITABLE_ETC       = (1 << 6),
  FLATPAK_RUN_FLAG_NO_SESSION_BUS_PROXY = (1 << 7),
  FLATPAK_RUN_FLAG_NO_SYSTEM_BUS_PROXY = (1 << 8),
  FLATPAK_RUN_FLAG_SET_PERSONALITY    = (1 << 9),
  FLATPAK_RUN_FLAG_FILE_FORWARDING    = (1 << 10),
  FLATPAK_RUN_FLAG_DIE_WITH_PARENT    = (1 << 11),
  FLATPAK_RUN_FLAG_LOG_A11Y_BUS       = (1 << 12),
  FLATPAK_RUN_FLAG_NO_A11Y_BUS_PROXY  = (1 << 13),
  FLATPAK_RUN_FLAG_SANDBOX            = (1 << 14),
  FLATPAK_RUN_FLAG_NO_DOCUMENTS_PORTAL = (1 << 15),
  FLATPAK_RUN_FLAG_BLUETOOTH          = (1 << 16),
  FLATPAK_RUN_FLAG_CANBUS            = (1 << 17),
  FLATPAK_RUN_FLAG_DO_NOT_REAP        = (1 << 18),
  FLATPAK_RUN_FLAG_NO_PROC            = (1 << 19),
  FLATPAK_RUN_FLAG_PARENT_EXPOSE_PIDS = (1 << 20),
  FLATPAK_RUN_FLAG_PARENT_SHARE_PIDS  = (1 << 21),
  FLATPAK_RUN_FLAG_NO_A11Y_FILTERING  = (1 << 22),
} FlatpakRunFlags;

typedef struct FlatpakDir          FlatpakDir;
typedef struct FlatpakDeploy       FlatpakDeploy;
typedef struct FlatpakOciRegistry  FlatpakOciRegistry;
typedef struct _FlatpakOciManifest FlatpakOciManifest;
typedef struct _FlatpakOciImage    FlatpakOciImage;

#endif /* __FLATPAK_COMMON_TYPES_H__ */
