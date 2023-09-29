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

#include "flatpak-json-private.h"

G_BEGIN_DECLS

#define FLATPAK_OCI_MEDIA_TYPE_DESCRIPTOR "application/vnd.oci.descriptor.v1+json"
#define FLATPAK_OCI_MEDIA_TYPE_IMAGE_MANIFEST "application/vnd.oci.image.manifest.v1+json"
#define FLATPAK_DOCKER_MEDIA_TYPE_IMAGE_MANIFEST2 "application/vnd.docker.distribution.manifest.v2+json"
#define FLATPAK_OCI_MEDIA_TYPE_IMAGE_INDEX "application/vnd.oci.image.index.v1+json"
#define FLATPAK_OCI_MEDIA_TYPE_IMAGE_LAYER_GZIP "application/vnd.oci.image.layer.v1.tar+gzip"
#define FLATPAK_OCI_MEDIA_TYPE_IMAGE_LAYER_ZSTD "application/vnd.oci.image.layer.v1.tar+zstd"
#define FLATPAK_OCI_MEDIA_TYPE_IMAGE_CONFIG "application/vnd.oci.image.config.v1+json"
#define FLATPAK_DOCKER_MEDIA_TYPE_IMAGE_IMAGE_CONFIG "application/vnd.docker.container.image.v1+json"

#define FLATPAK_OCI_SIGNATURE_TYPE_FLATPAK "flatpak oci image signature"

const char * flatpak_arch_to_oci_arch (const char *flatpak_arch);
void flatpak_oci_export_labels (GHashTable *source,
                                GHashTable *dest);
void flatpak_oci_copy_labels (GHashTable *source,
                              GHashTable *dest);

typedef struct
{
  char       *mediatype;
  char       *digest;
  gint64      size;
  char      **urls;
  GHashTable *annotations;
} FlatpakOciDescriptor;

FlatpakOciDescriptor *flatpak_oci_descriptor_new (const char *mediatype,
                                                  const char *digest,
                                                  gint64      size);
void flatpak_oci_descriptor_copy (FlatpakOciDescriptor *source,
                                  FlatpakOciDescriptor *dest);
void flatpak_oci_descriptor_destroy (FlatpakOciDescriptor *self);
void flatpak_oci_descriptor_free (FlatpakOciDescriptor *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakOciDescriptor, flatpak_oci_descriptor_free)

typedef struct
{
  char  *architecture;
  char  *os;
  char  *os_version;
  char **os_features;
  char  *variant;
  char **features;
} FlatpakOciManifestPlatform;

typedef struct
{
  FlatpakOciDescriptor       parent;
  FlatpakOciManifestPlatform platform;
} FlatpakOciManifestDescriptor;

FlatpakOciManifestDescriptor * flatpak_oci_manifest_descriptor_new (void);
const char * flatpak_oci_manifest_descriptor_get_ref (FlatpakOciManifestDescriptor *m);
void flatpak_oci_manifest_descriptor_destroy (FlatpakOciManifestDescriptor *self);
void flatpak_oci_manifest_descriptor_free (FlatpakOciManifestDescriptor *self);


#define FLATPAK_TYPE_OCI_VERSIONED flatpak_oci_versioned_get_type ()
G_DECLARE_FINAL_TYPE (FlatpakOciVersioned, flatpak_oci_versioned, FLATPAK_OCI, VERSIONED, FlatpakJson)

struct _FlatpakOciVersioned
{
  FlatpakJson parent;

  int         version;
  char       *mediatype;
};

struct _FlatpakOciVersionedClass
{
  FlatpakJsonClass parent_class;
};

FlatpakOciVersioned *flatpak_oci_versioned_from_json (GBytes     *bytes,
                                                      const char *content_type,
                                                      GError    **error);
const char *         flatpak_oci_versioned_get_mediatype (FlatpakOciVersioned *self);
gint64               flatpak_oci_versioned_get_version (FlatpakOciVersioned *self);

