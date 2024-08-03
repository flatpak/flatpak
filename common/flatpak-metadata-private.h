/*
 * Copyright 2014 Red Hat, Inc
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "libglnx.h"

#include <glib.h>

G_BEGIN_DECLS

/* See flatpak-metadata(5) */

#define FLATPAK_METADATA_GROUP_APPLICATION "Application"
#define FLATPAK_METADATA_GROUP_RUNTIME "Runtime"
#define FLATPAK_METADATA_KEY_COMMAND "command"
#define FLATPAK_METADATA_KEY_NAME "name"
#define FLATPAK_METADATA_KEY_REQUIRED_FLATPAK "required-flatpak"
#define FLATPAK_METADATA_KEY_RUNTIME "runtime"
#define FLATPAK_METADATA_KEY_SDK "sdk"
#define FLATPAK_METADATA_KEY_TAGS "tags"

#define FLATPAK_METADATA_GROUP_CONTEXT "Context"
#define FLATPAK_METADATA_KEY_SHARED "shared"
#define FLATPAK_METADATA_KEY_SOCKETS "sockets"
#define FLATPAK_METADATA_KEY_FILESYSTEMS "filesystems"
#define FLATPAK_METADATA_KEY_PERSISTENT "persistent"
#define FLATPAK_METADATA_KEY_DEVICES "devices"
#define FLATPAK_METADATA_KEY_FEATURES "features"
#define FLATPAK_METADATA_KEY_UNSET_ENVIRONMENT "unset-environment"

#define FLATPAK_METADATA_GROUP_INSTANCE "Instance"
#define FLATPAK_METADATA_KEY_INSTANCE_PATH "instance-path"
#define FLATPAK_METADATA_KEY_INSTANCE_ID "instance-id"
#define FLATPAK_METADATA_KEY_ORIGINAL_APP_PATH "original-app-path"
#define FLATPAK_METADATA_KEY_APP_PATH "app-path"
#define FLATPAK_METADATA_KEY_APP_COMMIT "app-commit"
#define FLATPAK_METADATA_KEY_APP_EXTENSIONS "app-extensions"
#define FLATPAK_METADATA_KEY_ARCH "arch"
#define FLATPAK_METADATA_KEY_BRANCH "branch"
#define FLATPAK_METADATA_KEY_FLATPAK_VERSION "flatpak-version"
#define FLATPAK_METADATA_KEY_ORIGINAL_RUNTIME_PATH "original-runtime-path"
#define FLATPAK_METADATA_KEY_RUNTIME_PATH "runtime-path"
#define FLATPAK_METADATA_KEY_RUNTIME_COMMIT "runtime-commit"
#define FLATPAK_METADATA_KEY_RUNTIME_EXTENSIONS "runtime-extensions"
#define FLATPAK_METADATA_KEY_SESSION_BUS_PROXY "session-bus-proxy"
#define FLATPAK_METADATA_KEY_SYSTEM_BUS_PROXY "system-bus-proxy"
#define FLATPAK_METADATA_KEY_EXTRA_ARGS "extra-args"
#define FLATPAK_METADATA_KEY_SANDBOX "sandbox"
#define FLATPAK_METADATA_KEY_BUILD "build"
#define FLATPAK_METADATA_KEY_DEVEL "devel"

#define FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY "Session Bus Policy"
#define FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY "System Bus Policy"
#define FLATPAK_METADATA_GROUP_PREFIX_POLICY "Policy "
#define FLATPAK_METADATA_GROUP_ENVIRONMENT "Environment"

#define FLATPAK_METADATA_GROUP_PREFIX_EXTENSION "Extension "
#define FLATPAK_METADATA_KEY_ADD_LD_PATH "add-ld-path"
#define FLATPAK_METADATA_KEY_AUTODELETE "autodelete"
#define FLATPAK_METADATA_KEY_DIRECTORY "directory"
#define FLATPAK_METADATA_KEY_DOWNLOAD_IF "download-if"
#define FLATPAK_METADATA_KEY_ENABLE_IF "enable-if"
#define FLATPAK_METADATA_KEY_AUTOPRUNE_UNLESS "autoprune-unless"
#define FLATPAK_METADATA_KEY_MERGE_DIRS "merge-dirs"
#define FLATPAK_METADATA_KEY_NO_AUTODOWNLOAD "no-autodownload"
#define FLATPAK_METADATA_KEY_SUBDIRECTORIES "subdirectories"
#define FLATPAK_METADATA_KEY_SUBDIRECTORY_SUFFIX "subdirectory-suffix"
#define FLATPAK_METADATA_KEY_LOCALE_SUBSET "locale-subset"
#define FLATPAK_METADATA_KEY_VERSION "version"
#define FLATPAK_METADATA_KEY_VERSIONS "versions"

#define FLATPAK_METADATA_KEY_COLLECTION_ID "collection-id"

#define FLATPAK_METADATA_GROUP_EXTRA_DATA "Extra Data"
#define FLATPAK_METADATA_KEY_EXTRA_DATA_CHECKSUM "checksum"
#define FLATPAK_METADATA_KEY_EXTRA_DATA_INSTALLED_SIZE "installed-size"
#define FLATPAK_METADATA_KEY_EXTRA_DATA_NAME "name"
#define FLATPAK_METADATA_KEY_EXTRA_DATA_SIZE "size"
#define FLATPAK_METADATA_KEY_EXTRA_DATA_URI "uri"
#define FLATPAK_METADATA_KEY_NO_RUNTIME "NoRuntime"

#define FLATPAK_METADATA_GROUP_EXTENSION_OF "ExtensionOf"
#define FLATPAK_METADATA_KEY_PRIORITY "priority"
#define FLATPAK_METADATA_KEY_REF "ref"
#define FLATPAK_METADATA_KEY_TAG "tag"

#define FLATPAK_METADATA_GROUP_DCONF "X-DConf"
#define FLATPAK_METADATA_KEY_DCONF_PATHS "paths"
#define FLATPAK_METADATA_KEY_DCONF_MIGRATE_PATH "migrate-path"

#define FLATPAK_METADATA_GROUP_USB_DEVICES "USB Devices"
#define FLATPAK_METADATA_KEY_USB_ENUMERABLE_DEVICES "enumerable-devices"
#define FLATPAK_METADATA_KEY_USB_HIDDEN_DEVICES "hidden-devices"

G_END_DECLS
