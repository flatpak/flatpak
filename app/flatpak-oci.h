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

#ifndef __FLATPAK_OCI_H__
#define __FLATPAK_OCI_H__

#include "libglnx/libglnx.h"

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <archive.h>
#include <archive_entry.h>

#define FLATPAK_TYPE_OCI_DIR flatpak_oci_dir_get_type ()
#define FLATPAK_OCI_DIR(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_OCI_DIR, FlatpakOciDir))
#define FLATPAK_IS_OCI_DIR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_OCI_DIR))

GType flatpak_oci_dir_get_type (void);

typedef struct FlatpakOciDir FlatpakOciDir;

#define FLATPAK_TYPE_OCI_LAYER_WRITER flatpak_oci_layer_writer_get_type ()
#define FLATPAK_OCI_LAYER_WRITER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_OCI_LAYER_WRITER, FlatpakOciLayerWriter))
#define FLATPAK_IS_OCI_LAYER_WRITER(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_OCI_LAYER_WRITER))

GType flatpak_oci_layer_writer_get_type (void);

typedef struct FlatpakOciLayerWriter FlatpakOciLayerWriter;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakOciDir, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakOciLayerWriter, g_object_unref)

const char * flatpak_arch_to_oci_arch (const char *flatpak_arch);

FlatpakOciDir *flatpak_oci_dir_new        (void);
gboolean       flatpak_oci_dir_open       (FlatpakOciDir  *self,
                                           GFile          *dir,
                                           GCancellable   *cancellable,
                                           GError        **error);
gboolean       flatpak_oci_dir_ensure     (FlatpakOciDir  *self,
                                           GFile          *dir,
                                           GCancellable   *cancellable,
                                           GError        **error);
char *         flatpak_oci_dir_write_blob (FlatpakOciDir  *self,
                                           GBytes         *data,
                                           GCancellable   *cancellable,
                                           GError        **error);
gboolean       flatpak_oci_dir_set_ref    (FlatpakOciDir  *self,
                                           const char     *ref,
                                           guint64         object_size,
                                           const char     *object_sha256,
                                           GCancellable   *cancellable,
                                           GError        **error);
gboolean       flatpak_oci_dir_load_ref    (FlatpakOciDir  *self,
                                            const char     *ref,
                                            guint64        *size_out,
                                            char          **digest_out,
                                            char          **mediatype_out,
                                            GCancellable   *cancellable,
                                            GError        **error);
GBytes *       flatpak_oci_dir_load_object (FlatpakOciDir  *self,
                                            const char     *digest,
                                            GCancellable   *cancellable,
                                            GError        **error);
struct archive *flatpak_oci_dir_load_layer (FlatpakOciDir  *self,
                                            const char     *digest,
                                            GCancellable   *cancellable,
                                            GError        **error);
JsonObject *   flatpak_oci_dir_load_json   (FlatpakOciDir  *self,
                                            const char     *digest,
                                            GCancellable   *cancellable,
                                            GError        **error);
JsonObject *   flatpak_oci_dir_find_manifest (FlatpakOciDir  *self,
                                              const char     *ref,
                                              const char     *os,
                                              const char     *arch,
                                              GCancellable   *cancellable,
                                              GError        **error);

char *      flatpak_oci_manifest_get_config      (JsonObject *manifest);
char **     flatpak_oci_manifest_get_layers      (JsonObject *manifest);
GHashTable *flatpak_oci_manifest_get_annotations (JsonObject *manifest);

guint64     flatpak_oci_config_get_created (JsonObject *config);

FlatpakOciLayerWriter *flatpak_oci_layer_writer_new   (FlatpakOciDir          *dir);
struct archive *       flatpak_oci_layer_writer_open  (FlatpakOciLayerWriter  *self,
                                                       GCancellable           *cancellable,
                                                       GError                **error);
gboolean               flatpak_oci_layer_writer_close (FlatpakOciLayerWriter  *self,
                                                       char                  **uncompressed_sha256_out,
                                                       guint64                *uncompressed_size_out,
                                                       char                  **compressed_sha256_out,
                                                       guint64                *compressed_size_out,
                                                       GCancellable           *cancellable,
                                                       GError                **error);



typedef struct FlatpakJsonWriter FlatpakJsonWriter;

FlatpakJsonWriter *flatpak_json_writer_new (void);
GBytes *flatpak_json_writer_get_result (FlatpakJsonWriter *self);
void flatpak_json_writer_free (FlatpakJsonWriter *self);

void flatpak_json_writer_open_struct (FlatpakJsonWriter *writer);
void flatpak_json_writer_open_array (FlatpakJsonWriter *writer);
void flatpak_json_writer_close (FlatpakJsonWriter *writer);
void flatpak_json_writer_add_struct_property (FlatpakJsonWriter *writer, const gchar *name);
void flatpak_json_writer_add_array_property (FlatpakJsonWriter *writer, const gchar *name);
void flatpak_json_writer_add_string_property (FlatpakJsonWriter *writer, const gchar *name, const char *value);
void flatpak_json_writer_add_uint64_property (FlatpakJsonWriter *writer, const gchar *name, guint64 value);
void flatpak_json_writer_add_bool_property (FlatpakJsonWriter *writer, const gchar *name, gboolean value);
void flatpak_json_writer_add_array_string (FlatpakJsonWriter *writer, const gchar *string);
void flatpak_json_writer_add_array_struct (FlatpakJsonWriter *writer);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakJsonWriter, flatpak_json_writer_free)


#endif /* __FLATPAK_OCI_H__ */
