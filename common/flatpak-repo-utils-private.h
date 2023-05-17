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

#pragma once

#include "libglnx.h"

/**
 * FLATPAK_SUMMARY_INDEX_GVARIANT_FORMAT:
 *
 * dict
 *   s: subset name
 *  ->
 *   ay - checksum of subsummary
 *   aay - previous subsummary checksums
 *   a{sv} - per subset metadata
 * a{sv} - metadata

 */
#define FLATPAK_SUMMARY_INDEX_GVARIANT_STRING "(a{s(ayaaya{sv})}a{sv})"
#define FLATPAK_SUMMARY_INDEX_GVARIANT_FORMAT G_VARIANT_TYPE (FLATPAK_SUMMARY_INDEX_GVARIANT_STRING)

#define FLATPAK_REF_GROUP "Flatpak Ref"
#define FLATPAK_REF_VERSION_KEY "Version"
#define FLATPAK_REF_URL_KEY "Url"
#define FLATPAK_REF_RUNTIME_REPO_KEY "RuntimeRepo"
#define FLATPAK_REF_SUGGEST_REMOTE_NAME_KEY "SuggestRemoteName"
#define FLATPAK_REF_TITLE_KEY "Title"
#define FLATPAK_REF_GPGKEY_KEY "GPGKey"
#define FLATPAK_REF_IS_RUNTIME_KEY "IsRuntime"
#define FLATPAK_REF_NAME_KEY "Name"
#define FLATPAK_REF_BRANCH_KEY "Branch"
#define FLATPAK_REF_COLLECTION_ID_KEY "CollectionID"
#define FLATPAK_REF_DEPLOY_COLLECTION_ID_KEY "DeployCollectionID"
#define FLATPAK_REF_DEPLOY_SIDELOAD_COLLECTION_ID_KEY "DeploySideloadCollectionID"

#define FLATPAK_REPO_GROUP "Flatpak Repo"
#define FLATPAK_REPO_VERSION_KEY "Version"
#define FLATPAK_REPO_URL_KEY "Url"
#define FLATPAK_REPO_SUBSET_KEY "Subset"
#define FLATPAK_REPO_TITLE_KEY "Title"
#define FLATPAK_REPO_DEFAULT_BRANCH_KEY "DefaultBranch"
#define FLATPAK_REPO_GPGKEY_KEY "GPGKey"
#define FLATPAK_REPO_NODEPS_KEY "NoDeps"
#define FLATPAK_REPO_COMMENT_KEY "Comment"
#define FLATPAK_REPO_DESCRIPTION_KEY "Description"
#define FLATPAK_REPO_HOMEPAGE_KEY "Homepage"
#define FLATPAK_REPO_ICON_KEY "Icon"
#define FLATPAK_REPO_FILTER_KEY "Filter"
#define FLATPAK_REPO_AUTHENTICATOR_NAME_KEY "AuthenticatorName"
#define FLATPAK_REPO_AUTHENTICATOR_INSTALL_KEY "AuthenticatorInstall"

#define FLATPAK_REPO_COLLECTION_ID_KEY "CollectionID"
#define FLATPAK_REPO_DEPLOY_COLLECTION_ID_KEY "DeployCollectionID"
#define FLATPAK_REPO_DEPLOY_SIDELOAD_COLLECTION_ID_KEY "DeploySideloadCollectionID"

#define FLATPAK_SPARSE_CACHE_KEY_ENDOFLINE "eol"
#define FLATPAK_SPARSE_CACHE_KEY_ENDOFLINE_REBASE "eolr"
#define FLATPAK_SPARSE_CACHE_KEY_TOKEN_TYPE "tokt"
#define FLATPAK_SPARSE_CACHE_KEY_EXTRA_DATA_SIZE "eds"
