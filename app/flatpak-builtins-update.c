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
#include "flatpak-cli-transaction.h"
#include "flatpak-quiet-transaction.h"
#include "flatpak-utils-private.h"
#include "flatpak-error.h"

static char *opt_arch;
static char *opt_commit;
static char **opt_subpaths;
static char **opt_sideload_repos;
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
static gboolean opt_noninteractive;

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
  { "noninteractive", 0, 0, G_OPTION_ARG_NONE, &opt_noninteractive, N_("Produce minimal output and don't ask questions"), NULL },
  /* Translators: A sideload is when you install from a local USB drive rather than the Internet. */
  { "sideload-repo", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_sideload_repos, N_("Use this local repo for sideloads"), N_("PATH") },
  { NULL }
};

gboolean
flatpak_builtin_update (int           argc,
                        char        **argv,
                        GCancellable *cancellable,
                        GError      **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  char **prefs = NULL;
  int i, j, k, n_prefs;
  const char *default_branch = NULL;
  FlatpakKinds kinds;
  g_autoptr(GPtrArray) transactions = NULL;
  gboolean has_updates;

  context = g_option_context_new (_("[REF…] - Update applications or runtimes"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, cancellable, error))
    return FALSE;

  if (opt_appstream)
    {
      if (!update_appstream (dirs, argc >= 2 ? argv[1] : NULL, opt_arch, 0, FALSE, cancellable, error))
        return FALSE;

      return TRUE;
    }

  if (opt_noninteractive)
    opt_yes = TRUE; /* Implied */

  prefs = &argv[1];
  n_prefs = argc - 1;

  /* Backwards compat for old "NAME [BRANCH]" argument version */
  if (argc == 3 && looks_like_branch (argv[2]))
    {
      default_branch = argv[2];
      n_prefs = 1;
    }

  /* It doesn't make sense to use the same commit for more than one thing */
  if (opt_commit && n_prefs != 1)
    return usage_error (context, _("With --commit, only one REF may be specified"), error);

  transactions = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

  /* Walk through the array backwards so we can safely remove */
  for (k = dirs->len; k > 0; k--)
    {
      FlatpakTransaction *transaction;

      FlatpakDir *dir = g_ptr_array_index (dirs, k - 1);
      OstreeRepo *repo = flatpak_dir_get_repo (dir);
      if (repo == NULL)
        {
          g_ptr_array_remove_index (dirs, k - 1);
          continue;
        }

      if (opt_noninteractive)
        transaction = flatpak_quiet_transaction_new (dir, error);
      else
        transaction = flatpak_cli_transaction_new (dir, opt_yes, FALSE, opt_arch != NULL, error);
      if (transaction == NULL)
        return FALSE;

      flatpak_transaction_set_no_pull (transaction, opt_no_pull);
      flatpak_transaction_set_no_deploy (transaction, opt_no_deploy);
      flatpak_transaction_set_disable_static_deltas (transaction, opt_no_static_deltas);
      flatpak_transaction_set_disable_dependencies (transaction, opt_no_deps);
      flatpak_transaction_set_disable_related (transaction, opt_no_related);
      if (opt_arch)
        flatpak_transaction_set_default_arch (transaction, opt_arch);

      for (int i = 0; opt_sideload_repos != NULL && opt_sideload_repos[i] != NULL; i++)
        flatpak_transaction_add_sideload_repo (transaction, opt_sideload_repos[i]);

      g_ptr_array_insert (transactions, 0, transaction);
    }

  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);

  if (!opt_noninteractive)
    g_print (_("Looking for updates…\n"));

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

      for (k = 0; k < dirs->len; k++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, k);
          FlatpakTransaction *transaction = g_ptr_array_index (transactions, k);

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
                  if (flatpak_transaction_add_update (transaction, refs[i], (const char **) opt_subpaths, opt_commit, error))
                    continue;

                  if (g_error_matches (*error, FLATPAK_ERROR, FLATPAK_ERROR_REMOTE_NOT_FOUND))
                    {
                      g_printerr (_("Unable to update %s: %s\n"), refs[i], (*error)->message);
                      g_clear_error (error);
                    }
                  else
                    {
                      return FALSE;
                    }
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
                  if (flatpak_transaction_add_update (transaction, refs[i], (const char **) opt_subpaths, opt_commit, error))
                    continue;

                  if (g_error_matches (*error, FLATPAK_ERROR, FLATPAK_ERROR_REMOTE_NOT_FOUND))
                    {
                      g_printerr (_("Unable to update %s: %s\n"), refs[i], (*error)->message);
                      g_clear_error (error);
                    }
                  else
                    {
                      return FALSE;
                    }
                }
            }
        }

      if (n_prefs > 0 && !found)
        {
          g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                       "%s not installed", pref);
          return FALSE;
        }
    }

  /* Add uninstall operations for any runtimes that are unused and EOL.
   * Strictly speaking these are not updates but "update" is the command people
   * run to keep their system maintained. It would be possible to do this in
   * the transaction that updates them to being EOL, but doing it here seems
   * more future-proof since we may want to use additional conditions to
   * determine if something is unused. See
   * https://github.com/flatpak/flatpak/issues/3799
   */
  if ((kinds & FLATPAK_KINDS_RUNTIME) && n_prefs == 0 && !opt_no_deps)
    {
      for (k = 0; k < dirs->len; k++)
        {
          FlatpakTransaction *transaction = g_ptr_array_index (transactions, k);
          flatpak_transaction_set_include_unused_uninstall_ops (transaction, TRUE);
        }
    }

  has_updates = FALSE;

  for (k = 0; k < dirs->len; k++)
    {
      FlatpakTransaction *transaction = g_ptr_array_index (transactions, k);

      if (flatpak_transaction_is_empty (transaction))
        continue;

      if (!flatpak_transaction_run (transaction, cancellable, error))
        {
          if (g_error_matches (*error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED))
            g_clear_error (error);  /* Don't report on stderr */

          return FALSE;
        }

      if (!flatpak_transaction_is_empty (transaction))
        has_updates = TRUE;
    }

  if (!has_updates)
    g_print (_("Nothing to do.\n"));

  if (n_prefs == 0)
    {
      if (!update_appstream (dirs, NULL, opt_arch, FLATPAK_APPSTREAM_TTL, TRUE, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_complete_update (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  FlatpakKinds kinds;
  int i;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, NULL, NULL))
    return FALSE;

  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);

  switch (completion->argc)
    {
    case 0:
    default: /* REF */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          flatpak_complete_partial_ref (completion, kinds, opt_arch, dir, NULL);
        }

      break;
    }

  return TRUE;
}
