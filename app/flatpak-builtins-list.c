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
static char *opt_arch;
static char *opt_app_runtime;
static const char **opt_cols;

static GOptionEntry options[] = {
  { "show-details", 'd', 0, G_OPTION_ARG_NONE, &opt_show_details, N_("Show extra information"), NULL },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("List installed runtimes"), NULL },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, N_("List installed applications"), NULL },
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to show"), N_("ARCH") },
  { "all", 'a', 0, G_OPTION_ARG_NONE, &opt_all, N_("List all refs (including locale/debug)"), NULL },
  { "app-runtime", 0, 0, G_OPTION_ARG_STRING, &opt_app_runtime, N_("List all applications using RUNTIME"), N_("RUNTIME") },
  { "columns", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_cols, N_("What information to show"), N_("FIELD,…")  },
  { NULL }
};

static Column all_columns[] = {
  { "name",         N_("Name"),           N_("Show the name"),           1, FLATPAK_ELLIPSIZE_MODE_END, 1, 1 },
  { "description",  N_("Description"),    N_("Show the description"),    1, FLATPAK_ELLIPSIZE_MODE_END, 1, 0 },
  { "application",  N_("Application ID"), N_("Show the application ID"), 1, FLATPAK_ELLIPSIZE_MODE_START, 0, 1 },
  { "version",      N_("Version"),        N_("Show the version"),        1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "branch",       N_("Branch"),         N_("Show the branch"),         1, FLATPAK_ELLIPSIZE_MODE_NONE, 0, 1 },
  { "arch",         N_("Arch"),           N_("Show the architecture"),   1, FLATPAK_ELLIPSIZE_MODE_NONE, 0, 1, 1 },
  { "runtime",      N_("Runtime"),        N_("Show the used runtime"),   1, FLATPAK_ELLIPSIZE_MODE_START, 0, 0 },
  { "origin",       N_("Origin"),         N_("Show the origin remote"),  1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1, 1 },
  { "installation", N_("Installation"),   N_("Show the installation"),   1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "ref",          N_("Ref"),            N_("Show the ref"),            1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "active",       N_("Active commit"),  N_("Show the active commit"),  1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "latest",       N_("Latest commit"),  N_("Show the latest commit"),  1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "size",         N_("Installed size"), N_("Show the installed size"), 1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 0 },
  { "options",      N_("Options"),        N_("Show options"),            1, FLATPAK_ELLIPSIZE_MODE_END, 1, 0 },
  { NULL }
};

/* Associates a flatpak installation's directory with
 * the list of references for apps and runtimes */
typedef struct
{
  FlatpakDir *dir;
  GPtrArray  *refs;
} RefsData;

static RefsData *
refs_data_new (FlatpakDir *dir, GPtrArray *refs)
{
  RefsData *refs_data = g_new0 (RefsData, 1);

  refs_data->dir = g_object_ref (dir);
  refs_data->refs = g_ptr_array_ref (refs);
  return refs_data;
}

static void
refs_data_free (RefsData *refs_data)
{
  g_object_unref (refs_data->dir);
  g_ptr_array_unref (refs_data->refs);
  g_free (refs_data);
}

static char *
strip_last_element (const char *id)
{
  gsize id_len = strlen (id);
  while (id_len > 0 &&
         id[id_len - 1] != '.')
    id_len--;

  if (id_len > 0)
    id_len--; /* Remove the dot too */

  return g_strndup (id, id_len);
}

