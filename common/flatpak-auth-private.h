/*
 * Copyright © 2019 Red Hat, Inc
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

#ifndef __FLATPAK_AUTH_H__
#define __FLATPAK_AUTH_H__

#include <flatpak-common-types-private.h>
#include "flatpak-dbus-generated.h"

#define FLATPAK_AUTHENTICATOR_OBJECT_PATH "/org/freedesktop/Flatpak/Authenticator"
#define FLATPAK_AUTHENTICATOR_REQUEST_OBJECT_PATH_PREFIX "/org/freedesktop/Flatpak/Authenticator/request/"

#define FLATPAK_REMOTE_CONFIG_AUTHENTICATOR_NAME "xa.authenticator-name"
#define FLATPAK_REMOTE_CONFIG_AUTHENTICATOR_OPTIONS_PREFIX "xa.authenticator-options."

enum {
      FLATPAK_AUTH_RESPONSE_OK,
      FLATPAK_AUTH_RESPONSE_CANCELLED,
      FLATPAK_AUTH_RESPONSE_ERROR,
};

typedef FlatpakAuthenticator AutoFlatpakAuthenticator;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoFlatpakAuthenticator, g_object_unref)

typedef FlatpakAuthenticatorRequest AutoFlatpakAuthenticatorRequest;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoFlatpakAuthenticatorRequest, g_object_unref)

FlatpakAuthenticator *       flatpak_auth_new_for_remote            (FlatpakDir                   *dir,
                                                                     const char                   *remote,
                                                                     GCancellable                 *cancellable,
                                                                     GError                      **error);
FlatpakAuthenticatorRequest *flatpak_auth_create_request            (FlatpakAuthenticator         *authenticator,
                                                                     GCancellable                 *cancellable,
                                                                     GError                      **error);
gboolean                     flatpak_auth_request_ref_tokens        (FlatpakAuthenticator         *authenticator,
                                                                     FlatpakAuthenticatorRequest  *request,
                                                                     const char                   *remote,
                                                                     const char                   *remote_uri,
                                                                     GVariant                     *refs,
                                                                     GVariant                     *options,
                                                                     const char                   *parent_window,
                                                                     GCancellable                 *cancellable,
                                                                     GError                      **error);
char *                       flatpak_auth_create_request_path       (const char                   *peer,
                                                                     const char                   *token,
                                                                     GError                      **error);

#endif /* __FLATPAK_AUTH_H__ */
