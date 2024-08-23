/*
 * Copyright © 2014 Red Hat, Inc
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

#ifndef __FLATPAK_UTILS_HTTP_H__
#define __FLATPAK_UTILS_HTTP_H__

#include <string.h>

typedef enum {
  FLATPAK_HTTP_ERROR_NOT_CHANGED = 0,
  FLATPAK_HTTP_ERROR_UNAUTHORIZED = 1,
} FlatpakHttpErrorEnum;

#define FLATPAK_HTTP_ERROR flatpak_http_error_quark ()

GQuark flatpak_http_error_quark (void);

typedef struct FlatpakHttpSession FlatpakHttpSession;

FlatpakHttpSession* flatpak_create_http_session (const char *user_agent);
void flatpak_http_session_free (FlatpakHttpSession* http_session);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FlatpakHttpSession, flatpak_http_session_free)

typedef struct FlatpakCertificates FlatpakCertificates;

FlatpakCertificates* flatpak_get_certificates_for_uri (const char *uri,
                                                       GError    **error);
FlatpakCertificates * flatpak_certificates_copy (FlatpakCertificates *other);
void flatpak_certificates_free (FlatpakCertificates *certificates);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FlatpakCertificates, flatpak_certificates_free)

typedef enum {
  FLATPAK_HTTP_FLAGS_NONE = 0,
  FLATPAK_HTTP_FLAGS_ACCEPT_OCI = 1 << 0,
  FLATPAK_HTTP_FLAGS_STORE_COMPRESSED = 1 << 1,
  FLATPAK_HTTP_FLAGS_NOCHECK_STATUS = 1 << 2,
  FLATPAK_HTTP_FLAGS_HEAD = 1 << 3,
} FlatpakHTTPFlags;

typedef void (*FlatpakLoadUriProgress) (guint64  downloaded_bytes,
                                        gpointer user_data);

GBytes * flatpak_load_uri_full (FlatpakHttpSession    *http_session,
                                const char            *uri,
                                FlatpakCertificates   *certificates,
                                FlatpakHTTPFlags       flags,
                                const char            *auth,
                                const char            *token,
                                FlatpakLoadUriProgress progress,
                                gpointer               user_data,
                                int                   *out_status,
                                char                 **out_content_type,
                                char                 **out_www_authenticate,
                                GCancellable          *cancellable,
                                GError               **error);
GBytes * flatpak_load_uri (FlatpakHttpSession    *http_session,
                           const char            *uri,
                           FlatpakHTTPFlags       flags,
                           const char            *token,
                           FlatpakLoadUriProgress progress,
                           gpointer               user_data,
                           char                 **out_content_type,
                           GCancellable          *cancellable,
                           GError               **error);
gboolean flatpak_download_http_uri (FlatpakHttpSession    *http_session,
                                    const char            *uri,
                                    FlatpakCertificates   *certificates,
                                    FlatpakHTTPFlags       flags,
                                    GOutputStream         *out,
                                    const char            *token,
                                    FlatpakLoadUriProgress progress,
                                    gpointer               user_data,
                                    GCancellable          *cancellable,
                                    GError               **error);
gboolean flatpak_cache_http_uri (FlatpakHttpSession    *http_session,
                                 const char            *uri,
                                 FlatpakCertificates   *certificates,
                                 FlatpakHTTPFlags       flags,
                                 int                    dest_dfd,
                                 const char            *dest_subpath,
                                 FlatpakLoadUriProgress progress,
                                 gpointer               user_data,
                                 GCancellable          *cancellable,
                                 GError               **error);

#endif /* __FLATPAK_UTILS_HTTP_H__ */
