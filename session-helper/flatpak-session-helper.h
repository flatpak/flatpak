/*
 * Copyright © 2021 Collabora Ltd.
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
 */

#ifndef __FLATPAK_SESSION_HELPER_H__
#define __FLATPAK_SESSION_HELPER_H__

#define FLATPAK_SESSION_HELPER_BUS_NAME "org.freedesktop.Flatpak"

#define FLATPAK_SESSION_HELPER_PATH "/org/freedesktop/Flatpak/SessionHelper"
#define FLATPAK_SESSION_HELPER_INTERFACE "org.freedesktop.Flatpak.SessionHelper"

#define FLATPAK_SESSION_HELPER_PATH_DEVELOPMENT "/org/freedesktop/Flatpak/Development"
#define FLATPAK_SESSION_HELPER_INTERFACE_DEVELOPMENT "org.freedesktop.Flatpak.Development"

typedef enum {
  FLATPAK_HOST_COMMAND_FLAGS_CLEAR_ENV = 1 << 0,
  FLATPAK_HOST_COMMAND_FLAGS_WATCH_BUS = 1 << 1,
  FLATPAK_HOST_COMMAND_FLAGS_NONE = 0
} FlatpakHostCommandFlags;

#endif
