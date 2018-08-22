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

#if !defined(__FLATPAK_H_INSIDE__) && !defined(FLATPAK_COMPILATION)
#error "Only <flatpak.h> can be included directly."
#endif

#ifndef __FLATPAK_TRANSACTION_H__
#define __FLATPAK_TRANSACTION_H__

#include <gio/gio.h>
#include <flatpak-installation.h>

#define FLATPAK_TYPE_TRANSACTION flatpak_transaction_get_type ()
#define FLATPAK_TYPE_TRANSACTION_PROGRESS flatpak_transaction_progress_get_type ()
#define FLATPAK_TYPE_TRANSACTION_OPERATION flatpak_transaction_operation_get_type ()

/**
 * FlatpakTransactionOperationType
 * @FLATPAK_TRANSACTION_OPERATION_INSTALL: Install a ref from a remote
 * @FLATPAK_TRANSACTION_OPERATION_UPDATE: Update an installed ref
 * @FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE: Install a bundle from a file
 * @FLATPAK_TRANSACTION_OPERATION_UNINSTALL: Uninstall a ref
 * @FLATPAK_TRANSACTION_OPERATION_LAST_TYPE: The (currently) last operation type
 *
 * The type of a #FlatpakTransactionOperation.
 */
typedef enum {
  FLATPAK_TRANSACTION_OPERATION_INSTALL,
  FLATPAK_TRANSACTION_OPERATION_UPDATE,
  FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE,
  FLATPAK_TRANSACTION_OPERATION_UNINSTALL,
  FLATPAK_TRANSACTION_OPERATION_LAST_TYPE
} FlatpakTransactionOperationType;

/**
 * FlatpakTransactionErrorDetails
 * @FLATPAK_TRANSACTION_ERROR_DETAILS_NON_FATAL: The operation failure was not fatal
 *
 * The details for #FlatpakTransaction::operation-error.
 */
typedef enum {
  FLATPAK_TRANSACTION_ERROR_DETAILS_NON_FATAL = 1 << 0,
} FlatpakTransactionErrorDetails;

/**
 * FlatpakTransactionResult
 * @FLATPAK_TRANSACTION_RESULT_NO_CHANGE: The update caused no changes
 *
 * The details for #FlatpakTransaction::operation-done.
 */
typedef enum {
  FLATPAK_TRANSACTION_RESULT_NO_CHANGE = 1 << 0,
} FlatpakTransactionResult;

/**
 * FlatpakTransactionRemoteReason
 * @FLATPAK_TRANSACTION_REMOTE_GENERIC_REPO: The remote specified in the flatpakref has other apps too
 * @FLATPAK_TRANSACTION_REMOTE_RUNTIME_DEPS: The remote has runtimes needed for the app
 *
 * The reason for #FlatpakTransaction::add-new-remote.
 */
typedef enum {
  FLATPAK_TRANSACTION_REMOTE_GENERIC_REPO,
  FLATPAK_TRANSACTION_REMOTE_RUNTIME_DEPS,
} FlatpakTransactionRemoteReason;

FLATPAK_EXTERN
G_DECLARE_FINAL_TYPE (FlatpakTransactionProgress, flatpak_transaction_progress, FLATPAK, TRANSACTION_PROGRESS, GObject)

FLATPAK_EXTERN
G_DECLARE_FINAL_TYPE (FlatpakTransactionOperation, flatpak_transaction_operation, FLATPAK, TRANSACTION_OPERATION, GObject)

FLATPAK_EXTERN
G_DECLARE_DERIVABLE_TYPE (FlatpakTransaction, flatpak_transaction, FLATPAK, TRANSACTION, GObject)

struct _FlatpakTransactionClass
{
  GObjectClass parent_class;

  void         (*new_operation)        (FlatpakTransaction          *transaction,
                                        FlatpakTransactionOperation *operation,
                                        FlatpakTransactionProgress  *progress);
  void (*operation_done)       (FlatpakTransaction          *transaction,
                                FlatpakTransactionOperation *operation,
                                FlatpakTransactionResult     details);
  gboolean (*operation_error)  (FlatpakTransaction            *transaction,
                                FlatpakTransactionOperation   *operation,
                                const GError                  *error,
                                FlatpakTransactionErrorDetails detail);
  int (*choose_remote_for_ref) (FlatpakTransaction *transaction,
                                const char         *for_ref,
                                const char         *runtime_ref,
                                const char * const *remotes);
  void (*end_of_lifed)         (FlatpakTransaction *transaction,
                                const char         *ref,
                                const char         *reason,
                                const char         *rebase);
  gboolean (*ready)            (FlatpakTransaction *transaction);

  gboolean (*add_new_remote) (FlatpakTransaction            *transaction,
                              FlatpakTransactionRemoteReason reason,
                              const char                    *from_id,
                              const char                    *remote_name,
                              const char                    *url);
  gpointer padding[10];
};

FLATPAK_EXTERN
FlatpakTransaction *flatpak_transaction_new_for_installation (FlatpakInstallation *installation,
                                                              GCancellable        *cancellable,
                                                              GError             **error);

