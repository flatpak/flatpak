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

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakOciDir, g_object_unref)

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
GHashTable *flatpak_oci_manifest_get_annotations2 (JsonObject *manifest);

guint64     flatpak_oci_config_get_created (JsonObject *config);


#endif /* __FLATPAK_OCI_H__ */
