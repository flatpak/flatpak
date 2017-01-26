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
#include "flatpak-utils.h"
#include "flatpak-dir.h"

gboolean    looks_like_branch (const char  *branch);
GBytes *    download_uri      (const char  *url,
                               GError     **error);

FlatpakDir * flatpak_find_installed_pref (const char *pref,
                                          FlatpakKinds kinds,
                                          const char *default_arch,
                                          const char *default_branch,
                                          gboolean search_all,
                                          gboolean search_user,
                                          gboolean search_system,
                                          char **search_installations,
                                          char **out_ref,
                                          GCancellable *cancellable,
                                          GError **error);

#endif /* __FLATPAK_BUILTINS_UTILS_H__ */
