/*
 * Copyright © 2014 Red Hat, Inc
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

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include <glib/gi18n.h>
#include <gio/gio.h>
#include "libglnx/libglnx.h"

#ifdef USE_SYSTEM_HELPER
#include <polkit/polkit.h>
#include "flatpak-polkit-agent-text-listener.h"

/* Work with polkit before and after autoptr support was added */
typedef PolkitSubject AutoPolkitSubject;
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoPolkitSubject, g_object_unref)
#endif

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-utils-private.h"

static int opt_verbose;
static gboolean opt_ostree_verbose;
static gboolean opt_version;
static gboolean opt_default_arch;
static gboolean opt_supported_arches;
static gboolean opt_gl_drivers;
static gboolean opt_list_installations;
static gboolean opt_user;
static gboolean opt_system;
static char **opt_installations;
static gboolean opt_help;

static gboolean is_in_complete;

typedef struct
{
  const char *name;
  const char *description;
  gboolean (*fn)(int           argc,
                 char        **argv,
                 GCancellable *cancellable,
                 GError      **error);
  gboolean (*complete)(FlatpakCompletion *completion);
  gboolean deprecated;
} FlatpakCommand;

static FlatpakCommand commands[] = {
  /* translators: please keep the leading space */
  { N_(" Manage installed applications and runtimes") },
  { "install", N_("Install an application or runtime"), flatpak_builtin_install, flatpak_complete_install },
  { "update", N_("Update an installed application or runtime"), flatpak_builtin_update, flatpak_complete_update },
  /* Alias upgrade to update to help users of yum/dnf */
  { "upgrade", NULL, flatpak_builtin_update, flatpak_complete_update, TRUE },
  { "uninstall", N_("Uninstall an installed application or runtime"), flatpak_builtin_uninstall, flatpak_complete_uninstall },
  /* Alias remove to uninstall to help users of yum/dnf/apt */
  { "remove", NULL, flatpak_builtin_uninstall, flatpak_complete_uninstall, TRUE },
  { "mask", N_("Mask out updates and automatic installation"), flatpak_builtin_mask, flatpak_complete_mask },
  { "pin", N_("Pin a runtime to prevent automatic removal"), flatpak_builtin_pin, flatpak_complete_pin },
  { "list", N_("List installed apps and/or runtimes"), flatpak_builtin_list, flatpak_complete_list },
  { "info", N_("Show info for installed app or runtime"), flatpak_builtin_info, flatpak_complete_info },
  { "history", N_("Show history"), flatpak_builtin_history, flatpak_complete_history },
  { "config", N_("Configure flatpak"), flatpak_builtin_config, flatpak_complete_config },
  { "repair", N_("Repair flatpak installation"), flatpak_builtin_repair, flatpak_complete_repair },
  { "create-usb", N_("Put applications or runtimes onto removable media"), flatpak_builtin_create_usb, flatpak_complete_create_usb },

  /* translators: please keep the leading newline and space */
  { N_("\n Finding applications and runtimes") },
  { "search", N_("Search for remote apps/runtimes"), flatpak_builtin_search, flatpak_complete_search },

  /* translators: please keep the leading newline and space */
  { N_("\n Running applications") },
  { "run", N_("Run an application"), flatpak_builtin_run, flatpak_complete_run },
  { "override", N_("Override permissions for an application"), flatpak_builtin_override, flatpak_complete_override },
  { "make-current", N_("Specify default version to run"), flatpak_builtin_make_current_app, flatpak_complete_make_current_app },
  { "enter", N_("Enter the namespace of a running application"), flatpak_builtin_enter, flatpak_complete_enter },
  { "ps", N_("Enumerate running applications"), flatpak_builtin_ps, flatpak_complete_ps },
  { "kill", N_("Stop a running application"), flatpak_builtin_kill, flatpak_complete_kill },

  /* translators: please keep the leading newline and space */
  { N_("\n Manage file access") },
  { "documents", N_("List exported files"), flatpak_builtin_document_list, flatpak_complete_document_list },
  { "document-export", N_("Grant an application access to a specific file"), flatpak_builtin_document_export, flatpak_complete_document_export },
  { "document-unexport", N_("Revoke access to a specific file"), flatpak_builtin_document_unexport, flatpak_complete_document_unexport },
  { "document-info", N_("Show information about a specific file"), flatpak_builtin_document_info, flatpak_complete_document_info },
  { "document-list", NULL, flatpak_builtin_document_list, flatpak_complete_document_list, TRUE },

  /* translators: please keep the leading newline and space */
  { N_("\n Manage dynamic permissions") },
  { "permissions", N_("List permissions"), flatpak_builtin_permission_list, flatpak_complete_permission_list },
  { "permission-remove", N_("Remove item from permission store"), flatpak_builtin_permission_remove, flatpak_complete_permission_remove },
  { "permission-list", NULL, flatpak_builtin_permission_list, flatpak_complete_permission_list, TRUE },
  { "permission-set", N_("Set permissions"), flatpak_builtin_permission_set, flatpak_complete_permission_set },
  { "permission-show", N_("Show app permissions"), flatpak_builtin_permission_show, flatpak_complete_permission_show },
  { "permission-reset", N_("Reset app permissions"), flatpak_builtin_permission_reset, flatpak_complete_permission_reset },

  /* translators: please keep the leading newline and space */
  { N_("\n Manage remote repositories") },
  { "remotes", N_("List all configured remotes"), flatpak_builtin_remote_list, flatpak_complete_remote_list },
  { "remote-add", N_("Add a new remote repository (by URL)"), flatpak_builtin_remote_add, flatpak_complete_remote_add },
  { "remote-modify", N_("Modify properties of a configured remote"), flatpak_builtin_remote_modify, flatpak_complete_remote_modify },
  { "remote-delete", N_("Delete a configured remote"), flatpak_builtin_remote_delete, flatpak_complete_remote_delete },
  { "remote-list", NULL, flatpak_builtin_remote_list, flatpak_complete_remote_list, TRUE },
  { "remote-ls", N_("List contents of a configured remote"), flatpak_builtin_remote_ls, flatpak_complete_remote_ls },
  { "remote-info", N_("Show information about a remote app or runtime"), flatpak_builtin_remote_info, flatpak_complete_remote_info },

  /* translators: please keep the leading newline and space */
  { N_("\n Build applications") },
  { "build-init", N_("Initialize a directory for building"), flatpak_builtin_build_init, flatpak_complete_build_init },
  { "build", N_("Run a build command inside the build dir"), flatpak_builtin_build, flatpak_complete_build  },
  { "build-finish", N_("Finish a build dir for export"), flatpak_builtin_build_finish, flatpak_complete_build_finish },
  { "build-export", N_("Export a build dir to a repository"), flatpak_builtin_build_export, flatpak_complete_build_export },
  { "build-bundle", N_("Create a bundle file from a ref in a local repository"), flatpak_builtin_build_bundle, flatpak_complete_build_bundle },
  { "build-import-bundle", N_("Import a bundle file"), flatpak_builtin_build_import, flatpak_complete_build_import },
  { "build-sign", N_("Sign an application or runtime"), flatpak_builtin_build_sign, flatpak_complete_build_sign },
  { "build-update-repo", N_("Update the summary file in a repository"), flatpak_builtin_build_update_repo, flatpak_complete_build_update_repo },
  { "build-commit-from", N_("Create new commit based on existing ref"), flatpak_builtin_build_commit_from, flatpak_complete_build_commit_from },
  { "repo", N_("Show information about a repo"), flatpak_builtin_repo, flatpak_complete_repo },

  { NULL }
};

