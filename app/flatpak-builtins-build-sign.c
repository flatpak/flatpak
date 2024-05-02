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
#include "flatpak-repo-utils-private.h"
#include "flatpak-utils-private.h"

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
  g_autoptr(GError) my_error = NULL;
  const char *location;
  const char *branch;
  const char *id = NULL;
  g_autofree char *commit_checksum = NULL;
  int i;
  char **iter;
  g_autoptr(GPtrArray) refs = g_ptr_array_new_with_free_func (g_free);
  const char *collection_id;

  context = g_option_context_new (_("LOCATION [ID [BRANCH]] - Sign an application or runtime"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("LOCATION must be specified"), error);

  if (argc > 4)
    return usage_error (context, _("Too many arguments"), error);

  location = argv[1];
  if (argc >= 3)
    id = argv[2];

  if (argc >= 4)
    branch = argv[3];
  else
    branch = "master";

  if (id != NULL && !flatpak_is_valid_name (id, -1, &my_error))
    return flatpak_fail (error, _("'%s' is not a valid name: %s"), id, my_error->message);

  if (!flatpak_is_valid_branch (branch, -1, &my_error))
    return flatpak_fail (error, _("'%s' is not a valid branch name: %s"), branch, my_error->message);

  if (opt_gpg_key_ids == NULL)
    return flatpak_fail (error, _("No gpg key ids specified"));

  repofile = g_file_new_for_commandline_arg (location);
  repo = ostree_repo_new (repofile);

  if (!ostree_repo_open (repo, cancellable, error))
    return FALSE;

  collection_id = ostree_repo_get_collection_id (repo);

  if (id)
    {
      g_autofree char *ref = NULL;
      if (opt_runtime)
        ref = flatpak_build_runtime_ref (id, branch, opt_arch);
      else
        ref = flatpak_build_app_ref (id, branch, opt_arch);

      g_ptr_array_add (refs, g_steal_pointer (&ref));
    }
  else
    {
      g_autoptr(GHashTable) all_refs = NULL;
      GHashTableIter hashiter;
      gpointer key, value;

      if (!ostree_repo_list_refs_ext (repo, NULL, &all_refs,
                                      OSTREE_REPO_LIST_REFS_EXT_NONE,
                                      cancellable, error))
        return FALSE;

      /* Merge the prefix refs to the full refs table */
      g_hash_table_iter_init (&hashiter, all_refs);
      while (g_hash_table_iter_next (&hashiter, &key, &value))
        {
          if (g_str_has_prefix (key, "app/") ||
              g_str_has_prefix (key, "runtime/"))
            g_ptr_array_add (refs, g_strdup (key));
        }
    }

  for (i = 0; i < refs->len; i++)
    {
      const char *ref = g_ptr_array_index (refs, i);

      if (!flatpak_repo_resolve_rev (repo, collection_id, NULL, ref, FALSE,
                                     &commit_checksum, cancellable, error))
        return FALSE;

      for (iter = opt_gpg_key_ids; iter && *iter; iter++)
        {
          const char *keyid = *iter;
          g_autoptr(GError) local_error = NULL;

          if (!ostree_repo_sign_commit (repo,
                                        commit_checksum,
                                        keyid,
                                        opt_gpg_homedir,
                                        cancellable,
                                        &local_error))
            {
              if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
                {
                  g_propagate_error (error, g_steal_pointer (&local_error));
                  return FALSE;
                }
            }
        }
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
