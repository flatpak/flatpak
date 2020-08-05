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
#include "flatpak-utils-private.h"
#include "flatpak-cli-transaction.h"
#include "flatpak-quiet-transaction.h"
#include <flatpak-dir-private.h>
#include <flatpak-installation-private.h>
#include "flatpak-error.h"

static char *opt_arch;
static gboolean opt_keep_ref;
static gboolean opt_force_remove;
static gboolean opt_no_related;
static gboolean opt_runtime;
static gboolean opt_app;
static gboolean opt_all;
static gboolean opt_yes;
static gboolean opt_unused;
static gboolean opt_delete_data;
static gboolean opt_noninteractive;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to uninstall"), N_("ARCH") },
  { "keep-ref", 0, 0, G_OPTION_ARG_NONE, &opt_keep_ref, N_("Keep ref in local repository"), NULL },
  { "no-related", 0, 0, G_OPTION_ARG_NONE, &opt_no_related, N_("Don't uninstall related refs"), NULL },
  { "force-remove", 0, 0, G_OPTION_ARG_NONE, &opt_force_remove, N_("Remove files even if running"), NULL },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Look for runtime with the specified name"), NULL },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, N_("Look for app with the specified name"), NULL },
  { "all", 0, 0, G_OPTION_ARG_NONE, &opt_all, N_("Uninstall all"), NULL },
  { "unused", 0, 0, G_OPTION_ARG_NONE, &opt_unused, N_("Uninstall unused"), NULL },
  { "delete-data", 0, 0, G_OPTION_ARG_NONE, &opt_delete_data, N_("Delete app data"), NULL },
  { "assumeyes", 'y', 0, G_OPTION_ARG_NONE, &opt_yes, N_("Automatically answer yes for all questions"), NULL },
  { "noninteractive", 0, 0, G_OPTION_ARG_NONE, &opt_noninteractive, N_("Produce minimal output and don't ask questions"), NULL },
  { NULL }
};

typedef struct
{
  FlatpakDir *dir;
  GHashTable *refs_hash;
  GPtrArray  *refs;
} UninstallDir;

static UninstallDir *
uninstall_dir_new (FlatpakDir *dir)
{
  UninstallDir *udir = g_new0 (UninstallDir, 1);

  udir->dir = g_object_ref (dir);
  udir->refs = g_ptr_array_new_with_free_func (g_free);
  udir->refs_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  return udir;
}

static void
uninstall_dir_free (UninstallDir *udir)
{
  g_object_unref (udir->dir);
  g_hash_table_unref (udir->refs_hash);
  g_ptr_array_unref (udir->refs);
  g_free (udir);
}

static void
uninstall_dir_add_ref (UninstallDir *udir,
                       const char   *ref)
{
  if (g_hash_table_insert (udir->refs_hash, g_strdup (ref), NULL))
    g_ptr_array_add (udir->refs, g_strdup (ref));
}

static UninstallDir *
uninstall_dir_ensure (GHashTable *uninstall_dirs,
                      FlatpakDir *dir)
{
  UninstallDir *udir;

  udir = g_hash_table_lookup (uninstall_dirs, dir);
  if (udir == NULL)
    {
      udir = uninstall_dir_new (dir);
      g_hash_table_insert (uninstall_dirs, udir->dir, udir);
    }

  return udir;
}

