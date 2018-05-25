/*
 * Copyright © 2016 Red Hat, Inc
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

#ifndef __FLATPAK_TRANSACTION_H__
#define __FLATPAK_TRANSACTION_H__

#include <gio/gio.h>

/* Note: This header is actually public in libflatpak, even if its in common/ */

#define FLATPAK_TYPE_TRANSACTION flatpak_transaction_get_type ()

typedef enum {
  FLATPAK_TRANSACTION_OPERATION_INSTALL,
  FLATPAK_TRANSACTION_OPERATION_UPDATE,
  FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE
} FlatpakTransactionOperationType;

typedef enum {
  FLATPAK_TRANSACTION_ERROR_NON_FATAL = 1 << 0,
} FlatpakTransactionError;

FLATPAK_EXTERN
G_DECLARE_FINAL_TYPE (FlatpakTransaction, flatpak_transaction, FLATPAK, TRANSACTION, GObject)

FLATPAK_EXTERN
void                flatpak_transaction_set_no_pull               (FlatpakTransaction  *self,
                                                                   gboolean             no_pull);
FLATPAK_EXTERN
void                flatpak_transaction_set_no_deploy             (FlatpakTransaction  *self,
                                                                   gboolean             no_deploy);
FLATPAK_EXTERN
void                flatpak_transaction_set_disable_static_deltas (FlatpakTransaction  *self,
                                                                   gboolean             disable_static_deltas);
FLATPAK_EXTERN
void                flatpak_transaction_set_disable_dependencies  (FlatpakTransaction  *self,
                                                                   gboolean             disable_dependencies);
FLATPAK_EXTERN
void                flatpak_transaction_set_disable_related       (FlatpakTransaction  *self,
                                                                   gboolean             disable_related);
FLATPAK_EXTERN
void                flatpak_transaction_set_reinstall             (FlatpakTransaction   *self,
                                                                   gboolean             reinstall);
FLATPAK_EXTERN
gboolean            flatpak_transaction_update_metadata           (FlatpakTransaction  *self,
                                                                   gboolean             all_remotes,
                                                                   GCancellable        *cancellable,
                                                                   GError             **error);
FLATPAK_EXTERN
gboolean            flatpak_transaction_run                       (FlatpakTransaction  *self,
                                                                   GCancellable        *cancellable,
                                                                   GError             **error);
FLATPAK_EXTERN
gboolean            flatpak_transaction_add_install               (FlatpakTransaction  *self,
                                                                   const char          *remote,
                                                                   const char          *ref,
                                                                   const char         **subpaths,
                                                                   GError             **error);
FLATPAK_EXTERN
gboolean            flatpak_transaction_add_install_bundle        (FlatpakTransaction  *self,
                                                                   GFile               *file,
                                                                   GBytes              *gpg_data,
                                                                   GError             **error);
FLATPAK_EXTERN
gboolean            flatpak_transaction_add_update                (FlatpakTransaction  *self,
                                                                   const char          *ref,
                                                                   const char         **subpaths,
                                                                   const char          *commit,
                                                                   GError             **error);
FLATPAK_EXTERN
gboolean            flatpak_transaction_is_empty                  (FlatpakTransaction  *self);

#endif /* __FLATPAK_TRANSACTION_H__ */
