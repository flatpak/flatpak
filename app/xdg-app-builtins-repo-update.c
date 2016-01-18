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

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static char *opt_title;
static char *opt_gpg_homedir;
static char **opt_gpg_key_ids;

static GOptionEntry options[] = {
  { "title", 0, 0, G_OPTION_ARG_STRING, &opt_title, "A nice name to use for this repository", "TITLE" },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids, "GPG Key ID to sign the commit with", "KEY-ID"},
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "HOMEDIR"},
  { NULL }
};


gboolean
xdg_app_builtin_build_update_repo (int argc, char **argv,
                                   GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) repofile = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *location;
  g_autoptr(GError) my_error = NULL;

  context = g_option_context_new ("LOCATION - Update repository metadata");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, "LOCATION must be specified", error);

  location = argv[1];

  repofile = g_file_new_for_commandline_arg (location);
  repo = ostree_repo_new (repofile);

  if (!ostree_repo_open (repo, cancellable, error))
    return FALSE;

  if (opt_title &&
      !xdg_app_repo_set_title (repo, opt_title, error))
    return FALSE;

  g_print ("Updating appstream branch\n");
  if (!xdg_app_repo_generate_appstream (repo, (const char **)opt_gpg_key_ids, opt_gpg_homedir, cancellable, &my_error))
    {
      if (g_error_matches (my_error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT))
        g_print ("WARNING: Can't find appstream-builder, unable to update appstream branch\n");
      else
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return FALSE;
        }
    }

  g_print ("Updating summary\n");
  if (!xdg_app_repo_update (repo, (const char **)opt_gpg_key_ids, opt_gpg_homedir, cancellable, error))
    return FALSE;

  return TRUE;
}
