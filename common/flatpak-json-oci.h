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

#ifndef __FLATPAK_JSON_OCI_H__
#define __FLATPAK_JSON_OCI_H__

#include "flatpak-json.h"

G_BEGIN_DECLS

#define FLATPAK_OCI_MEDIA_TYPE_DESCRIPTOR "application/vnd.oci.descriptor.v1+json"
#define FLATPAK_OCI_MEDIA_TYPE_IMAGE_MANIFEST "application/vnd.oci.image.manifest.v1+json"
#define FLATPAK_OCI_MEDIA_TYPE_IMAGE_MANIFESTLIST "application/vnd.oci.image.manifest.list.v1+json"
#define FLATPAK_OCI_MEDIA_TYPE_IMAGE_LAYER "application/vnd.oci.image.layer.v1.tar+gzip"
#define FLATPAK_OCI_MEDIA_TYPE_IMAGE_LAYER_NONDISTRIBUTABLE "application/vnd.oci.image.layer.nondistributable.v1.tar+gzip"
#define FLATPAK_OCI_MEDIA_TYPE_IMAGE_CONFIG "application/vnd.oci.image.config.v1+json"

const char * flatpak_arch_to_oci_arch (const char *flatpak_arch);

typedef struct {
  char *mediatype;
  char *digest;
  gint64 size;
  char **urls;
} FlatpakOciDescriptor;

void flatpak_oci_descriptor_destroy (FlatpakOciDescriptor *self);
void flatpak_oci_descriptor_free (FlatpakOciDescriptor *self);

typedef struct
{
  char *architecture;
  char *os;
  char *os_version;
  char **os_features;
  char *variant;
  char **features;
} FlatpakOciManifestPlatform;

typedef struct
{
  FlatpakOciDescriptor parent;
  FlatpakOciManifestPlatform platform;
} FlatpakOciManifestDescriptor;

void flatpak_oci_manifest_descriptor_destroy (FlatpakOciManifestDescriptor *self);
void flatpak_oci_manifest_descriptor_free (FlatpakOciManifestDescriptor *self);

#define FLATPAK_TYPE_OCI_REF flatpak_oci_ref_get_type ()
G_DECLARE_FINAL_TYPE (FlatpakOciRef, flatpak_oci_ref, FLATPAK_OCI, REF, FlatpakJson)

struct _FlatpakOciRef {
  FlatpakJson parent;

  FlatpakOciDescriptor descriptor;
};

struct _FlatpakOciRefClass {
  FlatpakJsonClass parent_class;
};

FlatpakOciRef *flatpak_oci_ref_new           (const char     *mediatype,
                                              const char     *digest,
                                              gint64          size);
const char *   flatpak_oci_ref_get_mediatype (FlatpakOciRef  *self);
const char *   flatpak_oci_ref_get_digest    (FlatpakOciRef  *self);
gint64         flatpak_oci_ref_get_size      (FlatpakOciRef  *self);
const char **  flatpak_oci_ref_get_urls      (FlatpakOciRef  *self);
void           flatpak_oci_ref_set_urls      (FlatpakOciRef  *self,
                                              const char    **urls);


#define FLATPAK_TYPE_OCI_VERSIONED flatpak_oci_versioned_get_type ()
G_DECLARE_FINAL_TYPE (FlatpakOciVersioned, flatpak_oci_versioned, FLATPAK_OCI, VERSIONED, FlatpakJson)

struct _FlatpakOciVersioned {
  FlatpakJson parent;

  int version;
  char *mediatype;
};

struct _FlatpakOciVersionedClass {
  FlatpakJsonClass parent_class;
};

FlatpakOciVersioned *flatpak_oci_versioned_from_json     (GBytes               *bytes,
                                                          GError              **error);
const char *         flatpak_oci_versioned_get_mediatype (FlatpakOciVersioned  *self);
gint64               flatpak_oci_versioned_get_version   (FlatpakOciVersioned  *self);

#define FLATPAK_TYPE_OCI_MANIFEST flatpak_oci_manifest_get_type ()
G_DECLARE_FINAL_TYPE (FlatpakOciManifest, flatpak_oci_manifest, FLATPAK, OCI_MANIFEST, FlatpakOciVersioned)

