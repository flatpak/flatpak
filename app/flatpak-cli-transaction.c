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

static void
flatpak_cli_transaction_free (FlatpakCliTransaction *cli)
{
  g_free (cli);
}

FlatpakTransaction *
flatpak_cli_transaction_new (FlatpakDir *dir,
                             gboolean disable_interaction)
{
  FlatpakTransaction *transaction = flatpak_transaction_new (dir);
  FlatpakCliTransaction *cli = g_new0 (FlatpakCliTransaction, 1);

  cli->transaction = transaction;
  cli->disable_interaction = disable_interaction;
  g_object_set_data_full (G_OBJECT (transaction), "cli", cli, (GDestroyNotify)flatpak_cli_transaction_free);

  g_signal_connect (transaction, "choose-remote-for-ref", G_CALLBACK (choose_remote_for_ref), cli);

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
