/*
 * Copyright © 2018 Red Hat, Inc
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

#ifndef __FLATPAK_PORTAL_APP_INFO_H__
#define __FLATPAK_PORTAL_APP_INFO_H__

#define FLATPAK_METADATA_GROUP_APPLICATION "Application"
#define FLATPAK_METADATA_GROUP_RUNTIME "Runtime"
#define FLATPAK_METADATA_GROUP_INSTANCE "Instance"
#define FLATPAK_METADATA_GROUP_CONTEXT "Context"
#define FLATPAK_METADATA_KEY_NAME "name"
#define FLATPAK_METADATA_KEY_ARCH "arch"
#define FLATPAK_METADATA_KEY_RUNTIME "runtime"
#define FLATPAK_METADATA_KEY_BRANCH "branch"
#define FLATPAK_METADATA_KEY_EXTRA_ARGS "extra-args"
#define FLATPAK_METADATA_KEY_APP_COMMIT "app-commit"
#define FLATPAK_METADATA_KEY_RUNTIME_COMMIT "runtime-commit"
#define FLATPAK_METADATA_KEY_SHARED "shared"
#define FLATPAK_METADATA_KEY_SOCKETS "sockets"
#define FLATPAK_METADATA_KEY_DEVICES "devices"
#define FLATPAK_METADATA_KEY_DEVEL "devel"
#define FLATPAK_METADATA_KEY_INSTANCE_PATH "instance-path"
#define FLATPAK_METADATA_KEY_INSTANCE_ID "instance-id"

GKeyFile * flatpak_invocation_lookup_app_info (GDBusMethodInvocation *invocation,
                                               GCancellable          *cancellable,
                                               GError               **error);

void flatpak_connection_track_name_owners (GDBusConnection *connection);

#endif /* __FLATPAK_PORTAL_APP_INFO_H__ */
