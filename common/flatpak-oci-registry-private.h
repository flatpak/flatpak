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
#include "flatpak-json-oci-private.h"
#include "flatpak-utils-private.h"

#define FLATPAK_TYPE_OCI_REGISTRY flatpak_oci_registry_get_type ()
#define FLATPAK_OCI_REGISTRY(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_OCI_REGISTRY, FlatpakOciRegistry))
#define FLATPAK_IS_OCI_REGISTRY(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_OCI_REGISTRY))

GType flatpak_oci_registry_get_type (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakOciRegistry, g_object_unref)

#define FLATPAK_TYPE_OCI_LAYER_WRITER flatpak_oci_layer_writer_get_type ()
#define FLATPAK_OCI_LAYER_WRITER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_OCI_LAYER_WRITER, FlatpakOciLayerWriter))
#define FLATPAK_IS_OCI_LAYER_WRITER(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_OCI_LAYER_WRITER))

GType flatpak_oci_layer_writer_get_type (void);

typedef enum {
  FLATPAK_OCI_ERROR_NOT_CHANGED = 0,
} FlatpakOciErrorEnum;

#define FLATPAK_OCI_ERROR flatpak_oci_error_quark ()

FLATPAK_EXTERN GQuark  flatpak_oci_error_quark (void);

typedef struct FlatpakOciLayerWriter FlatpakOciLayerWriter;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakOciLayerWriter, g_object_unref)

FlatpakOciRegistry  *  flatpak_oci_registry_new (const char           *uri,
                                                 gboolean for_write,
                                                 int tmp_dfd,
                                                 GCancellable         * cancellable,
                                                 GError              **error);
const char          *  flatpak_oci_registry_get_uri (FlatpakOciRegistry *self);
FlatpakOciIndex     *  flatpak_oci_registry_load_index (FlatpakOciRegistry *self,
                                                        GCancellable       *cancellable,
                                                        GError            **error);
gboolean               flatpak_oci_registry_save_index (FlatpakOciRegistry *self,
                                                        FlatpakOciIndex    *index,
                                                        GCancellable       *cancellable,
                                                        GError            **error);
int                    flatpak_oci_registry_download_blob (FlatpakOciRegistry    *self,
                                                           const char            *repository,
                                                           gboolean               manifest,
                                                           const char            *digest,
                                                           FlatpakLoadUriProgress progress_cb,
                                                           gpointer               user_data,
                                                           GCancellable          *cancellable,
                                                           GError               **error);
GBytes             *   flatpak_oci_registry_load_blob (FlatpakOciRegistry *self,
                                                       const char         *repository,
                                                       gboolean            manifest,
                                                       const char         *digest,
                                                       GCancellable       *cancellable,
                                                       GError            **error);
char *                 flatpak_oci_registry_store_blob (FlatpakOciRegistry *self,
                                                        GBytes             *data,
                                                        GCancellable       *cancellable,
                                                        GError            **error);
gboolean               flatpak_oci_registry_mirror_blob (FlatpakOciRegistry    *self,
                                                         FlatpakOciRegistry    *source_registry,
                                                         const char            *repository,
                                                         gboolean               manifest,
                                                         const char            *digest,
                                                         FlatpakLoadUriProgress progress_cb,
                                                         gpointer               user_data,
                                                         GCancellable          *cancellable,
                                                         GError               **error);
FlatpakOciDescriptor * flatpak_oci_registry_store_json (FlatpakOciRegistry *self,
                                                        FlatpakJson        *json,
                                                        GCancellable       *cancellable,
                                                        GError            **error);
FlatpakOciVersioned *  flatpak_oci_registry_load_versioned (FlatpakOciRegistry *self,
                                                            const char         *repository,
                                                            const char         *digest,
                                                            gsize              *out_size,
                                                            GCancellable       *cancellable,
                                                            GError            **error);
FlatpakOciLayerWriter *flatpak_oci_registry_write_layer (FlatpakOciRegistry *self,
                                                         GCancellable       *cancellable,
                                                         GError            **error);

struct archive *flatpak_oci_layer_writer_get_archive (FlatpakOciLayerWriter *self);
gboolean        flatpak_oci_layer_writer_close (FlatpakOciLayerWriter *self,
                                                char                 **uncompressed_digest_out,
                                                FlatpakOciDescriptor **res_out,
                                                GCancellable          *cancellable,
                                                GError               **error);

gboolean flatpak_archive_read_open_fd_with_checksum (struct archive *a,
                                                     int             fd,
                                                     GChecksum      *checksum,
                                                     GError        **error);

GBytes *flatpak_oci_sign_data (GBytes       *data,
                               const gchar **okey_ids,
                               const char   *homedir,
                               GError      **error);

FlatpakOciSignature *flatpak_oci_verify_signature (OstreeRepo *repo,
                                                   const char *remote_name,
                                                   GBytes     *signature,
                                                   GError    **error);

gboolean flatpak_oci_index_ensure_cached (SoupSession  *soup_session,
                                          const char   *uri,
                                          GFile        *index,
                                          char        **index_uri_out,
                                          GCancellable *cancellable,
                                          GError      **error);

GVariant *flatpak_oci_index_make_summary (GFile        *index,
                                          const char   *index_uri,
                                          GCancellable *cancellable,
                                          GError      **error);

GBytes *flatpak_oci_index_make_appstream (SoupSession  *soup_session,
                                          GFile        *index,
                                          const char   *index_uri,
                                          const char   *arch,
                                          int           icons_dfd,
                                          GCancellable *cancellable,
                                          GError      **error);

#endif /* __FLATPAK_OCI_REGISTRY_H__ */
