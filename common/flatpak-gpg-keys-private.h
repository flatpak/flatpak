/*
 * Copyright 2026 Philip Withnall
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
 *       Philip Withnall <philip@tecnocode.co.uk>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef __FLATPAK_GPG_KEYS_H__
#define __FLATPAK_GPG_KEYS_H__

#include <glib.h>
#include <gio/gio.h>
#include <gpgme.h>
#include <ostree.h>

#include "libglnx.h"

G_DEFINE_AUTO_CLEANUP_FREE_FUNC (gpgme_data_t, gpgme_data_release, NULL)
G_DEFINE_AUTO_CLEANUP_FREE_FUNC (gpgme_ctx_t, gpgme_release, NULL)
G_DEFINE_AUTO_CLEANUP_FREE_FUNC (gpgme_key_t, gpgme_key_unref, NULL)

void flatpak_gpgme_error_to_gio_error (gpgme_error_t   gpg_error,
                                       GError        **error);
gboolean flatpak_gpgme_ctx_tmp_home_dir (gpgme_ctx_t    gpgme_ctx,
                                         GLnxTmpDir    *tmpdir,
                                         OstreeRepo    *repo,
                                         const char    *remote_name,
                                         GCancellable  *cancellable,
                                         GError       **error);

char **flatpak_gpg_keys_validate_binding (OstreeRepo    *repo,
                                          const char    *remote_name,
                                          GFile         *gpg_keys_file,
                                          GCancellable  *cancellable,
                                          GError       **error);

#endif /* __FLATPAK_GPG_KEYS_H__ */
