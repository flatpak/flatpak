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
#include "flatpak-run-private.h"
#include "flatpak-table-printer.h"
#include "flatpak-utils-private.h"
#include "flatpak-error.h"
#include <glib/gi18n.h>


struct _FlatpakCliTransaction
{
  FlatpakTransaction   parent;

  gboolean             disable_interaction;
  gboolean             stop_on_first_error;
  gboolean             non_default_arch;
  GError              *first_operation_error;

  GHashTable          *eol_actions;

  int                  rows;
  int                  cols;
  int                  table_width;
  int                  table_height;

  int                  n_ops;
  int                  op;
  int                  op_progress;

  gboolean             installing;
  gboolean             updating;
  gboolean             uninstalling;

  int                  download_col;

  FlatpakTablePrinter *printer;
  int                  progress_row;
  char                *progress_msg;
  int                  speed_len;

  gboolean             did_interaction;
};

struct _FlatpakCliTransactionClass
{
  FlatpakTransactionClass parent_class;
};

G_DEFINE_TYPE (FlatpakCliTransaction, flatpak_cli_transaction, FLATPAK_TYPE_TRANSACTION);

static int
choose_remote_for_ref (FlatpakTransaction *transaction,
                       const char         *for_ref,
                       const char         *runtime_ref,
                       const char * const *remotes)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  int n_remotes = g_strv_length ((char **) remotes);
  int chosen = -1;
  const char *pref;

  pref = strchr (for_ref, '/') + 1;

  self->did_interaction = TRUE;

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
      if (flatpak_yes_no_prompt (TRUE, _("Do you want to install it?")))
        chosen = 0;
    }
  else
    {
      flatpak_format_choices ((const char **) remotes,
                              _("Required runtime for %s (%s) found in remotes:"),
                              pref, runtime_ref);
      chosen = flatpak_number_prompt (TRUE, 0, n_remotes, _("Which do you want to install (0 to abort)?"));
      chosen -= 1; /* convert from base-1 to base-0 (and -1 to abort) */
    }

  return chosen;
}

static gboolean
add_new_remote (FlatpakTransaction            *transaction,
                FlatpakTransactionRemoteReason reason,
                const char                    *from_id,
                const char                    *remote_name,
                const char                    *url)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);

  self->did_interaction = TRUE;

  if (self->disable_interaction)
    {
      g_print (_("Configuring %s as new remote '%s'"), url, remote_name);
      return TRUE;
    }

  if (reason == FLATPAK_TRANSACTION_REMOTE_GENERIC_REPO)
    {
      if (flatpak_yes_no_prompt (TRUE, /* default to yes on Enter */
                                 _("The remote '%s', referred to by '%s' at location %s contains additional applications.\n"
                                   "Should the remote be kept for future installations?"),
                                 remote_name, from_id, url))
        return TRUE;
    }
  else if (reason == FLATPAK_TRANSACTION_REMOTE_RUNTIME_DEPS)
    {
      if (flatpak_yes_no_prompt (TRUE, /* default to yes on Enter */
                                 _("The application %s depends on runtimes from:\n  %s\n"
                                   "Configure this as new remote '%s'"),
                                 from_id, url, remote_name))
        return TRUE;
    }

  return FALSE;
}

static void
install_authenticator (FlatpakTransaction            *old_transaction,
                       const char                    *remote,
                       const char                    *ref)
{
  FlatpakCliTransaction *old_cli = FLATPAK_CLI_TRANSACTION (old_transaction);
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

  old_cli->did_interaction = TRUE;

  transaction2 = flatpak_cli_transaction_new (dir, old_cli->disable_interaction, TRUE, FALSE, &local_error);
  if (transaction2 == NULL)
    {
      g_printerr ("Unable to install authenticator: %s\n", local_error->message);
      return;
    }

  g_print ("Installing required authenticator for remote %s\n", remote);
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
redraw (FlatpakCliTransaction *self)
{
  int top;
  int row;
  int current_row;
  int current_col;
  int skip;

  /* We may have resized and thus repositioned the cursor since last redraw */
  flatpak_get_window_size (&self->rows, &self->cols);
  if (flatpak_get_cursor_pos (&current_row, &current_col))
    {
      /* We're currently displaying the last row of the table, extept the
         very first time where the user pressed return for the prompt causing us
         to scroll down one extra row */
      top = current_row - self->table_height + 1;
      if (top > 0)
        {
          row = top;
          skip = 0;
        }
      else
        {
          row = 1;
          skip = 1 - top;
        }

      g_print (FLATPAK_ANSI_ROW_N FLATPAK_ANSI_CLEAR, row);
      // we update table_height and end_row here, since we might have added to the table
      flatpak_table_printer_print_full (self->printer, skip, self->cols,
                                        &self->table_height, &self->table_width);
      return TRUE;
    }
  return FALSE;
}

static void
set_op_progress (FlatpakCliTransaction       *self,
                 FlatpakTransactionOperation *op,
                 const char                  *progress)
{
  if (flatpak_fancy_output ())
    {
      int row = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (op), "row"));
      g_autofree char *cell = g_strdup_printf ("[%s]", progress);
      flatpak_table_printer_set_cell (self->printer, row, 1, cell);
    }
}

