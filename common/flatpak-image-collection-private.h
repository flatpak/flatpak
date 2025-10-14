/*
 * Copyright © 2024 Red Hat, Inc
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
 *       Owen Taylor <otaylor@redhat.com>
 */

#ifndef __FLATPAK_IMAGE_COLLECTION_H__
#define __FLATPAK_IMAGE_COLLECTION_H__

#include <glib.h>
#include <gio/gio.h>

#include <flatpak-common-types-private.h>
#include <flatpak-image-source-private.h>

#define FLATPAK_TYPE_IMAGE_COLLECTION flatpak_image_collection_get_type ()
#define FLATPAK_IMAGE_COLLECTION(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_IMAGE_COLLECTION, FlatpakImageCollection))
#define FLATPAK_IS_IMAGE_COLLECTION(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_IMAGE_COLLECTION))

GType flatpak_image_collection_get_type (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakImageCollection, g_object_unref)


FlatpakImageCollection *flatpak_image_collection_new (const char   *location,
                                                      GCancellable *cancellable,
                                                      GError      **error);

FlatpakImageSource *flatpak_image_collection_lookup_ref (FlatpakImageCollection *self,
                                                         const char             *ref);
FlatpakImageSource *flatpak_image_collection_lookup_digest (FlatpakImageCollection *self,
                                                            const char             *digest);

GPtrArray *flatpak_image_collection_get_sources (FlatpakImageCollection *self);

#endif /* __FLATPAK_IMAGE_COLLECTION_H__ */
