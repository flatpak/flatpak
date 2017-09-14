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

#ifndef FLATPAK_ERROR_H
#define FLATPAK_ERROR_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * FlatpakError:
 * @FLATPAK_ERROR_ALREADY_INSTALLED: App/runtime is already installed
 * @FLATPAK_ERROR_NOT_INSTALLED: App/runtime is not installed
 * @FLATPAK_ERROR_ONLY_PULLED: App/runtime was only pulled into the local
 *                             repository but not installed.
 *
 * Error codes for library functions.
 */
typedef enum {
  FLATPAK_ERROR_ALREADY_INSTALLED,
  FLATPAK_ERROR_NOT_INSTALLED,
  FLATPAK_ERROR_ONLY_PULLED
} FlatpakError;

#define FLATPAK_ERROR flatpak_error_quark ()

FLATPAK_EXTERN GQuark  flatpak_error_quark (void);

G_END_DECLS

#endif /* FLATPAK_ERROR_H */