static gboolean
opt_verbose_cb (const gchar *option_name,
                const gchar *value,
                gpointer     data,
                GError     **error)
{
  opt_verbose++;
  return TRUE;
}


GOptionEntry global_entries[] = {
  { "verbose", 'v', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, &opt_verbose_cb, N_("Show debug information, -vv for more detail"), NULL },
  { "ostree-verbose", 0, 0, G_OPTION_ARG_NONE, &opt_ostree_verbose, N_("Show OSTree debug information"), NULL },
  { "help", '?', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &opt_help, NULL, NULL },
  { NULL }
};

static GOptionEntry empty_entries[] = {
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, N_("Print version information and exit"), NULL },
  { "default-arch", 0, 0, G_OPTION_ARG_NONE, &opt_default_arch, N_("Print default arch and exit"), NULL },
  { "supported-arches", 0, 0, G_OPTION_ARG_NONE, &opt_supported_arches, N_("Print supported arches and exit"), NULL },
  { "gl-drivers", 0, 0, G_OPTION_ARG_NONE, &opt_gl_drivers, N_("Print active gl drivers and exit"), NULL },
  { "installations", 0, 0, G_OPTION_ARG_NONE, &opt_list_installations, N_("Print paths for system installations and exit"), NULL },
  { NULL }
};

