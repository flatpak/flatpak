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

static char *opt_arch;
static gboolean opt_runtime;
static char **opt_gpg_key_ids;
static char *opt_gpg_homedir;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, "Arch to install for", "ARCH" },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, "Look for runtime with the specified name", },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids, "GPG Key ID to sign the commit with", "KEY-ID"},
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "HOMEDIR"},
  { NULL }
};


gboolean
xdg_app_builtin_build_sign (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) repofile = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *location;
  const char *branch;
  const char *id;
  g_autofree char *commit_checksum = NULL;
  g_autofree char *ref = NULL;
  char **iter;

  context = g_option_context_new ("LOCATION ID [BRANCH] - Create a repository from a build directory");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, XDG_APP_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 3)
    {
      usage_error (context, "LOCATION and DIRECTORY must be specified", error);
      return FALSE;
    }

  location = argv[1];
  id = argv[2];

  if (argc >= 4)
    branch = argv[3];
  else
    branch = "master";

  if (!xdg_app_is_valid_name (id))
    return xdg_app_fail (error, "'%s' is not a valid name", id);

  if (!xdg_app_is_valid_branch (branch))
    return xdg_app_fail (error, "'%s' is not a valid branch name", branch);

  if (opt_gpg_key_ids == NULL)
    return xdg_app_fail (error, "No gpg key ids specified");

  if (opt_runtime)
    ref = xdg_app_build_runtime_ref (id, branch, opt_arch);
  else
    ref = xdg_app_build_app_ref (id, branch, opt_arch);

  repofile = g_file_new_for_commandline_arg (location);
  repo = ostree_repo_new (repofile);

  if (!ostree_repo_open (repo, cancellable, error))
    return FALSE;

  if (!ostree_repo_resolve_rev (repo, ref, TRUE, &commit_checksum, error))
    return FALSE;

  for (iter = opt_gpg_key_ids; iter && *iter; iter++)
    {
      const char *keyid = *iter;

      if (!ostree_repo_sign_commit (repo,
                                    commit_checksum,
                                    keyid,
                                    opt_gpg_homedir,
                                    cancellable,
                                    error))
        return FALSE;
    }

  return TRUE;
}
