/*
 * Copyright © 2016 Red Hat, Inc
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

#ifndef __FLATPAK_OCI_REGISTRY_H__
#define __FLATPAK_OCI_REGISTRY_H__

#include "libglnx/libglnx.h"

#include <glib.h>
#include <gio/gio.h>
#include <archive.h>
#include "flatpak-json-oci.h"
#include "flatpak-utils.h"

#define FLATPAK_TYPE_OCI_REGISTRY flatpak_oci_registry_get_type ()
#define FLATPAK_OCI_REGISTRY(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_OCI_REGISTRY, FlatpakOciRegistry))
#define FLATPAK_IS_OCI_REGISTRY(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_OCI_REGISTRY))

GType flatpak_oci_registry_get_type (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakOciRegistry, g_object_unref)

#define FLATPAK_TYPE_OCI_LAYER_WRITER flatpak_oci_layer_writer_get_type ()
#define FLATPAK_OCI_LAYER_WRITER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_OCI_LAYER_WRITER, FlatpakOciLayerWriter))
#define FLATPAK_IS_OCI_LAYER_WRITER(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_OCI_LAYER_WRITER))

GType flatpak_oci_layer_writer_get_type (void);

typedef struct FlatpakOciLayerWriter FlatpakOciLayerWriter;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakOciLayerWriter, g_object_unref)

FlatpakOciRegistry  *  flatpak_oci_registry_new                  (const char           *uri,
                                                                  gboolean              for_write,
                                                                  int                   tmp_dfd,
                                                                  GCancellable         *cancellable,
                                                                  GError              **error);
FlatpakOciRef       *  flatpak_oci_registry_load_ref             (FlatpakOciRegistry   *self,
                                                                  const char           *ref,
                                                                  GCancellable         *cancellable,
                                                                  GError              **error);
gboolean               flatpak_oci_registry_set_ref              (FlatpakOciRegistry   *self,
                                                                  const char           *ref,
                                                                  FlatpakOciRef        *data,
                                                                  GCancellable         *cancellable,
                                                                  GError              **error);
int                    flatpak_oci_registry_download_blob        (FlatpakOciRegistry   *self,
                                                                  const char           *digest,
                                                                  FlatpakLoadUriProgress progress_cb,
                                                                  gpointer               user_data,
                                                                  GCancellable         *cancellable,
                                                                  GError              **error);
GBytes             *   flatpak_oci_registry_load_blob            (FlatpakOciRegistry   *self,
                                                                  const char           *digest,
                                                                  GCancellable         *cancellable,
                                                                  GError              **error);
char *                 flatpak_oci_registry_store_blob           (FlatpakOciRegistry   *self,
                                                                  GBytes               *data,
                                                                  GCancellable         *cancellable,
                                                                  GError              **error);
FlatpakOciRef *        flatpak_oci_registry_store_json           (FlatpakOciRegistry   *self,
                                                                  FlatpakJson          *json,
                                                                  GCancellable         *cancellable,
                                                                  GError              **error);
FlatpakOciVersioned *  flatpak_oci_registry_load_versioned       (FlatpakOciRegistry   *self,
                                                                  const char           *digest,
                                                                  GCancellable         *cancellable,
                                                                  GError              **error);
FlatpakOciLayerWriter *flatpak_oci_registry_write_layer          (FlatpakOciRegistry   *self,
                                                                  GCancellable         *cancellable,
                                                                  GError              **error);
FlatpakOciManifest    *flatpak_oci_registry_chose_image          (FlatpakOciRegistry   *self,
                                                                  const char           *tag,
                                                                  char                **out_digest,
                                                                  GCancellable         *cancellable,
                                                                  GError              **error);

struct archive *flatpak_oci_layer_writer_get_archive (FlatpakOciLayerWriter  *self);
gboolean        flatpak_oci_layer_writer_close       (FlatpakOciLayerWriter  *self,
                                                      char                 **uncompressed_digest_out,
                                                      FlatpakOciRef         **ref_out,
                                                      GCancellable          *cancellable,
                                                      GError               **error);

#endif /* __FLATPAK_OCI_REGISTRY_H__ */
