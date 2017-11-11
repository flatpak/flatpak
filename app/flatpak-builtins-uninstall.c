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
#include "flatpak-utils.h"

static char *opt_arch;
static gboolean opt_keep_ref;
static gboolean opt_force_remove;
static gboolean opt_no_related;
static gboolean opt_runtime;
static gboolean opt_app;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to uninstall"), N_("ARCH") },
  { "keep-ref", 0, 0, G_OPTION_ARG_NONE, &opt_keep_ref, N_("Keep ref in local repository"), NULL },
  { "no-related", 0, 0, G_OPTION_ARG_NONE, &opt_no_related, N_("Don't uninstall related refs"), NULL },
  { "force-remove", 0, 0, G_OPTION_ARG_NONE, &opt_force_remove, N_("Remove files even if running"), NULL },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Look for runtime with the specified name"), NULL },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, N_("Look for app with the specified name"), NULL },
  { NULL }
};

gboolean
flatpak_builtin_uninstall (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
  char **prefs = NULL;
  int i, j, n_prefs;
  const char *default_branch = NULL;
  g_autofree char *ref = NULL;
  FlatpakHelperUninstallFlags flags = 0;
  g_autoptr(GPtrArray) related = NULL;
  FlatpakKinds kinds;
  FlatpakKinds kind;
  g_autoptr(GHashTable) uninstall_refs_hash = NULL;
  g_autoptr(GPtrArray) uninstall_refs = NULL;

  context = g_option_context_new (_("REF... - Uninstall an application"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("Must specify at least one REF"), error);

  prefs = &argv[1];
  n_prefs = argc - 1;

  /* Backwards compat for old "REPOSITORY NAME [BRANCH]" argument version */
  if (argc == 3 && looks_like_branch (argv[2]))
    {
      default_branch = argv[2];
      n_prefs = 1;
    }

  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);
  uninstall_refs = g_ptr_array_new_with_free_func (g_free);
  uninstall_refs_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  for (j = 0; j < n_prefs; j++)
    {
      const char *pref = NULL;
      FlatpakKinds matched_kinds;
      g_autofree char *id = NULL;
      g_autofree char *arch = NULL;
      g_autofree char *branch = NULL;
      g_autoptr(GError) local_error = NULL;
      g_autofree char *origin = NULL;

      pref = prefs[j];

      if (!flatpak_split_partial_ref_arg (pref, kinds, opt_arch, default_branch,
                                          &matched_kinds, &id, &arch, &branch, error))
        return FALSE;

      ref = flatpak_dir_find_installed_ref (dir, id, branch, arch,
                                            kinds, &kind, error);
      if (ref == NULL)
        return FALSE;

      if (g_hash_table_insert (uninstall_refs_hash, g_strdup (ref), NULL))
        g_ptr_array_add (uninstall_refs, g_strdup (ref));

      /* TODO: when removing runtimes, look for apps that use it, require --force */

      if (opt_no_related)
        continue;

      origin = flatpak_dir_get_origin (dir, ref, NULL, NULL);
      if (origin == NULL)
        continue;

      related = flatpak_dir_find_local_related (dir, ref, origin,
                                                NULL, &local_error);
      if (related == NULL)
        {
          g_printerr (_("Warning: Problem looking for related refs: %s\n"),
                      local_error->message);
          continue;
        }

      for (i = 0; i < related->len; i++)
        {
          FlatpakRelated *rel = g_ptr_array_index (related, i);
          g_autoptr(GVariant) deploy_data = NULL;

          if (!rel->delete)
            continue;

          deploy_data = flatpak_dir_get_deploy_data (dir, rel->ref, NULL, NULL);

          if (deploy_data != NULL &&
              g_hash_table_insert (uninstall_refs_hash, g_strdup (rel->ref), NULL))
            g_ptr_array_add (uninstall_refs, g_strdup (rel->ref));
        }
    }

  if (opt_keep_ref)
    flags |= FLATPAK_HELPER_UNINSTALL_FLAGS_KEEP_REF;
  if (opt_force_remove)
    flags |= FLATPAK_HELPER_UNINSTALL_FLAGS_FORCE_REMOVE;

  for (i = 0; i < uninstall_refs->len; i++)
    {
      const char *ref = (char *)g_ptr_array_index (uninstall_refs, i);
      const char *pref = strchr (ref, '/') + 1;
      g_print (_("Uninstalling: %s\n"), pref);
      if (!flatpak_dir_uninstall (dir, ref, flags,
                                  cancellable, error))
        return FALSE;
    }

  return TRUE;
}

gboolean
flatpak_complete_uninstall (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
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