GOptionEntry user_entries[] = {
  { "user", 0, 0, G_OPTION_ARG_NONE, &opt_user, N_("Work on the user installation"), NULL },
  { "system", 0, 0, G_OPTION_ARG_NONE, &opt_system, N_("Work on the system-wide installation (default)"), NULL },
  { "installation", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_installations, N_("Work on a non-default system-wide installation"), N_("NAME") },
  { NULL }
};

static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
{
  g_printerr ("F: %s\n", message);
}

static void
no_message_handler (const char     *log_domain,
                    GLogLevelFlags  log_level,
                    const char     *message,
                    gpointer        user_data)
{
}

static GOptionContext *
flatpak_option_context_new_with_commands (FlatpakCommand *f_commands)
{
  GOptionContext *context;
  GString *summary;

  context = g_option_context_new (_("COMMAND"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  summary = g_string_new (_("Builtin Commands:"));

  while (f_commands->name != NULL)
    {
      if (!f_commands->deprecated)
        {
          if (f_commands->fn != NULL)
            {
              g_string_append_printf (summary, "\n  %s", f_commands->name);
              /* Note: the 23 is there to align command descriptions with
               * the option descriptions produced by GOptionContext.
               */
              if (f_commands->description)
                g_string_append_printf (summary, "%*s%s", (int) (23 - strlen (f_commands->name)), "", _(f_commands->description));
            }
          else
            {
              g_string_append_printf (summary, "\n%s", _(f_commands->name));
            }
        }
      f_commands++;
    }

  g_option_context_set_summary (context, summary->str);

  g_string_free (summary, TRUE);

  return context;
}

static void
check_environment (void)
{
  const char * const *dirs;
  gboolean has_system = FALSE;
  gboolean has_user = FALSE;
  g_autofree char *system_exports = NULL;
  g_autofree char *user_exports = NULL;
  int i;
  int rows, cols;

  /* Only print warnings on ttys */
  if (!flatpak_fancy_output ())
    return;

  /* Don't recommend restarting the session when we're not in one */
  if (!g_getenv ("DBUS_SESSION_BUS_ADDRESS"))
    return;

  /* Avoid interfering with tests */
  if (g_getenv ("FLATPAK_SYSTEM_DIR") || g_getenv ("FLATPAK_USER_DIR"))
    return;

  system_exports = g_build_filename (FLATPAK_SYSTEMDIR, "exports/share", NULL);
  user_exports = g_build_filename (g_get_user_data_dir (), "flatpak/exports/share", NULL);

  dirs = g_get_system_data_dirs ();
  for (i = 0; dirs[i]; i++)
    {
      /* There should never be a relative path but just in case we don't want
       * g_file_new_for_path() to take the current directory into account.
       */
      if (!g_str_has_prefix (dirs[i], "/"))
        continue;

      /* Normalize the path using GFile to e.g. replace // with / */
      g_autoptr(GFile) dir_file = g_file_new_for_path (dirs[i]);
      g_autofree char *dir_path = g_file_get_path (dir_file);

      if (g_str_has_prefix (dir_path, system_exports))
        has_system = TRUE;
      if (g_str_has_prefix (dir_path, user_exports))
        has_user = TRUE;
    }

  flatpak_get_window_size (&rows, &cols);
  if (cols > 80)
    cols = 80;

  if (!has_system && !has_user)
    {
      g_autofree char *missing = NULL;
      missing = g_strdup_printf ("\n\n '%s'\n '%s'\n\n", system_exports, user_exports);
      g_print ("\n");
      /* Translators: this text is automatically wrapped, don't insert line breaks */
      print_wrapped (cols,
                     _("Note that the directories %s are not in the search path "
                       "set by the XDG_DATA_DIRS environment variable, so applications "
                       "installed by Flatpak may not appear on your desktop until the "
                       "session is restarted."),
                     missing);
      g_print ("\n");
    }
  else if (!has_system || !has_user)
    {
      g_autofree char *missing = NULL;
      missing = g_strdup_printf ("\n\n '%s'\n\n", !has_system ? system_exports : user_exports);
      g_print ("\n");
      /* Translators: this text is automatically wrapped, don't insert line breaks */
      print_wrapped (cols,
                     _("Note that the directory %s is not in the search path "
                       "set by the XDG_DATA_DIRS environment variable, so applications "
                       "installed by Flatpak may not appear on your desktop until the "
                       "session is restarted."),
                     missing);
      g_print ("\n");
    }
}

gboolean
flatpak_option_context_parse (GOptionContext     *context,
                              const GOptionEntry *main_entries,
                              int                *argc,
                              char             ***argv,
                              FlatpakBuiltinFlags flags,
                              GPtrArray         **out_dirs,
                              GCancellable       *cancellable,
                              GError            **error)
{
  g_autoptr(GPtrArray) dirs = NULL;

  if (__builtin_popcount (flags & (FLATPAK_BUILTIN_FLAG_NO_DIR |
                                   FLATPAK_BUILTIN_FLAG_ONE_DIR |
                                   FLATPAK_BUILTIN_FLAG_STANDARD_DIRS |
                                   FLATPAK_BUILTIN_FLAG_ALL_DIRS)) != 1)
    g_assert_not_reached ();

  if (!(flags & FLATPAK_BUILTIN_FLAG_NO_DIR))
    g_option_context_add_main_entries (context, user_entries, NULL);

  if (main_entries != NULL)
    g_option_context_add_main_entries (context, main_entries, NULL);

  g_option_context_add_main_entries (context, global_entries, NULL);

  /* We never want help output to interrupt completion */
  if (is_in_complete)
    g_option_context_set_help_enabled (context, FALSE);

  if (!g_option_context_parse (context, argc, argv, error))
    return FALSE;

  /* We never want verbose output in the complete case, that breaks completion */
  if (is_in_complete)
    {
      g_log_set_default_handler (no_message_handler, NULL);
    }
  else
    {
      if (opt_verbose > 0)
        g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);
      if (opt_verbose > 1)
        g_log_set_handler (G_LOG_DOMAIN "2", G_LOG_LEVEL_DEBUG, message_handler, NULL);

      if (opt_ostree_verbose)
        g_log_set_handler ("OSTree", G_LOG_LEVEL_DEBUG, message_handler, NULL);

      if (opt_verbose > 0 || opt_ostree_verbose)
        flatpak_disable_fancy_output ();
    }

  if (!(flags & FLATPAK_BUILTIN_FLAG_NO_DIR))
    {
      dirs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
      int i;

      if (!(flags & FLATPAK_BUILTIN_FLAG_ONE_DIR))
        {
          /*
           * FLATPAK_BUILTIN_FLAG_STANDARD_DIRS or FLATPAK_BUILTIN_FLAG_ALL_DIRS
           * must be set.
           */

          /* If nothing is set, then we put the system dir first, which can be used as the default */
          if (opt_system || (!opt_user && opt_installations == NULL))
            g_ptr_array_add (dirs, flatpak_dir_get_system_default ());

          if (opt_user || (!opt_system && opt_installations == NULL))
            g_ptr_array_add (dirs, flatpak_dir_get_user ());

          if (opt_installations != NULL)
            {
              for (i = 0; opt_installations[i] != NULL; i++)
                {
                  FlatpakDir *installation_dir = NULL;

                  /* Already included the default system installation */
                  if (opt_system && g_strcmp0 (opt_installations[i], "default") == 0)
                    continue;

                  installation_dir = flatpak_dir_get_system_by_id (opt_installations[i], cancellable, error);
                  if (installation_dir == NULL)
                    return FALSE;

                  g_ptr_array_add (dirs, installation_dir);
                }
            }

          if (flags & FLATPAK_BUILTIN_FLAG_ALL_DIRS &&
              opt_installations == NULL && !opt_user && !opt_system)
            {
              g_autoptr(GPtrArray) system_dirs = NULL;

              g_ptr_array_set_size (dirs, 0);
              /* The first dir should be the default */
              g_ptr_array_add (dirs, flatpak_dir_get_system_default ());
              g_ptr_array_add (dirs, flatpak_dir_get_user ());

              system_dirs = flatpak_dir_get_system_list (cancellable, error);
              if (system_dirs == NULL)
                return FALSE;

              for (i = 0; i < system_dirs->len; i++)
                {
                  FlatpakDir *dir = g_ptr_array_index (system_dirs, i);
                  const char *id = flatpak_dir_get_id (dir);
                  if (g_strcmp0 (id, SYSTEM_DIR_DEFAULT_ID) != 0)
                    g_ptr_array_add (dirs, g_object_ref (dir));
                }
            }
        }
      else /* FLATPAK_BUILTIN_FLAG_ONE_DIR */
        {
          FlatpakDir *dir;

          if ((opt_system && opt_user) ||
              (opt_system && opt_installations != NULL) ||
              (opt_user && opt_installations != NULL) ||
              (opt_installations != NULL && opt_installations[1] != NULL))
            return usage_error (context, _("Multiple installations specified for a command "
                                           "that works on one installation"), error);

          if (opt_system || (!opt_user && opt_installations == NULL))
            dir = flatpak_dir_get_system_default ();
          else if (opt_user)
            dir = flatpak_dir_get_user ();
          else if (opt_installations != NULL)
            {
              dir = flatpak_dir_get_system_by_id (opt_installations[0], cancellable, error);
              if (dir == NULL)
                return FALSE;
            }
          else
            g_assert_not_reached ();

          g_ptr_array_add (dirs, dir);
        }

      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);

          if (flags & FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO)
            {
              if (!flatpak_dir_maybe_ensure_repo (dir, cancellable, error))
                return FALSE;
            }
          else
            {
              if (!flatpak_dir_ensure_repo (dir, cancellable, error))
                return FALSE;
            }

          flatpak_log_dir_access (dir);
        }
    }

  if (out_dirs)
    *out_dirs = g_steal_pointer (&dirs);

  return TRUE;
}

