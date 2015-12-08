/* xdg-app-error.c
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

#include "xdg-app-portal-error.h"

#include <gio/gio.h>

static const GDBusErrorEntry xdg_app_error_entries[] = {
  {XDG_APP_PORTAL_ERROR_FAILED,                           "org.freedesktop.XdgApp.Failed"},
  {XDG_APP_PORTAL_ERROR_INVALID_ARGUMENT,                 "org.freedesktop.XdgApp.InvalidArgument"},
  {XDG_APP_PORTAL_ERROR_NOT_FOUND,                        "org.freedesktop.XdgApp.NotFound"},
  {XDG_APP_PORTAL_ERROR_EXISTS,                           "org.freedesktop.XdgApp.Exists"},
  {XDG_APP_PORTAL_ERROR_NOT_ALLOWED,                      "org.freedesktop.XdgApp.NotAllowed"},
  {XDG_APP_PORTAL_ERROR_CANCELLED,                        "org.freedesktop.XdgApp.Cancelled"},
  {XDG_APP_PORTAL_ERROR_WINDOW_DESTROYED,                 "org.freedesktop.XdgApp.WindowDestroyed"},
};

GQuark
xdg_app_error_quark (void)
{
  static volatile gsize quark_volatile = 0;

  g_dbus_error_register_error_domain ("xdg-app--error-quark",
                                      &quark_volatile,
                                      xdg_app_error_entries,
                                      G_N_ELEMENTS (xdg_app_error_entries));
  return (GQuark) quark_volatile;
}
