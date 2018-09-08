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

#include "flatpak-builtins.h"
#include "flatpak-utils-private.h"

static int opt_verbose;
static gboolean opt_ostree_verbose;
static gboolean opt_version;
static gboolean opt_default_arch;
static gboolean opt_supported_arches;
static gboolean opt_gl_drivers;
static gboolean opt_user;
static gboolean opt_system;
static char **opt_installations;

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
  { N_(" Manage installed apps and runtimes") },
  { "install", N_("Install an application or runtime"), flatpak_builtin_install, flatpak_complete_install },
  { "update", N_("Update an installed application or runtime"), flatpak_builtin_update, flatpak_complete_update },
  { "uninstall", N_("Uninstall an installed application or runtime"), flatpak_builtin_uninstall, flatpak_complete_uninstall },
  /* Alias remove to uninstall to help users of yum/dnf/apt */
  { "remove", NULL, flatpak_builtin_uninstall, flatpak_complete_uninstall, TRUE },
  { "list", N_("List installed apps and/or runtimes"), flatpak_builtin_list, flatpak_complete_list },
  { "info", N_("Show info for installed app or runtime"), flatpak_builtin_info, flatpak_complete_info },
  { "config", N_("Configure flatpak"), flatpak_builtin_config, flatpak_complete_config },
  { "repair", N_("Repair flatpak installation"), flatpak_builtin_repair, flatpak_complete_repair },
  { "create-usb", N_("Put apps and/or runtimes onto removable media"), flatpak_builtin_create_usb, flatpak_complete_create_usb },

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

  /* translators: please keep the leading newline and space */
  { N_("\n Manage file access") },
  { "document-export", N_("Grant an application access to a specific file"), flatpak_builtin_document_export, flatpak_complete_document_export },
  { "document-unexport", N_("Revoke access to a specific file"), flatpak_builtin_document_unexport, flatpak_complete_document_unexport },
  { "document-info", N_("Show information about a specific file"), flatpak_builtin_document_info, flatpak_complete_document_info },
  { "document-list", N_("List exported files"), flatpak_builtin_document_list, flatpak_complete_document_list },

  /* translators: please keep the leading newline and space */
  { N_("\n Manage dynamic permissions") },
  { "permission-remove", N_("Remove item from permission store"), flatpak_builtin_permission_remove, flatpak_complete_permission_remove },
  { "permission-list", N_("List permissions"), flatpak_builtin_permission_list, flatpak_complete_permission_list },
  { "permission-show", N_("Show app permissions"), flatpak_builtin_permission_show, flatpak_complete_permission_show },
  { "permission-reset", N_("Reset app permissions"), flatpak_builtin_permission_reset, flatpak_complete_permission_reset },

  /* translators: please keep the leading newline and space */
  { N_("\n Manage remote repositories") },
  { "remotes", N_("List all configured remotes"), flatpak_builtin_list_remotes, flatpak_complete_list_remotes },
  { "remote-add", N_("Add a new remote repository (by URL)"), flatpak_builtin_add_remote, flatpak_complete_add_remote },
  { "remote-modify", N_("Modify properties of a configured remote"), flatpak_builtin_modify_remote, flatpak_complete_modify_remote },
  { "remote-delete", N_("Delete a configured remote"), flatpak_builtin_delete_remote, flatpak_complete_delete_remote },
  { "remote-list", NULL, flatpak_builtin_list_remotes, flatpak_complete_list_remotes, TRUE },
  { "remote-ls", N_("List contents of a configured remote"), flatpak_builtin_ls_remote, flatpak_complete_ls_remote },
  { "remote-info", N_("Show information about a remote app or runtime"), flatpak_builtin_info_remote, flatpak_complete_info_remote },

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
  { "repo", N_("Print information about a repo"), flatpak_builtin_repo, flatpak_complete_repo },

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
  { "verbose", 'v', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, &opt_verbose_cb, N_("Print debug information during command processing, -vv for more detail"), NULL },
  { "ostree-verbose", 0, 0, G_OPTION_ARG_NONE, &opt_ostree_verbose, N_("Print OSTree debug information during command processing"), NULL },
  { "help", '?', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, N_("Show help options"), NULL, NULL },
  { NULL }
};

static GOptionEntry empty_entries[] = {
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, N_("Print version information and exit"), NULL },
  { "default-arch", 0, 0, G_OPTION_ARG_NONE, &opt_default_arch, N_("Print default arch and exit"), NULL },
  { "supported-arches", 0, 0, G_OPTION_ARG_NONE, &opt_supported_arches, N_("Print supported arches and exit"), NULL },
  { "gl-drivers", 0, 0, G_OPTION_ARG_NONE, &opt_gl_drivers, N_("Print active gl drivers and exit"), NULL },
  { NULL }
};

GOptionEntry user_entries[] = {
  { "user", 0, 0, G_OPTION_ARG_NONE, &opt_user, N_("Work on user installations"), NULL },
  { "system", 0, 0, G_OPTION_ARG_NONE, &opt_system, N_("Work on system-wide installations (default)"), NULL },
  { "installation", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_installations, N_("Work on specific system-wide installation(s)"), N_("NAME") },
  { NULL }
};

