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
#include "flatpak-utils-private.h"
#include "flatpak-table-printer.h"

static gboolean opt_show_details;
static gboolean opt_runtime;
static gboolean opt_app;
static gboolean opt_all;
static gboolean opt_only_updates;
static char *opt_arch;

static GOptionEntry options[] = {
  { "show-details", 'd', 0, G_OPTION_ARG_NONE, &opt_show_details, N_("Show arches and branches"), NULL },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Show only runtimes"), NULL },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, N_("Show only apps"), NULL },
  { "updates", 0, 0, G_OPTION_ARG_NONE, &opt_only_updates, N_("Show only those where updates are available"), NULL },
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Limit to this arch (* for all)"), N_("ARCH") },
  { "all", 'a', 0, G_OPTION_ARG_NONE, &opt_all, N_("List all refs (including locale/debug)"), NULL },
  { NULL }
};

typedef struct RemoteDirPair
{
  gchar              *remote_name;
  FlatpakRemoteState *state;
  FlatpakDir         *dir;
} RemoteDirPair;

static void
remote_dir_pair_free (RemoteDirPair *pair)
{
  g_free (pair->remote_name);
  flatpak_remote_state_unref (pair->state);
  g_object_unref (pair->dir);
  g_free (pair);
}

static RemoteDirPair *
remote_dir_pair_new (const char *remote_name, FlatpakDir *dir, FlatpakRemoteState *state)
{
  RemoteDirPair *pair = g_new (RemoteDirPair, 1);

  pair->remote_name = g_strdup (remote_name);
  pair->state = state;
  pair->dir = g_object_ref (dir);
  return pair;
}

