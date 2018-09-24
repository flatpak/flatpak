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

#ifndef __FLATPAK_BUILTINS_UTILS_H__
#define __FLATPAK_BUILTINS_UTILS_H__

#include <glib.h>
#include "libglnx/libglnx.h"
#include "flatpak-utils-private.h"
#include "flatpak-dir-private.h"
#include "flatpak-permission-dbus-generated.h"

/* Appstream data expires after a day */
#define FLATPAK_APPSTREAM_TTL 86400

gboolean    looks_like_branch (const char *branch);
GBytes *    download_uri (const char *url,
                          GError    **error);

GBytes * flatpak_load_gpg_keys (char        **gpg_import,
                                GCancellable *cancellable,
                                GError      **error);

FlatpakDir * flatpak_find_installed_pref (const char   *pref,
                                          FlatpakKinds  kinds,
                                          const char   *default_arch,
                                          const char   *default_branch,
                                          gboolean      search_all,
                                          gboolean      search_user,
                                          gboolean      search_system,
                                          char        **search_installations,
                                          char        **out_ref,
                                          GCancellable *cancellable,
                                          GError      **error);

gboolean flatpak_resolve_duplicate_remotes (GPtrArray    *dirs,
                                            const char   *remote_name,
                                            FlatpakDir  **out_dir,
                                            GCancellable *cancellable,
                                            GError      **error);

gboolean update_appstream (GPtrArray    *dirs,
                           const char   *remote,
                           const char   *arch,
                           guint64       ttl,
                           gboolean      quiet,
                           GCancellable *cancellable,
                           GError      **error);

char ** get_permission_tables (XdpDbusPermissionStore *store);


#endif /* __FLATPAK_BUILTINS_UTILS_H__ */
