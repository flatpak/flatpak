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
#include "flatpak-builtins-utils.h"
#include "flatpak-repo-utils-private.h"
#include "flatpak-utils-private.h"
#include "flatpak-table-printer.h"
#include "flatpak-variant-impl-private.h"

static gboolean opt_show_details;
static gboolean opt_runtime;
static gboolean opt_app;
static gboolean opt_all;
static gboolean opt_only_updates;
static gboolean opt_cached;
static gboolean opt_sideloaded;
static char *opt_arch;
static char *opt_app_runtime;
static const char **opt_cols;
static gboolean opt_json;

static GOptionEntry options[] = {
  { "show-details", 'd', 0, G_OPTION_ARG_NONE, &opt_show_details, N_("Show arches and branches"), NULL },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Show only runtimes"), NULL },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, N_("Show only apps"), NULL },
  { "updates", 0, 0, G_OPTION_ARG_NONE, &opt_only_updates, N_("Show only those where updates are available"), NULL },
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Limit to this arch (* for all)"), N_("ARCH") },
  { "all", 'a', 0, G_OPTION_ARG_NONE, &opt_all, N_("List all refs (including locale/debug)"), NULL },
  { "app-runtime", 0, 0, G_OPTION_ARG_STRING, &opt_app_runtime, N_("List all applications using RUNTIME"), N_("RUNTIME") },
  { "columns", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_cols, N_("What information to show"), N_("FIELD,…") },
  { "cached", 0, 0, G_OPTION_ARG_NONE, &opt_cached, N_("Use local caches even if they are stale"), NULL },
  /* Translators: A sideload is when you install from a local USB drive rather than the Internet. */
  { "sideloaded", 0, 0, G_OPTION_ARG_NONE, &opt_sideloaded, N_("Only list refs available as sideloads"), NULL },
  { "json", 'j', 0, G_OPTION_ARG_NONE, &opt_json, N_("Show output in JSON format"), NULL },
  { NULL }
};

static Column all_columns[] = {
  { "name",           N_("Name"),           N_("Show the name"),           1, FLATPAK_ELLIPSIZE_MODE_END, 1, 1 },
  { "description",    N_("Description"),    N_("Show the description"),    1, FLATPAK_ELLIPSIZE_MODE_END, 1, 0 },
  { "application",    N_("Application ID"),    N_("Show the application ID"), 1, FLATPAK_ELLIPSIZE_MODE_START, 0, 1 },
  { "version",        N_("Version"),        N_("Show the version"),        1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "branch",         N_("Branch"),         N_("Show the branch"),         1, FLATPAK_ELLIPSIZE_MODE_NONE, 0, 1 },
  { "arch",           N_("Arch"),           N_("Show the architecture"),   1, FLATPAK_ELLIPSIZE_MODE_NONE, 0, 0 },
  { "origin",         N_("Origin"),         N_("Show the origin remote"),  1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1, 1 },
  { "ref",            N_("Ref"),            N_("Show the ref"),            1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "commit",         N_("Commit"),         N_("Show the active commit"),  1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "runtime",        N_("Runtime"),        N_("Show the runtime"),        1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "installed-size", N_("Installed size"), N_("Show the installed size"), 1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "download-size",  N_("Download size"),  N_("Show the download size"),  1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "options",        N_("Options"),        N_("Show options"),            1, FLATPAK_ELLIPSIZE_MODE_END, 1, 0 },
  { NULL }
};


typedef struct RemoteStateDirPair
{
  FlatpakRemoteState *state;
  FlatpakDir         *dir;
} RemoteStateDirPair;

static void
remote_state_dir_pair_free (RemoteStateDirPair *pair)
{
  flatpak_remote_state_unref (pair->state);
  g_object_unref (pair->dir);
  g_free (pair);
}

static RemoteStateDirPair *
remote_state_dir_pair_new (FlatpakDir *dir, FlatpakRemoteState *state)
{
  RemoteStateDirPair *pair = g_new (RemoteStateDirPair, 1);

  pair->state = state;
  pair->dir = g_object_ref (dir);
  return pair;
}

static char *
strip_last_element (const char *id,
                    gsize id_len)
{
  while (id_len > 0 &&
         id[id_len - 1] != '.')
    id_len--;

  if (id_len > 0)
    id_len--; /* Remove the dot too */

  return g_strndup (id, id_len);
}