gboolean
usage_error (GOptionContext *context, const char *message, GError **error)
{
  g_autofree char *hint = NULL;

  hint = g_strdup_printf (_("See '%s --help'"), g_get_prgname ());
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s\n\n%s", message, hint);
  return FALSE;
}

static FlatpakCommand *
extract_command (int         *argc,
                 char       **argv,
                 const char **command_name_out)
{
  FlatpakCommand *command;
  const char *command_name = NULL;
  int in, out;

  /*
   * Parse the global options. We rearrange the options as
   * necessary, in order to pass relevant options through
   * to the commands, but also have them take effect globally.
   */
  for (in = 1, out = 1; in < *argc; in++, out++)
    {
      /* The non-option is the command, take it out of the arguments */
      if (argv[in][0] != '-')
        {
          if (command_name == NULL)
            {
              command_name = argv[in];
              out--;
              continue;
            }
        }

      argv[out] = argv[in];
    }

  *argc = out;
  argv[out] = NULL;

  command = commands;
  while (command->name)
    {
      if (command->fn != NULL &&
          g_strcmp0 (command_name, command->name) == 0)
        break;
      command++;
    }

  *command_name_out = command_name;

  return command;
}

static const char *
find_similar_command (const char *word,
                      gboolean   *option)
{
  int i, d, k;
  const char *suggestion;
  GOptionEntry *entries[3] = { global_entries, empty_entries, user_entries };

  d = G_MAXINT;
  suggestion = NULL;

  for (i = 0; commands[i].name; i++)
    {
      if (!commands[i].fn)
        continue;

      int d1 = flatpak_levenshtein_distance (word, commands[i].name);
      if (d1 < d)
        {
          d = d1;
          suggestion = commands[i].name;
          *option = FALSE;
        }
    }

  for (k = 0; k < 3; k++)
    {
      for (i = 0; entries[k][i].long_name; i++)
        {
          int d1 = flatpak_levenshtein_distance (word, entries[k][i].long_name);
          if (d1 < d)
            {
              d = d1;
              suggestion = entries[k][i].long_name;
              *option = TRUE;
            }
        }
    }

  return suggestion;
}