static void
spin_op_progress (FlatpakCliTransaction       *self,
                  FlatpakTransactionOperation *op)
{
  const char *p[] = {
    "|",
    "/",
    "—",
    "\\",
  };

  set_op_progress (self, op, p[self->op_progress++ % G_N_ELEMENTS (p)]);
}

static char *
format_duration (guint64 duration)
{
  int h, m, s;

  m = duration / 60;
  s = duration % 60;
  h = m / 60;
  m = m % 60;

  if (h > 0)
    return g_strdup_printf ("%02d:%02d:%02d", h, m, s);
  else
    return g_strdup_printf ("%02d:%02d", m, s);
}

static void
progress_changed_cb (FlatpakTransactionProgress *progress,
                     gpointer                    data)
{
  FlatpakCliTransaction *cli = data;
  FlatpakTransaction *self = FLATPAK_TRANSACTION (cli);
  g_autoptr(FlatpakTransactionOperation) op = flatpak_transaction_get_current_operation (self);
  g_autoptr(GString) str = g_string_new ("");
  int i;
  int n_full, partial;
  g_autofree char *speed = NULL;
  int bar_length;
  const char *partial_blocks[] = {
    " ",
    "▏",
    "▎",
    "▍",
    "▌",
    "▋",
    "▊",
    "▉",
  };
  const char *full_block = "█";

  guint percent = flatpak_transaction_progress_get_progress (progress);
  guint64 start_time = flatpak_transaction_progress_get_start_time (progress);
  guint64 elapsed_time = (g_get_monotonic_time () - start_time) / G_USEC_PER_SEC;
  guint64 transferred = flatpak_transaction_progress_get_bytes_transferred (progress);
  guint64 max = flatpak_transaction_operation_get_download_size (op);

  if (elapsed_time > 0)
    {
      g_autofree char *formatted_bytes_sec = g_format_size (transferred / elapsed_time);
      g_autofree char *remaining = NULL;
      if (elapsed_time > 3 && percent > 0)
        {
          guint64 total_time = elapsed_time * 100 / (double) percent;
          remaining = format_duration (total_time - elapsed_time);
        }
      speed = g_strdup_printf ("%s/s%s%s", formatted_bytes_sec, remaining ? "  " : "", remaining ? remaining : "");
      cli->speed_len = MAX (cli->speed_len, strlen (speed) + 2);
    }

  spin_op_progress (cli, op);

  bar_length = MIN (20, cli->table_width - (strlen (cli->progress_msg) + 6 + cli->speed_len));

  n_full = (bar_length * percent) / 100;
  partial = (((bar_length * percent) % 100) * G_N_ELEMENTS (partial_blocks)) / 100;
  /* The above should guarantee this: */
  g_assert (partial >= 0);
  g_assert (partial < G_N_ELEMENTS (partial_blocks));

  g_string_append (str, cli->progress_msg);
  g_string_append (str, " ");

  if (flatpak_fancy_output ())
    g_string_append (str, FLATPAK_ANSI_FAINT_ON);

  for (i = 0; i < n_full; i++)
    g_string_append (str, full_block);

  if (i < bar_length)
    {
      g_string_append (str, partial_blocks[partial]);
      i++;
    }

  if (flatpak_fancy_output ())
    g_string_append (str, FLATPAK_ANSI_FAINT_OFF);

  for (; i < bar_length; i++)
    g_string_append (str, " ");

  g_string_append (str, " ");
  g_string_append_printf (str, "%3d%%", percent);

  if (speed)
    g_string_append_printf (str, "  %s", speed);

  if (flatpak_fancy_output ())
    {
      flatpak_table_printer_set_cell (cli->printer, cli->progress_row, 0, str->str);
      if (flatpak_transaction_operation_get_operation_type (op) != FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
        {
          g_autofree char *formatted_max = NULL;
          g_autofree char *formatted = NULL;
          g_autofree char *text = NULL;
          int row;

          // avoid "bytes"
          formatted = transferred < 1000 ? g_format_size (1000) : g_format_size (transferred);
          formatted_max = max < 1000 ? g_format_size (1000) : g_format_size (max);

          text = g_strdup_printf ("%s / %s", formatted, formatted_max);
          row = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (op), "row"));
          flatpak_table_printer_set_decimal_cell (cli->printer, row, cli->download_col, text);
        }
      if (!redraw (cli))
        g_print ("\r%s", str->str); /* redraw failed, just update the progress */
    }
  else
    g_print ("\n%s", str->str);
}

static void
set_progress (FlatpakCliTransaction *self,
              const char            *text)
{
  flatpak_table_printer_set_cell (self->printer, self->progress_row, 0, text);
}