static gboolean
flatpak_delete_data (gboolean    yes_opt,
                     const char *app_id,
                     GError    **error)
{
  g_autofree char *path = g_build_filename (g_get_home_dir (), ".var", "app", app_id, NULL);
  g_autoptr(GFile) file = g_file_new_for_path (path);

  if (!yes_opt &&
      !flatpak_yes_no_prompt (FALSE, _("Delete data for %s?"), app_id))
    return TRUE;

  if (g_file_query_exists (file, NULL))
    {
      if (!flatpak_rm_rf (file, NULL, error))
        return FALSE;
    }

  if (!reset_permissions_for_app (app_id, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_builtin_uninstall (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  char **prefs = NULL;
  int i, j, k, n_prefs;
  const char *default_branch = NULL;
  FlatpakKinds kinds;
  g_autoptr(GHashTable) uninstall_dirs = NULL;

  context = g_option_context_new (_("[REF…] - Uninstall an application"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, cancellable, error))
    return FALSE;

  if (argc < 2 && !opt_all && !opt_unused && !opt_delete_data)
    return usage_error (context, _("Must specify at least one REF, --unused, --all or --delete-data"), error);

  if (argc >= 2 && opt_all)
    return usage_error (context, _("Must not specify REFs when using --all"), error);

  if (argc >= 2 && opt_unused)
    return usage_error (context, _("Must not specify REFs when using --unused"), error);

  prefs = &argv[1];
  n_prefs = argc - 1;

  /* Backwards compat for old "NAME [BRANCH]" argument version */
  if (argc == 3 && flatpak_is_valid_name (argv[1], NULL) && looks_like_branch (argv[2]))
    {
      default_branch = argv[2];
      n_prefs = 1;
    }

  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);
  uninstall_dirs = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) uninstall_dir_free);

  if (opt_all)
    {
      for (j = 0; j < dirs->len; j++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, j);
          UninstallDir *udir;
          g_auto(GStrv) app_refs = NULL;
          g_auto(GStrv) runtime_refs = NULL;

          flatpak_dir_maybe_ensure_repo (dir, NULL, NULL);
          if (flatpak_dir_get_repo (dir) == NULL)
            continue;

          udir = uninstall_dir_ensure (uninstall_dirs, dir);

          if (flatpak_dir_list_refs (dir, "app", &app_refs, NULL, NULL))
            {
              for (k = 0; app_refs[k] != NULL; k++)
                uninstall_dir_add_ref (udir, app_refs[k]);
            }

          if (flatpak_dir_list_refs (dir, "runtime", &runtime_refs, NULL, NULL))
            {
              for (k = 0; runtime_refs[k] != NULL; k++)
                uninstall_dir_add_ref (udir, runtime_refs[k]);
            }
        }
    }
  else if (opt_unused)
    {
      gboolean found_something_to_uninstall = FALSE;

      for (j = 0; j < dirs->len; j++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, j);
          g_autoptr(FlatpakInstallation) installation = NULL;
          UninstallDir *udir;
          g_autoptr(GPtrArray) unused = NULL;
          g_autoptr(GPtrArray) pinned = NULL;

          flatpak_dir_maybe_ensure_repo (dir, NULL, NULL);
          if (flatpak_dir_get_repo (dir) == NULL)
            continue;

          installation = flatpak_installation_new_for_dir (dir, NULL, NULL);
          pinned = flatpak_installation_list_pinned_refs (installation, opt_arch, cancellable, error);
          if (pinned == NULL)
            return FALSE;

          if (pinned->len > 0)
            {
              g_print (_("\nThese runtimes in installation '%s' are pinned and won't be removed; see flatpak-pin(1):\n"),
                       flatpak_dir_get_name_cached (dir));
              for (i = 0; i < pinned->len; i++)
                {
                  FlatpakInstalledRef *rref = g_ptr_array_index (pinned, i);
                  g_autofree char *ref = flatpak_ref_format_ref (FLATPAK_REF (rref));
                  g_print ("  %s\n", ref);
                }
            }

          udir = uninstall_dir_ensure (uninstall_dirs, dir);

          unused = flatpak_installation_list_unused_refs (installation, opt_arch, cancellable, error);
          if (unused == NULL)
            return FALSE;

          for (i = 0; i < unused->len; i++)
            {
              FlatpakInstalledRef *rref = g_ptr_array_index (unused, i);
              g_autofree char *ref = flatpak_ref_format_ref (FLATPAK_REF (rref));

              uninstall_dir_add_ref (udir, ref);
              found_something_to_uninstall = TRUE;
            }

          if (udir->refs->len > 0)
            found_something_to_uninstall = TRUE;
        }

      if (!found_something_to_uninstall)
        {
          g_print (_("Nothing unused to uninstall\n"));
          return TRUE;
        }
    }
  else
    {
      for (j = 0; j < n_prefs; j++)
        {
          const char *pref = NULL;
          FlatpakKinds matched_kinds;
          g_autofree char *id = NULL;
          g_autofree char *arch = NULL;
          g_autofree char *branch = NULL;
          g_autoptr(GError) local_error = NULL;
          g_autoptr(GPtrArray) ref_dir_pairs = NULL;
          UninstallDir *udir = NULL;
          gboolean found_exact_name_match = FALSE;
          g_autoptr(GPtrArray) chosen_pairs = NULL;

          pref = prefs[j];

          flatpak_split_partial_ref_arg_novalidate (pref, kinds, opt_arch, default_branch,
                                                    &matched_kinds, &id, &arch, &branch);

          /* We used _novalidate so that the id can be partial, but we can still validate the branch */
          if (branch != NULL && !flatpak_is_valid_branch (branch, &local_error))
            return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF, _("Invalid branch %s: %s"), branch, local_error->message);

          ref_dir_pairs = g_ptr_array_new_with_free_func ((GDestroyNotify) ref_dir_pair_free);
          for (k = 0; k < dirs->len; k++)
            {
              FlatpakDir *dir = g_ptr_array_index (dirs, k);
              g_auto(GStrv) refs = NULL;
              char **iter;

              refs = flatpak_dir_find_installed_refs (dir, id, branch, arch, kinds,
                                                      FIND_MATCHING_REFS_FLAGS_FUZZY, error);
              if (refs == NULL)
                return FALSE;
              else if (g_strv_length (refs) == 0)
                continue;

              for (iter = refs; iter && *iter; iter++)
                {
                  const char *ref = *iter;
                  g_auto(GStrv) parts = NULL;
                  RefDirPair *pair;

                  parts = flatpak_decompose_ref (ref, NULL);
                  g_assert (parts != NULL);
                  if (g_strcmp0 (id, parts[1]) == 0)
                    found_exact_name_match = TRUE;

                  pair = ref_dir_pair_new (ref, dir);
                  g_ptr_array_add (ref_dir_pairs, pair);
                }
            }

          if (ref_dir_pairs->len == 0)
            {
              if (n_prefs == 1)
                {
                  g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                               _("%s/%s/%s not installed"),
                               id ? id : "*unspecified*",
                               arch ? arch : "*unspecified*",
                               branch ? branch : "*unspecified*");
                  return FALSE;
                }

              g_printerr (_("Warning: %s is not installed\n"), pref);
              continue;
            }

          /* Don't show fuzzy matches if an exact match was found in any installation */
          if (found_exact_name_match)
            {
              /* Walk through the array backwards so we can safely remove */
              for (guint i = ref_dir_pairs->len; i > 0; i--)
                {
                  RefDirPair *pair = g_ptr_array_index (ref_dir_pairs, i - 1);
                  g_auto(GStrv) parts = NULL;

                  parts = flatpak_decompose_ref (pair->ref, NULL);
                  if (g_strcmp0 (id, parts[1]) != 0)
                    g_ptr_array_remove_index (ref_dir_pairs, i - 1);
                }
            }

          chosen_pairs = g_ptr_array_new ();

          if (!flatpak_resolve_matching_installed_refs (opt_yes, FALSE, ref_dir_pairs, id, chosen_pairs, error))
            return FALSE;

          for (i = 0; i < chosen_pairs->len; i++)
            {
              RefDirPair *pair = g_ptr_array_index (chosen_pairs, i);
              udir = uninstall_dir_ensure (uninstall_dirs, pair->dir);
              uninstall_dir_add_ref (udir, pair->ref);
            }
        }
    }

  GLNX_HASH_TABLE_FOREACH_V (uninstall_dirs, UninstallDir *, udir)
  {
    g_autoptr(FlatpakTransaction) transaction = NULL;

    if (opt_noninteractive)
      transaction = flatpak_quiet_transaction_new (udir->dir, error);
    else
      transaction = flatpak_cli_transaction_new (udir->dir, opt_yes, TRUE, opt_arch != NULL, error);
    if (transaction == NULL)
      return FALSE;

    flatpak_transaction_set_disable_prune (transaction, opt_keep_ref);
    flatpak_transaction_set_force_uninstall (transaction, opt_force_remove);
    flatpak_transaction_set_disable_related (transaction, opt_no_related);

    /* This disables the remote metadata update, since uninstall is a local-only op */
    flatpak_transaction_set_no_pull (transaction, TRUE);

    for (i = 0; i < udir->refs->len; i++)
      {
        const char *ref = (char *) g_ptr_array_index (udir->refs, i);

        if (!flatpak_transaction_add_uninstall (transaction, ref, error))
          return FALSE;
      }

    if (!flatpak_transaction_run (transaction, cancellable, error))
      {
        if (g_error_matches (*error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED))
          g_clear_error (error); /* Don't report on stderr */

        return FALSE;
      }

    if (opt_delete_data)
      {
        for (i = 0; i < udir->refs->len; i++)
          {
            const char *ref = (char *) g_ptr_array_index (udir->refs, i);
            g_auto(GStrv) pref = flatpak_decompose_ref (ref, NULL);

            if (!flatpak_delete_data (opt_yes, pref[1], error))
              return FALSE;
          }
      }
  }

  if (opt_delete_data && argc < 2)
    {
      g_autoptr(GFileEnumerator) enumerator = NULL;
      g_autofree char *path = g_build_filename (g_get_home_dir (), ".var", "app", NULL);
      g_autoptr(GFile) app_dir = g_file_new_for_path (path);

      enumerator = g_file_enumerate_children (app_dir,
                                              G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                              G_FILE_QUERY_INFO_NONE,
                                              cancellable, error);
      if (!enumerator)
        return FALSE;

      while (TRUE)
        {
          GFileInfo *info;
          GFile *file;
          g_autofree char *ref = NULL;

          if (!g_file_enumerator_iterate (enumerator, &info, &file, cancellable, error))
            return FALSE;

          if (info == NULL)
            break;

          if (g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY)
            continue;

          ref = flatpak_find_current_ref (g_file_info_get_name (info), cancellable, NULL);
          if (ref)
            continue;

          if (!flatpak_delete_data (opt_yes, g_file_info_get_name (info), error))
            return FALSE;
        }
    }

  return TRUE;
}

gboolean
flatpak_complete_uninstall (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  int i;
  FlatpakKinds kinds;

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
