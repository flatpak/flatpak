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

#include <libsoup/soup.h>

typedef enum {
  FLATPAK_HTTP_ERROR_NOT_CHANGED = 0,
  FLATPAK_HTTP_ERROR_UNAUTHORIZED = 1,
} FlatpakHttpErrorEnum;

#define FLATPAK_HTTP_ERROR flatpak_http_error_quark ()

FLATPAK_EXTERN GQuark  flatpak_http_error_quark (void);


SoupSession * flatpak_create_soup_session (const char *user_agent);

typedef enum {
  FLATPAK_HTTP_FLAGS_NONE = 0,
  FLATPAK_HTTP_FLAGS_ACCEPT_OCI = 1 << 0,
  FLATPAK_HTTP_FLAGS_STORE_COMPRESSED = 2 << 0,
} FlatpakHTTPFlags;

typedef void (*FlatpakLoadUriProgress) (guint64  downloaded_bytes,
                                        gpointer user_data);

GBytes * flatpak_load_uri (SoupSession           *soup_session,
                           const char            *uri,
                           FlatpakHTTPFlags       flags,
                           const char            *token,
                           FlatpakLoadUriProgress progress,
                           gpointer               user_data,
                           char                 **out_content_type,
                           GCancellable          *cancellable,
                           GError               **error);
gboolean flatpak_download_http_uri (SoupSession           *soup_session,
                                    const char            *uri,
                                    FlatpakHTTPFlags       flags,
                                    GOutputStream         *out,
                                    const char            *token,
                                    FlatpakLoadUriProgress progress,
                                    gpointer               user_data,
                                    GCancellable          *cancellable,
                                    GError               **error);
gboolean flatpak_cache_http_uri (SoupSession           *soup_session,
                                 const char            *uri,
                                 FlatpakHTTPFlags       flags,
                                 int                    dest_dfd,
                                 const char            *dest_subpath,
                                 FlatpakLoadUriProgress progress,
                                 gpointer               user_data,
                                 GCancellable          *cancellable,
                                 GError               **error);

#endif /* __FLATPAK_UTILS_HTTP_H__ */
