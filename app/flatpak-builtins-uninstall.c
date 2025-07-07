/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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

#include "libglnx.h"

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
  GHashTable *runtime_app_map;
  GHashTable *extension_app_map;
  GPtrArray  *refs;
} UninstallDir;

static UninstallDir *
uninstall_dir_new (FlatpakDir *dir)
{
  UninstallDir *udir = g_new0 (UninstallDir, 1);

  udir->dir = g_object_ref (dir);
  udir->refs = g_ptr_array_new_with_free_func ((GDestroyNotify)flatpak_decomposed_unref);
  udir->refs_hash = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash, (GEqualFunc)flatpak_decomposed_equal, (GDestroyNotify)flatpak_decomposed_unref, NULL);

  return udir;
}

static void
uninstall_dir_free (UninstallDir *udir)
{
  g_object_unref (udir->dir);
  g_hash_table_unref (udir->refs_hash);
  g_clear_pointer (&udir->runtime_app_map, g_hash_table_unref);
  g_clear_pointer (&udir->extension_app_map, g_hash_table_unref);
  g_ptr_array_unref (udir->refs);
  g_free (udir);
}

static void
uninstall_dir_add_ref (UninstallDir *udir,
                       FlatpakDecomposed *ref)
{
  if (g_hash_table_insert (udir->refs_hash, flatpak_decomposed_ref (ref), NULL))
    g_ptr_array_add (udir->refs, flatpak_decomposed_ref (ref));
}

