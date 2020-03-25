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

#if !defined(__FLATPAK_H_INSIDE__) && !defined(FLATPAK_COMPILATION)
#error "Only <flatpak.h> can be included directly."
#endif

#ifndef __FLATPAK_REMOTE_PRIVATE_H__
#define __FLATPAK_REMOTE_PRIVATE_H__

#include <flatpak-remote.h>
#include <flatpak-dir-private.h>
#include <ostree.h>

FlatpakRemote *flatpak_remote_new_with_dir (const char *name,
                                            FlatpakDir *dir);

gboolean flatpak_remote_commit (FlatpakRemote *self,
                                FlatpakDir    *dir,
                                GCancellable  *cancellable,
                                GError       **error);
gboolean flatpak_remote_commit_filter (FlatpakRemote *self,
                                       FlatpakDir    *dir,
                                       GCancellable  *cancellable,
                                       GError       **error);

#endif /* __FLATPAK_REMOTE_PRIVATE_H__ */
