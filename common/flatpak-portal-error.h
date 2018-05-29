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

#include <glib.h>

G_BEGIN_DECLS

/**
 * FlatpakPortalError:
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


#define FLATPAK_PORTAL_ERROR flatpak_portal_error_quark ()

FLATPAK_EXTERN GQuark  flatpak_portal_error_quark (void);

G_END_DECLS

#endif /* FLATPAK_PORTAL_ERROR_H */