static gboolean
print_table_for_refs (gboolean      print_apps,
                      GPtrArray   * refs_array,
                      const char   *arch,
                      const char   *app_runtime,
                      Column       *columns,
                      GCancellable *cancellable,
                      GError      **error)
{
  g_autoptr(FlatpakTablePrinter) printer = NULL;
  int i;
  FlatpakKinds match_kinds;
  g_autofree char *match_id = NULL;
  g_autofree char *match_arch = NULL;
  g_autofree char *match_branch = NULL;
  int rows, cols;

  if (columns[0].name == NULL)
    return TRUE;

  printer = flatpak_table_printer_new ();

  flatpak_table_printer_set_columns (printer, columns,
                                     opt_cols == NULL && !opt_show_details);

  if (app_runtime)
    {
      if (!flatpak_split_partial_ref_arg (app_runtime, FLATPAK_KINDS_RUNTIME, NULL, NULL,
                                          &match_kinds, &match_id, &match_arch, &match_branch, error))
        return FALSE;
    }

  for (i = 0; i < refs_array->len; i++)
    {
      RefsData *refs_data = NULL;
      FlatpakDir *dir = NULL;
      GPtrArray *dir_refs = NULL;
      g_autoptr(GHashTable) pref_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      int j;

      refs_data = (RefsData *) g_ptr_array_index (refs_array, i);
      dir = refs_data->dir;
      dir_refs = refs_data->refs;

      for (j = 0; j < dir_refs->len; j++)
        {
          FlatpakDecomposed *ref = g_ptr_array_index (dir_refs, j);
          char *partial_ref = flatpak_make_valid_id_prefix (flatpak_decomposed_get_pref (ref));
          g_hash_table_insert (pref_hash, partial_ref, ref);
        }

      for (j = 0; j < dir_refs->len; j++)
        {
          FlatpakDecomposed *ref = g_ptr_array_index (dir_refs, j);
          const char *partial_ref;
          const char *repo = NULL;
          g_autoptr(FlatpakDeploy) deploy = NULL;
          g_autoptr(GBytes) deploy_data = NULL;
          const char *active;
          const char *alt_id;
          const char *eol;
          const char *eol_rebase;
          const char *appdata_name;
          const char *appdata_summary;
          const char *appdata_version;
          const char *runtime;
          g_autofree char *latest = NULL;
          g_autofree const char **subpaths = NULL;
          int k;

          partial_ref = flatpak_decomposed_get_pref (ref);

          if (arch != NULL && !flatpak_decomposed_is_arch (ref, arch))
            continue;

          deploy = flatpak_dir_load_deployed (dir, ref, NULL, cancellable, NULL);
          deploy_data = flatpak_deploy_get_deploy_data (deploy, FLATPAK_DEPLOY_VERSION_CURRENT, cancellable, NULL);
          if (deploy_data == NULL)
            continue;

          runtime = flatpak_deploy_data_get_runtime (deploy_data);

          if (app_runtime)
            {
              if (runtime)
                {
                  g_auto(GStrv) pref = g_strsplit (runtime, "/", 3);
                  if ((match_id && pref[0] && strcmp (pref[0], match_id) != 0) ||
                      (match_arch && pref[1] && strcmp (pref[1], match_arch) != 0) ||
                      (match_branch && pref[2] && strcmp (pref[2], match_branch) != 0))
                    continue;
                }
            }

          g_autofree char *ref_id = flatpak_decomposed_dup_id (ref);
          g_autofree char *ref_arch = flatpak_decomposed_dup_arch (ref);
          g_autofree char *ref_branch = flatpak_decomposed_dup_branch (ref);

          if (!opt_all &&
              flatpak_decomposed_is_runtime (ref) &&
              flatpak_decomposed_id_is_subref (ref))
            {
              g_autofree char *parent_id = strip_last_element (ref_id);
              g_autofree char *prefix_partial_ref = g_strconcat (parent_id, "/", ref_arch, "/", ref_branch, NULL);

              if (g_hash_table_lookup (pref_hash, prefix_partial_ref))
                continue;
            }

          repo = flatpak_deploy_data_get_origin (deploy_data);

          active = flatpak_deploy_data_get_commit (deploy_data);
          alt_id = flatpak_deploy_data_get_alt_id (deploy_data);
          eol = flatpak_deploy_data_get_eol (deploy_data);
          eol_rebase = flatpak_deploy_data_get_eol_rebase (deploy_data);
          appdata_name = flatpak_deploy_data_get_appdata_name (deploy_data);
          appdata_summary = flatpak_deploy_data_get_appdata_summary (deploy_data);
          appdata_version = flatpak_deploy_data_get_appdata_version (deploy_data);

          latest = flatpak_dir_read_latest (dir, repo, flatpak_decomposed_get_ref (ref), NULL, NULL, NULL);
          if (latest)
            {
              if (strcmp (active, latest) == 0)
                {
                  g_free (latest);
                  latest = g_strdup ("-");
                }
              else
                {
                  latest[MIN (strlen (latest), 12)] = 0;
                }
            }
          else
            {
              latest = g_strdup ("?");
            }

          for (k = 0; columns[k].name; k++)
            {
              if (strcmp (columns[k].name, "name") == 0)
                {
                  const char *name = NULL;
                  g_autofree char *readable_id = NULL;

                  if (appdata_name)
                    name = appdata_name;
                  else
                    {
                      readable_id = flatpak_decomposed_dup_readable_id (ref);
                      name = readable_id;
                    }

                  flatpak_table_printer_add_column (printer, name);
                }
              else if (strcmp (columns[k].name, "description") == 0)
                {
                  const char *description = appdata_summary ? appdata_summary : "";
                  flatpak_table_printer_add_column (printer, description);
                }
              else if (strcmp (columns[k].name, "version") == 0)
                flatpak_table_printer_add_column (printer, appdata_version ? appdata_version : "");
              else if (strcmp (columns[k].name, "installation") == 0)
                flatpak_table_printer_add_column (printer, flatpak_dir_get_name_cached (dir));
              else if (strcmp (columns[k].name, "runtime") == 0)
                flatpak_table_printer_add_column (printer, runtime ? runtime : "");
              else if (strcmp (columns[k].name, "ref") == 0)
                flatpak_table_printer_add_column (printer, partial_ref);
              else if (strcmp (columns[k].name, "application") == 0)
                flatpak_table_printer_add_column (printer, ref_id);
              else if (strcmp (columns[k].name, "arch") == 0)
                flatpak_table_printer_add_column (printer, ref_arch);
              else if (strcmp (columns[k].name, "branch") == 0)
                flatpak_table_printer_add_column (printer, ref_branch);
              else if (strcmp (columns[k].name, "origin") == 0)
                flatpak_table_printer_add_column (printer, repo);
              else if (strcmp (columns[k].name, "active") == 0)
                flatpak_table_printer_add_column_len (printer, active, 12);
              else if (strcmp (columns[k].name, "latest") == 0)
                flatpak_table_printer_add_column_len (printer, latest, 12);
              else if (strcmp (columns[k].name, "size") == 0)
                {
                  g_autofree char *size_s = NULL;
                  guint64 size = 0;

                  size = flatpak_deploy_data_get_installed_size (deploy_data);
                  size_s = g_format_size (size);
                  flatpak_table_printer_add_decimal_column (printer, size_s);
                }
              else if (strcmp (columns[k].name, "options") == 0)
                {
                  flatpak_table_printer_add_column (printer, ""); /* Options */

                  if (refs_array->len > 1)
                    {
                      g_autofree char *source = flatpak_dir_get_name (dir);
                      flatpak_table_printer_append_with_comma (printer, source);
                    }

                  if (alt_id)
                    flatpak_table_printer_append_with_comma_printf (printer, "alt-id=%.12s", alt_id);

                  if (flatpak_decomposed_is_app (ref))
                    {
                      g_autoptr(FlatpakDecomposed) current = flatpak_dir_current_ref (dir, ref_id, cancellable);
                      if (current && flatpak_decomposed_equal (ref, current))
                        flatpak_table_printer_append_with_comma (printer, "current");
                    }
                  else
                    {
                      if (print_apps)
                        flatpak_table_printer_append_with_comma (printer, "runtime");
                    }

                  subpaths = flatpak_deploy_data_get_subpaths (deploy_data);
                  if (subpaths[0] != NULL)
                    {
                      g_autofree char *paths = g_strjoinv (" ", (char **) subpaths);
                      g_autofree char *value = g_strconcat ("partial (", paths, ")", NULL);
                      flatpak_table_printer_append_with_comma (printer, value);
                    }

                  if (eol)
                    flatpak_table_printer_append_with_comma_printf (printer, "eol=%s", eol);
                  if (eol_rebase)
                    flatpak_table_printer_append_with_comma_printf (printer, "eol-rebase=%s", eol_rebase);
                }
            }

          flatpak_table_printer_set_key (printer, flatpak_decomposed_get_ref (ref));
          flatpak_table_printer_finish_row (printer);
        }
    }

  flatpak_table_printer_sort (printer, (GCompareFunc) flatpak_compare_ref);

  if (flatpak_table_printer_get_current_row (printer) > 0)
    {
      flatpak_get_window_size (&rows, &cols);
      flatpak_table_printer_print_full (printer, 0, cols, NULL, NULL);
      g_print ("\n");
    }

  return TRUE;
}