static void
new_operation (FlatpakTransaction          *transaction,
               FlatpakTransactionOperation *op,
               FlatpakTransactionProgress  *progress)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  FlatpakTransactionOperationType op_type = flatpak_transaction_operation_get_operation_type (op);
  g_autofree char *text = NULL;

  self->op++;
  self->op_progress = 0;

  switch (op_type)
    {
    case FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE:
    case FLATPAK_TRANSACTION_OPERATION_INSTALL:
      if (self->n_ops == 1)
        text = g_strdup (_("Installing…"));
      else
        text = g_strdup_printf (_("Installing %d/%d…"), self->op, self->n_ops);
      break;

    case FLATPAK_TRANSACTION_OPERATION_UPDATE:
      if (self->n_ops == 1)
        text = g_strdup (_("Updating…"));
      else
        text = g_strdup_printf (_("Updating %d/%d…"), self->op, self->n_ops);
      break;

    case FLATPAK_TRANSACTION_OPERATION_UNINSTALL:
      if (self->n_ops == 1)
        text = g_strdup (_("Uninstalling…"));
      else
        text = g_strdup_printf (_("Uninstalling %d/%d…"), self->op, self->n_ops);
      break;

    default:
      g_assert_not_reached ();
      break;
    }

  if (flatpak_fancy_output ())
    {
      set_progress (self, text);
      spin_op_progress (self, op);
      redraw (self);
    }
  else
    g_print ("\r%-*s", self->table_width, text);

  g_free (self->progress_msg);
  self->progress_msg = g_steal_pointer (&text);

  g_signal_connect (progress, "changed", G_CALLBACK (progress_changed_cb), self);
  flatpak_transaction_progress_set_update_frequency (progress, FLATPAK_CLI_UPDATE_INTERVAL_MS);
}

static void
operation_done (FlatpakTransaction          *transaction,
                FlatpakTransactionOperation *op,
                const char                  *commit,
                FlatpakTransactionResult     details)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  FlatpakTransactionOperationType op_type = flatpak_transaction_operation_get_operation_type (op);

  if (op_type == FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
    set_op_progress (self, op, FLATPAK_ANSI_GREEN "-" FLATPAK_ANSI_COLOR_RESET);
  else
    set_op_progress (self, op, FLATPAK_ANSI_GREEN "✓" FLATPAK_ANSI_COLOR_RESET);

  if (flatpak_fancy_output ())
    redraw (self);
}

static gboolean
operation_error (FlatpakTransaction            *transaction,
                 FlatpakTransactionOperation   *op,
                 const GError                  *error,
                 FlatpakTransactionErrorDetails detail)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  FlatpakTransactionOperationType op_type = flatpak_transaction_operation_get_operation_type (op);
  const char *ref = flatpak_transaction_operation_get_ref (op);
  g_autoptr(FlatpakRef) rref = flatpak_ref_parse (ref, NULL);
  g_autofree char *msg = NULL;
  gboolean non_fatal = (detail & FLATPAK_TRANSACTION_ERROR_DETAILS_NON_FATAL) != 0;
  g_autofree char *text = NULL;

  if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_SKIPPED))
    {
      set_op_progress (self, op, "⍻");
      msg = g_strdup_printf (_("Info: %s was skipped"), flatpak_ref_get_name (rref));
      if (flatpak_fancy_output ())
        {
          flatpak_table_printer_set_cell (self->printer, self->progress_row, 0, msg);
          self->progress_row++;
          flatpak_table_printer_add_span (self->printer, "");
          flatpak_table_printer_finish_row (self->printer);
          redraw (self);
        }
      else
        g_print ("\r%-*s\n", self->table_width, msg); /* override progress, and go to next line */

      return TRUE;
    }

  set_op_progress (self, op, "✗");

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
  else if (error)
    msg = g_strdup (error->message);
  else
    msg = g_strdup (_("(internal error, please report)"));

  if (!non_fatal && self->first_operation_error == NULL)
    g_propagate_prefixed_error (&self->first_operation_error,
                                g_error_copy (error),
                                _("Failed to %s %s: "),
                                op_type_to_string (op_type), flatpak_ref_get_name (rref));

  text = g_strconcat (non_fatal ? _("Warning:") : _("Error:"), " ", msg, NULL);

  if (flatpak_fancy_output ())
    {
      flatpak_table_printer_set_cell (self->printer, self->progress_row, 0, text);
      self->progress_row++;
      flatpak_table_printer_add_span (self->printer, "");
      flatpak_table_printer_finish_row (self->printer);
      redraw (self);
    }
  else
    g_printerr ("\r%-*s\n", self->table_width, text);

  if (!non_fatal && self->stop_on_first_error)
    return FALSE;

  return TRUE; /* Continue */
}

static gboolean
webflow_start (FlatpakTransaction *transaction,
               const char         *remote,
               const char         *url,
               GVariant           *options,
               guint               id)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  const char *browser;
  g_autoptr(GError) local_error = NULL;
  const char *args[3] = { NULL, url, NULL };

  self->did_interaction = TRUE;

  if (!self->disable_interaction)
    {
      g_print (_("Authentication required for remote '%s'\n"), remote);
      if (!flatpak_yes_no_prompt (TRUE, _("Open browser?")))
        return FALSE;
    }

  /* Allow hard overrides with $BROWSER */
  browser = g_getenv ("BROWSER");
  if (browser != NULL)
    {
      args[0] = browser;
      if (!g_spawn_async (NULL, (char **)args, NULL, G_SPAWN_SEARCH_PATH,
                          NULL, NULL, NULL, &local_error))
        {
          g_printerr ("Failed to start browser %s: %s\n", browser, local_error->message);
          return FALSE;
        }
    }
  else
    {
      if (!g_app_info_launch_default_for_uri (url, NULL, &local_error))
        {
          g_printerr ("Failed to show url: %s\n", local_error->message);
          return FALSE;
        }
    }

  g_print ("Waiting for browser...\n");

  return TRUE;
}

static void
webflow_done (FlatpakTransaction *transaction,
              GVariant           *options,
              guint               id)
{
  g_print ("Browser done\n");
}

