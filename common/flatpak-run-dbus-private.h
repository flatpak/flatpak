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

#include "flatpak-bwrap-private.h"
#include "flatpak-common-types-private.h"
#include "flatpak-context-private.h"

G_BEGIN_DECLS

gboolean flatpak_run_add_session_dbus_args (FlatpakBwrap   *app_bwrap,
                                            FlatpakBwrap   *proxy_arg_bwrap,
                                            FlatpakContext *context,
                                            FlatpakRunFlags flags,
                                            const char     *app_id);

gboolean flatpak_run_add_system_dbus_args (FlatpakBwrap   *app_bwrap,
                                           FlatpakBwrap   *proxy_arg_bwrap,
                                           FlatpakContext *context,
                                           FlatpakRunFlags flags);

gboolean flatpak_run_add_a11y_dbus_args (FlatpakBwrap    *app_bwrap,
                                         FlatpakBwrap    *proxy_arg_bwrap,
                                         FlatpakContext  *context,
                                         FlatpakRunFlags  flags,
                                         const char      *app_id);

gboolean flatpak_run_maybe_start_dbus_proxy (FlatpakBwrap *app_bwrap,
                                             FlatpakBwrap *proxy_arg_bwrap,
                                             const char   *app_info_path,
                                             GError      **error);

G_END_DECLS
