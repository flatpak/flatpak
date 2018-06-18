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
#include "flatpak-installation-private.h"
#include "flatpak-utils-private.h"
#include "flatpak-error.h"
#include <glib/gi18n.h>
#include <sys/ioctl.h>


struct _FlatpakCliTransaction {
  FlatpakTransaction parent;

  gboolean disable_interaction;
  gboolean stop_on_first_error;
  gboolean is_user;
  gboolean aborted;
  GError *first_operation_error;

  gboolean progress_initialized;
  int progress_n_columns;
  int progress_last_width;
};

struct _FlatpakCliTransactionClass {
  FlatpakCliTransactionClass parent_class;
};

G_DEFINE_TYPE (FlatpakCliTransaction, flatpak_cli_transaction, FLATPAK_TYPE_TRANSACTION);

static int
choose_remote_for_ref (FlatpakTransaction *transaction,
                       const char *for_ref,
                       const char *runtime_ref,
                       const char * const *remotes)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  int n_remotes = g_strv_length ((char **)remotes);
  int chosen = -1;
  const char *pref;
  int i;

  pref = strchr (for_ref, '/') + 1;

  if (self->disable_interaction)
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
    case FLATPAK_TRANSACTION_OPERATION_UNINSTALL:
      return _("uninstall");
    default:
      return "Unknown type"; /* Should not happen */
    }
}

#define BAR_LENGTH 20
#define BAR_CHARS " -=#"


static void
progress_changed_cb (FlatpakTransactionProgress *progress,
                     gpointer data)
{
  FlatpakCliTransaction *cli = data;
  g_autoptr(GString) str = g_string_new ("");
  int i;
  int n_full, remainder, partial;
  int width, padded_width;

  guint percent = flatpak_transaction_progress_get_progress (progress);
  g_autofree char *status = flatpak_transaction_progress_get_status (progress);

  if (!cli->progress_initialized)
    {
      struct winsize w;
      cli->progress_n_columns = 80;
      if (ioctl (STDOUT_FILENO, TIOCGWINSZ, &w) == 0)
        cli->progress_n_columns = w.ws_col;
      cli->progress_last_width = 0;
      cli->progress_initialized = TRUE;
    }

  g_string_append (str, "[");

  n_full = (BAR_LENGTH * percent) / 100;
  remainder = percent - (n_full * 100 / BAR_LENGTH);
  partial = (remainder * strlen(BAR_CHARS) * BAR_LENGTH) / 100;

  for (i = 0; i < n_full; i++)
    g_string_append_c (str, BAR_CHARS[strlen(BAR_CHARS)-1]);

  if (i < BAR_LENGTH)
    {
      g_string_append_c (str, BAR_CHARS[partial]);
      i++;
    }

  for (; i < BAR_LENGTH; i++)
    g_string_append (str, " ");

  g_string_append (str, "] ");
  g_string_append (str, status);

  g_print ("\r");
  width = MIN (strlen (str->str), cli->progress_n_columns);
  padded_width = MAX (cli->progress_last_width, width);
  cli->progress_last_width = width;
  g_print ("%-*.*s", padded_width, padded_width, str->str);
}

static void
progress_done (FlatpakTransaction *transaction)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);

  if (self->progress_initialized)
    g_print("\n");
}

static void
new_operation (FlatpakTransaction *transaction,
               const char *ref,
               const char *remote,
               const char *bundle_path,
               FlatpakTransactionOperationType operation_type,
               FlatpakTransactionProgress *progress)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  const char *pref;
  g_autofree char *bundle_basename = NULL;

  pref = strchr (ref, '/') + 1;

  switch (operation_type)
    {
    case FLATPAK_TRANSACTION_OPERATION_INSTALL:
      if (self->is_user)
        g_print (_("Installing for user: %s from %s\n"), pref, remote);
      else
        g_print (_("Installing: %s from %s\n"), pref, remote);
      break;
    case FLATPAK_TRANSACTION_OPERATION_UPDATE:
      if (self->is_user)
        g_print (_("Updating for user: %s from %s\n"), pref, remote);
      else
        g_print (_("Updating: %s from %s\n"), pref, remote);
      break;
    case FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE:
      {
        bundle_basename = g_path_get_basename (bundle_path);
        if (self->is_user)
          g_print (_("Installing for user: %s from bundle %s\n"), pref, bundle_basename);
        else
          g_print (_("Installing: %s from bundle %s\n"), pref, bundle_basename);
      }
      break;
    case FLATPAK_TRANSACTION_OPERATION_UNINSTALL:
      if (self->is_user)
        g_print (_("Uninstalling for user: %s\n"), pref);
      else
        g_print (_("Uninstalling: %s\n"), pref);
      break;
    default:
      g_assert_not_reached ();
      break;
    }

  self->progress_initialized = FALSE;
  g_signal_connect (progress, "changed", G_CALLBACK (progress_changed_cb), self);
  flatpak_transaction_progress_set_update_frequency (progress, FLATPAK_CLI_UPDATE_FREQUENCY);

}

