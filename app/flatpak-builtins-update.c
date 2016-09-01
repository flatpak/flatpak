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

  if (!flatpak_dir_update_appstream (dir, remote, opt_arch, &changed,
                                     NULL, cancellable, error))
    return FALSE;

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

gboolean
flatpak_builtin_update (int           argc,
                        char        **argv,
                        GCancellable *cancellable,
                        GError      **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  char *name = NULL;
  char *branch = NULL;
  gboolean failed = FALSE;
  gboolean found = FALSE;
  int i;

  context = g_option_context_new (_("[NAME [BRANCH]] - Update an application or runtime"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc >= 2)
    name = argv[1];
  if (argc >= 3)
    branch = argv[2];

  if (opt_arch == NULL)
    opt_arch = (char *)flatpak_get_arch ();

  if (!flatpak_split_partial_ref_arg (name, &opt_arch, &branch, error))
    return FALSE;

  if (!opt_app && !opt_runtime)
    opt_app = opt_runtime = TRUE;

  if (opt_appstream)
    return update_appstream (dir, name, cancellable, error);

  if (opt_app)
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

          if (name != NULL && strcmp (parts[1], name) != 0)
            continue;

          if (strcmp (parts[2], opt_arch) != 0)
            continue;

          if (branch != NULL && strcmp (parts[3], branch) != 0)
            continue;

          found = TRUE;
          if (!do_update (dir, refs[i], cancellable, error))
            return FALSE;
        }
    }

  if (opt_runtime)
    {
      g_auto(GStrv) refs = NULL;

      if (!flatpak_dir_list_refs (dir, "runtime", &refs,
                                  cancellable,
                                  error))
        return FALSE;

      for (i = 0; refs != NULL && refs[i] != NULL; i++)
        {
          g_auto(GStrv) parts = flatpak_decompose_ref (refs[i], error);
          g_autoptr(GError) local_error = NULL;

          if (parts == NULL)
            return FALSE;

          if (name != NULL && strcmp (parts[1], name) != 0)
            continue;

          if (strcmp (parts[2], opt_arch) != 0)
            continue;

          if (branch != NULL && strcmp (parts[3], branch) != 0)
            continue;

          found = TRUE;
          if (!do_update (dir, refs[i], cancellable, &local_error))
            {
              g_printerr (_("Error updating: %s\n"), local_error->message);
              failed = TRUE;
            }
        }
    }

  if (name && !found)
    {
      g_set_error (error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED,
                   "%s not installed", name);
      return FALSE;
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
  int i;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv, 0, &dir, NULL, NULL))
    return FALSE;

  if (!opt_app && !opt_runtime)
    opt_app = opt_runtime = TRUE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* NAME */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      refs = flatpak_dir_find_installed_refs (dir, NULL, NULL, opt_arch,
                                              opt_app, opt_runtime, &error);
      if (refs == NULL)
        flatpak_completion_debug ("find local refs error: %s", error->message);
      for (i = 0; refs != NULL && refs[i] != NULL; i++)
        {
          g_auto(GStrv) parts = flatpak_decompose_ref (refs[i], NULL);
          if (parts)
            flatpak_complete_word (completion, "%s \n", parts[1]);
        }
      break;

    case 2: /* Branch */
      refs = flatpak_dir_find_installed_refs (dir, completion->argv[1], NULL, opt_arch,
                                              opt_app, opt_runtime, &error);
      if (refs == NULL)
        flatpak_completion_debug ("find remote refs error: %s", error->message);
      for (i = 0; refs != NULL && refs[i] != NULL; i++)
        {
          g_auto(GStrv) parts = flatpak_decompose_ref (refs[i], NULL);
          if (parts)
            flatpak_complete_word (completion, "%s ", parts[3]);
        }
      break;

    default:
      break;
    }

  return TRUE;
}
