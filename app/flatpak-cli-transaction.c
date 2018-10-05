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
#include <sys/ioctl.h>


struct _FlatpakCliTransaction
{
  FlatpakTransaction parent;

  char              *name;
  gboolean           disable_interaction;
  gboolean           stop_on_first_error;
  gboolean           is_user;
  gboolean           aborted;
  GError            *first_operation_error;

  gboolean           progress_initialized;
  int                progress_n_columns;
  int                progress_last_width;
};

struct _FlatpakCliTransactionClass
{
  FlatpakCliTransactionClass parent_class;
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

static gboolean
add_new_remote (FlatpakTransaction            *transaction,
                FlatpakTransactionRemoteReason reason,
                const char                    *from_id,
                const char                    *remote_name,
                const char                    *url)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);

  if (self->disable_interaction)
    {
      g_print (_("Configuring %s as new remote '%s'"), url, remote_name);
      return TRUE;
    }

  if (reason == FLATPAK_TRANSACTION_REMOTE_GENERIC_REPO)
    {
      if (flatpak_yes_no_prompt (_("The remote '%s', refered to by '%s' at location %s contains additional applications.\n"
                                   "Should the remote be kept for future installations?"),
                                 remote_name, from_id, url))
        return TRUE;
    }
  else if (reason == FLATPAK_TRANSACTION_REMOTE_RUNTIME_DEPS)
    {
      if (flatpak_yes_no_prompt (_("The application %s depends on runtimes from:\n  %s\n"
                                   "Configure this as new remote '%s'"),
                                 from_id, url, remote_name))
        return TRUE;
    }

  return FALSE;
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
                     gpointer                    data)
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
  partial = (remainder * strlen (BAR_CHARS) * BAR_LENGTH) / 100;

  for (i = 0; i < n_full; i++)
    g_string_append_c (str, BAR_CHARS[strlen (BAR_CHARS) - 1]);

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
    g_print ("\n");
}

static void
new_operation (FlatpakTransaction          *transaction,
               FlatpakTransactionOperation *operation,
               FlatpakTransactionProgress  *progress)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  const char *pref;
  g_autofree char *bundle_basename = NULL;
  const char *ref = flatpak_transaction_operation_get_ref (operation);
  const char *remote = flatpak_transaction_operation_get_remote (operation);
  GFile *bundle = flatpak_transaction_operation_get_bundle_path (operation);
  FlatpakTransactionOperationType operation_type = flatpak_transaction_operation_get_operation_type (operation);

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
        bundle_basename = g_file_get_basename (bundle);
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
operation_done (FlatpakTransaction          *transaction,
                FlatpakTransactionOperation *operation,
                const char                  *commit,
                FlatpakTransactionResult     details)
{
  FlatpakTransactionOperationType operation_type = flatpak_transaction_operation_get_operation_type (operation);
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
operation_error (FlatpakTransaction            *transaction,
                 FlatpakTransactionOperation   *operation,
                 const GError                  *error,
                 FlatpakTransactionErrorDetails detail)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  FlatpakTransactionOperationType operation_type = flatpak_transaction_operation_get_operation_type (operation);
  const char *ref = flatpak_transaction_operation_get_ref (operation);
  const char *pref;

  progress_done (transaction);

  pref = strchr (ref, '/') + 1;

  if (g_error_matches (error, FLATPAK_ERROR, FLATPAK_ERROR_SKIPPED))
    {
      g_printerr ("%s\n", error->message);
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
              const char         *ref,
              const char         *reason,
              const char         *rebase)
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
      if (old_tags == NULL || !g_strv_contains ((const char * const *)old_tags, tag))
        g_ptr_array_add (tags_array, g_strdup (tag));
    }
}