gboolean
flatpak_builtin_ls_remote (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  GHashTableIter refs_iter;
  GHashTableIter iter;
  gpointer refs_key;
  gpointer refs_value;
  gpointer key;
  gpointer value;
  guint n_keys;
  g_autofree const char **keys = NULL;
  int i;
  const char **arches = flatpak_get_arches ();
  const char *opt_arches[] = {NULL, NULL};
  gboolean has_remote;
  g_autoptr(GHashTable) pref_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GHashTable) refs_hash = g_hash_table_new_full (g_direct_hash, g_direct_equal, (GDestroyNotify) g_hash_table_unref, (GDestroyNotify) remote_dir_pair_free);

  context = g_option_context_new (_(" [REMOTE or URI] - Show available runtimes and applications"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS, &dirs, cancellable, error))
    return FALSE;

  if (!opt_app && !opt_runtime)
    opt_app = opt_runtime = TRUE;

  if (argc > 2)
    return usage_error (context, _("Too many arguments"), error);

  has_remote = (argc == 2);

  if (has_remote)
    {
      g_autoptr(FlatpakDir) preferred_dir = NULL;
      g_autoptr(GHashTable) refs = NULL;
      RemoteDirPair *remote_dir_pair = NULL;
      g_autoptr(FlatpakRemoteState) state = NULL;
      gboolean is_local = FALSE;

      is_local = g_str_has_prefix (argv[1], "file:");
      if (is_local)
        preferred_dir = flatpak_dir_get_system_default ();
      else
        {
          if (!flatpak_resolve_duplicate_remotes (dirs, argv[1], &preferred_dir, cancellable, error))
            return FALSE;
        }

      state = flatpak_dir_get_remote_state (preferred_dir, argv[1], cancellable, error);
      if (state == NULL)
        return FALSE;

      if (!flatpak_dir_list_remote_refs (preferred_dir, state, &refs,
                                         cancellable, error))
        return FALSE;

      remote_dir_pair = remote_dir_pair_new (argv[1], preferred_dir, g_steal_pointer (&state));
      g_hash_table_insert (refs_hash, g_steal_pointer (&refs), remote_dir_pair);
    }
  else
    {
      int i;
      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          g_auto(GStrv) remotes = NULL;
          int j;

          remotes = flatpak_dir_list_remotes (dir, cancellable, error);
          if (remotes == NULL)
            return FALSE;

          for (j = 0; remotes[j] != NULL; j++)
            {
              g_autoptr(GHashTable) refs = NULL;
              RemoteDirPair *remote_dir_pair = NULL;
              const char *remote_name = remotes[j];
              g_autoptr(FlatpakRemoteState) state = NULL;

              if (flatpak_dir_get_remote_disabled (dir, remote_name))
                continue;

              state = flatpak_dir_get_remote_state (dir, remote_name,
                                                    cancellable, error);
              if (state == NULL)
                return FALSE;

              if (!flatpak_dir_list_remote_refs (dir, state, &refs,
                                                 cancellable, error))
                return FALSE;

              remote_dir_pair = remote_dir_pair_new (remote_name, dir, g_steal_pointer (&state));
              g_hash_table_insert (refs_hash, g_steal_pointer (&refs), remote_dir_pair);
            }
        }
    }

  if (opt_arch != NULL)
    {
      if (strcmp (opt_arch, "*") == 0)
        arches = NULL;
      else
        {
          opt_arches[0] = opt_arch;
          arches = opt_arches;
        }
    }

  FlatpakTablePrinter *printer = flatpak_table_printer_new ();

  i = 0;
  flatpak_table_printer_set_column_title (printer, i++, _("Ref"));
  if (!has_remote)
    flatpak_table_printer_set_column_title (printer, i++, _("Origin"));
  flatpak_table_printer_set_column_title (printer, i++, _("Commit"));
  flatpak_table_printer_set_column_title (printer, i++, _("Installed size"));
  flatpak_table_printer_set_column_title (printer, i++, _("Download size"));
  flatpak_table_printer_set_column_title (printer, i++, _("Options"));

  g_hash_table_iter_init (&refs_iter, refs_hash);
  while (g_hash_table_iter_next (&refs_iter, &refs_key, &refs_value))
    {
      GHashTable *refs = refs_key;
      RemoteDirPair *remote_dir_pair = refs_value;
      const char *remote = remote_dir_pair->remote_name;
      FlatpakDir *dir = remote_dir_pair->dir;
      FlatpakRemoteState *state = remote_dir_pair->state;

      g_autoptr(GHashTable) names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

      g_hash_table_iter_init (&iter, refs);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          FlatpakCollectionRef *coll_ref = key;
          char *ref = coll_ref->ref_name;
          char *partial_ref;
          const char *slash = strchr (ref, '/');

          if (slash == NULL)
            {
              g_debug ("Invalid remote ref %s", ref);
              continue;
            }

          partial_ref = flatpak_make_valid_id_prefix (slash + 1);
          g_hash_table_insert (pref_hash, partial_ref, ref);
        }

      g_hash_table_iter_init (&iter, refs);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          FlatpakCollectionRef *coll_ref = key;
          const char *ref = coll_ref->ref_name;
          const char *checksum = value;
          const char *name = NULL;
          g_auto(GStrv) parts = NULL;

          parts = flatpak_decompose_ref (ref, NULL);
          if (parts == NULL)
            {
              g_debug ("Invalid remote ref %s", ref);
              continue;
            }

          if (opt_only_updates)
            {
              g_autoptr(GVariant) deploy_data = flatpak_dir_get_deploy_data (dir, ref, cancellable, NULL);

              if (deploy_data == NULL)
                continue;

              if (g_strcmp0 (flatpak_deploy_data_get_origin (deploy_data), remote) != 0)
                continue;

              if (g_strcmp0 (flatpak_deploy_data_get_commit (deploy_data), checksum) == 0)
                continue;
            }

          if (arches != NULL && !g_strv_contains (arches, parts[2]))
            continue;

          if (strcmp (parts[0], "runtime") == 0 && !opt_runtime)
            continue;

          if (strcmp (parts[0], "app") == 0 && !opt_app)
            continue;

          if (!opt_show_details)
            name = parts[1];
          else
            name = ref;

          if (!opt_all &&
              strcmp (parts[0], "runtime") == 0 &&
              flatpak_id_has_subref_suffix (parts[1]))
            {
              g_autofree char *prefix_partial_ref = NULL;
              char *last_dot = strrchr (parts[1], '.');

              *last_dot = 0;
              prefix_partial_ref = g_strconcat (parts[1], "/", parts[2], "/", parts[3], NULL);
              *last_dot = '.';

              if (g_hash_table_lookup (pref_hash, prefix_partial_ref))
                continue;
            }

          if (!opt_all && opt_arch == NULL &&
              /* Hide non-primary arches if the primary arch exists */
              strcmp (arches[0], parts[2]) != 0)
            {
              g_autofree char *alt_arch_ref = g_strconcat (parts[0], "/", parts[1], "/", arches[0], "/", parts[3], NULL);
              g_autoptr(FlatpakCollectionRef) alt_arch_coll_ref = flatpak_collection_ref_new (coll_ref->collection_id, alt_arch_ref);
              if (g_hash_table_lookup (refs, alt_arch_coll_ref))
                continue;
            }

          if (g_hash_table_lookup (names, name) == NULL)
            g_hash_table_insert (names, g_strdup (name), g_strdup (checksum));
        }
      keys = (const char **) g_hash_table_get_keys_as_array (names, &n_keys);
      g_qsort_with_data (keys, n_keys, sizeof (char *), (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);

      for (i = 0; i < n_keys; i++)
        {
          const char *ref = keys[i];

          flatpak_table_printer_add_column (printer, ref);

          if (!has_remote)
            flatpak_table_printer_add_column (printer, remote);

          if (opt_show_details)
            {
              g_autofree char *value = NULL;
              g_autoptr(GVariant) sparse = NULL;
              guint64 installed_size;
              guint64 download_size;
              g_autofree char *installed = NULL;
              g_autofree char *download = NULL;

              value = g_strdup ((char *) g_hash_table_lookup (names, keys[i]));
              value[MIN (strlen (value), 12)] = 0;
              flatpak_table_printer_add_column (printer, value);


              if (!flatpak_remote_state_lookup_cache (state, ref,
                                                      &download_size, &installed_size, NULL,
                                                      error))
                return FALSE;

              /* The sparse cache is optional */
              sparse = flatpak_remote_state_lookup_sparse_cache (state, ref, NULL);

              installed = g_format_size (installed_size);
              flatpak_table_printer_add_decimal_column (printer, installed);

              download = g_format_size (download_size);
              flatpak_table_printer_add_decimal_column (printer, download);

              flatpak_table_printer_add_column (printer, ""); /* Extra */
              if (sparse)
                {
                  const char *eol;

                  if (g_variant_lookup (sparse, "eol", "&s", &eol))
                    flatpak_table_printer_append_with_comma_printf (printer, "eol=%s", eol);
                  if (g_variant_lookup (sparse, "eolr", "&s", &eol))
                    flatpak_table_printer_append_with_comma_printf (printer, "eol-rebase=%s", eol);
                }
            }
          flatpak_table_printer_finish_row (printer);
        }
    }

  flatpak_table_printer_print (printer);
  flatpak_table_printer_free (printer);

  return TRUE;
}

gboolean
flatpak_complete_ls_remote (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  int i;

  context = g_option_context_new ("");

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS, &dirs, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* REMOTE */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          int j;
          g_auto(GStrv) remotes = flatpak_dir_list_remotes (dir, NULL, NULL);
          if (remotes == NULL)
            return FALSE;
          for (j = 0; remotes[j] != NULL; j++)
            flatpak_complete_word (completion, "%s ", remotes[j]);
        }

      break;
    }

  return TRUE;
}