#define FLATPAK_TYPE_OCI_MANIFEST flatpak_oci_manifest_get_type ()
G_DECLARE_FINAL_TYPE (FlatpakOciManifest, flatpak_oci_manifest, FLATPAK, OCI_MANIFEST, FlatpakOciVersioned)

struct _FlatpakOciManifest
{
  FlatpakOciVersioned    parent;

  FlatpakOciDescriptor   config;
  FlatpakOciDescriptor **layers;
  GHashTable            *annotations;
};

struct _FlatpakOciManifestClass
{
  FlatpakOciVersionedClass parent_class;
};


FlatpakOciManifest *flatpak_oci_manifest_new (void);
void                flatpak_oci_manifest_set_config (FlatpakOciManifest   *self,
                                                     FlatpakOciDescriptor *desc);
void                flatpak_oci_manifest_set_layers (FlatpakOciManifest    *self,
                                                     FlatpakOciDescriptor **descs);
void                flatpak_oci_manifest_set_layer (FlatpakOciManifest   *self,
                                                    FlatpakOciDescriptor *desc);
int                 flatpak_oci_manifest_get_n_layers (FlatpakOciManifest *self);
const char *        flatpak_oci_manifest_get_layer_digest (FlatpakOciManifest *self,
                                                           int                 i);
GHashTable *        flatpak_oci_manifest_get_annotations (FlatpakOciManifest *self);

/* Only useful for delta manifest */
FlatpakOciDescriptor *flatpak_oci_manifest_find_delta_for (FlatpakOciManifest *deltamanifest,
                                                           const char         *from_diffid,
                                                           const char         *to_diffid);


#define FLATPAK_TYPE_OCI_INDEX flatpak_oci_index_get_type ()
G_DECLARE_FINAL_TYPE (FlatpakOciIndex, flatpak_oci_index, FLATPAK, OCI_INDEX, FlatpakOciVersioned)

struct _FlatpakOciIndex
{
  FlatpakOciVersioned            parent;

  FlatpakOciManifestDescriptor **manifests;
  GHashTable                    *annotations;
};

struct _FlatpakOciIndexClass
{
  FlatpakOciVersionedClass parent_class;
};

FlatpakOciIndex *             flatpak_oci_index_new (void);
void                          flatpak_oci_index_add_manifest (FlatpakOciIndex      *self,
                                                              const char           *ref,
                                                              FlatpakOciDescriptor *desc);
gboolean                      flatpak_oci_index_remove_manifest (FlatpakOciIndex *self,
                                                                 const char      *ref);
FlatpakOciManifestDescriptor *flatpak_oci_index_get_manifest (FlatpakOciIndex *self,
                                                              const char      *ref);
FlatpakOciManifestDescriptor *flatpak_oci_index_get_only_manifest (FlatpakOciIndex *self);
FlatpakOciManifestDescriptor *flatpak_oci_index_get_manifest_for_arch (FlatpakOciIndex *self,
                                                                       const char      *oci_arch);

int                           flatpak_oci_index_get_n_manifests (FlatpakOciIndex *self);
/* Only useful for delta index */
FlatpakOciDescriptor *flatpak_oci_index_find_delta_for (FlatpakOciIndex *delta_index,
                                                        const char      *for_digest);


#define FLATPAK_TYPE_OCI_IMAGE flatpak_oci_image_get_type ()
G_DECLARE_FINAL_TYPE (FlatpakOciImage, flatpak_oci_image, FLATPAK, OCI_IMAGE, FlatpakJson)

typedef struct
{
  char  *type;
  char **diff_ids;
} FlatpakOciImageRootfs;

typedef struct
{
  char       *user;
  char       *working_dir;
  gint64      memory;
  gint64      memory_swap;
  gint64      cpu_shares;
  char      **env;
  char      **cmd;
  char      **entrypoint;
  char      **exposed_ports;
  char      **volumes;
  GHashTable *labels;
} FlatpakOciImageConfig;

