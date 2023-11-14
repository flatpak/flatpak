/*
 * Copyright © 2014 Red Hat, Inc
 * Copyright © 2023 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later
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

#include <gio/gio.h>

const char * const *flatpak_get_locale_categories (void);
char *flatpak_get_lang_from_locale (const char *locale);
char **flatpak_get_current_locale_langs (void);
const GPtrArray *flatpak_get_system_locales (void);
const GPtrArray *flatpak_get_user_locales (void);

/* Only for regression tests, should not be used directly */
GDBusProxy *flatpak_locale_get_localed_dbus_proxy (void);
void flatpak_get_locale_langs_from_localed_dbus (GDBusProxy *proxy,
                                                 GPtrArray  *langs);
GDBusProxy *flatpak_locale_get_accounts_dbus_proxy (void);
gboolean flatpak_get_all_langs_from_accounts_dbus (GDBusProxy *proxy,
                                                   GPtrArray  *langs);
void flatpak_get_locale_langs_from_accounts_dbus (GDBusProxy *proxy,
                                                  GPtrArray  *langs);
void flatpak_get_locale_langs_from_accounts_dbus_for_user (GDBusProxy *proxy,
                                                           GPtrArray  *langs,
                                                           guint uid);