static gboolean
basic_auth_start (FlatpakTransaction *transaction,
                  const char         *remote,
                  const char         *realm,
                  GVariant           *options,
                  guint               id)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  char *user, *password, *previous_error = NULL;

  if (self->disable_interaction)
    return FALSE;

  self->did_interaction = TRUE;

  if (g_variant_lookup (options, "previous-error", "&s", &previous_error))
    g_print ("%s\n", previous_error);

  g_print (_("Login required remote %s (realm %s)\n"), remote, realm);
  user = flatpak_prompt (FALSE, _("User"));
  if (user == NULL)
    return FALSE;

  password = flatpak_password_prompt (_("Password"));
  if (password == NULL)
    return FALSE;

  flatpak_transaction_complete_basic_auth (transaction, id, user, password, NULL);
  return TRUE;
}


typedef enum {
  EOL_UNDECIDED,
  EOL_IGNORE,        /* Don't do anything, we already printed a warning */
  EOL_NO_REBASE,     /* Choose to not rebase */
  EOL_REBASE,        /* Choose to rebase */
} EolAction;

static gboolean
end_of_lifed_with_rebase (FlatpakTransaction *transaction,
                          const char         *remote,
                          const char         *ref_str,
                          const char         *reason,
                          const char         *rebased_to_ref,
                          const char        **previous_ids)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  g_autoptr(FlatpakDecomposed) ref = flatpak_decomposed_new_from_ref (ref_str, NULL);
  g_autofree char *name = NULL;
  EolAction action = EOL_UNDECIDED;
  EolAction old_action = EOL_UNDECIDED;
  gboolean can_rebase = rebased_to_ref != NULL && remote != NULL;
  FlatpakInstallation *installation = flatpak_transaction_get_installation (transaction);
  FlatpakDir *dir = flatpak_installation_get_dir (installation, NULL);

  if (ref == NULL)
    return FALSE; /* Shouldn't happen, the ref should be valid */

  name = flatpak_decomposed_dup_id (ref);

  self->did_interaction = TRUE;

  if (flatpak_decomposed_id_is_subref (ref))
    {
      GLNX_HASH_TABLE_FOREACH_KV (self->eol_actions, FlatpakDecomposed *, eoled_ref, gpointer, value)
        {
          guint old_eol_action = GPOINTER_TO_UINT (value);

          if (flatpak_decomposed_id_is_subref_of (ref, eoled_ref))
              {
                old_action = old_eol_action; /* Do the same */
                break;
              }
        }
    }

  if (old_action != EOL_UNDECIDED)
    {
      switch (old_action)
        {
        default:
        case EOL_IGNORE:
          if (!can_rebase)
            action = EOL_IGNORE;
          /* Else, ask if we want to rebase */
          break;
        case EOL_REBASE:
        case EOL_NO_REBASE:
          if (can_rebase)
            action = old_action;
          else
            action = EOL_IGNORE;
        }
    }

  if (action == EOL_UNDECIDED)
    {
      gboolean is_pinned = flatpak_dir_ref_is_pinned (dir, flatpak_decomposed_get_ref (ref));
      g_autofree char *branch = flatpak_decomposed_dup_branch (ref);
      action = EOL_IGNORE;

      if (rebased_to_ref)
        if (is_pinned)
          g_print (_("Info: (pinned) %s//%s is end-of-life, in favor of %s\n"), name, branch, rebased_to_ref);
        else
          g_print (_("Info: %s//%s is end-of-life, in favor of %s\n"), name, branch, rebased_to_ref);
      else if (reason)
        {
          if (is_pinned)
            g_print (_("Info: (pinned) %s//%s is end-of-life, with reason:\n"), name, branch);
          else
            g_print (_("Info: %s//%s is end-of-life, with reason:\n"), name, branch);
          g_print ("   %s\n", reason);
        }

      if (flatpak_decomposed_is_runtime (ref))
        {
          g_autoptr(GPtrArray) apps = flatpak_dir_list_app_refs_with_runtime (dir, ref, NULL, NULL);
          if (apps && apps->len > 0)
            {
              g_print (_("Applications using this runtime:\n"));
              g_print ("   ");
              for (int i = 0; i < apps->len; i++)
                {
                  FlatpakDecomposed *app_ref = g_ptr_array_index (apps, i);
                  g_autofree char *id = flatpak_decomposed_dup_id (app_ref);
                  if (i != 0)
                    g_print (", ");
                  g_print ("%s", id);
                }
              g_print ("\n");
            }
        }

      if (rebased_to_ref && remote)
        {
          if (self->disable_interaction ||
              flatpak_yes_no_prompt (TRUE, _("Replace it with %s?"), rebased_to_ref))
            {
              if (self->disable_interaction)
                g_print (_("Updating to rebased version\n"));

              action = EOL_REBASE;
            }
          else
            action = EOL_NO_REBASE;
        }
    }
  else
    {
        g_debug ("%s is end-of-life, using action from parent ren", name);
    }

  /* Cache for later comparison and reuse */
  g_hash_table_insert (self->eol_actions, flatpak_decomposed_ref (ref), GUINT_TO_POINTER (action));

  if (action == EOL_REBASE)
    {
      g_autoptr(GError) error = NULL;

      if (!flatpak_transaction_add_rebase (transaction, remote, rebased_to_ref, NULL, previous_ids, &error))
        {
          g_propagate_prefixed_error (&self->first_operation_error,
                                      g_error_copy (error),
                                      _("Failed to rebase %s to %s: "),
                                      name, rebased_to_ref);
          return FALSE;
        }

      if (!flatpak_transaction_add_uninstall (transaction, ref_str, &error))
        {
          /* NOT_INSTALLED error is expected in case the op that triggered this was install not update */
          if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED))
            g_clear_error (&error);
          else
            {
              g_propagate_prefixed_error (&self->first_operation_error,
                                          g_error_copy (error),
                                          _("Failed to uninstall %s for rebase to %s: "),
                                          name, rebased_to_ref);
              return FALSE;
            }
        }

      return TRUE; /* skip install/update op of end-of-life ref */
    }
  else /* IGNORE or NO_REBASE */
    return FALSE;
}

