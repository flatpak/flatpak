#ifndef __FLATPAK_OCI_SIGNATURES_H__
#define __FLATPAK_OCI_SIGNATURES_H__

#include "flatpak-json-oci-private.h"

#include <glib.h>
#include <gio/gio.h>
#include <ostree.h>

typedef struct _FlatpakOciSignatures FlatpakOciSignatures;

gboolean flatpak_remote_has_gpg_key (OstreeRepo   *repo,
                                     const char   *remote_name);

FlatpakOciSignatures *flatpak_oci_signatures_new (void);

void flatpak_oci_signatures_free (FlatpakOciSignatures *self);

void flatpak_oci_signatures_add_signature (FlatpakOciSignatures *self,
                                           GBytes               *signature);

gboolean flatpak_oci_signatures_load_from_dfd (FlatpakOciSignatures *self,
                                               int                   dfd,
                                               GCancellable         *cancellable,
                                               GError              **error);

gboolean flatpak_oci_signatures_save_to_dfd (FlatpakOciSignatures *self,
                                             int                   dfd,
                                             GCancellable         *cancellable,
                                             GError              **error);
gboolean flatpak_oci_signatures_verify (FlatpakOciSignatures *self,
                                        OstreeRepo           *repo,
                                        const char           *remote_name,
                                        const char           *registry_url,
                                        const char           *repository_name,
                                        const char           *digest,
                                        GError              **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FlatpakOciSignatures, flatpak_oci_signatures_free)

#endif /* __FLATPAK_OCI_SIGNATURES_H__ */
