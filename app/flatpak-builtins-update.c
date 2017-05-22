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
#include <unistd.h>
#include <string.h>

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-transaction.h"
#include "flatpak-utils.h"
#include "flatpak-error.h"

static char *opt_arch;
static char *opt_commit;
static char **opt_subpaths;
static gboolean opt_force_remove;
static gboolean opt_no_pull;
static gboolean opt_no_deploy;
static gboolean opt_no_related;
static gboolean opt_no_deps;
static gboolean opt_no_static_deltas;
static gboolean opt_runtime;
static gboolean opt_app;
static gboolean opt_appstream;
static gboolean opt_yes;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to update for"), N_("ARCH") },
  { "commit", 0, 0, G_OPTION_ARG_STRING, &opt_commit, N_("Commit to deploy"), N_("COMMIT") },
  { "force-remove", 0, 0, G_OPTION_ARG_NONE, &opt_force_remove, N_("Remove old files even if running"), NULL },
  { "no-pull", 0, 0, G_OPTION_ARG_NONE, &opt_no_pull, N_("Don't pull, only update from local cache"), NULL },
  { "no-deploy", 0, 0, G_OPTION_ARG_NONE, &opt_no_deploy, N_("Don't deploy, only download to local cache"), NULL },
  { "no-related", 0, 0, G_OPTION_ARG_NONE, &opt_no_related, N_("Don't update related refs"), NULL},
  { "no-deps", 0, 0, G_OPTION_ARG_NONE, &opt_no_deps, N_("Don't verify/install runtime dependencies"), NULL },
  { "no-static-deltas", 0, 0, G_OPTION_ARG_NONE, &opt_no_static_deltas, N_("Don't use static deltas"), NULL },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Look for runtime with the specified name"), NULL },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, N_("Look for app with the specified name"), NULL },
  { "appstream", 0, 0, G_OPTION_ARG_NONE, &opt_appstream, N_("Update appstream for remote"), NULL },
  { "subpath", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_subpaths, N_("Only update this subpath"), N_("PATH") },
  { "assumeyes", 'y', 0, G_OPTION_ARG_NONE, &opt_yes, N_("Automatically answer yes for all questions"), NULL },
  { NULL }
};

static gboolean
update_appstream (FlatpakDir *dir, const char *remote, GCancellable *cancellable, GError **error)
{
  gboolean changed;
  gboolean res;

  if (opt_arch == NULL)
    opt_arch = (char *)flatpak_get_arch ();

  if (remote == NULL)
    {
      g_auto(GStrv) remotes = NULL;
      int i;

      remotes = flatpak_dir_list_remotes (dir, cancellable, error);
      if (remotes == NULL)
        return FALSE;

      for (i = 0; remotes[i] != NULL; i++)
        {
          g_autoptr(GError) local_error = NULL;
          FlatpakTerminalProgress terminal_progress = { 0 };

          if (flatpak_dir_get_remote_disabled (dir, remotes[i]))
            continue;

          g_print (_("Updating appstream for remote %s\n"), remotes[i]);
          g_autoptr(OstreeAsyncProgress) progress = flatpak_progress_new (flatpak_terminal_progress_cb, &terminal_progress);
          if (!flatpak_dir_update_appstream (dir, remotes[i], opt_arch, &changed,
                                             progress, cancellable, &local_error))
            g_printerr ("Error updating: %s\n", local_error->message);
          ostree_async_progress_finish (progress);
          flatpak_terminal_progress_end (&terminal_progress);
        }
    }
  else
    {
      FlatpakTerminalProgress terminal_progress = { 0 };
      g_autoptr(OstreeAsyncProgress) progress = flatpak_progress_new (flatpak_terminal_progress_cb, &terminal_progress);
      res = flatpak_dir_update_appstream (dir, remote, opt_arch, &changed,
                                          progress, cancellable, error);
      ostree_async_progress_finish (progress);
      if (!res)
        return FALSE;
      flatpak_terminal_progress_end (&terminal_progress);
    }

  return TRUE;
}