static gboolean
ls_remote (GHashTable *refs_hash, const char **arches, const char *app_runtime, Column *columns, GCancellable *cancellable, GError **error)
{
  g_autoptr(FlatpakTablePrinter) printer = NULL;
  guint n_keys;
  g_autofree FlatpakDecomposed **keys = NULL;
  int i, j;
  FlatpakKinds match_kinds;
  g_autofree char *match_id = NULL;
  g_autofree char *match_arch = NULL;
  g_autofree char *match_branch = NULL;
  gboolean need_cache_data = FALSE;
  gboolean need_appstream_data = FALSE;

  printer = flatpak_table_printer_new ();

  flatpak_table_printer_set_columns (printer, columns,
                                     opt_cols == NULL && !opt_show_details);

  if (app_runtime)
    {
      need_cache_data = TRUE;
      if (!flatpak_split_partial_ref_arg (app_runtime, FLATPAK_KINDS_RUNTIME, NULL, NULL,
                                          &match_kinds, &match_id, &match_arch, &match_branch, error))
        return FALSE;
    }

  for (j = 0; columns[j].name; j++)
    {
      if (strcmp (columns[j].name, "download-size") == 0 ||
          strcmp (columns[j].name, "installed-size") == 0 ||
          strcmp (columns[j].name, "runtime") == 0)
        need_cache_data = TRUE;
      if (strcmp (columns[j].name, "name") == 0 ||
          strcmp (columns[j].name, "description") == 0 ||
          strcmp (columns[j].name, "version") == 0)
        need_appstream_data = TRUE;
    }

  GLNX_HASH_TABLE_FOREACH_KV (refs_hash, GHashTable *, refs, RemoteStateDirPair *, remote_state_dir_pair)
    {
      FlatpakDir *dir = remote_state_dir_pair->dir;
      FlatpakRemoteState *state = remote_state_dir_pair->state;
      const char *remote = state->remote_name;
      g_autoptr(AsMetadata) mdata = NULL;
      g_autoptr(GHashTable) pref_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL); /* value owned by refs */
      g_autoptr(GHashTable) names = g_hash_table_new_full ((GHashFunc)flatpak_decomposed_hash, (GEqualFunc)flatpak_decomposed_equal, (GDestroyNotify)flatpak_decomposed_unref, g_free);

      GLNX_HASH_TABLE_FOREACH (refs, FlatpakDecomposed *, ref)
        {
          char *partial_ref = flatpak_make_valid_id_prefix (flatpak_decomposed_get_pref (ref));
          g_hash_table_insert (pref_hash, partial_ref, ref);
        }

      GLNX_HASH_TABLE_FOREACH_KV (refs, FlatpakDecomposed *, ref, const char *, checksum)
        {
          if (opt_only_updates)
            {
              g_autoptr(GBytes) deploy_data = flatpak_dir_get_deploy_data (dir, ref, FLATPAK_DEPLOY_VERSION_ANY, cancellable, NULL);

              if (deploy_data == NULL)
                continue;

              if (g_strcmp0 (flatpak_deploy_data_get_origin (deploy_data), remote) != 0)
                continue;

              if (g_strcmp0 (flatpak_deploy_data_get_commit (deploy_data), checksum) == 0)
                continue;
            }

          if (arches != NULL && !flatpak_decomposed_is_arches (ref, -1, arches))
            continue;

          if (flatpak_decomposed_is_runtime (ref) && !opt_runtime)
            continue;

          if (flatpak_decomposed_is_app (ref) && !opt_app)
            continue;

          if (!opt_all &&
              flatpak_decomposed_is_runtime (ref) &&
              flatpak_decomposed_id_is_subref (ref))
            {
              g_autoptr(FlatpakDecomposed) parent_ref = NULL;
              gsize id_len;
              const char *id = flatpak_decomposed_peek_id (ref, &id_len);
              g_autofree char *parent_id = strip_last_element (id, id_len);

              parent_ref = flatpak_decomposed_new_from_decomposed (ref, FLATPAK_KINDS_RUNTIME,
                                                                   parent_id, NULL, NULL, NULL);

              if (parent_ref != NULL &&
                  g_hash_table_lookup (pref_hash, flatpak_decomposed_get_pref (parent_ref)))
                continue;
            }

          if (!opt_all && opt_arch == NULL &&
              /* Hide non-primary arches if the primary arch exists */
              !flatpak_decomposed_is_arch (ref, arches[0]))
            {
              g_autoptr(FlatpakDecomposed) alt_arch = flatpak_decomposed_new_from_decomposed (ref, 0, NULL, arches[0], NULL, NULL);

              if (alt_arch && g_hash_table_lookup (refs, alt_arch))
                continue;
            }

          if (g_hash_table_lookup (names, ref) == NULL)
            g_hash_table_insert (names, flatpak_decomposed_ref (ref), g_strdup (checksum));
        }

      if (need_appstream_data)
        {
          mdata = as_metadata_new ();
          flatpak_dir_load_appstream_data (dir, remote, NULL, mdata, NULL, NULL);
        }

      keys = (FlatpakDecomposed **) g_hash_table_get_keys_as_array (names, &n_keys);
      qsort (keys, n_keys, sizeof (char *), (GCompareFunc) flatpak_decomposed_strcmp_p);

      for (i = 0; i < n_keys; i++)
        {
          FlatpakDecomposed *ref = keys[i];
          const char *ref_str = flatpak_decomposed_get_ref (ref);
          guint64 installed_size;
          guint64 download_size;
          g_autofree char *runtime = NULL;
          AsComponent *cpt = NULL;
          gboolean has_sparse_cache;
          VarMetadataRef sparse_cache;
          g_autofree char *id = flatpak_decomposed_dup_id (ref);
          g_autofree char *arch = flatpak_decomposed_dup_arch (ref);
          g_autofree char *branch = flatpak_decomposed_dup_branch (ref);

          /* The sparse cache is optional */
          has_sparse_cache = flatpak_remote_state_lookup_sparse_cache (state, ref_str, &sparse_cache, NULL);
          if (!opt_all && has_sparse_cache)
            {
              const char *eol = var_metadata_lookup_string (sparse_cache, FLATPAK_SPARSE_CACHE_KEY_ENDOFLIFE, NULL);
              const char *eol_rebase = var_metadata_lookup_string (sparse_cache, FLATPAK_SPARSE_CACHE_KEY_ENDOFLIFE_REBASE, NULL);

              if (eol != NULL || eol_rebase != NULL)
                continue;
            }

          if (need_cache_data)
            {
              g_autofree char *metadata = NULL;
              g_autoptr(GKeyFile) metakey = NULL;

              if (!flatpak_remote_state_load_data (state, ref_str,
                                                   &download_size, &installed_size, &metadata,
                                                   error))
                return FALSE;

              metakey = g_key_file_new ();
              if (g_key_file_load_from_data (metakey, metadata, -1, 0, NULL))
                runtime = g_key_file_get_string (metakey, "Application", "runtime", NULL);
            }

          if (need_appstream_data)
            cpt = metadata_find_component (mdata, ref_str);

          if (app_runtime && runtime)
            {
              g_auto(GStrv) pref = g_strsplit (runtime, "/", 3);
              if ((match_id && pref[0] && strcmp (pref[0], match_id) != 0) ||
                  (match_arch && pref[1] && strcmp (pref[1], match_arch) != 0) ||
                  (match_branch && pref[2] && strcmp (pref[2], match_branch) != 0))
                continue;
            }

          for (j = 0; columns[j].name; j++)
            {
              if (strcmp (columns[j].name, "name") == 0)
                {
                  const char *name = NULL;
                  g_autofree char *readable_id = NULL;

                  if (cpt)
                    name = as_component_get_name (cpt);

                  if (name == NULL)
                    readable_id = flatpak_decomposed_dup_readable_id (ref);

                  flatpak_table_printer_add_column (printer, name ? name : readable_id);
                }
              else if (strcmp (columns[j].name, "description") == 0)
                {
                  const char *comment = NULL;
                  if (cpt)
                      comment = as_component_get_summary (cpt);

                  flatpak_table_printer_add_column (printer, comment);
                }
              else if (strcmp (columns[j].name, "version") == 0)
                flatpak_table_printer_add_column (printer, cpt ? component_get_version_latest (cpt) : "");
              else if (strcmp (columns[j].name, "ref") == 0)
                flatpak_table_printer_add_column (printer, ref_str);
              else if (strcmp (columns[j].name, "application") == 0)
                flatpak_table_printer_add_column (printer, id);
              else if (strcmp (columns[j].name, "arch") == 0)
                flatpak_table_printer_add_column (printer, arch);
              else if (strcmp (columns[j].name, "branch") == 0)
                flatpak_table_printer_add_column (printer, branch);
              else if (strcmp (columns[j].name, "origin") == 0)
                flatpak_table_printer_add_column (printer, remote);
              else if (strcmp (columns[j].name, "commit") == 0)
                {
                  g_autofree char *value = NULL;

                  value = g_strdup ((char *) g_hash_table_lookup (names, keys[i]));
                  value[MIN (strlen (value), 12)] = 0;
                  flatpak_table_printer_add_column (printer, value);
                }
              else if (strcmp (columns[j].name, "installed-size") == 0)
                {
                  g_autofree char *installed = g_format_size (installed_size);
                  flatpak_table_printer_add_decimal_column (printer, installed);
                }
              else if (strcmp (columns[j].name, "download-size") == 0)
                {
                  g_autofree char *download = g_format_size (download_size);
                  flatpak_table_printer_add_decimal_column (printer, download);
                }
              else if (strcmp (columns[j].name, "runtime") == 0)
                {
                  flatpak_table_printer_add_column (printer, runtime);
                }
              else if (strcmp (columns[j].name, "options") == 0)
                {
                  flatpak_table_printer_add_column (printer, ""); /* Extra */
                  if (has_sparse_cache)
                    {
                      const char *eol = var_metadata_lookup_string (sparse_cache, FLATPAK_SPARSE_CACHE_KEY_ENDOFLIFE, NULL);
                      const char *eol_rebase = var_metadata_lookup_string (sparse_cache, FLATPAK_SPARSE_CACHE_KEY_ENDOFLIFE_REBASE, NULL);

                      if (eol)
                        flatpak_table_printer_append_with_comma_printf (printer, "eol=%s", eol);
                      if (eol_rebase)
                        flatpak_table_printer_append_with_comma_printf (printer, "eol-rebase=%s", eol_rebase);
                    }
                }
            }

          flatpak_table_printer_finish_row (printer);
        }
    }

  if (flatpak_table_printer_get_current_row (printer) > 0)
    {
      opt_json ? flatpak_table_printer_print_json (printer) : flatpak_table_printer_print (printer);
    }

  return TRUE;
}

