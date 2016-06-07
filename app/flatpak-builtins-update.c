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
#include <unistd.h>
#include <string.h>

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-utils.h"

static char *opt_arch;
static char *opt_commit;
static char **opt_subpaths;
static gboolean opt_force_remove;
static gboolean opt_no_pull;
static gboolean opt_no_deploy;
static gboolean opt_runtime;
static gboolean opt_app;
static gboolean opt_appstream;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to update for", "ARCH" },
  { "commit", 0, 0, G_OPTION_ARG_STRING, &opt_commit, "Commit to deploy", "COMMIT" },
  { "force-remove", 0, 0, G_OPTION_ARG_NONE, &opt_force_remove, "Remove old files even if running", NULL },
  { "no-pull", 0, 0, G_OPTION_ARG_NONE, &opt_no_pull, "Don't pull, only update from local cache", },
  { "no-deploy", 0, 0, G_OPTION_ARG_NONE, &opt_no_deploy, "Don't deploy, only download to local cache", },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, "Look for runtime with the specified name", },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, "Look for app with the specified name", },
  { "appstream", 0, 0, G_OPTION_ARG_NONE, &opt_appstream, "Update appstream for remote", },
  { "subpath", 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_subpaths, "Only update this subpath", "PATH" },
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
  g_autofree char *repository = NULL;

  repository = flatpak_dir_get_origin (dir, ref, cancellable, error);
  if (repository == NULL)
    return FALSE;

  if (flatpak_dir_get_remote_disabled (dir, repository))
    g_print ("Not updating %s due to disabled remote %s\n", ref, repository);

  if (!flatpak_dir_update (dir,
                           opt_no_pull,
                           opt_no_deploy,
                           ref, repository, opt_commit, (const char **)opt_subpaths,
                           NULL,
                           cancellable, error))
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
  const char *name = NULL;
  const char *branch = NULL;
  gboolean failed = FALSE;
  int i;

  context = g_option_context_new ("[NAME [BRANCH]] - Update an application or runtime");

  if (!flatpak_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 1)
    return usage_error (context, "NAME must be specified", error);

  if (argc >= 2)
    name = argv[1];
  if (argc >= 3)
    branch = argv[2];

  if (opt_arch == NULL)
    opt_arch = (char *)flatpak_get_arch ();

  if (!opt_app && !opt_runtime)
    opt_app = opt_runtime = TRUE;

  if (opt_appstream)
    return update_appstream (dir, name, cancellable, error);

  if (branch == NULL || name == NULL)
    {
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

              g_print ("Updating application %s %s\n", parts[1], parts[3]);

              if (!do_update (dir, refs[i],
                              cancellable, error))
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

              g_print ("Updating runtime %s %s\n", parts[1], parts[3]);
              if (!do_update (dir, refs[i], cancellable, &local_error))
                {
                  g_printerr ("error updating: %s\n", local_error->message);
                  failed = TRUE;
                }
            }
        }

    }
  else
    {
      gboolean is_app;
      g_autofree char *ref = NULL;

      ref = flatpak_dir_find_installed_ref (dir,
                                            name,
                                            branch,
                                            opt_arch,
                                            opt_app, opt_runtime, &is_app,
                                            error);
      if (ref == NULL)
        return FALSE;

      if (!do_update (dir, ref,
                      cancellable,
                      error))
        return FALSE;
    }

  flatpak_dir_cleanup_removed (dir, cancellable, NULL);

  if (failed)
    return flatpak_fail (error, "One or more updates failed");

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