typedef struct
{
  char    *created;
  char    *created_by;
  char    *author;
  char    *comment;
  gboolean empty_layer;
} FlatpakOciImageHistory;

struct _FlatpakOciImage
{
  FlatpakJson              parent;

  char                    *created;
  char                    *author;
  char                    *architecture;
  char                    *os;
  FlatpakOciImageRootfs    rootfs;
  FlatpakOciImageConfig    config;
  FlatpakOciImageHistory **history;
};

struct _FlatpakOciImageClass
{
  FlatpakJsonClass parent_class;
};

FlatpakOciImage *flatpak_oci_image_new (void);
void             flatpak_oci_image_set_created (FlatpakOciImage *image,
                                                const char      *created);
void             flatpak_oci_image_set_architecture (FlatpakOciImage *image,
                                                     const char      *arch);
void             flatpak_oci_image_set_os (FlatpakOciImage *image,
                                           const char      *os);
void             flatpak_oci_image_set_layers (FlatpakOciImage *image,
                                               const char     **layers);
int              flatpak_oci_image_get_n_layers (FlatpakOciImage *image);
void             flatpak_oci_image_set_layer (FlatpakOciImage *image,
                                              const char      *layer);
GHashTable *     flatpak_oci_image_get_labels (FlatpakOciImage *self);
int              flatpak_oci_image_add_history (FlatpakOciImage *image);

FlatpakOciImage * flatpak_oci_image_from_json (GBytes *bytes,
                                               GError **error);

void flatpak_oci_add_labels_for_commit (GHashTable *labels,
                                        const char *ref,
                                        const char *commit,
                                        GVariant   *commit_data);

#define FLATPAK_TYPE_OCI_SIGNATURE flatpak_oci_signature_get_type ()
G_DECLARE_FINAL_TYPE (FlatpakOciSignature, flatpak_oci_signature, FLATPAK, OCI_SIGNATURE, FlatpakJson)

typedef struct
{
  char *digest;
} FlatpakOciSignatureCriticalImage;

typedef struct
{
  char *ref;
} FlatpakOciSignatureCriticalIdentity;

typedef struct
{
  char                               *type;
  FlatpakOciSignatureCriticalImage    image;
  FlatpakOciSignatureCriticalIdentity identity;
} FlatpakOciSignatureCritical;

typedef struct
{
  char  *creator;
  gint64 timestamp;
} FlatpakOciSignatureOptional;

struct _FlatpakOciSignature
{
  FlatpakJson                 parent;

  FlatpakOciSignatureCritical critical;
  FlatpakOciSignatureOptional optional;
};

struct _FlatpakOciSignatureClass
{
  FlatpakJsonClass parent_class;
};

FlatpakOciSignature *flatpak_oci_signature_new (const char *digest,
                                                const char *ref);


#define FLATPAK_TYPE_OCI_INDEX_RESPONSE flatpak_oci_index_response_get_type ()
G_DECLARE_FINAL_TYPE (FlatpakOciIndexResponse, flatpak_oci_index_response, FLATPAK, OCI_INDEX_RESPONSE, FlatpakJson)

typedef struct
{
  char       *digest;
  char       *mediatype;
  char       *os;
  char       *architecture;
  GHashTable *annotations;
  GHashTable *labels;
  char      **tags;
} FlatpakOciIndexImage;

typedef struct
{
  char                  *digest;
  char                  *mediatype;
  char                 **tags;
  FlatpakOciIndexImage **images;
} FlatpakOciIndexImageList;

typedef struct
{
  char                      *name;
  FlatpakOciIndexImage     **images;
  FlatpakOciIndexImageList **lists;
} FlatpakOciIndexRepository;

struct _FlatpakOciIndexResponse
{
  FlatpakJson                 parent;

  char                       *registry;
  FlatpakOciIndexRepository **results;
};

struct _FlatpakOciIndexResponseClass
{
  FlatpakJsonClass parent_class;
};

#endif /* __FLATPAK_JSON_OCI_H__ */