gboolean
flatpak_builtin_remote_ls (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  const char **arches = flatpak_get_arches ();
  const char *opt_arches[] = {NULL, NULL};
  gboolean has_remote;
  g_autoptr(GHashTable) refs_hash = g_hash_table_new_full (g_direct_hash, g_direct_equal, (GDestroyNotify) g_hash_table_unref, (GDestroyNotify) remote_state_dir_pair_free);
  g_autofree char *col_help = NULL;
  g_autofree Column *columns = NULL;

  context = g_option_context_new (_(" [REMOTE or URI] - Show available runtimes and applications"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);
  col_help = column_help (all_columns);
  g_option_context_set_description (context, col_help);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS, &dirs, cancellable, error))
    return FALSE;

  if (!opt_app && !opt_runtime)
    {
      opt_app = TRUE;
      opt_runtime = !opt_app_runtime;
    }

  if (argc > 2)
    return usage_error (context, _("Too many arguments"), error);

  has_remote = (argc == 2);

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

  if (has_remote)
    {
      g_autoptr(FlatpakDir) preferred_dir = NULL;
      g_autoptr(GHashTable) refs = NULL;
      RemoteStateDirPair *remote_state_dir_pair = NULL;
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

      state = get_remote_state (preferred_dir, argv[1], opt_cached, opt_sideloaded, opt_arches[0], NULL, cancellable, error);
      if (state == NULL)
        return FALSE;

      if (arches == NULL &&
          !ensure_remote_state_all_arches (preferred_dir, state, opt_cached, opt_sideloaded, cancellable, error))
        return FALSE;

      if (!flatpak_dir_list_remote_refs (preferred_dir, state, &refs,
                                         cancellable, error))
        return FALSE;

      remote_state_dir_pair = remote_state_dir_pair_new (preferred_dir, g_steal_pointer (&state));
      g_hash_table_insert (refs_hash, g_steal_pointer (&refs), remote_state_dir_pair);
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
              RemoteStateDirPair *remote_state_dir_pair = NULL;
              const char *remote_name = remotes[j];
              g_autoptr(FlatpakRemoteState) state = NULL;

              if (flatpak_dir_get_remote_disabled (dir, remote_name))
                continue;

              state = get_remote_state (dir, remote_name, opt_cached, opt_sideloaded, opt_arches[0], NULL,
                                        cancellable, error);
              if (state == NULL)
                return FALSE;

              if (arches == NULL &&
                  !ensure_remote_state_all_arches (dir, state, opt_cached, opt_sideloaded, cancellable, error))
                return FALSE;

              if (!flatpak_dir_list_remote_refs (dir, state, &refs,
                                                 cancellable, error))
                return FALSE;

              remote_state_dir_pair = remote_state_dir_pair_new (dir, g_steal_pointer (&state));
              g_hash_table_insert (refs_hash, g_steal_pointer (&refs), remote_state_dir_pair);
            }
        }
    }

  /* show origin by default if listing multiple remotes */
  all_columns[5].def = !has_remote;

  columns = handle_column_args (all_columns, opt_show_details, opt_cols, error);
  if (columns == NULL)
    return FALSE;

  return ls_remote (refs_hash, arches, opt_app_runtime, columns, cancellable, error);
}

gboolean
flatpak_complete_remote_ls (FlatpakCompletion *completion)
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
      flatpak_complete_columns (completion, all_columns);

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