static int
cmpstringp (const void *p1, const void *p2)
{
  return strcmp (*(char * const *) p1, *(char * const *) p2);
}

static void
append_permissions (GPtrArray  *permissions,
                    GKeyFile   *metadata,
                    GKeyFile   *old_metadata,
                    const char *group)
{
  g_auto(GStrv) options = g_key_file_get_string_list (metadata, FLATPAK_METADATA_GROUP_CONTEXT, group, NULL, NULL);
  g_auto(GStrv) old_options = NULL;
  int i;

  if (options == NULL)
    return;

  qsort (options, g_strv_length (options), sizeof (const char *), cmpstringp);

  if (old_metadata)
    old_options = g_key_file_get_string_list (old_metadata, FLATPAK_METADATA_GROUP_CONTEXT, group, NULL, NULL);

  for (i = 0; options[i] != NULL; i++)
    {
      const char *option = options[i];
      if (option[0] == '!')
        continue;

      if (old_options && g_strv_contains ((const char * const *) old_options, option))
        continue;

      if (strcmp (group, FLATPAK_METADATA_KEY_DEVICES) == 0 && strcmp (option, "all") == 0)
        option = "devices";

      g_ptr_array_add (permissions, g_strdup (option));
    }
}

static void
append_bus (GPtrArray  *talk,
            GPtrArray  *own,
            GKeyFile   *metadata,
            GKeyFile   *old_metadata,
            const char *group)
{
  g_auto(GStrv) keys = NULL;
  gsize i, keys_count;

  keys = g_key_file_get_keys (metadata, group, &keys_count, NULL);
  if (keys == NULL)
    return;

  qsort (keys, g_strv_length (keys), sizeof (const char *), cmpstringp);

  for (i = 0; i < keys_count; i++)
    {
      const char *key = keys[i];
      g_autofree char *value = g_key_file_get_string (metadata, group, key, NULL);

      if (g_strcmp0 (value, "none") == 0)
        continue;

      if (old_metadata)
        {
          g_autofree char *old_value = g_key_file_get_string (old_metadata, group, key, NULL);
          if (g_strcmp0 (old_value, value) == 0)
            continue;
        }

      if (g_strcmp0 (value, "own") == 0)
        g_ptr_array_add (own, g_strdup (key));
      else
        g_ptr_array_add (talk, g_strdup (key));
    }
}

static void
append_tags (GPtrArray *tags_array,
             GKeyFile  *metadata,
             GKeyFile  *old_metadata)
{
  gsize i, size = 0;
  g_auto(GStrv) tags = g_key_file_get_string_list (metadata, FLATPAK_METADATA_GROUP_APPLICATION, "tags",
                                                   &size, NULL);
  g_auto(GStrv) old_tags = NULL;

  if (old_metadata)
    old_tags = g_key_file_get_string_list (old_metadata, FLATPAK_METADATA_GROUP_APPLICATION, "tags",
                                           NULL, NULL);

  for (i = 0; i < size; i++)
    {
      const char *tag = tags[i];
      if (old_tags == NULL || !g_strv_contains ((const char * const *) old_tags, tag))
        g_ptr_array_add (tags_array, g_strdup (tag));
    }
}

static void
print_perm_line (int        idx,
                 GPtrArray *items,
                 int        cols)
{
  g_autoptr(GString) res = g_string_new (NULL);
  int i;

  g_string_append_printf (res, "    [%d] %s", idx, (char *) items->pdata[0]);

  for (i = 1; i < items->len; i++)
    {
      char *p;
      int len;

      p = strrchr (res->str, '\n');
      if (!p)
        p = res->str;

      len = (res->str + strlen (res->str)) - p;
      if (len + strlen ((char *) items->pdata[i]) + 2 >= cols)
        g_string_append_printf (res, ",\n        %s", (char *) items->pdata[i]);
      else
        g_string_append_printf (res, ", %s", (char *) items->pdata[i]);
    }

  g_print ("%s\n", res->str);
}