static void
operation_done (FlatpakTransaction *transaction,
                const char *ref,
                const char *remote,
                FlatpakTransactionOperationType operation_type,
                const char *commit,
                FlatpakTransactionResult details)
{
  g_autofree char *short_commit = g_strndup (commit, 12);

  progress_done (transaction);

  if (operation_type != FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
    {
      if (details & FLATPAK_TRANSACTION_RESULT_NO_CHANGE)
        g_print (_("No updates.\n"));
      else
        g_print (_("Now at %s.\n"), short_commit);
    }
}

static gboolean
operation_error (FlatpakTransaction *transaction,
                 const char *ref,
                 const char *remote,
                 FlatpakTransactionOperationType operation_type,
                 GError *error,
                 FlatpakTransactionErrorDetails detail)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  const char *pref;

  progress_done (transaction);

  pref = strchr (ref, '/') + 1;

  if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_SKIPPED))
    {
      g_printerr ("%s", error->message);
      return TRUE;
    }

  if (detail & FLATPAK_TRANSACTION_ERROR_DETAILS_NON_FATAL)
    {
      g_printerr (_("Warning: Failed to %s %s: %s\n"),
                  op_type_to_string (operation_type), pref, error->message);
    }
  else
    {
      if (self->first_operation_error == NULL)
        g_propagate_prefixed_error (&self->first_operation_error,
                                    g_error_copy (error),
                                    _("Failed to %s %s: "),
                                    op_type_to_string (operation_type), pref);

      if (self->stop_on_first_error)
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
                 const char *rebase)
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
flatpak_cli_transaction_finalize (GObject *object)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (object);

  if (self->first_operation_error)
    g_error_free (self->first_operation_error);

  G_OBJECT_CLASS (flatpak_cli_transaction_parent_class)->finalize (object);
}

static void
flatpak_cli_transaction_init (FlatpakCliTransaction *self)
{
}

static void
flatpak_cli_transaction_class_init (FlatpakCliTransactionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FlatpakTransactionClass *transaction_class = FLATPAK_TRANSACTION_CLASS (klass);

  object_class->finalize = flatpak_cli_transaction_finalize;
  transaction_class->new_operation = new_operation;
  transaction_class->operation_done = operation_done;
  transaction_class->operation_error = operation_error;
  transaction_class->choose_remote_for_ref = choose_remote_for_ref;
  transaction_class->end_of_lifed = end_of_lifed;
}

FlatpakTransaction *
flatpak_cli_transaction_new (FlatpakDir *dir,
                             gboolean disable_interaction,
                             gboolean stop_on_first_error,
                             GError **error)
{
  g_autoptr(FlatpakInstallation) installation = NULL;
  g_autoptr(FlatpakCliTransaction) self = NULL;

  installation = flatpak_installation_new_for_dir (dir, NULL, error);
  if (installation == NULL)
    return NULL;

  self = g_initable_new (FLATPAK_TYPE_CLI_TRANSACTION,
                         NULL, error,
                         "installation", installation,
                         NULL);
  if (self == NULL)
    return NULL;

  self->disable_interaction = disable_interaction;
  self->stop_on_first_error = stop_on_first_error;
  self->is_user = flatpak_dir_is_user (dir);

  return (FlatpakTransaction *)g_steal_pointer (&self);
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
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  g_autoptr(GError) local_error = NULL;
  gboolean res;

  res = flatpak_transaction_run (transaction, cancellable, &local_error);


  /* If we got some weird error (i.e. not ABORTED because we chose to abort
     on an error, report that */
  if (!res)
    {
      if (g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED))
        {
          self->aborted = TRUE;
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
    }

  if (self->first_operation_error)
    {
      /* We always want to return an error if there was some kind of operation error,
         as that causes the main CLI to return an error status. */

      if (self->stop_on_first_error)
        {
          /* For the install/stop_on_first_error we return the first operation error,
             as we have not yet printed it.  */

          g_propagate_error (error, g_steal_pointer (&self->first_operation_error));
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

gboolean
flatpak_cli_transaction_was_aborted (FlatpakTransaction *transaction)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);

  return self->aborted;
}
