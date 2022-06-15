/*
 * Copyright © 2022 Red Hat, Inc
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

#ifndef __FLATPAK_URI_PRIVATE_H__
#define __FLATPAK_URI_PRIVATE_H__

#include <string.h>

#include "flatpak-utils-private.h"

/* This file is mainly a backport of GUri for older versions of glib, and some helpers around it */

void        flatpak_uri_encode_query_arg         (GString    *query,
                                                  const char *key,
                                                  const char *value);
GHashTable *flatpak_parse_http_header_param_list (const char *header);
GDateTime * flatpak_parse_http_time              (const char *date_string);
char *      flatpak_format_http_date             (GDateTime  *date);

/* Same as SOUP_HTTP_URI_FLAGS, means all possible flags for http uris */
#define FLATPAK_HTTP_URI_FLAGS (G_URI_FLAGS_HAS_PASSWORD | G_URI_FLAGS_ENCODED_PATH | G_URI_FLAGS_ENCODED_QUERY | G_URI_FLAGS_ENCODED_FRAGMENT | G_URI_FLAGS_SCHEME_NORMALIZE)

#if !GLIB_CHECK_VERSION (2, 66, 0)

typedef enum {
  G_URI_FLAGS_NONE            = 0,
  G_URI_FLAGS_PARSE_RELAXED   = 1 << 0,
  G_URI_FLAGS_HAS_PASSWORD    = 1 << 1,
  G_URI_FLAGS_HAS_AUTH_PARAMS = 1 << 2,
  G_URI_FLAGS_ENCODED         = 1 << 3,
  G_URI_FLAGS_NON_DNS         = 1 << 4,
  G_URI_FLAGS_ENCODED_QUERY   = 1 << 5,
  G_URI_FLAGS_ENCODED_PATH    = 1 << 6,
  G_URI_FLAGS_ENCODED_FRAGMENT = 1 << 7,
  G_URI_FLAGS_SCHEME_NORMALIZE = 1 << 8,
} GUriFlags;

typedef enum {
  G_URI_HIDE_NONE        = 0,
  G_URI_HIDE_USERINFO    = 1 << 0,
  G_URI_HIDE_PASSWORD    = 1 << 1,
  G_URI_HIDE_AUTH_PARAMS = 1 << 2,
  G_URI_HIDE_QUERY       = 1 << 3,
  G_URI_HIDE_FRAGMENT    = 1 << 4,
} GUriHideFlags;

typedef struct _GUri GUri;

GUri *       flatpak_g_uri_ref               (GUri           *uri);
void         flatpak_g_uri_unref             (GUri           *uri);
GUri *       flatpak_g_uri_parse             (const gchar    *uri_string,
                                              GUriFlags       flags,
                                              GError        **error);
GUri *       flatpak_g_uri_parse_relative    (GUri           *base_uri,
                                              const gchar    *uri_ref,
                                              GUriFlags       flags,
                                              GError        **error);
char *       flatpak_g_uri_to_string_partial (GUri           *uri,
                                              GUriHideFlags   flags);
GUri *       flatpak_g_uri_build             (GUriFlags       flags,
                                              const gchar    *scheme,
                                              const gchar    *userinfo,
                                              const gchar    *host,
                                              gint            port,
                                              const gchar    *path,
                                              const gchar    *query,
                                              const gchar    *fragment);
const gchar *flatpak_g_uri_get_scheme        (GUri           *uri);
const gchar *flatpak_g_uri_get_userinfo      (GUri           *uri);
const gchar *flatpak_g_uri_get_user          (GUri           *uri);
const gchar *flatpak_g_uri_get_password      (GUri           *uri);
const gchar *flatpak_g_uri_get_auth_params   (GUri           *uri);
const gchar *flatpak_g_uri_get_host          (GUri           *uri);
gint         flatpak_g_uri_get_port          (GUri           *uri);
const gchar *flatpak_g_uri_get_path          (GUri           *uri);
const gchar *flatpak_g_uri_get_query         (GUri           *uri);
const gchar *flatpak_g_uri_get_fragment      (GUri           *uri);
GUriFlags    flatpak_g_uri_get_flags         (GUri           *uri);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GUri, flatpak_g_uri_unref)

#define g_uri_ref flatpak_g_uri_ref
#define g_uri_unref flatpak_g_uri_unref
#define g_uri_parse flatpak_g_uri_parse
#define g_uri_parse_relative flatpak_g_uri_parse_relative
#define g_uri_to_string_partial flatpak_g_uri_to_string_partial
#define g_uri_build flatpak_g_uri_build
#define g_uri_get_scheme flatpak_g_uri_get_scheme
#define g_uri_get_userinfo flatpak_g_uri_get_userinfo
#define g_uri_get_user flatpak_g_uri_get_user
#define g_uri_get_password flatpak_g_uri_get_password
#define g_uri_get_auth_params flatpak_g_uri_get_auth_params
#define g_uri_get_host flatpak_g_uri_get_host
#define g_uri_get_port flatpak_g_uri_get_port
#define g_uri_get_path flatpak_g_uri_get_path
#define g_uri_get_query flatpak_g_uri_get_query
#define g_uri_get_fragment flatpak_g_uri_get_fragment
#define g_uri_get_flags flatpak_g_uri_get_flags

#endif


#endif /* __FLATPAK_URI_PRIVATE_H__ */
