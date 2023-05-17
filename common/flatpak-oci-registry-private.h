/*
 * Copyright © 2014-2019 Red Hat, Inc
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

#include "libglnx.h"

#include <glib.h>
#include <gio/gio.h>
#include "flatpak-dir-private.h"
#include "flatpak-json-oci-private.h"
#include "flatpak-utils-http-private.h"
#include "flatpak-utils-private.h"

struct archive;

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

FlatpakOciRegistry  *  flatpak_oci_registry_new (const char           *uri,
                                                 gboolean for_write,
                                                 int tmp_dfd,
                                                 GCancellable         * cancellable,
                                                 GError              **error);
void                   flatpak_oci_registry_set_token (FlatpakOciRegistry *self,
                                                       const char *token);
gboolean               flatpak_oci_registry_is_local (FlatpakOciRegistry *self);
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
                                                           const char           **alt_uris,
                                                           FlatpakLoadUriProgress progress_cb,
                                                           gpointer               user_data,
                                                           GCancellable          *cancellable,
                                                           GError               **error);
char *                 flatpak_oci_registry_get_token (FlatpakOciRegistry *self,
                                                       const char         *repository,
                                                       const char         *digest,
                                                       const char         *basic_auth,
                                                       GCancellable       *cancellable,
                                                       GError            **error);
GBytes             *   flatpak_oci_registry_load_blob (FlatpakOciRegistry *self,
                                                       const char         *repository,
                                                       gboolean            manifest,
                                                       const char         *digest,
                                                       const char        **alt_uris,
                                                       char              **out_content_type,
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
                                                         const char          **alt_uris,
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
                                                            const char        **alt_uris,
                                                            gsize              *out_size,
                                                            GCancellable       *cancellable,
                                                            GError            **error);
FlatpakOciImage *      flatpak_oci_registry_load_image_config (FlatpakOciRegistry *self,
                                                               const char         *repository,
                                                               const char         *digest,
                                                               const char        **alt_uris,
                                                               gsize              *out_size,
                                                               GCancellable       *cancellable,
                                                               GError            **error);
FlatpakOciLayerWriter *flatpak_oci_registry_write_layer (FlatpakOciRegistry *self,
                                                         GCancellable       *cancellable,
                                                         GError            **error);

int                     flatpak_oci_registry_apply_delta (FlatpakOciRegistry    *self,
                                                          int                    delta_fd,
                                                          GFile                 *content_dir,
                                                          GCancellable          *cancellable,
                                                          GError               **error);
char *                  flatpak_oci_registry_apply_delta_to_blob (FlatpakOciRegistry    *self,
                                                                  int                    delta_fd,
                                                                  GFile                 *content_dir,
                                                                  GCancellable          *cancellable,
                                                                  GError               **error);
FlatpakOciManifest *   flatpak_oci_registry_find_delta_manifest (FlatpakOciRegistry    *registry,
                                                                 const char            *oci_repository,
                                                                 const char            *for_digest,
                                                                 const char            *delta_manifest_uri,
                                                                 GCancellable          *cancellable);

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

gboolean flatpak_oci_index_ensure_cached (FlatpakHttpSession  *http_session,
                                          const char          *uri,
                                          GFile               *index,
                                          char               **index_uri_out,
                                          GCancellable        *cancellable,
                                          GError             **error);

GVariant *flatpak_oci_index_make_summary (GFile        *index,
                                          const char   *index_uri,
                                          GCancellable *cancellable,
                                          GError      **error);

GBytes *flatpak_oci_index_make_appstream (FlatpakHttpSession  *http_session,
                                          GFile               *index,
                                          const char          *index_uri,
                                          const char          *arch,
                                          int                  icons_dfd,
                                          GCancellable        *cancellable,
                                          GError             **error);

typedef void (*FlatpakOciPullProgress) (guint64  total_size,
                                        guint64  pulled_size,
                                        guint32  n_layers,
                                        guint32  pulled_layers,
                                        gpointer data);

char * flatpak_pull_from_oci (OstreeRepo            *repo,
                              FlatpakOciRegistry    *registry,
                              const char            *oci_repository,
                              const char            *digest,
                              const char            *delta_url,
                              FlatpakOciManifest    *manifest,
                              FlatpakOciImage       *image_config,
                              const char            *remote,
                              const char            *ref,
                              FlatpakPullFlags       flags,
                              FlatpakOciPullProgress progress_cb,
                              gpointer               progress_data,
                              GCancellable          *cancellable,
                              GError               **error);

gboolean flatpak_mirror_image_from_oci (FlatpakOciRegistry    *dst_registry,
                                        FlatpakOciRegistry    *registry,
                                        const char            *oci_repository,
                                        const char            *digest,
                                        const char            *remote,
                                        const char            *ref,
                                        const char            *delta_url,
                                        OstreeRepo            *repo,
                                        FlatpakOciPullProgress progress_cb,
                                        gpointer               progress_data,
                                        GCancellable          *cancellable,
                                        GError               **error);

#endif /* __FLATPAK_OCI_REGISTRY_H__ */