FLATPAK_EXTERN
void        flatpak_transaction_progress_set_update_frequency (FlatpakTransactionProgress *self,
                                                               guint                       update_frequency);
FLATPAK_EXTERN
char *      flatpak_transaction_progress_get_status (FlatpakTransactionProgress *self);
FLATPAK_EXTERN
gboolean    flatpak_transaction_progress_get_is_estimating (FlatpakTransactionProgress *self);
FLATPAK_EXTERN
int         flatpak_transaction_progress_get_progress (FlatpakTransactionProgress *self);


FLATPAK_EXTERN
FlatpakTransactionOperationType flatpak_transaction_operation_get_operation_type (FlatpakTransactionOperation *self);
FLATPAK_EXTERN
const char *                    flatpak_transaction_operation_get_ref (FlatpakTransactionOperation *self);
FLATPAK_EXTERN
const char *                    flatpak_transaction_operation_get_remote (FlatpakTransactionOperation *self);
FLATPAK_EXTERN
GFile *                         flatpak_transaction_operation_get_bundle_path (FlatpakTransactionOperation *self);
FLATPAK_EXTERN
const char *                    flatpak_transaction_operation_get_commit (FlatpakTransactionOperation *self);
FLATPAK_EXTERN
GKeyFile *                      flatpak_transaction_operation_get_metadata (FlatpakTransactionOperation *self);
FLATPAK_EXTERN
GKeyFile *                      flatpak_transaction_operation_get_old_metadata (FlatpakTransactionOperation *self);
FLATPAK_EXTERN
const char *                    flatpak_transaction_operation_type_to_string (FlatpakTransactionOperationType kind);

FLATPAK_EXTERN
void                flatpak_transaction_set_no_pull (FlatpakTransaction *self,
                                                     gboolean            no_pull);
FLATPAK_EXTERN
void                flatpak_transaction_set_no_deploy (FlatpakTransaction *self,
                                                       gboolean            no_deploy);
FLATPAK_EXTERN
void                flatpak_transaction_set_disable_static_deltas (FlatpakTransaction *self,
                                                                   gboolean            disable_static_deltas);
FLATPAK_EXTERN
void                flatpak_transaction_set_disable_prune (FlatpakTransaction *self,
                                                           gboolean            disable_prune);
FLATPAK_EXTERN
void                flatpak_transaction_set_disable_dependencies (FlatpakTransaction *self,
                                                                  gboolean            disable_dependencies);
FLATPAK_EXTERN
void                flatpak_transaction_set_disable_related (FlatpakTransaction *self,
                                                             gboolean            disable_related);
FLATPAK_EXTERN
void                flatpak_transaction_set_reinstall (FlatpakTransaction *self,
                                                       gboolean            reinstall);
FLATPAK_EXTERN
void                flatpak_transaction_set_force_uninstall (FlatpakTransaction *self,
                                                             gboolean            force_uninstall);
FLATPAK_EXTERN
void                flatpak_transaction_set_default_arch (FlatpakTransaction *self,
                                                          const char         *arch);
FLATPAK_EXTERN
void                flatpak_transaction_add_dependency_source (FlatpakTransaction  *self,
                                                               FlatpakInstallation *installation);
FLATPAK_EXTERN
void                flatpak_transaction_add_default_dependency_sources (FlatpakTransaction *self);
FLATPAK_EXTERN
gboolean            flatpak_transaction_run (FlatpakTransaction *self,
                                             GCancellable       *cancellable,
                                             GError            **error);
FLATPAK_EXTERN
FlatpakTransactionOperation *flatpak_transaction_get_current_operation (FlatpakTransaction *self);
FLATPAK_EXTERN
FlatpakInstallation *flatpak_transaction_get_installation (FlatpakTransaction *self);
FLATPAK_EXTERN
GList *flatpak_transaction_get_operations (FlatpakTransaction *self);

FLATPAK_EXTERN
gboolean            flatpak_transaction_add_install (FlatpakTransaction *self,
                                                     const char         *remote,
                                                     const char         *ref,
                                                     const char        **subpaths,
                                                     GError            **error);
FLATPAK_EXTERN
gboolean            flatpak_transaction_add_install_bundle (FlatpakTransaction *self,
                                                            GFile              *file,
                                                            GBytes             *gpg_data,
                                                            GError            **error);
FLATPAK_EXTERN
gboolean            flatpak_transaction_add_install_flatpakref (FlatpakTransaction *self,
                                                                GBytes             *flatpakref_data,
                                                                GError            **error);
FLATPAK_EXTERN
gboolean            flatpak_transaction_add_update (FlatpakTransaction *self,
                                                    const char         *ref,
                                                    const char        **subpaths,
                                                    const char         *commit,
                                                    GError            **error);
FLATPAK_EXTERN
gboolean            flatpak_transaction_add_uninstall (FlatpakTransaction *self,
                                                       const char         *ref,
                                                       GError            **error);
FLATPAK_EXTERN
gboolean            flatpak_transaction_is_empty (FlatpakTransaction *self);

#endif /* __FLATPAK_TRANSACTION_H__ */