static void
print_perm_line (FlatpakTablePrinter *printer,
                 const char          *title,
                 GPtrArray           *items)
{
  g_autoptr(GString) res = g_string_new (NULL);
  int i;

  if (items->len == 0)
    return;

  g_string_append_printf (res, "  %s: ", title);
  for (i = 0; i < items->len; i++)
    {
      if (i != 0)
        g_string_append (res, ", ");
      g_string_append (res, (char *) items->pdata[i]);
    }

  flatpak_table_printer_add_span (printer, res->str);
  flatpak_table_printer_finish_row (printer);
}

static void
print_permissions (FlatpakTablePrinter *printer,
                   GKeyFile            *metadata,
                   GKeyFile            *old_metadata,
                   const char          *ref)
{
  g_autoptr(GPtrArray) permissions = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) session_bus_talk = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) session_bus_own = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) system_bus_talk = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) system_bus_own = g_ptr_array_new_with_free_func (g_free);
  g_autoptr(GPtrArray) tags = g_ptr_array_new_with_free_func (g_free);

  if (metadata == NULL)
    return;

  /* Only apps have permissions */
  if (!g_str_has_prefix (ref, "app/"))
    return;

  append_permissions (permissions, metadata, old_metadata, FLATPAK_METADATA_KEY_SHARED);
  append_permissions (permissions, metadata, old_metadata, FLATPAK_METADATA_KEY_SOCKETS);
  append_permissions (permissions, metadata, old_metadata, FLATPAK_METADATA_KEY_DEVICES);
  append_permissions (permissions, metadata, old_metadata, FLATPAK_METADATA_KEY_FEATURES);

  print_perm_line (printer,
                   old_metadata ?  _("new permissions") : _("permissions"),
                   permissions);

  append_permissions (files, metadata, old_metadata, FLATPAK_METADATA_KEY_FILESYSTEMS);
  print_perm_line (printer,
                   old_metadata ?  _("new file access") : _("file access"),
                   files);

  append_bus (session_bus_talk, session_bus_own,
              metadata, old_metadata, FLATPAK_METADATA_GROUP_SESSION_BUS_POLICY);
  print_perm_line (printer,
                   old_metadata ? _("new dbus access") : _("dbus access"),
                   session_bus_talk);
  print_perm_line (printer,
                   old_metadata ? _("new dbus ownership") : _("dbus ownership"),
                   session_bus_own);

  append_bus (system_bus_talk, system_bus_own,
              metadata, old_metadata, FLATPAK_METADATA_GROUP_SYSTEM_BUS_POLICY);
  print_perm_line (printer,
                   old_metadata ? _("new system dbus access") : _("system dbus access"),
                   system_bus_talk);
  print_perm_line (printer,
                   old_metadata ? _("new system dbus ownership") : _("system dbus ownership"),
                   system_bus_own);
  append_tags (tags, metadata, old_metadata);
  print_perm_line (printer,
                   old_metadata ? _("new tags") : _("tags"),
                   tags);
}