struct _FlatpakOciManifest
{
  FlatpakOciVersioned parent;

  FlatpakOciDescriptor config;
  FlatpakOciDescriptor **layers;
  GHashTable     *annotations;
};

struct _FlatpakOciManifestClass
{
  FlatpakOciVersionedClass parent_class;
};


FlatpakOciManifest *flatpak_oci_manifest_new              (void);
void                flatpak_oci_manifest_set_config       (FlatpakOciManifest  *self,
                                                           FlatpakOciRef       *ref);
void                flatpak_oci_manifest_set_layers       (FlatpakOciManifest  *self,
                                                           FlatpakOciRef      **refs);
void                flatpak_oci_manifest_set_layer        (FlatpakOciManifest  *self,
                                                           FlatpakOciRef       *ref);
int                 flatpak_oci_manifest_get_n_layers     (FlatpakOciManifest  *self);
const char *        flatpak_oci_manifest_get_layer_digest (FlatpakOciManifest  *self,
                                                           int                  i);
GHashTable *        flatpak_oci_manifest_get_annotations  (FlatpakOciManifest  *self);

#define FLATPAK_TYPE_OCI_MANIFEST_LIST flatpak_oci_manifest_list_get_type ()
G_DECLARE_FINAL_TYPE (FlatpakOciManifestList, flatpak_oci_manifest_list, FLATPAK, OCI_MANIFEST_LIST, FlatpakOciVersioned)

struct _FlatpakOciManifestList
{
  FlatpakOciVersioned parent;

  FlatpakOciManifestDescriptor **manifests;
  GHashTable     *annotations;
};

struct _FlatpakOciManifestListClass
{
  FlatpakOciVersionedClass parent_class;
};

#define FLATPAK_TYPE_OCI_IMAGE flatpak_oci_image_get_type ()
G_DECLARE_FINAL_TYPE (FlatpakOciImage, flatpak_oci_image, FLATPAK, OCI_IMAGE, FlatpakJson)

typedef struct
{
  char *type;
  char **diff_ids;
} FlatpakOciImageRootfs;

typedef struct
{
  char *user;
  char *working_dir;
  gint64 memory;
  gint64 memory_swap;
  gint64 cpu_shares;
  char **env;
  char **cmd;
  char **entrypoint;
  char **exposed_ports;
  char **volumes;
  GHashTable *labels;
} FlatpakOciImageConfig;

typedef struct
{
  char *created;
  char *created_by;
  char *author;
  char *comment;
  gboolean empty_layer;
} FlatpakOciImageHistory;

struct _FlatpakOciImage
{
  FlatpakJson parent;

  char *created;
  char *author;
  char *architecture;
  char *os;
  FlatpakOciImageRootfs rootfs;
  FlatpakOciImageConfig config;
  FlatpakOciImageHistory **history;
};

struct _FlatpakOciImageClass
{
  FlatpakJsonClass parent_class;
};

FlatpakOciImage *flatpak_oci_image_new              (void);
void             flatpak_oci_image_set_created      (FlatpakOciImage  *image,
                                                     const char       *created);
void             flatpak_oci_image_set_architecture (FlatpakOciImage  *image,
                                                     const char       *arch);
void             flatpak_oci_image_set_os           (FlatpakOciImage  *image,
                                                     const char       *os);
void             flatpak_oci_image_set_layers       (FlatpakOciImage  *image,
                                                     const char      **layers);
void             flatpak_oci_image_set_layer        (FlatpakOciImage  *image,
                                                     const char       *layer);

void flatpak_oci_add_annotations_for_commit (GHashTable       *annotations,
                                             const char       *ref,
                                             const char       *commit,
                                             GVariant         *commit_data);
void flatpak_oci_parse_commit_annotations  (GHashTable       *annotations,
                                            guint64          *out_timestamp,
                                            char            **out_subject,
                                            char            **out_body,
                                            char            **out_ref,
                                            char            **out_commit,
                                            char            **out_parent_commit,
                                            GVariantBuilder  *metadata_builder);

#endif /* __FLATPAK_JSON_OCI_H__ */
