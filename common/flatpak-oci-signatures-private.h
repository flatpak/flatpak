#ifndef __FLATPAK_OCI_SIGNATURES_H__
#define __FLATPAK_OCI_SIGNATURES_H__

#include "flatpak-json-oci-private.h"

#include <glib.h>
#include <gio/gio.h>
#include <ostree.h>

GBytes *flatpak_oci_sign_data (GBytes       *data,
                               const gchar **okey_ids,
                               const char   *homedir,
                               GError      **error);

FlatpakOciSignature *flatpak_oci_verify_signature (OstreeRepo *repo,
                                                   const char *remote_name,
                                                   GBytes     *signature,
                                                   GError    **error);

#endif /* __FLATPAK_OCI_SIGNATURES_H__ */
