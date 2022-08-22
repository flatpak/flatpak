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
 * @FLATPAK_ERROR_ALREADY_INSTALLED: App/runtime/remote is already installed
 * @FLATPAK_ERROR_NOT_INSTALLED: App/runtime is not installed
 * @FLATPAK_ERROR_ONLY_PULLED: App/runtime was only pulled into the local
 *                             repository but not installed.
 * @FLATPAK_ERROR_DIFFERENT_REMOTE: The App/Runtime is already installed, but from a different remote.
 * @FLATPAK_ERROR_ABORTED: The transaction was aborted (returned %TRUE in operation-error signal).
 * @FLATPAK_ERROR_SKIPPED: The App/Runtime install was skipped due to earlier errors.
 * @FLATPAK_ERROR_NEED_NEW_FLATPAK: The App/Runtime needs a more recent version of flatpak.
 * @FLATPAK_ERROR_REMOTE_NOT_FOUND: The specified remote was not found.
 * @FLATPAK_ERROR_RUNTIME_NOT_FOUND: A runtime needed for the app was not found.
 * @FLATPAK_ERROR_DOWNGRADE: The pulled commit is a downgrade, and a downgrade wasn't
 *                           specifically allowed. (Since: 1.0)
 * @FLATPAK_ERROR_INVALID_REF: A ref could not be parsed. (Since: 1.0.3)
 * @FLATPAK_ERROR_INVALID_DATA: Invalid data. (Since: 1.0.3)
 * @FLATPAK_ERROR_UNTRUSTED: Missing GPG key or signature. (Since: 1.0.3)
 * @FLATPAK_ERROR_SETUP_FAILED: Sandbox setup failed. (Since: 1.0.3)
 * @FLATPAK_ERROR_EXPORT_FAILED: Exporting data failed. (Since: 1.0.3)
 * @FLATPAK_ERROR_REMOTE_USED: Remote can't be uninstalled. (Since: 1.0.3)
 * @FLATPAK_ERROR_RUNTIME_USED: Runtime can't be uninstalled. (Since: 1.0.3)
 * @FLATPAK_ERROR_INVALID_NAME: Application, runtime, remote, or alias name is invalid. (Since: 1.0.3)
 * @FLATPAK_ERROR_OUT_OF_SPACE: More disk space needed. (Since: 1.2.0)
 * @FLATPAK_ERROR_WRONG_USER: An operation is being attempted by the wrong user (such as
 *                            root operating on a user installation). (Since: 1.2.0)
 * @FLATPAK_ERROR_NOT_CACHED: Cached data was requested, but it was not available. (Since: 1.4.0)
 * @FLATPAK_ERROR_REF_NOT_FOUND: The specified ref was not found. (Since: 1.4.0)
 * @FLATPAK_ERROR_PERMISSION_DENIED: An operation was not allowed by the administrative policy.
 *                                   For example, an app is not allowed to be installed due
 *                                   to not complying with the parental controls policy. (Since: 1.5.1)
 * @FLATPAK_ERROR_AUTHENTICATION_FAILED: An authentication operation failed, for example, no
 *                                       correct password was supplied. (Since: 1.7.3)
 * @FLATPAK_ERROR_NOT_AUTHORIZED: An operation tried to access a ref, or information about it that it
 *                                was not authorized. For example, when succesfully authenticating with a
 *                                server but the user doesn't have permissions for a private ref. (Since: 1.7.3)
 * @FLATPAK_ERROR_ALIAS_NOT_FOUND: The specified alias was not found. (Since: 1.13.4)
 * @FLATPAK_ERROR_ALIAS_ALREADY_EXISTS: The specified alias was already exists. (Since: 1.13.4)
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
  FLATPAK_ERROR_INVALID_REF,
  FLATPAK_ERROR_INVALID_DATA,
  FLATPAK_ERROR_UNTRUSTED,
  FLATPAK_ERROR_SETUP_FAILED,
  FLATPAK_ERROR_EXPORT_FAILED,
  FLATPAK_ERROR_REMOTE_USED,
  FLATPAK_ERROR_RUNTIME_USED,
  FLATPAK_ERROR_INVALID_NAME,
  FLATPAK_ERROR_OUT_OF_SPACE,
  FLATPAK_ERROR_WRONG_USER,
  FLATPAK_ERROR_NOT_CACHED,
  FLATPAK_ERROR_REF_NOT_FOUND,
  FLATPAK_ERROR_PERMISSION_DENIED,
  FLATPAK_ERROR_AUTHENTICATION_FAILED,
  FLATPAK_ERROR_NOT_AUTHORIZED,
  FLATPAK_ERROR_ALIAS_NOT_FOUND,
  FLATPAK_ERROR_ALIAS_ALREADY_EXISTS,
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