static void
print_permissions (FlatpakCliTransaction *self,
                   const char            *ref,
                   GKeyFile              *metadata,
                   GKeyFile              *old_metadata)
{
  g_autoptr(FlatpakRef) rref = flatpak_ref_parse (ref, NULL);
  g_autoptr(GPtrArray) permissions = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) session_bus_talk = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) session_bus_own = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) system_bus_talk = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) system_bus_own = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) tags = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(FlatpakTablePrinter) printer = NULL;
  int max_permission_width;
  int n_permission_cols;
  int i, j;
  int rows, cols;
  int table_rows, table_cols;

  if (metadata == NULL)
    return;

  /* Only apps have permissions */
  if (flatpak_ref_get_kind (rref) != FLATPAK_REF_KIND_APP)
    return;

  append_permissions (permissions, metadata, old_metadata, FLATPAK_METADATA_KEY_SHARED);
  append_permissions (permissions, metadata, old_metadata, FLATPAK_METADATA_KEY_SOCKETS);
  append_permissions (permissions, metadata, old_metadata, FLATPAK_METADATA_KEY_DEVICES);
  append_permissions (permissions, metadata, old_metadata, FLATPAK_METADATA_KEY_FEATURES);
  append_permissions (files, metadata, old_metadata, FLATPAK_METADATA_KEY_FILESYSTEMS);
  append_bus (session_bus_talk, session_bus_own,
              metadata, old_metadata, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY);
  append_bus (system_bus_talk, system_bus_own,
              metadata, old_metadata, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY);
  append_tags (tags, metadata, old_metadata);

  j = 1;
  if (files->len > 0)
    g_ptr_array_add (permissions, g_strdup_printf ("file access [%d]", j++));
  if (session_bus_talk->len > 0)
    g_ptr_array_add (permissions, g_strdup_printf ("dbus access [%d]", j++));
  if (session_bus_own->len > 0)
    g_ptr_array_add (permissions, g_strdup_printf ("bus ownership [%d]", j++));
  if (system_bus_talk->len > 0)
    g_ptr_array_add (permissions, g_strdup_printf ("system dbus access [%d]", j++));
  if (system_bus_own->len > 0)
    g_ptr_array_add (permissions, g_strdup_printf ("system bus ownership [%d]", j++));
  if (tags->len > 0)
    g_ptr_array_add (permissions, g_strdup_printf ("tags [%d]", j++));

  /* Early exit if no (or no new) permissions */
  if (permissions->len == 0)
    return;

  g_print ("\n");

  if (old_metadata)
    g_print (_("New %s permissions:"), flatpak_ref_get_name (rref));
  else
    g_print (_("%s permissions:"), flatpak_ref_get_name (rref));

  g_print ("\n");

  flatpak_get_window_size (&rows, &cols);
  max_permission_width = 0;
  for (i = 0; i < permissions->len; i++)
    max_permission_width = MAX (max_permission_width, strlen (g_ptr_array_index (permissions, i)));

  /* At least 4 columns, but more if we're guaranteed to fit */
  n_permission_cols =  MAX (4, cols / (max_permission_width + 4));

  printer = flatpak_table_printer_new ();
  for (i = 0; i < permissions->len; i++)
    {
      char *perm = g_ptr_array_index (permissions, i);
      if (i % n_permission_cols == 0)
        {
          g_autofree char *text = NULL;

          if (i > 0)
            flatpak_table_printer_finish_row (printer);

          text = g_strdup_printf ("    %s", perm);
          flatpak_table_printer_add_column (printer, text);
        }
      else
        flatpak_table_printer_add_column (printer, perm);
    }
  flatpak_table_printer_finish_row (printer);

  for (i = 0; i < n_permission_cols; i++)
    flatpak_table_printer_set_column_expand (printer, i, TRUE);

  flatpak_table_printer_print_full (printer, 0, cols, &table_rows, &table_cols);

  g_print ("\n\n");

  j = 1;
  if (files->len > 0)
    print_perm_line (j++, files, cols);
  if (session_bus_talk->len > 0)
    print_perm_line (j++, session_bus_talk, cols);
  if (session_bus_own->len > 0)
    print_perm_line (j++, session_bus_own, cols);
  if (system_bus_talk->len > 0)
    print_perm_line (j++, system_bus_talk, cols);
  if (system_bus_own->len > 0)
    print_perm_line (j++, system_bus_own, cols);
  if (tags->len > 0)
    print_perm_line (j++, tags, cols);
}

static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (user_data);
  g_autofree char *text = NULL;

  text = g_strconcat (_("Warning: "), message, NULL);

  if (flatpak_fancy_output ())
    {
      flatpak_table_printer_set_cell (self->printer, self->progress_row, 0, text);
      self->progress_row++;
      flatpak_table_printer_add_span (self->printer, "");
      flatpak_table_printer_finish_row (self->printer);
      redraw (self);
    }
  else
    g_print ("\r%-*s\n", self->table_width, text);
}

