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
#include "flatpak-utils.h"
#include "flatpak-error.h"

static char *opt_arch;
static char *opt_commit;
static char **opt_subpaths;
static gboolean opt_force_remove;
static gboolean opt_no_pull;
static gboolean opt_no_deploy;
static gboolean opt_no_related;
static gboolean opt_runtime;
static gboolean opt_app;
static gboolean opt_appstream;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to update for"), N_("ARCH") },
  { "commit", 0, 0, G_OPTION_ARG_STRING, &opt_commit, N_("Commit to deploy"), N_("COMMIT") },
  { "force-remove", 0, 0, G_OPTION_ARG_NONE, &opt_force_remove, N_("Remove old files even if running"), NULL },
  { "no-pull", 0, 0, G_OPTION_ARG_NONE, &opt_no_pull, N_("Don't pull, only update from local cache"), NULL },
  { "no-deploy", 0, 0, G_OPTION_ARG_NONE, &opt_no_deploy, N_("Don't deploy, only download to local cache"), NULL },
  { "no-related", 0, 0, G_OPTION_ARG_NONE, &opt_no_related, N_("Don't update related refs"), NULL},
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Look for runtime with the specified name"), NULL },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, N_("Look for app with the specified name"), NULL },
  { "appstream", 0, 0, G_OPTION_ARG_NONE, &opt_appstream, N_("Update appstream for remote"), NULL },
  { "subpath", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_subpaths, N_("Only update this subpath"), N_("PATH") },
  { NULL }
};

