/*
 * Copyright © 2021 Red Hat, Inc
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

#ifndef __FLATPAK_PRUNE_H__
#define __FLATPAK_PRUNE_H__

#include "flatpak-utils-private.h"

gboolean flatpak_repo_prune  (OstreeRepo    *repo,
                              int            depth,
                              gboolean       dry_run,
                              int           *out_objects_total,
                              int           *out_objects_pruned,
                              guint64       *out_pruned_object_size_total,
                              GCancellable  *cancellable,
                              GError       **error);

#endif /* __FLATPAK_PRUNE_H__ */
