/*
 * Copyright © 2019 Red Hat, Inc
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include "flatpak-quiet-transaction.h"
#include "flatpak-transaction-private.h"
#include "flatpak-installation-private.h"
#include "flatpak-run-private.h"
#include "flatpak-table-printer.h"
#include "flatpak-utils-private.h"
#include "flatpak-error.h"
#include <glib/gi18n.h>


struct _FlatpakQuietTransaction
{
  FlatpakTransaction parent;
  gboolean got_error;
};

struct _FlatpakQuietTransactionClass
{
  FlatpakTransactionClass parent_class;
};

G_DEFINE_TYPE (FlatpakQuietTransaction, flatpak_quiet_transaction, FLATPAK_TYPE_TRANSACTION);

static int
choose_remote_for_ref (FlatpakTransaction *transaction,
                       const char         *for_ref,
                       const char         *runtime_ref,
                       const char * const *remotes)
{
  return 0;
}

static gboolean
add_new_remote (FlatpakTransaction            *transaction,
                FlatpakTransactionRemoteReason reason,
                const char                    *from_id,
                const char                    *remote_name,
                const char                    *url)
{
  return TRUE;
}

static void
new_operation (FlatpakTransaction          *transaction,
               FlatpakTransactionOperation *op,
               FlatpakTransactionProgress  *progress)
{
  FlatpakTransactionOperationType op_type = flatpak_transaction_operation_get_operation_type (op);
  const char *ref = flatpak_transaction_operation_get_ref (op);

  switch (op_type)
    {
    case FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE:
    case FLATPAK_TRANSACTION_OPERATION_INSTALL:
      g_print (_("Installing %s\n"), ref);
      break;

    case FLATPAK_TRANSACTION_OPERATION_UPDATE:
      g_print (_("Updating %s\n"), ref);
      break;

    case FLATPAK_TRANSACTION_OPERATION_UNINSTALL:
      g_print (_("Uninstalling %s\n"), ref);
      break;

    default:
      g_assert_not_reached ();
      break;
    }
}

static char *
op_type_to_string (FlatpakTransactionOperationType operation_type)
{
  switch (operation_type)
    {
    case FLATPAK_TRANSACTION_OPERATION_INSTALL:
      return _("install");

    case FLATPAK_TRANSACTION_OPERATION_UPDATE:
      return _("update");

    case FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE:
      return _("install bundle");

    case FLATPAK_TRANSACTION_OPERATION_UNINSTALL:
      return _("uninstall");

    default:
      return "Unknown type"; /* Should not happen */
    }
}

static gboolean
operation_error (FlatpakTransaction            *transaction,
                 FlatpakTransactionOperation   *op,
                 const GError                  *error,
                 FlatpakTransactionErrorDetails detail)
{
  FlatpakQuietTransaction *self = FLATPAK_QUIET_TRANSACTION (transaction);
  FlatpakTransactionOperationType op_type = flatpak_transaction_operation_get_operation_type (op);
  const char *ref = flatpak_transaction_operation_get_ref (op);
  g_autoptr(FlatpakRef) rref = flatpak_ref_parse (ref, NULL);
  g_autofree char *msg = NULL;
  gboolean non_fatal = (detail & FLATPAK_TRANSACTION_ERROR_DETAILS_NON_FATAL) != 0;

  if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_SKIPPED))
    {
      g_print (_("Info: %s was skipped"), flatpak_ref_get_name (rref));
      return TRUE;
    }

  if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED))
    msg = g_strdup_printf (_("%s already installed"), flatpak_ref_get_name (rref));
  else if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED))
    msg = g_strdup_printf (_("%s not installed"), flatpak_ref_get_name (rref));
  else if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED))
    msg = g_strdup_printf (_("%s not installed"), flatpak_ref_get_name (rref));
  else if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_NEED_NEW_FLATPAK))
    msg = g_strdup_printf (_("%s needs a later flatpak version"), flatpak_ref_get_name (rref));
  else if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_OUT_OF_SPACE))
    msg = g_strdup (_("Not enough disk space to complete this operation"));
  else
    msg = g_strdup (error->message);

  g_printerr (_("%s Failed to %s %s: %s\n"),
              non_fatal ? _("Warning:") : _("Error:"),
              op_type_to_string (op_type),
              flatpak_ref_get_name (rref),
              msg);

  if (non_fatal)
    return TRUE; /* Continue */

  self->got_error = TRUE;

  return non_fatal; /* Continue if non-fatal */
}