static gpointer
install_polkit_agent (void)
{
  gpointer agent = NULL;

#ifdef USE_SYSTEM_HELPER
  PolkitAgentListener *listener = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GDBusConnection) bus = NULL;

  bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &local_error);

  if (bus == NULL)
    {
      g_debug ("Unable to connect to system bus: %s", local_error->message);
      return NULL;
    }

  /* Install a polkit agent as fallback, in case we're running on a console */
  listener = flatpak_polkit_agent_text_listener_new (NULL, &local_error);
  if (listener == NULL)
    {
      g_debug ("Failed to create polkit agent listener: %s", local_error->message);
    }
  else
    {
      g_autoptr(AutoPolkitSubject) subject = NULL;
      GVariantBuilder opt_builder;
      g_autoptr(GVariant) options = NULL;

      subject = polkit_unix_process_new_for_owner (getpid (), 0, getuid ());

      g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
      if (g_strcmp0 (g_getenv ("FLATPAK_FORCE_TEXT_AUTH"), "1") != 0)
        g_variant_builder_add (&opt_builder, "{sv}", "fallback", g_variant_new_boolean (TRUE));
      options = g_variant_ref_sink (g_variant_builder_end (&opt_builder));

      agent = polkit_agent_listener_register_with_options (listener,
                                                           POLKIT_AGENT_REGISTER_FLAGS_RUN_IN_THREAD,
                                                           subject,
                                                           NULL,
                                                           options,
                                                           NULL,
                                                           &local_error);
      if (agent == NULL)
        {
          g_debug ("Failed to register polkit agent listener: %s", local_error->message);
        }
      g_object_unref (listener);
    }