static void
uninstall_dir_remove_ref (UninstallDir *udir,
                          FlatpakDecomposed *ref)
{
  g_hash_table_remove (udir->refs_hash, ref);
  g_ptr_array_remove (udir->refs, ref);
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

static gboolean
confirm_runtime_removal (gboolean           yes_opt,
                         UninstallDir      *udir,
                         FlatpakDecomposed *ref)
{
  g_autoptr(GPtrArray) apps = NULL;
  g_autoptr(GError) local_error = NULL;
  g_autofree char *ref_name = NULL;
  const char *ref_branch;
  const char *on = "";
  const char *off = "";
  gboolean is_extension;

  if (flatpak_fancy_output ())
    {
      on = FLATPAK_ANSI_BOLD_ON;
      off = FLATPAK_ANSI_BOLD_OFF;
    }

  is_extension = flatpak_dir_is_runtime_extension (udir->dir, ref);
  if (is_extension)
    {
      apps = flatpak_dir_list_app_refs_with_runtime_extension (udir->dir,
                                                               &udir->runtime_app_map,
                                                               &udir->extension_app_map,
                                                               ref, NULL, &local_error);
      if (apps == NULL)
        g_info ("Unable to list apps using extension %s: %s\n",
                flatpak_decomposed_get_ref (ref), local_error->message);
    }
  else
    {
      apps = flatpak_dir_list_app_refs_with_runtime (udir->dir,
                                                     &udir->runtime_app_map,
                                                     ref, NULL, &local_error);
      if (apps == NULL)
        g_info ("Unable to list apps using runtime %s: %s\n",
                flatpak_decomposed_get_ref (ref), local_error->message);
    }

  if (apps == NULL || apps->len == 0)
    return TRUE;

  /* Exclude any apps that will be removed by the current transaction */
  for (guint i = 0; i < udir->refs->len; i++)
    {
      FlatpakDecomposed *uninstall_ref = g_ptr_array_index (udir->refs, i);
      guint j;

      if (flatpak_decomposed_is_runtime (uninstall_ref))
        continue;

      if (g_ptr_array_find_with_equal_func (apps, uninstall_ref,
                                            (GEqualFunc)flatpak_decomposed_equal, &j))
        g_ptr_array_remove_index_fast (apps, j);
    }

  if (apps->len == 0)
    return TRUE;

  ref_name = flatpak_decomposed_dup_id (ref);
  ref_branch = flatpak_decomposed_get_branch (ref);

  if (is_extension)
    g_print (_("Info: applications using the extension %s%s%s branch %s%s%s:\n"),
             on, ref_name, off, on, ref_branch, off);
  else
    g_print (_("Info: applications using the runtime %s%s%s branch %s%s%s:\n"),
             on, ref_name, off, on, ref_branch, off);

  g_print ("   ");
  for (guint i = 0; i < apps->len; i++)
    {
      FlatpakDecomposed *app_ref = g_ptr_array_index (apps, i);
      g_autofree char *id = flatpak_decomposed_dup_id (app_ref);
      if (i != 0)
        g_print (", ");
      g_print ("%s", id);
    }
  g_print ("\n");

  if (!yes_opt &&
      !flatpak_yes_no_prompt (FALSE, _("Really remove?")))
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

  context = g_option_context_new (_("[REF…] - Uninstall applications or runtimes"));
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

  if (opt_noninteractive)
    opt_yes = TRUE; /* Implied */

  prefs = &argv[1];
  n_prefs = argc - 1;

  /* Backwards compat for old "NAME [BRANCH]" argument version */
  if (argc == 3 && flatpak_is_valid_name (argv[1], -1, NULL) && looks_like_branch (argv[2]))
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
          g_autoptr(GPtrArray) refs = NULL;

          flatpak_dir_maybe_ensure_repo (dir, NULL, NULL);
          if (flatpak_dir_get_repo (dir) == NULL)
            continue;

          udir = uninstall_dir_ensure (uninstall_dirs, dir);

          refs = flatpak_dir_list_refs (dir, FLATPAK_KINDS_APP | FLATPAK_KINDS_RUNTIME, cancellable, error);
          if (refs == NULL)
            return FALSE;

          for (k = 0; k < refs->len; k++)
            {
              FlatpakDecomposed *ref = g_ptr_array_index (refs, k);
              uninstall_dir_add_ref (udir, ref);
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
          g_auto(GStrv) unused = NULL;
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
                  const char *ref = flatpak_ref_format_ref_cached (FLATPAK_REF (rref));
                  g_print ("  %s\n", ref);
                }
            }

          udir = uninstall_dir_ensure (uninstall_dirs, dir);

          unused = flatpak_dir_list_unused_refs (dir, opt_arch, NULL, NULL, NULL, FLATPAK_DIR_FILTER_NONE, cancellable, error);
          if (unused == NULL)
            return FALSE;

          for (char **iter = unused; iter && *iter; iter++)
            {
              const char *ref = *iter;
              g_autoptr(FlatpakDecomposed) d = flatpak_decomposed_new_from_ref (ref, NULL);

              if (d)
                {
                  uninstall_dir_add_ref (udir, d);
                  found_something_to_uninstall = TRUE;
                }
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
          g_autofree char *match_id = NULL;
          g_autofree char *match_arch = NULL;
          g_autofree char *match_branch = NULL;
          g_autoptr(GError) local_error = NULL;
          g_autoptr(GPtrArray) ref_dir_pairs = NULL;
          UninstallDir *udir = NULL;
          gboolean found_exact_name_match = FALSE;
          g_autoptr(GPtrArray) chosen_pairs = NULL;
          FindMatchingRefsFlags matching_refs_flags;

          pref = prefs[j];

          if (!flatpak_allow_fuzzy_matching (pref))
            matching_refs_flags = FIND_MATCHING_REFS_FLAGS_NONE;
          else
            matching_refs_flags = FIND_MATCHING_REFS_FLAGS_FUZZY;

          if (matching_refs_flags & FIND_MATCHING_REFS_FLAGS_FUZZY)
            {
              flatpak_split_partial_ref_arg_novalidate (pref, kinds, opt_arch, default_branch,
                                                        &matched_kinds, &match_id, &match_arch, &match_branch);

              /* We used _novalidate so that the id can be partial, but we can still validate the branch */
              if (match_branch != NULL && !flatpak_is_valid_branch (match_branch, -1, &local_error))
                return flatpak_fail_error (error, FLATPAK_ERROR_INVALID_REF,
                                           _("Invalid branch %s: %s"), match_branch, local_error->message);
            }
          else if (!flatpak_split_partial_ref_arg (pref, kinds, opt_arch, default_branch,
                                                   &matched_kinds, &match_id, &match_arch, &match_branch, error))
            {
              return FALSE;
            }

          ref_dir_pairs = g_ptr_array_new_with_free_func ((GDestroyNotify) ref_dir_pair_free);
          for (k = 0; k < dirs->len; k++)
            {
              FlatpakDir *dir = g_ptr_array_index (dirs, k);
              g_autoptr(GPtrArray)  refs = NULL;

              refs = flatpak_dir_find_installed_refs (dir, match_id, match_branch, match_arch, kinds,
                                                      matching_refs_flags, error);
              if (refs == NULL)
                return FALSE;
              else if (refs->len == 0)
                continue;

              for (int m = 0; m < refs->len; m++)
                {
                  FlatpakDecomposed *ref = g_ptr_array_index (refs, m);
                  RefDirPair *pair;

                  if (match_id != NULL && flatpak_decomposed_is_id (ref, match_id))
                    found_exact_name_match = TRUE;

                  pair = ref_dir_pair_new (ref, dir);
                  g_ptr_array_add (ref_dir_pairs, pair);
                }
            }

          if (ref_dir_pairs->len == 0)
            {
              if (n_prefs == 1)
                {
                  g_autoptr(GString) err_str = g_string_new ("");
                  g_string_append_printf (err_str, _("No installed refs found for ‘%s’"), match_id);

                  if (match_arch)
                    g_string_append_printf (err_str, _(" with arch ‘%s’"), match_arch);
                  if (match_branch)
                    g_string_append_printf (err_str, _(" with branch ‘%s’"), match_branch);

                  g_set_error_literal (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                                       err_str->str);
                  return FALSE;
                }

              g_printerr (_("Warning: %s is not installed\n"), pref);
              continue;
            }

          /* Don't show fuzzy matches if an exact match was found in any installation */
          if (found_exact_name_match)
            {
              /* Walk through the array backwards so we can safely remove */
              for (i = ref_dir_pairs->len; i > 0; i--)
                {
                  RefDirPair *pair = g_ptr_array_index (ref_dir_pairs, i - 1);

                  if (match_id != NULL && !flatpak_decomposed_is_id (pair->ref, match_id))
                    g_ptr_array_remove_index (ref_dir_pairs, i - 1);
                }
            }

          chosen_pairs = g_ptr_array_new ();

          if (!flatpak_resolve_matching_installed_refs (opt_yes, FALSE, ref_dir_pairs, match_id, chosen_pairs, error))
            return FALSE;

          for (i = 0; i < chosen_pairs->len; i++)
            {
              RefDirPair *pair = g_ptr_array_index (chosen_pairs, i);
              udir = uninstall_dir_ensure (uninstall_dirs, pair->dir);
              uninstall_dir_add_ref (udir, pair->ref);
            }
        }
    }

  if (n_prefs > 0 && g_hash_table_size (uninstall_dirs) == 0)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   _("None of the specified refs are installed"));
      return FALSE;
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

    /* Walk through the array backwards so we can safely remove */
    for (i = udir->refs->len; i > 0; i--)
      {
        FlatpakDecomposed *ref = g_ptr_array_index (udir->refs, i - 1);

         /* In case it's a runtime for an installed app or an optional runtime
          * extension of an installed app, prompt the user for confirmation (in
          * the former case the transaction will error out if executed).  This
          * is limited to checking within the same installation; it won't
          * prompt for a user app depending on a system runtime.
         */
        if (!opt_force_remove && !opt_unused &&
            !confirm_runtime_removal (opt_yes, udir, ref))
          {
            uninstall_dir_remove_ref (udir, ref);
            continue;
          }

        if (!flatpak_transaction_add_uninstall (transaction, flatpak_decomposed_get_ref (ref), error))
          return FALSE;
      }

    /* These caches may no longer be valid once the transaction runs */
    g_clear_pointer (&udir->runtime_app_map, g_hash_table_unref);
    g_clear_pointer (&udir->extension_app_map, g_hash_table_unref);

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
            FlatpakDecomposed *ref = g_ptr_array_index (udir->refs, i);
            g_autofree char *id = flatpak_decomposed_dup_id (ref);

            if (!flatpak_delete_data (opt_yes, id, error))
              return FALSE;
          }
      }
  }

  if (opt_delete_data && argc < 2)
    {
      g_autoptr(GFileEnumerator) enumerator = NULL;
      g_autofree char *path = g_build_filename (g_get_home_dir (), ".var", "app", NULL);
      g_autoptr(GFile) app_dir = g_file_new_for_path (path);
      gboolean found_something_to_delete_data = FALSE;

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
          g_autoptr(FlatpakDecomposed) ref = NULL;

          if (!g_file_enumerator_iterate (enumerator, &info, &file, cancellable, error))
            return FALSE;

          if (info == NULL)
            break;

          if (g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY)
            continue;

          ref = flatpak_find_current_ref (g_file_info_get_name (info), cancellable, NULL);
          if (ref)
            continue;

          found_something_to_delete_data = TRUE;

          if (!flatpak_delete_data (opt_yes, g_file_info_get_name (info), error))
            return FALSE;
        }

      if (!found_something_to_delete_data)
        {
          g_print (_("No app data to delete\n"));
          return TRUE;
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
