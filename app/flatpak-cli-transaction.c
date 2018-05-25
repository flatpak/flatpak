/*
 * Copyright © 2018 Red Hat, Inc
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

#include "config.h"

#include "flatpak-cli-transaction.h"
#include "flatpak-transaction-private.h"
#include "flatpak-utils-private.h"
#include "flatpak-error.h"
#include <glib/gi18n.h>

typedef struct {
  FlatpakTransaction *transaction;
  gboolean disable_interaction;
  gboolean stop_on_first_error;
  GError *first_operation_error;
} FlatpakCliTransaction;

static int
choose_remote_for_ref (FlatpakTransaction *transaction,
                       const char *for_ref,
                       const char *runtime_ref,
                       const char * const *remotes,
                       gpointer data)
{
  FlatpakCliTransaction *cli = data;
  int n_remotes = g_strv_length ((char **)remotes);
  int chosen = -1;
  const char *pref;
  int i;

  pref = strchr (for_ref, '/') + 1;

  if (cli->disable_interaction)
    {
      g_print (_("Required runtime for %s (%s) found in remote %s\n"),
               pref, runtime_ref, remotes[0]);
      chosen = 0;
    }
  else if (n_remotes == 1)
    {
      g_print (_("Required runtime for %s (%s) found in remote %s\n"),
               pref, runtime_ref, remotes[0]);
      if (flatpak_yes_no_prompt (_("Do you want to install it?")))
        chosen = 0;
    }
  else
    {
      g_print (_("Required runtime for %s (%s) found in remotes: %s\n"),
               pref, runtime_ref, remotes[0]);
      for (i = 0; remotes[i] != NULL; i++)
        {
          g_print ("%d) %s\n", i + 1, remotes[i]);
        }
      chosen = flatpak_number_prompt (0, n_remotes, _("Which do you want to install (0 to abort)?"));
      chosen -= 1; /* convert from base-1 to base-0 (and -1 to abort) */
    }

  return chosen;
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
    default:
      return "Unknown type"; /* Should not happen */
    }
}

static gboolean
operation_error (FlatpakTransaction *transaction,
                 const char *ref,
                 FlatpakTransactionOperationType operation_type,
                 GError *error,
                 FlatpakTransactionError detail,
                 gpointer data)
{
  FlatpakCliTransaction *cli = data;
  const char *pref;

  pref = strchr (ref, '/') + 1;

  if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_SKIPPED))
    {
      g_printerr ("%s", error->message);
      return TRUE;
    }

  if (detail & FLATPAK_TRANSACTION_ERROR_NON_FATAL)
    {
      g_printerr (_("Warning: Failed to %s %s: %s\n"),
                  op_type_to_string (operation_type), pref, error->message);
    }
  else
    {
      if (cli->first_operation_error == NULL)
        g_propagate_prefixed_error (&cli->first_operation_error,
                                    g_error_copy (error),
                                    _("Failed to %s %s: "),
                                    op_type_to_string (operation_type), pref);

      if (cli->stop_on_first_error)
        return FALSE;

      g_printerr (_("Error: Failed to %s %s: %s\n"),
                  op_type_to_string (operation_type), pref, error->message);
    }

  return TRUE; /* Continue */
}

static void
end_of_lifed (FlatpakTransaction *transaction,
                 const char *ref,
                 const char *reason,
                 const char *rebase,
                 gpointer data)
{
  if (rebase)
    {
      g_printerr (_("Warning: %s is end-of-life, in preference of %s\n"), ref, rebase);
    }
  else if (reason)
    {
      g_printerr (_("Warning: %s is end-of-life, with reason: %s\n"), ref, reason);
    }
}

static void
flatpak_cli_transaction_free (FlatpakCliTransaction *cli)
{
  if (cli->first_operation_error)
    g_error_free (cli->first_operation_error);
  g_free (cli);
}

FlatpakTransaction *
flatpak_cli_transaction_new (FlatpakDir *dir,
                             gboolean disable_interaction,
                             gboolean stop_on_first_error)
{
  FlatpakTransaction *transaction = flatpak_transaction_new (dir);
  FlatpakCliTransaction *cli = g_new0 (FlatpakCliTransaction, 1);

  cli->transaction = transaction;
  cli->disable_interaction = disable_interaction;
  cli->stop_on_first_error = stop_on_first_error;
  g_object_set_data_full (G_OBJECT (transaction), "cli", cli, (GDestroyNotify)flatpak_cli_transaction_free);

  g_signal_connect (transaction, "choose-remote-for-ref", G_CALLBACK (choose_remote_for_ref), cli);
  g_signal_connect (transaction, "operation-error", G_CALLBACK (operation_error), cli);
  g_signal_connect (transaction, "end-of-lifed", G_CALLBACK (end_of_lifed), cli);

  return transaction;
}

gboolean
flatpak_cli_transaction_add_install (FlatpakTransaction *transaction,
                                     const char *remote,
                                     const char *ref,
                                     const char **subpaths,
                                     GError **error)
{
  g_autoptr(GError) local_error = NULL;

    if (!flatpak_transaction_add_install (transaction, remote, ref, subpaths, &local_error))
      {
        if (g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_ALREADY_INSTALLED))
          {
            g_printerr (_("Skipping: %s\n"), local_error->message);
            return TRUE;
          }

        g_propagate_error (error, g_steal_pointer (&local_error));
        return FALSE;
      }

    return TRUE;
}


gboolean
flatpak_cli_transaction_run (FlatpakTransaction *transaction,
                             GCancellable *cancellable,
                             GError **error)
{
  FlatpakCliTransaction *cli = g_object_get_data (G_OBJECT (transaction), "cli");
  g_autoptr(GError) local_error = NULL;
  gboolean res;

  res = flatpak_transaction_run (transaction, cancellable, &local_error);


  /* If we got some weird error (i.e. not ABORTED because we chose to abort
     on an error, report that */
  if (!res &&
      !g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  if (cli->first_operation_error)
    {
      /* We always want to return an error if there was some kind of operation error,
         as that causes the main CLI to return an error status. */

      if (cli->stop_on_first_error)
        {
          /* For the install/stop_on_first_error we return the first operation error,
             as we have not yet printed it.  */

          g_propagate_error (error, g_steal_pointer (&cli->first_operation_error));
          return FALSE;
        }
      else
        {
          /* For updates/!stop_on_first_error we already printed all errors so we make up
             a different one. */

          return flatpak_fail (error, _("There were one or more errors"));
        }
    }

  return TRUE;
}