#endif

  return agent;
}

static void
uninstall_polkit_agent (gpointer *agent)
{
#ifdef USE_SYSTEM_HELPER
  if (*agent)
    polkit_agent_listener_unregister (*agent);
#endif
}

static int
flatpak_run (int      argc,
             char   **argv,
             GError **res_error)
{
  FlatpakCommand *command;
  GError *error = NULL;
  GCancellable *cancellable = NULL;
  g_autofree char *prgname = NULL;
  gboolean success = FALSE;
  const char *command_name = NULL;

  __attribute__((cleanup (uninstall_polkit_agent))) gpointer polkit_agent = NULL;

  command = extract_command (&argc, argv, &command_name);

  if (!command->fn)
    {
      GOptionContext *context;
      g_autofree char *hint = NULL;
      g_autofree char *msg = NULL;

      context = flatpak_option_context_new_with_commands (commands);

      hint = g_strdup_printf (_("See '%s --help'"), g_get_prgname ());

      if (command_name != NULL)
        {
          const char *similar;
          gboolean option;

          similar = find_similar_command (command_name, &option);
          if (similar)
            msg = g_strdup_printf (_("'%s' is not a flatpak command. Did you mean '%s%s'?"),
                                   command_name, option ? "--" : "", similar);
          else
            msg = g_strdup_printf (_("'%s' is not a flatpak command"),
                                   command_name);
        }
      else
        {
          g_autoptr(GError) local_error = NULL;

          g_option_context_add_main_entries (context, empty_entries, NULL);
          g_option_context_add_main_entries (context, global_entries, NULL);
          if (g_option_context_parse (context, &argc, &argv, &local_error))
            {
              if (opt_version)
                {
                  g_print ("%s\n", PACKAGE_STRING);
                  exit (EXIT_SUCCESS);
                }

              if (opt_default_arch)
                {
                  g_print ("%s\n", flatpak_get_arch ());
                  exit (EXIT_SUCCESS);
                }

              if (opt_supported_arches)
                {
                  const char **arches = flatpak_get_arches ();
                  int i;
                  for (i = 0; arches[i] != NULL; i++)
                    g_print ("%s\n", arches[i]);
                  exit (EXIT_SUCCESS);
                }

              if (opt_gl_drivers)
                {
                  const char **drivers = flatpak_get_gl_drivers ();
                  int i;
                  for (i = 0; drivers[i] != NULL; i++)
                    g_print ("%s\n", drivers[i]);
                  exit (EXIT_SUCCESS);
                }

              if (opt_list_installations)
                {
                  GPtrArray *paths;

                  paths = flatpak_get_system_base_dir_locations (NULL, &local_error);
                  if (paths)
                    {
                      guint i;
                      for (i = 0; i < paths->len; i++)
                        {
                          GFile *file = paths->pdata[i];
                          g_print ("%s\n", flatpak_file_get_path_cached (file));
                        }
                      exit (EXIT_SUCCESS);
                    }
                }
            }

          if (local_error)
            msg = g_strdup (local_error->message);
          else
            msg = g_strdup (_("No command specified"));
        }

      g_option_context_free (context);

      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s\n\n%s", msg, hint);

      goto out;
    }

  prgname = g_strdup_printf ("%s %s", g_get_prgname (), command_name);
  g_set_prgname (prgname);

  /* Only print environment warnings in some commonly used interactive operations so we
     avoid messing up output in commands where you might parse the output. */
  if (g_strcmp0 (command->name, "install") == 0 ||
      g_strcmp0 (command->name, "update") == 0 ||
      g_strcmp0 (command->name, "remote-add") == 0 ||
      g_strcmp0 (command->name, "run") == 0)
    check_environment ();

  /* Don't talk to dbus in enter, as it must be thread-free to setns, also
     skip run/build for performance reasons (no need to connect to dbus). */
  if (g_strcmp0 (command->name, "enter") != 0 &&
      g_strcmp0 (command->name, "run") != 0 &&
      g_strcmp0 (command->name, "build") != 0)
    polkit_agent = install_polkit_agent ();

  /* g_vfs_get_default can spawn threads */
  if (g_strcmp0 (command->name, "enter") != 0)
    {
      g_autofree const char *old_env = NULL;

      /* avoid gvfs (http://bugzilla.gnome.org/show_bug.cgi?id=526454) */
      old_env = g_strdup (g_getenv ("GIO_USE_VFS"));
      g_setenv ("GIO_USE_VFS", "local", TRUE);
      g_vfs_get_default ();
      if (old_env)
        g_setenv ("GIO_USE_VFS", old_env, TRUE);
      else
        g_unsetenv ("GIO_USE_VFS");
    }

  if (!command->fn (argc, argv, cancellable, &error))
    goto out;

  success = TRUE;
out:
  /* Note: We allow failures with NULL error (it means don't print anything), useful when e.g. the user aborted */
  g_assert (!success || error == NULL);

  if (error)
    {
      g_propagate_error (res_error, error);
    }

  return success ? 0 : 1;
}

