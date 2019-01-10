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
}

FlatpakTransaction *
flatpak_quiet_transaction_new (FlatpakDir  *dir,
                               GError     **error)
{
  g_autoptr(FlatpakQuietTransaction) self = NULL;
  g_autoptr(FlatpakInstallation) installation = NULL;

  installation = flatpak_installation_new_for_dir (dir, NULL, error);
  if (installation == NULL)
    return NULL;

  flatpak_installation_set_no_interaction (installation, TRUE);

  self = g_initable_new (FLATPAK_TYPE_QUIET_TRANSACTION,
                         NULL, error,
                         "installation", installation,
                         NULL);

  if (self == NULL)
    return NULL;

  flatpak_transaction_add_default_dependency_sources (FLATPAK_TRANSACTION (self));

  return FLATPAK_TRANSACTION (g_steal_pointer (&self));
}
