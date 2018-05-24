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

typedef struct FlatpakDir     FlatpakDir;
typedef struct FlatpakDeploy  FlatpakDeploy;
typedef struct FlatpakOciRegistry FlatpakOciRegistry;
typedef struct _FlatpakOciManifest FlatpakOciManifest;
typedef struct FlatpakCompletion FlatpakCompletion;

#endif /* __FLATPAK_COMMON_TYPES_H__ */
