/* flatpak-error.c
 *
 * Copyright (C) 2015 Red Hat, Inc
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include "flatpak-portal-error.h"

#include <gio/gio.h>

static const GDBusErrorEntry flatpak_error_entries[] = {
  {FLATPAK_PORTAL_ERROR_FAILED,                           "org.freedesktop.portal.Error.Failed"},
  {FLATPAK_PORTAL_ERROR_INVALID_ARGUMENT,                 "org.freedesktop.portal.Error.InvalidArgument"},
  {FLATPAK_PORTAL_ERROR_NOT_FOUND,                        "org.freedesktop.portal.Error.NotFound"},
  {FLATPAK_PORTAL_ERROR_EXISTS,                           "org.freedesktop.portal.Error.Exists"},
  {FLATPAK_PORTAL_ERROR_NOT_ALLOWED,                      "org.freedesktop.portal.Error.NotAllowed"},
  {FLATPAK_PORTAL_ERROR_CANCELLED,                        "org.freedesktop.portal.Error.Cancelled"},
  {FLATPAK_PORTAL_ERROR_WINDOW_DESTROYED,                 "org.freedesktop.portal.Error.WindowDestroyed"},
};

GQuark
flatpak_portal_error_quark (void)
{
  static volatile gsize quark_volatile = 0;

  g_dbus_error_register_error_domain ("flatpak-portal-error-quark",
                                      &quark_volatile,
                                      flatpak_error_entries,
                                      G_N_ELEMENTS (flatpak_error_entries));
  return (GQuark) quark_volatile;
}
