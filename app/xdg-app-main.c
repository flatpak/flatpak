/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#include <gio/gio.h>
#include "libglnx/libglnx.h"
#include "libgsystem.h"

#include "xdg-app-builtins.h"

static gboolean opt_verbose;
static gboolean opt_version;
static gboolean opt_user;

typedef struct {
  const char *name;
  gboolean (*fn) (int argc, char **argv, GCancellable *cancellable, GError **error);
  const char *description;
  gboolean deprecated;
} XdgAppCommand;

static XdgAppCommand commands[] = {
  { " Manage installed apps and runtimes" },
  { "install", xdg_app_builtin_install, "Install an application or runtime from a remote"},
  { "update", xdg_app_builtin_update, "Update an installed application or runtime"},
  { "uninstall", xdg_app_builtin_uninstall, "Uninstall an installed application or runtime" },
  { "list", xdg_app_builtin_list, "List installed apps and/or runtimes" },
  { "info", xdg_app_builtin_info, "Show info for installed app or runtime" },

  { "\n Running applications" },
  { "run", xdg_app_builtin_run, "Run an application" },
  { "override", xdg_app_builtin_override, "Override permissions for an application" },
  { "export-file", xdg_app_builtin_export_file, "Grant an application access to a specific file" },
  { "make-current", xdg_app_builtin_make_current_app, "Specify default version to run" },
  { "enter", xdg_app_builtin_enter, "Enter the namespace of a running application" },

  { "\n Manage remote repositories" },
  { "remote-add", xdg_app_builtin_add_remote, "Add a new remote repository (by URL)" },
  { "remote-modify", xdg_app_builtin_modify_remote, "Modify properties of a configured remote" },
  { "remote-delete", xdg_app_builtin_delete_remote, "Delete a configured remote" },
  { "remote-list", xdg_app_builtin_list_remotes, "List all configured remotes"  },
  { "remote-ls", xdg_app_builtin_ls_remote, "List contents of a configured remote" },

  { "\n Build applications" },
  { "build-init", xdg_app_builtin_build_init, "Initialize a directory for building" },
  { "build", xdg_app_builtin_build, "Run a build command inside the build dir" },
  { "build-finish", xdg_app_builtin_build_finish, "Finish a build dir for export" },
  { "build-export", xdg_app_builtin_build_export, "Export a build dir to a repository" },
  { "build-bundle", xdg_app_builtin_build_bundle, "Create a bundle file from a build directory" },
  { "build-sign", xdg_app_builtin_build_sign, "Sign an application or runtime" },
  { "build-update-repo", xdg_app_builtin_build_update_repo, "Update the summary file in a repository" },

  /* Deprecated old names */
  { "install-runtime", xdg_app_builtin_install_runtime, NULL, TRUE },
  { "install-app", xdg_app_builtin_install_app, NULL, TRUE },
  { "update-app", xdg_app_builtin_update_app, NULL, TRUE },
  { "update-runtime", xdg_app_builtin_update_runtime, NULL, TRUE },
  { "uninstall-runtime", xdg_app_builtin_uninstall_runtime, NULL, TRUE },
  { "uninstall-app", xdg_app_builtin_uninstall_app, NULL, TRUE },
  { "install-bundle", xdg_app_builtin_install_bundle, NULL, TRUE },
  { "make-app-current", xdg_app_builtin_make_current_app, NULL, TRUE },
  { "add-remote", xdg_app_builtin_add_remote, NULL, TRUE },
  { "delete-remote", xdg_app_builtin_delete_remote, NULL, TRUE },
  { "modify-remote", xdg_app_builtin_modify_remote, NULL, TRUE },
  { "ls-remote", xdg_app_builtin_ls_remote, NULL, TRUE },
  { "list-remotes", xdg_app_builtin_list_remotes, NULL, TRUE },
  { "list-runtimes", xdg_app_builtin_list_runtimes, NULL, TRUE },
  { "list-apps", xdg_app_builtin_list_apps, NULL, TRUE },
  { NULL }
};

static GOptionEntry global_entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version information and exit", NULL },
  { NULL }
};

static GOptionEntry user_entries[] = {
  { "user", 0, 0, G_OPTION_ARG_NONE, &opt_user, "Work on user installations", NULL },
  { "system", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &opt_user, "Work on system-wide installations (default)", NULL },
  { NULL }
};

