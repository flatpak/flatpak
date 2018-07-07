/*
 * Copyright © 2018 Red Hat, Inc
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

#ifndef __FLATPAK_INSTALLATION_PRIVATE_H__
#define __FLATPAK_INSTALLATION_PRIVATE_H__

#include "flatpak-installation.h"

#include "flatpak-dir-private.h"

FlatpakDir *flatpak_installation_clone_dir_noensure (FlatpakInstallation *self);

FlatpakDir *flatpak_installation_clone_dir (FlatpakInstallation *self,
                                            GCancellable        *cancellable,
                                            GError             **error);

FlatpakInstallation *flatpak_installation_new_for_dir (FlatpakDir   *dir,
                                                       GCancellable *cancellable,
                                                       GError      **error);

#endif /* __FLATPAK_INSTALLATION_PRIVATE_H__ */