static int
complete (int    argc,
          char **argv)
{
  g_autoptr(FlatpakCompletion) completion = NULL;
  FlatpakCommand *command;
  const char *command_name = NULL;

  is_in_complete = TRUE;

  completion = flatpak_completion_new (argv[2], argv[3], argv[4]);
  if (completion == NULL)
    return 1;

  command = extract_command (&completion->argc, completion->argv, &command_name);
  flatpak_completion_debug ("command=%p '%s'", command->fn, command->name);

  if (!command->fn)
    {
      FlatpakCommand *c = commands;
      while (c->name)
        {
          if (c->fn != NULL)
            flatpak_complete_word (completion, "%s ", c->name);
          c++;
        }

      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, empty_entries);
      flatpak_complete_options (completion, user_entries);
    }
  else if (command->complete)
    {
      if (!command->complete (completion))
        return 1;
    }
  else
    {
      flatpak_complete_options (completion, global_entries);
    }

  return 0;
}

static void
handle_sigterm (int signum)
{
  flatpak_disable_raw_mode ();
  flatpak_show_cursor ();
  _exit (1);
}

int
main (int    argc,
      char **argv)
{
  g_autoptr(GError) error = NULL;
  int ret;
  struct sigaction action;

  /* The child repo shared between the client process and the
     system-helper really needs to support creating files that
     are readable by others, so override the umask to 022.
     Ideally this should be set when needed, but umask is thread-unsafe
     so there is really no local way to fix this.
  */
  umask(022);

  memset (&action, 0, sizeof (struct sigaction));
  action.sa_handler = handle_sigterm;
  sigaction (SIGTERM, &action, NULL);
  sigaction (SIGHUP, &action, NULL);
  sigaction (SIGINT, &action, NULL);

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_WARNING, message_handler, NULL);

  g_set_prgname (argv[0]);

  /* Avoid weird recursive type initialization deadlocks from libsoup */
  g_type_ensure (G_TYPE_SOCKET);

  if (argc >= 4 && strcmp (argv[1], "complete") == 0)
    return complete (argc, argv);

  ret = flatpak_run (argc, argv, &error);

  if (error != NULL)
    {
      const char *prefix = "";
      const char *suffix = "";
      if (flatpak_fancy_output ())
        {
          prefix = FLATPAK_ANSI_RED FLATPAK_ANSI_BOLD_ON;
          suffix = FLATPAK_ANSI_BOLD_OFF FLATPAK_ANSI_COLOR_RESET;
        }
      g_dbus_error_strip_remote_error (error);
      g_printerr ("%s%s %s%s\n", prefix, _("error:"), suffix, error->message);
    }

  return ret;
}