static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    g_printerr ("F: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

static GOptionContext *
flatpak_option_context_new_with_commands (FlatpakCommand *commands)
{
  GOptionContext *context;
  GString *summary;

  context = g_option_context_new (_("COMMAND"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  summary = g_string_new (_("Builtin Commands:"));

  while (commands->name != NULL)
    {
      if (!commands->deprecated)
        {
          if (commands->fn != NULL)
            {
              g_string_append_printf (summary, "\n  %s", commands->name);
              if (commands->description)
                g_string_append_printf (summary, "%*s%s", (int) (20 - strlen (commands->name)), "", _(commands->description));
            }
          else
            {
              g_string_append_printf (summary, "\n%s", _(commands->name));
            }
        }
      commands++;
    }

  g_option_context_set_summary (context, summary->str);

  g_string_free (summary, TRUE);

  return context;
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

  if (!(flags & FLATPAK_BUILTIN_FLAG_NO_DIR) &&
      !(flags & FLATPAK_BUILTIN_FLAG_ONE_DIR) &&
      !(flags & FLATPAK_BUILTIN_FLAG_STANDARD_DIRS) &&
      !(flags & FLATPAK_BUILTIN_FLAG_ALL_DIRS))
    g_assert_not_reached ();

  if (flags & FLATPAK_BUILTIN_FLAG_NO_DIR &&
      (flags & FLATPAK_BUILTIN_FLAG_ONE_DIR ||
       flags & FLATPAK_BUILTIN_FLAG_STANDARD_DIRS ||
       flags & FLATPAK_BUILTIN_FLAG_ALL_DIRS))
    g_assert_not_reached ();

  if (flags & FLATPAK_BUILTIN_FLAG_ONE_DIR &&
      (flags & FLATPAK_BUILTIN_FLAG_STANDARD_DIRS ||
       flags & FLATPAK_BUILTIN_FLAG_ALL_DIRS))
    g_assert_not_reached ();

  if (flags & FLATPAK_BUILTIN_FLAG_STANDARD_DIRS &&
      flags & FLATPAK_BUILTIN_FLAG_ALL_DIRS)
    g_assert_not_reached ();

  if (!(flags & FLATPAK_BUILTIN_FLAG_NO_DIR))
    g_option_context_add_main_entries (context, user_entries, NULL);

  if (main_entries != NULL)
    g_option_context_add_main_entries (context, main_entries, NULL);

  g_option_context_add_main_entries (context, global_entries, NULL);

  if (!g_option_context_parse (context, argc, argv, error))
    return FALSE;

  /* We never want verbose output in the complete case, that breaks completion */
  if (!is_in_complete)
    {
      if (opt_verbose > 0)
        g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);
      if (opt_verbose > 1)
        g_log_set_handler (G_LOG_DOMAIN "2", G_LOG_LEVEL_DEBUG, message_handler, NULL);

      if (opt_ostree_verbose)
        g_log_set_handler ("OSTree", G_LOG_LEVEL_DEBUG, message_handler, NULL);
    }

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
              g_ptr_array_add (dirs, flatpak_dir_get_user ());

              system_dirs = flatpak_dir_get_system_list (cancellable, error);
              if (system_dirs == NULL)
                return FALSE;

              for (i = 0; i < system_dirs->len; i++)
                {
                  FlatpakDir *dir = g_ptr_array_index (system_dirs, i);
                  g_ptr_array_add (dirs, g_object_ref (dir));
                }
            }
        }
      else /* FLATPAK_BUILTIN_FLAG_ONE_DIR */
        {
          FlatpakDir *dir;

          if (opt_system || (!opt_user && opt_installations == NULL))
            dir = flatpak_dir_get_system_default ();
          else if (opt_user)
            dir = flatpak_dir_get_user ();
          else if (opt_installations != NULL)
            {
              if (g_strv_length (opt_installations) > 1)
                return usage_error (context, _("The --installation option was used multiple times "
                                               "for a command that works on one installation"), error);
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
  g_autofree gchar *help = g_option_context_get_help (context, TRUE, NULL);

  g_printerr ("%s", help);
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
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

  command = extract_command (&argc, argv, &command_name);

  if (!command->fn)
    {
      GOptionContext *context;
      g_autofree char *help = NULL;

      context = flatpak_option_context_new_with_commands (commands);

      if (command_name != NULL)
        {
          g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       _("Unknown command '%s'"), command_name);
        }
      else
        {
          /* This will not return for some options (e.g. --version). */
          if (flatpak_option_context_parse (context, empty_entries, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, &error))
            {
              g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   _("No command specified"));
            }
        }

      help = g_option_context_get_help (context, FALSE, NULL);
      g_printerr ("%s", help);

      g_option_context_free (context);

      goto out;
    }

  prgname = g_strdup_printf ("%s %s", g_get_prgname (), command_name);
  g_set_prgname (prgname);

  if (!command->fn (argc, argv, cancellable, &error))
    goto out;

  success = TRUE;
out:
  g_assert (success || error);

  if (error)
    {
      g_propagate_error (res_error, error);
      return 1;
    }
  return 0;
}

static int
complete (int    argc,
          char **argv)
{
  FlatpakCommand *command;
  FlatpakCompletion *completion;
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

int
main (int    argc,
      char **argv)
{
  GError *error = NULL;
  g_autofree const char *old_env = NULL;
  int ret;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_WARNING, message_handler, NULL);

  g_set_prgname (argv[0]);

  /* Avoid weird recursive type initialization deadlocks from libsoup */
  g_type_ensure (G_TYPE_SOCKET);

  /* avoid gvfs (http://bugzilla.gnome.org/show_bug.cgi?id=526454) */
  old_env = g_strdup (g_getenv ("GIO_USE_VFS"));
  g_setenv ("GIO_USE_VFS", "local", TRUE);
  g_vfs_get_default ();
  if (old_env)
    g_setenv ("GIO_USE_VFS", old_env, TRUE);
  else
    g_unsetenv ("GIO_USE_VFS");

  if (argc >= 4 && strcmp (argv[1], "complete") == 0)
    return complete (argc, argv);

  flatpak_migrate_from_xdg_app ();

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
      g_error_free (error);
    }

  return ret;
}