static gboolean
update_appstream (FlatpakDir *dir, const char *remote, GCancellable *cancellable, GError **error)
{
  gboolean changed;

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
          if (flatpak_dir_get_remote_disabled (dir, remotes[i]))
            continue;

          g_print (_("Updating appstream for remote %s\n"), remotes[i]);
          if (!flatpak_dir_update_appstream (dir, remotes[i], opt_arch, &changed,
                                             NULL, cancellable, error))
            return FALSE;
        }
    }
  else
    {
      if (!flatpak_dir_update_appstream (dir, remote, opt_arch, &changed,
                                         NULL, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
do_update (FlatpakDir  * dir,
           const char   *ref,
           GCancellable *cancellable,
           GError      **error)
{
  g_auto(GStrv) parts = flatpak_decompose_ref (ref, error);
  g_autofree char *repository = NULL;
  g_autoptr(GPtrArray) related = NULL;
  g_autoptr(GError) update_error = NULL;
  int i;

  if (parts == NULL)
    return FALSE;

  repository = flatpak_dir_get_origin (dir, ref, cancellable, error);
  if (repository == NULL)
    return FALSE;

  if (strcmp (parts[0], "app") == 0)
    g_print (_("Updating application %s, branch %s\n"), parts[1], parts[3]);
  else
    g_print (_("Updating runtime %s, branch %s\n"), parts[1], parts[3]);

  if (flatpak_dir_get_remote_disabled (dir, repository))
    {
      g_print (_("Remote %s disabled\n"), repository);
      return TRUE;
    }

  if (!flatpak_dir_update (dir,
                           opt_no_pull,
                           opt_no_deploy,
                           ref, repository, opt_commit, (const char **)opt_subpaths,
                           NULL,
                           cancellable, &update_error))
    {
      if (g_error_matches (update_error, FLATPAK_ERROR,
                           FLATPAK_ERROR_ALREADY_INSTALLED))
        g_print (_("No updates.\n"));
      else
        g_printerr ("Error updating: %s\n", update_error->message);
    }
  else
    {
      g_autoptr(GVariant) deploy_data = NULL;
      g_autofree char *commit = NULL;
      deploy_data = flatpak_dir_get_deploy_data (dir, ref, NULL, NULL);
      commit = g_strndup (flatpak_deploy_data_get_commit (deploy_data), 12);
      g_print (_("Now at %s.\n"), commit);
    }


  if (!opt_no_related)
    {
      g_autoptr(GError) local_error = NULL;

      if (opt_no_pull)
        related = flatpak_dir_find_local_related (dir, ref, repository, NULL,
                                                  &local_error);
      else
        related = flatpak_dir_find_remote_related (dir, ref, repository, NULL,
                                                   &local_error);
      if (related == NULL)
        {
          g_printerr (_("Warning: Problem looking for related refs: %s\n"),
                      local_error->message);
          g_clear_error (&local_error);
        }
      else
        {
          for (i = 0; i < related->len; i++)
            {
              FlatpakRelated *rel = g_ptr_array_index (related, i);
              g_autoptr(GError) local_error = NULL;
              g_auto(GStrv) parts = NULL;

              if (!rel->download)
                continue;

              parts = g_strsplit (rel->ref, "/", 0);

              g_print (_("Updating related: %s\n"), parts[1]);

              if (!flatpak_dir_install_or_update (dir,
                                                  opt_no_pull,
                                                  opt_no_deploy,
                                                  rel->ref, repository,
                                                  (const char **)rel->subpaths,
                                                  NULL,
                                                  cancellable, &local_error))
                {
                  if (g_error_matches (local_error, FLATPAK_ERROR,
                                       FLATPAK_ERROR_ALREADY_INSTALLED))
                    g_print (_("No updates.\n"));
                  else
                    g_printerr ("Error updating: %s\n", local_error->message);
                  g_clear_error (&local_error);
                }
              else
                {
                  g_autoptr(GVariant) deploy_data = NULL;
                  g_autofree char *commit = NULL;
                  deploy_data = flatpak_dir_get_deploy_data (dir, rel->ref, NULL, NULL);
                  commit = g_strndup (flatpak_deploy_data_get_commit (deploy_data), 12);
                  g_print (_("Now at %s.\n"), commit);
                }
            }
        }
    }

  return TRUE;
}

static gboolean
looks_like_branch (const char *branch)
{
  /* In particular, / is not a valid branch char, so
     this lets us distinguish full or partial refs as
     non-branches. */
  if (!flatpak_is_valid_branch (branch, NULL))
    return FALSE;

  /* Dots are allowed in branches, but not really used much, while
     they are required for app ids, so thats a good check to
     distinguish the two */
  if (strchr (branch, '.') != NULL)
    return FALSE;

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
  gboolean failed = FALSE;
  FlatpakKinds kinds;
  g_autoptr(GHashTable) update_refs_hash = NULL;
  g_autoptr(GPtrArray) update_refs = NULL;

  context = g_option_context_new (_("[REF...] - Update applications or runtimes"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (opt_appstream)
    return update_appstream (dir, argc >= 2 ? argv[2] : NULL, cancellable, error);

  prefs = &argv[1];
  n_prefs = argc - 1;

  /* Backwards compat for old "REPOSITORY NAME [BRANCH]" argument version */
  if (argc == 3 && looks_like_branch (argv[2]))
    {
      default_branch = argv[2];
      n_prefs = 1;
    }

  update_refs = g_ptr_array_new_with_free_func (g_free);
  update_refs_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);

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
              if (g_hash_table_insert (update_refs_hash, g_strdup (refs[i]), NULL))
                g_ptr_array_add (update_refs, g_strdup (refs[i]));
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
              if (g_hash_table_insert (update_refs_hash, g_strdup (refs[i]), NULL))
                g_ptr_array_add (update_refs, g_strdup (refs[i]));
            }
        }

      if (n_prefs > 0 && !found)
        {
          g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                       "%s not installed", pref);
          return FALSE;
        }
    }

  for (i = 0; i < update_refs->len; i++)
    {
      const char *ref = (char *)g_ptr_array_index (update_refs, i);
      g_autoptr(GError) local_error = NULL;
      if (!do_update (dir, ref, cancellable, &local_error))
        {
          g_printerr (_("Error updating: %s\n"), local_error->message);
          failed = TRUE;
        }
    }

  if (failed)
    return flatpak_fail (error, _("One or more updates failed"));

  return TRUE;
}

gboolean
flatpak_complete_update (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) refs = NULL;
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