static void
message_handler (const gchar *log_domain,
                 GLogLevelFlags log_level,
                 const gchar *message,
                 gpointer user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    g_printerr ("XA: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

GOptionContext *
xdg_app_option_context_new_with_commands (XdgAppCommand *commands)
{
  GOptionContext *context;
  GString *summary;

  context = g_option_context_new ("COMMAND");

  summary = g_string_new ("Builtin Commands:");

  while (commands->name != NULL)
    {
      if (!commands->deprecated)
        {
          if (commands->fn != NULL)
            {
              g_string_append_printf (summary, "\n  %s", commands->name);
              if (commands->description)
                g_string_append_printf (summary, "%*s%s", (int)(20 - strlen (commands->name)), "", commands->description);
            }
          else
            g_string_append_printf (summary, "\n%s", commands->name);
        }
      commands++;
    }

  g_option_context_set_summary (context, summary->str);

  g_string_free (summary, TRUE);

  return context;
}

int
xdg_app_usage (XdgAppCommand *commands,
              gboolean is_error)
{
  GOptionContext *context;
  g_autofree char *help;

  context = xdg_app_option_context_new_with_commands (commands);

  g_option_context_add_main_entries (context, global_entries, NULL);

  help = g_option_context_get_help (context, FALSE, NULL);

  if (is_error)
    g_printerr ("%s", help);
  else
    g_print ("%s", help);

  g_option_context_free (context);

  return (is_error ? 1 : 0);
}

gboolean
xdg_app_option_context_parse (GOptionContext *context,
                              const GOptionEntry *main_entries,
                              int *argc,
                              char ***argv,
                              XdgAppBuiltinFlags flags,
                              XdgAppDir **out_dir,
                              GCancellable *cancellable,
                              GError **error)
{
  g_autoptr(XdgAppDir) dir = NULL;

  if (!(flags & XDG_APP_BUILTIN_FLAG_NO_DIR))
    g_option_context_add_main_entries (context, user_entries, NULL);

  if (main_entries != NULL)
    g_option_context_add_main_entries (context, main_entries, NULL);

  g_option_context_add_main_entries (context, global_entries, NULL);

  if (!g_option_context_parse (context, argc, argv, error))
    return FALSE;

  if (opt_version)
    {
      g_print ("%s\n", PACKAGE_STRING);
      exit (EXIT_SUCCESS);
    }

  if (!(flags & XDG_APP_BUILTIN_FLAG_NO_DIR))
    {
      dir = xdg_app_dir_get (opt_user);

      if (!xdg_app_dir_ensure_path (dir, cancellable, error))
        return FALSE;

      if (!(flags & XDG_APP_BUILTIN_FLAG_NO_REPO) &&
          !xdg_app_dir_ensure_repo (dir, cancellable,error))
        return FALSE;
    }

  if (opt_verbose)
    g_log_set_handler (NULL, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  if (out_dir)
    *out_dir = g_steal_pointer (&dir);

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

int
xdg_app_run (int    argc,
             char **argv,
             GError **res_error)
{
  XdgAppCommand *command;
  GError *error = NULL;
  GCancellable *cancellable = NULL;
  const char *command_name = NULL;
  g_autofree char *prgname = NULL;
  gboolean success = FALSE;
  int in, out;

  /*
   * Parse the global options. We rearrange the options as
   * necessary, in order to pass relevant options through
   * to the commands, but also have them take effect globally.
   */
  for (in = 1, out = 1; in < argc; in++, out++)
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

  argc = out;

  command = commands;
  while (command->name)
    {
      if (command->fn != NULL &&
          g_strcmp0 (command_name, command->name) == 0)
        break;
      command++;
    }

  if (!command->fn)
    {
      GOptionContext *context;
      g_autofree char *help;

      context = xdg_app_option_context_new_with_commands (commands);

      /* This will not return for some options (e.g. --version). */
      if (xdg_app_option_context_parse (context, NULL, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, &error))
        {
          if (command_name == NULL)
            {
              g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "No command specified");
            }
          else
            {
              g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Unknown command '%s'", command_name);
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

int
main (int    argc,
      char **argv)
{
  GError *error = NULL;
  g_autofree const char *old_env = NULL;
  int ret;

  setlocale (LC_ALL, "");

  g_log_set_handler (NULL, G_LOG_LEVEL_MESSAGE, message_handler, NULL);

  g_set_prgname (argv[0]);

  /* avoid gvfs (http://bugzilla.gnome.org/show_bug.cgi?id=526454) */
  old_env = g_strdup (g_getenv ("GIO_USE_VFS"));
  g_setenv ("GIO_USE_VFS", "local", TRUE);
  g_vfs_get_default ();
  if (old_env)
    g_setenv ("GIO_USE_VFS", old_env, TRUE);
  else
    g_unsetenv ("GIO_USE_VFS");

  ret = xdg_app_run (argc, argv, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    xdg_app_usage (commands, TRUE);

  if (error != NULL)
    {
      int is_tty = isatty (1);
      const char *prefix = "";
      const char *suffix = "";
      if (is_tty)
        {
          prefix = "\x1b[31m\x1b[1m"; /* red, bold */
          suffix = "\x1b[22m\x1b[0m"; /* bold off, color reset */
        }
      g_printerr ("%serror: %s%s\n", prefix, suffix, error->message);
      g_error_free (error);
    }

  return ret;
}