static gboolean
print_installed_refs (gboolean      app,
                      gboolean      runtime,
                      GPtrArray    *dirs,
                      const char   *arch,
                      const char   *app_runtime,
                      Column       *cols,
                      GCancellable *cancellable,
                      GError      **error)
{
  g_autoptr(GPtrArray) refs_array = NULL;
  int i;

  refs_array = g_ptr_array_new_with_free_func ((GDestroyNotify) refs_data_free);

  for (i = 0; i < dirs->len; i++)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, i);
      g_autoptr(GPtrArray) refs = NULL;

      refs = flatpak_dir_list_refs (dir, flatpak_kinds_from_bools (app, runtime),
                                    cancellable, error);
      if (refs == NULL)
        return FALSE;

      g_ptr_array_add (refs_array, refs_data_new (dir, refs));
    }

  if (!print_table_for_refs (app, refs_array, arch, app_runtime, cols, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_builtin_list (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autofree char *col_help = NULL;
  g_autofree Column *columns = NULL;

  context = g_option_context_new (_(" - List installed apps and/or runtimes"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);
  col_help = column_help (all_columns);
  g_option_context_set_description (context, col_help);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, cancellable, error))
    return FALSE;

  if (argc > 1)
    return usage_error (context, _("Too many arguments"), error);

  if (!opt_app && !opt_runtime)
    {
      opt_app = TRUE;
      opt_runtime = !opt_app_runtime;
    }

  /* Default to showing installation if we're listing multiple installations */
  if (dirs->len > 1)
    {
      int c = find_column (all_columns, "installation", NULL);
      all_columns[c].def = 1;
    }

  columns = handle_column_args (all_columns, opt_show_details, opt_cols, error);
  if (columns == NULL)
    return FALSE;

  return print_installed_refs (opt_app, opt_runtime,
                               dirs,
                               opt_arch,
                               opt_app_runtime,
                               columns,
                               cancellable, error);
}

gboolean
flatpak_complete_list (FlatpakCompletion *completion)
{
  flatpak_complete_options (completion, global_entries);
  flatpak_complete_options (completion, options);
  flatpak_complete_options (completion, user_entries);
  flatpak_complete_columns (completion, all_columns);
  return TRUE;
}
