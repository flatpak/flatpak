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

static char *opt_arch;
static gboolean opt_runtime;
static char **opt_gpg_key_ids;
static char *opt_gpg_homedir;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to install for"), N_("ARCH") },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Look for runtime with the specified name"), NULL },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids, N_("GPG Key ID to sign the commit with"), N_("KEY-ID") },
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, N_("GPG Homedir to use when looking for keyrings"), N_("HOMEDIR") },
  { NULL }
};


gboolean
flatpak_builtin_build_sign (int argc, char **argv, GCancellable *cancellable, GError **error)
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

  context = g_option_context_new (_("LOCATION ID [BRANCH] - Sign an application or runtime"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 3)
    {
      usage_error (context, _("LOCATION and DIRECTORY must be specified"), error);
      return FALSE;
    }

  location = argv[1];
  id = argv[2];

  if (argc >= 4)
    branch = argv[3];
  else
    branch = "master";

  if (!flatpak_is_valid_name (id))
    return flatpak_fail (error, _("'%s' is not a valid name"), id);

  if (!flatpak_is_valid_branch (branch))
    return flatpak_fail (error, _("'%s' is not a valid branch name"), branch);

  if (opt_gpg_key_ids == NULL)
    return flatpak_fail (error, _("No gpg key ids specified"));

  if (opt_runtime)
    ref = flatpak_build_runtime_ref (id, branch, opt_arch);
  else
    ref = flatpak_build_app_ref (id, branch, opt_arch);

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

gboolean
flatpak_complete_build_sign (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new ("");

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* LOCATION */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);

      flatpak_complete_dir (completion);
      break;

    case 2: /* ID */
      break;

    case 3: /* BRANCH */
      break;
    }

  return TRUE;
}