static gboolean
transaction_ready (FlatpakTransaction *transaction)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (transaction);
  GList *ops = flatpak_transaction_get_operations (transaction);
  GList *l;
  gboolean found_one;
  FlatpakTablePrinter *printer = NULL;

  if (ops == NULL)
    return TRUE;

  found_one = FALSE;
  for (l = ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      FlatpakTransactionOperationType type = flatpak_transaction_operation_get_operation_type (op);
      const char *ref = flatpak_transaction_operation_get_ref (op);
      const char *pref = strchr (ref, '/') + 1;

      if (type != FLATPAK_TRANSACTION_OPERATION_UNINSTALL)
        continue;

      if (!found_one)
        g_print (_("Uninstalling from %s:\n"), self->name);
      found_one = TRUE;
      g_print ("%s\n", pref);
    }

  found_one = FALSE;
  for (l = ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      FlatpakTransactionOperationType type = flatpak_transaction_operation_get_operation_type (op);
      const char *ref = flatpak_transaction_operation_get_ref (op);
      const char *remote = flatpak_transaction_operation_get_remote (op);
      const char *commit = flatpak_transaction_operation_get_commit (op);
      GKeyFile *metadata = flatpak_transaction_operation_get_metadata (op);
      const char *pref = strchr (ref, '/') + 1;

      if (type != FLATPAK_TRANSACTION_OPERATION_INSTALL &&
          type != FLATPAK_TRANSACTION_OPERATION_INSTALL_BUNDLE)
        continue;

      if (!found_one)
        {
          g_print (_("Installing in %s:\n"), self->name);
          printer = flatpak_table_printer_new ();
          found_one = TRUE;
        }

      flatpak_table_printer_add_column (printer, pref);
      flatpak_table_printer_add_column (printer, remote);
      flatpak_table_printer_add_column_len (printer, commit, 12);
      flatpak_table_printer_finish_row (printer);

      print_permissions (printer, metadata, NULL, ref);

    }
  if (printer)
    {
      flatpak_table_printer_print (printer);
      flatpak_table_printer_free (printer);
      printer = NULL;
    }

  found_one = FALSE;
  for (l = ops; l != NULL; l = l->next)
    {
      FlatpakTransactionOperation *op = l->data;
      FlatpakTransactionOperationType type = flatpak_transaction_operation_get_operation_type (op);
      const char *ref = flatpak_transaction_operation_get_ref (op);
      const char *remote = flatpak_transaction_operation_get_remote (op);
      const char *commit = flatpak_transaction_operation_get_commit (op);
      GKeyFile *metadata = flatpak_transaction_operation_get_metadata (op);
      GKeyFile *old_metadata = flatpak_transaction_operation_get_old_metadata (op);
      const char *pref = strchr (ref, '/') + 1;

      if (type != FLATPAK_TRANSACTION_OPERATION_UPDATE)
        continue;

      if (!found_one)
        {
          g_print (_("Updating in %s:\n"), self->name);
          printer = flatpak_table_printer_new ();
          found_one = TRUE;
        }

      flatpak_table_printer_add_column (printer, pref);
      flatpak_table_printer_add_column (printer, remote);
      flatpak_table_printer_add_column_len (printer, commit, 12);
      flatpak_table_printer_finish_row (printer);

      print_permissions (printer, metadata, old_metadata, ref);
    }
  if (printer)
    {
      flatpak_table_printer_print (printer);
      flatpak_table_printer_free (printer);
      printer = NULL;
    }

  g_list_free_full (ops, g_object_unref);

  if (!self->disable_interaction &&
      !flatpak_yes_no_prompt (_("Is this ok")))
    return FALSE;

  return TRUE;
}

static void
flatpak_cli_transaction_finalize (GObject *object)
{
  FlatpakCliTransaction *self = FLATPAK_CLI_TRANSACTION (object);

  if (self->first_operation_error)
    g_error_free (self->first_operation_error);

  g_free (self->name);

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
  transaction_class->add_new_remote = add_new_remote;
  transaction_class->ready = transaction_ready;
  transaction_class->new_operation = new_operation;
  transaction_class->operation_done = operation_done;
  transaction_class->operation_error = operation_error;
  transaction_class->choose_remote_for_ref = choose_remote_for_ref;
  transaction_class->end_of_lifed = end_of_lifed;
}

FlatpakTransaction *
flatpak_cli_transaction_new (FlatpakDir *dir,
                             gboolean    disable_interaction,
                             gboolean    stop_on_first_error,
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
  self->name = flatpak_dir_get_name (dir);
  self->is_user = flatpak_dir_is_user (dir);

  flatpak_transaction_add_default_dependency_sources (FLATPAK_TRANSACTION (self));

  return (FlatpakTransaction *) g_steal_pointer (&self);
}

gboolean
flatpak_cli_transaction_add_install (FlatpakTransaction *transaction,
                                     const char         *remote,
                                     const char         *ref,
                                     const char        **subpaths,
                                     GError            **error)
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
                             GCancellable       *cancellable,
                             GError            **error)
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
