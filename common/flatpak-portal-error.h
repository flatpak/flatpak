/* flatpak-portal-error.c
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

#ifndef FLATPAK_PORTAL_ERROR_H
#define FLATPAK_PORTAL_ERROR_H

#if !defined(__FLATPAK_H_INSIDE__) && !defined(FLATPAK_COMPILATION)
#error "Only <flatpak.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * FlatpakPortalError:
 * @FLATPAK_PORTAL_ERROR_FAILED: General portal failure
 * @FLATPAK_PORTAL_ERROR_INVALID_ARGUMENT: An argument was invalid
 * @FLATPAK_PORTAL_ERROR_NOT_FOUND: The object was not found
 * @FLATPAK_PORTAL_ERROR_EXISTS: The object already exists
 * @FLATPAK_PORTAL_ERROR_NOT_ALLOWED: The call was not allowed
 * @FLATPAK_PORTAL_ERROR_CANCELLED: The call was cancelled by the user
 * @FLATPAK_PORTAL_ERROR_WINDOW_DESTROYED: The window was destroyed by the user
 *
 * Error codes returned by portal calls.
 */
typedef enum {
  FLATPAK_PORTAL_ERROR_FAILED     = 0,
  FLATPAK_PORTAL_ERROR_INVALID_ARGUMENT,
  FLATPAK_PORTAL_ERROR_NOT_FOUND,
  FLATPAK_PORTAL_ERROR_EXISTS,
  FLATPAK_PORTAL_ERROR_NOT_ALLOWED,
  FLATPAK_PORTAL_ERROR_CANCELLED,
  FLATPAK_PORTAL_ERROR_WINDOW_DESTROYED,
} FlatpakPortalError;


/**
 * FLATPAK_PORTAL_ERROR:
 *
 * The error domain for #FlatpakPortalError errors.
 */
#define FLATPAK_PORTAL_ERROR flatpak_portal_error_quark ()

FLATPAK_EXTERN GQuark  flatpak_portal_error_quark (void);

G_END_DECLS

#endif /* FLATPAK_PORTAL_ERROR_H */