static gboolean
transaction_ready_pre_auth (FlatpakTransaction *transaction)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  GList *ops = flatpak_transaction_get_operations (transaction);
  GList *l;
  int i;
  FlatpakTablePrinter *printer;
  const char *op_shorthand[] = { "i", "u", "i", "r" };

  if (ops == NULL)
    return TRUE;

  self->n_ops = g_list_length (ops);

  for (l = ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      FlatpakTransactionOperationType type = flatpak_transaction_operation_get_operation_type (op);

      switch (type)
        {
        case FLATPAK_TRANSACTION_OPERATION_UNINSTALL:
          self->uninstalling = TRUE;
          break;

        case FLATPAK_TRANSACTION_OPERATION_INSTALL:
        case FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE:
          self->installing = TRUE;
          break;

        case FLATPAK_TRANSACTION_OPERATION_UPDATE:
          self->updating = TRUE;
          break;

        default:;
        }
    }

  /* first, show permissions */
  for (l = ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      FlatpakTransactionOperationType type = flatpak_transaction_operation_get_operation_type (op);

      if (type == FLATPAK_TRANSACTION_OPERATION_INSTALL ||
          type == FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE ||
          type == FLATPAK_TRANSACTION_OPERATION_UPDATE)
        {
          const char *ref = flatpak_transaction_operation_get_ref (op);
          GKeyFile *metadata = flatpak_transaction_operation_get_metadata (op);
          GKeyFile *old_metadata = flatpak_transaction_operation_get_old_metadata (op);

          print_permissions (self, ref, metadata, old_metadata);
        }
    }

  g_print ("\n");

  printer = self->printer = flatpak_table_printer_new ();
  i = 0;

  flatpak_table_printer_set_column_title (printer, i++, "   ");
  flatpak_table_printer_set_column_title (printer, i++, "   ");

  flatpak_table_printer_set_column_expand (printer, i, TRUE);
  flatpak_table_printer_set_column_title (printer, i++, _("ID"));

  flatpak_table_printer_set_column_expand (printer, i, TRUE);
  if (!self->non_default_arch)
    {
      flatpak_table_printer_set_column_skip_unique (printer, i, TRUE);
      flatpak_table_printer_set_column_skip_unique_string (printer, i, flatpak_get_arch ());
    }
  flatpak_table_printer_set_column_title (printer, i++, _("Arch"));

  flatpak_table_printer_set_column_expand (printer, i, TRUE);
  flatpak_table_printer_set_column_title (printer, i++, _("Branch"));

  flatpak_table_printer_set_column_expand (printer, i, TRUE);
  /* translators: This is short for operation, the title of a one-char column */
  flatpak_table_printer_set_column_title (printer, i++, _("Op"));

  if (self->installing || self->updating)
    {
      g_autofree char *text1 = NULL;
      g_autofree char *text2 = NULL;
      g_autofree char *text = NULL;
      int size;

      flatpak_table_printer_set_column_expand (printer, i, TRUE);
      flatpak_table_printer_set_column_title (printer, i++, _("Remote"));
      self->download_col = i;

      /* Avoid resizing the download column too much,
       * by making the title as long as typical content
       */
      text1 = g_strdup_printf ("< 999.9 kB (%s)", _("partial"));
      text2 = g_strdup_printf ("  123.4 MB / 999.9 MB");
      size = MAX (strlen (text1), strlen (text2));
      text = g_strdup_printf ("%-*s", size, _("Download"));
      flatpak_table_printer_set_column_title (printer, i++, text);
    }

  for (l = ops, i = 1; l != NULL; l = l->next, i++)
    {
      FlatpakTransactionOperation *op = l->data;
      FlatpakTransactionOperationType type = flatpak_transaction_operation_get_operation_type (op);
      FlatpakDecomposed *ref = flatpak_transaction_operation_get_decomposed (op);
      const char *remote = flatpak_transaction_operation_get_remote (op);
      g_autofree char *id = flatpak_decomposed_dup_id (ref);
      const char *branch = flatpak_decomposed_get_branch (ref);
      g_autofree char *arch = flatpak_decomposed_dup_arch (ref);
      g_autofree char *rownum = g_strdup_printf ("%2d.", i);

      flatpak_table_printer_add_column (printer, rownum);
      flatpak_table_printer_add_column (printer, "   ");
      flatpak_table_printer_add_column (printer, id);
      flatpak_table_printer_add_column (printer, arch);
      flatpak_table_printer_add_column (printer, branch);
      flatpak_table_printer_add_column (printer, op_shorthand[type]);

      if (type == FLATPAK_TRANSACTION_OPERATION_INSTALL ||
          type == FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE ||
          type == FLATPAK_TRANSACTION_OPERATION_UPDATE)
        {
          guint64 download_size;
          g_autofree char *formatted = NULL;
          g_autofree char *text = NULL;
          const char *prefix;

          download_size = flatpak_transaction_operation_get_download_size (op);
          formatted = g_format_size (download_size);

          if (download_size > 0)
            prefix = "< ";
          else
            prefix = "";

          flatpak_table_printer_add_column (printer, remote);
          if (flatpak_transaction_operation_get_subpaths (op) != NULL)
            text = g_strdup_printf ("%s%s (%s)", prefix, formatted, _("partial"));
          else
            text = g_strdup_printf ("%s%s", prefix, formatted);
          flatpak_table_printer_add_decimal_column (printer, text);
        }

      g_object_set_data (G_OBJECT (op), "row", GINT_TO_POINTER (flatpak_table_printer_get_current_row (printer)));
      flatpak_table_printer_finish_row (printer);
    }

  flatpak_get_window_size (&self->rows, &self->cols);

  g_print ("\n");

  flatpak_table_printer_print_full (printer, 0, self->cols,
                                    &self->table_height, &self->table_width);

  g_print ("\n");

  if (!self->disable_interaction)
    {
      g_autoptr(FlatpakInstallation) installation = flatpak_transaction_get_installation (transaction);
      const char *name;
      const char *id;
      gboolean ret;

      g_print ("\n");

      name = flatpak_installation_get_display_name (installation);
      id = flatpak_installation_get_id (installation);

      if (flatpak_installation_get_is_user (installation))
        ret = flatpak_yes_no_prompt (TRUE, _("Proceed with these changes to the user installation?"));
      else if (g_strcmp0 (id, SYSTEM_DIR_DEFAULT_ID) == 0)
        ret = flatpak_yes_no_prompt (TRUE, _("Proceed with these changes to the system installation?"));
      else
        ret = flatpak_yes_no_prompt (TRUE, _("Proceed with these changes to the %s?"), name);

      if (!ret)
        {
          g_list_free_full (ops, g_object_unref);
          return FALSE;
        }
    }
  else
    g_print ("\n\n");

  self->did_interaction = FALSE;

  return TRUE;
}