static void
install_authenticator (FlatpakTransaction            *old_transaction,
                       const char                    *remote,
                       const char                    *ref)
{
  g_autoptr(FlatpakTransaction)  transaction2 = NULL;
  g_autoptr(GError) local_error = NULL;
  FlatpakInstallation *installation = flatpak_transaction_get_installation (old_transaction);
  FlatpakDir *dir = flatpak_installation_get_dir (installation, NULL);

  if (dir == NULL)
    {
      /* This should not happen */
      g_warning ("No dir in install_authenticator");
      return;
    }

  transaction2 = flatpak_quiet_transaction_new (dir, &local_error);
  if (transaction2 == NULL)
    {
      g_printerr ("Unable to install authenticator: %s\n", local_error->message);
      return;
    }

  if (!flatpak_transaction_add_install (transaction2, remote, ref, NULL, &local_error))
    {
      if (!g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED))
        g_printerr ("Unable to install authenticator: %s\n", local_error->message);
      return;
    }

  if (!flatpak_transaction_run (transaction2, NULL, &local_error))
    {
      if (!g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED))
        g_printerr ("Unable to install authenticator: %s\n", local_error->message);
      return;
    }

  return;
}

static gboolean
end_of_lifed_with_rebase (FlatpakTransaction *transaction,
                          const char         *remote,
                          const char         *ref,
                          const char         *reason,
                          const char         *rebased_to_ref,
                          const char        **previous_ids)
{
  FlatpakQuietTransaction *self = FLATPAK_QUIET_TRANSACTION (transaction);
  g_autoptr(FlatpakRef) rref = flatpak_ref_parse (ref, NULL);

  if (rebased_to_ref)
    g_print (_("Info: %s is end-of-life, in favor of %s\n"), flatpak_ref_get_name (rref), rebased_to_ref);
  else if (reason)
    g_print (_("Info: %s is end-of-life, with reason: %s\n"), flatpak_ref_get_name (rref), reason);

  if (rebased_to_ref && remote)
    {
      g_autoptr(GError) error = NULL;

      g_print (_("Updating to rebased version\n"));

      if (!flatpak_transaction_add_rebase (transaction, remote, rebased_to_ref, NULL, previous_ids, &error))
        {
          g_printerr (_("Failed to rebase %s to %s: %s\n"),
                      flatpak_ref_get_name (rref), rebased_to_ref, error->message);
          self->got_error = TRUE;
          return FALSE;
        }

      if (!flatpak_transaction_add_uninstall (transaction, ref, &error))
        {
          /* NOT_INSTALLED error is expected in case the op that triggered this was install not update */
          if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED))
            g_clear_error (&error);
          else
            {
              g_printerr (_("Failed to uninstall %s for rebase to %s: %s\n"),
                          flatpak_ref_get_name (rref), rebased_to_ref, error->message);
              self->got_error = TRUE;
              return FALSE;
            }
        }

      return TRUE;
    }

  return FALSE;
}

static gboolean
flatpak_quiet_transaction_run (FlatpakTransaction *transaction,
                               GCancellable       *cancellable,
                               GError            **error)
{
  FlatpakQuietTransaction *self = FLATPAK_QUIET_TRANSACTION (transaction);
  gboolean res;

  res = FLATPAK_TRANSACTION_CLASS (flatpak_quiet_transaction_parent_class)->run (transaction, cancellable, error);

  if (self->got_error)
    {
      g_clear_error (error);
      return FALSE; /* Don't report on stderr, we already reported */
    }

  if (!res)
    return FALSE;

  return TRUE;
}

static void
flatpak_quiet_transaction_init (FlatpakQuietTransaction *transaction)
{
}

static void
flatpak_quiet_transaction_class_init (FlatpakQuietTransactionClass *class)
{
  FlatpakTransactionClass *transaction_class = FLATPAK_TRANSACTION_CLASS (class);

  transaction_class->choose_remote_for_ref = choose_remote_for_ref;
  transaction_class->add_new_remote = add_new_remote;
  transaction_class->new_operation = new_operation;
  transaction_class->operation_error = operation_error;
  transaction_class->end_of_lifed_with_rebase = end_of_lifed_with_rebase;
  transaction_class->run = flatpak_quiet_transaction_run;
  transaction_class->install_authenticator = install_authenticator;
}

FlatpakTransaction *
flatpak_quiet_transaction_new (FlatpakDir *dir,
                               GError    **error)
{
  g_autoptr(FlatpakQuietTransaction) self = NULL;
  g_autoptr(FlatpakInstallation) installation = NULL;

  installation = flatpak_installation_new_for_dir (dir, NULL, error);
  if (installation == NULL)
    return NULL;

  self = g_initable_new (FLATPAK_TYPE_QUIET_TRANSACTION,
                         NULL, error,
                         "installation", installation,
                         NULL);

  if (self == NULL)
    return NULL;

  flatpak_transaction_set_no_interaction (FLATPAK_TRANSACTION (self), TRUE);
  flatpak_transaction_add_default_dependency_sources (FLATPAK_TRANSACTION (self));

  return FLATPAK_TRANSACTION (g_steal_pointer (&self));
}
