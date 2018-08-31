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

#if !defined(__FLATPAK_H_INSIDE__) && !defined(FLATPAK_COMPILATION)
#error "Only <flatpak.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/* NOTE: If you add an error code below, also update the list in common/flatpak-utils.c */
/**
 * FlatpakError:
 * @FLATPAK_ERROR_ALREADY_INSTALLED: App/runtime is already installed
 * @FLATPAK_ERROR_NOT_INSTALLED: App/runtime is not installed
 * @FLATPAK_ERROR_ONLY_PULLED: App/runtime was only pulled into the local
 *                             repository but not installed.
 * @FLATPAK_ERROR_DIFFERENT_REMOTE: The App/Runtime is already installed, but from a different remote.
 * @FLATPAK_ERROR_ABORTED: The transaction was aborted (returned TRUE in operation-error signal).
 * @FLATPAK_ERROR_SKIPPED: The App/Runtime install was skipped due to earlier errors.
 * @FLATPAK_ERROR_NEED_NEW_FLATPAK: The App/Runtime needs a more recent version of flatpak.
 * @FLATPAK_ERROR_REMOTE_NOT_FOUND: The specified remote was not found.
 * @FLATPAK_ERROR_RUNTIME_NOT_FOUND: An runtime needed for the app was not found.
 * @FLATPAK_ERROR_DOWNGRADE: The pulled commit is a downgrade, and a downgrade wasn't
 *                           specifically allowed. (Since: 1.0)
 *
 * Error codes for library functions.
 */
typedef enum {
  FLATPAK_ERROR_ALREADY_INSTALLED,
  FLATPAK_ERROR_NOT_INSTALLED,
  FLATPAK_ERROR_ONLY_PULLED,
  FLATPAK_ERROR_DIFFERENT_REMOTE,
  FLATPAK_ERROR_ABORTED,
  FLATPAK_ERROR_SKIPPED,
  FLATPAK_ERROR_NEED_NEW_FLATPAK,
  FLATPAK_ERROR_REMOTE_NOT_FOUND,
  FLATPAK_ERROR_RUNTIME_NOT_FOUND,
  FLATPAK_ERROR_DOWNGRADE,
} FlatpakError;

/**
 * FLATPAK_ERROR:
 *
 * The error domain for #FlatpakError errors.
 */
#define FLATPAK_ERROR flatpak_error_quark ()

FLATPAK_EXTERN GQuark  flatpak_error_quark (void);

G_END_DECLS

#endif /* FLATPAK_ERROR_H */