static gboolean
transaction_ready (FlatpakTransaction *transaction)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  GList *ops = flatpak_transaction_get_operations (transaction);
  GList *l;
  FlatpakTablePrinter *printer;

  if (ops == NULL)
    return TRUE;

  printer = self->printer;

  if (self->did_interaction)
    {
      /* We did some interaction since ready_pre_auth which messes up the formating, so re-print table */
      flatpak_table_printer_print_full (printer, 0, self->cols,
                                        &self->table_height, &self->table_width);
      g_print ("\n\n");
    }

  for (l = ops; l; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      set_op_progress (self, op, " ");
    }

  g_list_free_full (ops, g_object_unref);

  flatpak_table_printer_add_span (printer, "");
  flatpak_table_printer_finish_row (printer);
  flatpak_table_printer_add_span (printer, "");
  self->progress_row = flatpak_table_printer_get_current_row (printer);
  flatpak_table_printer_finish_row (printer);

  self->table_height += 3; /* 2 for the added lines and one for the newline from the user after the prompt */

  if (flatpak_fancy_output ())
    {
      flatpak_hide_cursor ();
      flatpak_enable_raw_mode ();
      redraw (self);
    }

  g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_WARNING, message_handler, transaction);

  return TRUE;
}

static void
flatpak_cli_transaction_finalize (GObject *object)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (object);

  if (self->first_operation_error)
    g_error_free (self->first_operation_error);

  g_free (self->progress_msg);

  g_hash_table_unref (self->eol_actions);

  if (self->printer)
    flatpak_table_printer_free (self->printer);

  G_OBJECT_CLASS (flatpak_cli_transaction_parent_class)->finalize (object);
}

static void
flatpak_cli_transaction_init (FlatpakCliTransaction *self)
{
  self->eol_actions = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash, (GEqualFunc)flatpak_decomposed_equal,
                                             (GDestroyNotify)flatpak_decomposed_unref, NULL);
}

static gboolean flatpak_cli_transaction_run (FlatpakTransaction *transaction,
                                             GCancellable       *cancellable,
                                             GError            **error);

static void
flatpak_cli_transaction_class_init (FlatpakCliTransactionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FlatpakTransactionClass *transaction_class = FLATPAK_TRANSACTION_CLASS (klass);

  object_class->finalize = flatpak_cli_transaction_finalize;
  transaction_class->add_new_remote = add_new_remote;
  transaction_class->ready = transaction_ready;
  transaction_class->ready_pre_auth = transaction_ready_pre_auth;
  transaction_class->new_operation = new_operation;
  transaction_class->operation_done = operation_done;
  transaction_class->operation_error = operation_error;
  transaction_class->choose_remote_for_ref = choose_remote_for_ref;
  transaction_class->end_of_lifed_with_rebase = end_of_lifed_with_rebase;
  transaction_class->run = flatpak_cli_transaction_run;
  transaction_class->webflow_start = webflow_start;
  transaction_class->webflow_done = webflow_done;
  transaction_class->basic_auth_start = basic_auth_start;
  transaction_class->install_authenticator = install_authenticator;
}

FlatpakTransaction *
flatpak_cli_transaction_new (FlatpakDir *dir,
                             gboolean    disable_interaction,
                             gboolean    stop_on_first_error,
                             gboolean    non_default_arch,
                             GError    **error)
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
  self->non_default_arch = non_default_arch;

  flatpak_transaction_set_no_interaction (FLATPAK_TRANSACTION (self), disable_interaction);
  flatpak_transaction_add_default_dependency_sources (FLATPAK_TRANSACTION (self));

  return (FlatpakTransaction *) g_steal_pointer (&self);
}

static gboolean
flatpak_cli_transaction_run (FlatpakTransaction *transaction,
                             GCancellable       *cancellable,
                             GError            **error)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  gboolean res;

  res = FLATPAK_TRANSACTION_CLASS (flatpak_cli_transaction_parent_class)->run (transaction, cancellable, error);

  if (flatpak_fancy_output ())
    {
      flatpak_disable_raw_mode ();
      flatpak_show_cursor ();
    }

  if (res && self->n_ops > 0)
    {
      const char *text;

      if (self->uninstalling + self->installing + self->updating > 1)
        text = _("Changes complete.");
      else if (self->uninstalling)
        text = _("Uninstall complete.");
      else if (self->installing)
        text = _("Installation complete.");
      else
        text = _("Updates complete.");

      if (flatpak_fancy_output ())
        {
          set_progress (self, text);
          redraw (self);
        }
      else
        g_print ("\r%-*s", self->table_width, text);

      g_print ("\n");
    }

  if (self->first_operation_error)
    {
      g_clear_error (error);

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

  if (!res)
    return FALSE;

  return TRUE;
}