gboolean
flatpak_builtin_update (int           argc,
                        char        **argv,
                        GCancellable *cancellable,
                        GError      **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  char **prefs = NULL;
  int i, j, n_prefs;
  const char *default_branch = NULL;
  FlatpakKinds kinds;
  g_autoptr(FlatpakTransaction) transaction = NULL;

  context = g_option_context_new (_("[REF...] - Update applications or runtimes"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (opt_appstream)
    return update_appstream (dir, argc >= 2 ? argv[1] : NULL, cancellable, error);

  prefs = &argv[1];
  n_prefs = argc - 1;

  /* Backwards compat for old "REPOSITORY NAME [BRANCH]" argument version */
  if (argc == 3 && looks_like_branch (argv[2]))
    {
      default_branch = argv[2];
      n_prefs = 1;
    }

  transaction = flatpak_transaction_new (dir, opt_yes, opt_no_pull, opt_no_deploy,
                                         opt_no_static_deltas, !opt_no_deps, !opt_no_related);
  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);

  g_print ("Looking for updates...\n");

  for (j = 0; j == 0 || j < n_prefs; j++)
    {
      const char *pref = NULL;
      FlatpakKinds matched_kinds;
      g_autofree char *id = NULL;
      g_autofree char *arch = NULL;
      g_autofree char *branch = NULL;
      gboolean found = FALSE;

      if (n_prefs == 0)
        {
          matched_kinds = kinds;
        }
      else
        {
          pref = prefs[j];
          if (!flatpak_split_partial_ref_arg (pref, kinds, opt_arch, default_branch,
                                              &matched_kinds, &id, &arch, &branch, error))
            return FALSE;
        }

      if (kinds & FLATPAK_KINDS_APP)
        {
          g_auto(GStrv) refs = NULL;

          if (!flatpak_dir_list_refs (dir, "app", &refs,
                                      cancellable,
                                      error))
            return FALSE;

          for (i = 0; refs != NULL && refs[i] != NULL; i++)
            {
              g_auto(GStrv) parts = flatpak_decompose_ref (refs[i], error);
              if (parts == NULL)
                return FALSE;

              if (id != NULL && strcmp (parts[1], id) != 0)
                continue;

              if (arch != NULL && strcmp (parts[2], arch) != 0)
                continue;

              if (branch != NULL && strcmp (parts[3], branch) != 0)
                continue;

              found = TRUE;
              if (!flatpak_transaction_add_update (transaction, refs[i], (const char **)opt_subpaths, opt_commit, error))
                return FALSE;
            }
        }

      if (kinds & FLATPAK_KINDS_RUNTIME)
        {
          g_auto(GStrv) refs = NULL;

          if (!flatpak_dir_list_refs (dir, "runtime", &refs,
                                      cancellable,
                                      error))
            return FALSE;

          for (i = 0; refs != NULL && refs[i] != NULL; i++)
            {
              g_auto(GStrv) parts = flatpak_decompose_ref (refs[i], error);

              if (parts == NULL)
                return FALSE;

              if (id != NULL && strcmp (parts[1], id) != 0)
                continue;

              if (arch != NULL && strcmp (parts[2], arch) != 0)
                continue;

              if (branch != NULL && strcmp (parts[3], branch) != 0)
                continue;

              found = TRUE;
              if (!flatpak_transaction_add_update (transaction, refs[i], (const char **)opt_subpaths, opt_commit, error))
                return FALSE;
            }
        }

      if (n_prefs > 0 && !found)
        {
          g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                       "%s not installed", pref);
          return FALSE;
        }
    }

  if (!flatpak_transaction_update_metadata (transaction, n_prefs == 0, cancellable, error))
    return FALSE;

  if (!flatpak_transaction_run (transaction, FALSE, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_complete_update (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  FlatpakKinds kinds;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv, 0, &dir, NULL, NULL))
    return FALSE;

  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);

  switch (completion->argc)
    {
    case 0:
    default: /* REF */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);
      flatpak_complete_partial_ref (completion, kinds, opt_arch, dir, NULL);
      break;
    }

  return TRUE;
}
