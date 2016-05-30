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

static char *opt_title;
static char *opt_gpg_homedir;
static char **opt_gpg_key_ids;
static gboolean opt_prune;
static gboolean opt_generate_deltas;
static gint opt_prune_depth = -1;

static GOptionEntry options[] = {
  { "title", 0, 0, G_OPTION_ARG_STRING, &opt_title, "A nice name to use for this repository", "TITLE" },
  { "gpg-sign", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_gpg_key_ids, "GPG Key ID to sign the summary with", "KEY-ID"},
  { "gpg-homedir", 0, 0, G_OPTION_ARG_STRING, &opt_gpg_homedir, "GPG Homedir to use when looking for keyrings", "HOMEDIR"},
  { "generate-static-deltas", 0, 0, G_OPTION_ARG_NONE, &opt_generate_deltas, "Generate delta files", NULL },
  { "prune", 0, 0, G_OPTION_ARG_NONE, &opt_prune, "Prune unused objects", NULL },
  { "prune-depth", 0, 0, G_OPTION_ARG_INT, &opt_prune_depth, "Only traverse DEPTH parents for each commit (default: -1=infinite)", "DEPTH" },
  { NULL }
};

gboolean
flatpak_builtin_build_update_repo (int argc, char **argv,
                                   GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GFile) repofile = NULL;
  g_autoptr(OstreeRepo) repo = NULL;
  const char *location;
  g_autoptr(GError) my_error = NULL;

  context = g_option_context_new ("LOCATION - Update repository metadata");

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, "LOCATION must be specified", error);

  location = argv[1];

  repofile = g_file_new_for_commandline_arg (location);
  repo = ostree_repo_new (repofile);

  if (!ostree_repo_open (repo, cancellable, error))
    return FALSE;

  if (opt_title &&
      !flatpak_repo_set_title (repo, opt_title, error))
    return FALSE;

  g_print ("Updating appstream branch\n");
  if (!flatpak_repo_generate_appstream (repo, (const char **) opt_gpg_key_ids, opt_gpg_homedir, cancellable, &my_error))
    {
      if (g_error_matches (my_error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT))
        {
          g_print ("WARNING: Can't find appstream-builder, unable to update appstream branch\n");
        }
      else
        {
          g_propagate_error (error, g_steal_pointer (&my_error));
          return FALSE;
        }
    }

  if (opt_generate_deltas)
    {
      g_autoptr(GHashTable) all_refs = NULL;
      g_autoptr(GHashTable) all_deltas_hash = NULL;
      g_autoptr(GPtrArray) all_deltas = NULL;
      int i;
      GHashTableIter iter;
      gpointer key, value;
      g_autoptr(GVariantBuilder) parambuilder = NULL;
      g_autoptr(GVariant) params = NULL;

      parambuilder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
      /* Fall back for 1 meg files */
      g_variant_builder_add (parambuilder, "{sv}",
                             "min-fallback-size", g_variant_new_uint32 (1));
      g_variant_builder_add (parambuilder, "{sv}", "verbose",
                             g_variant_new_boolean (TRUE));
      params = g_variant_ref_sink (g_variant_builder_end (parambuilder));

      if (!ostree_repo_list_static_delta_names (repo, &all_deltas,
                                                cancellable, error))
        return FALSE;

      all_deltas_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      for (i = 0; i < all_deltas->len; i++)
        {
          g_print ("adding %s\n", (char *) g_ptr_array_index (all_deltas, i));
          g_hash_table_insert (all_deltas_hash,
                               g_strdup (g_ptr_array_index (all_deltas, i)),
                               NULL);
        }

      if (!ostree_repo_list_refs (repo, NULL, &all_refs,
                                  cancellable, error))
        return FALSE;

      g_hash_table_iter_init (&iter, all_refs);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          const char *ref = key;
          const char *commit = value;
          const char *from_parent = NULL;
          g_autoptr(GVariant) variant = NULL;
          g_autoptr(GVariant) parent_variant = NULL;
          g_autofree char *parent_commit = NULL;

          if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, commit,
                                         &variant, NULL))
            return FALSE;

          parent_commit = ostree_commit_get_parent (variant);

          if (parent_commit != NULL)
            ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, parent_commit,
                                      &parent_variant, NULL);


          /* From empty */
          g_print ("looking for %s\n", commit);
          if (!g_hash_table_contains (all_deltas_hash, commit))
            {
              g_print ("Generating from-empty delta for %s (%s)\n", ref, commit);
              if (!ostree_repo_static_delta_generate (repo, OSTREE_STATIC_DELTA_GENERATE_OPT_MAJOR,
                                                      NULL, commit, NULL,
                                                      params,
                                                      cancellable, error))
                return FALSE;
            }

          /* Mark this one as wanted */
          g_hash_table_insert (all_deltas_hash, g_strdup (commit), GINT_TO_POINTER (1));

          /* From parent */
          if (parent_variant != NULL)
            {
              from_parent = g_strdup_printf ("%s-%s", parent_commit, commit);


              if (!g_hash_table_contains (all_deltas_hash, from_parent))
                {
                  g_print ("Generating from-parent delta for %s (%s)\n", ref, from_parent);
                  if (!ostree_repo_static_delta_generate (repo, OSTREE_STATIC_DELTA_GENERATE_OPT_MAJOR,
                                                          parent_commit, commit, NULL,
                                                          params,
                                                          cancellable, error))
                    return FALSE;
                }

              /* Mark this one as wanted */
              g_hash_table_insert (all_deltas_hash, g_strdup (from_parent), GINT_TO_POINTER (1));
            }
        }
    }

  g_print ("Updating summary\n");
  if (!flatpak_repo_update (repo, (const char **) opt_gpg_key_ids, opt_gpg_homedir, cancellable, error))
    return FALSE;

  if (opt_prune)
    {
      gint n_objects_total;
      gint n_objects_pruned;
      guint64 objsize_total;
      g_autofree char *formatted_freed_size = NULL;

      if (!ostree_repo_prune (repo, OSTREE_REPO_PRUNE_FLAGS_REFS_ONLY, opt_prune_depth,
                              &n_objects_total, &n_objects_pruned, &objsize_total,
                              cancellable, error))
        return FALSE;

      formatted_freed_size = g_format_size_full (objsize_total, 0);

      g_print ("Total objects: %u\n", n_objects_total);
      if (n_objects_pruned == 0)
        g_print ("No unreachable objects\n");
      else
        g_print ("Deleted %u objects, %s freed\n",
                 n_objects_pruned, formatted_freed_size);
    }

  return TRUE;
}


gboolean
flatpak_complete_build_update_repo (FlatpakCompletion *completion)
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
    }

  return TRUE;
}
