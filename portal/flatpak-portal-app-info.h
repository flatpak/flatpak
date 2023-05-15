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

#include "flatpak-metadata-private.h"

GKeyFile * flatpak_invocation_lookup_app_info (GDBusMethodInvocation *invocation,
                                               GCancellable          *cancellable,
                                               GError               **error);

void flatpak_connection_track_name_owners (GDBusConnection *connection);

#endif /* __FLATPAK_PORTAL_APP_INFO_H__ */
