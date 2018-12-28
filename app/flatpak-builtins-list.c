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
#include "flatpak-run-private.h"

static gboolean opt_show_details;
static gboolean opt_runtime;
static gboolean opt_app;
static gboolean opt_all;
static char *opt_arch;
static char *opt_app_runtime;
static const char **opt_cols;
static const char **opt_filters;

static GOptionEntry options[] = {
  { "show-details", 'd', 0, G_OPTION_ARG_NONE, &opt_show_details, N_("Show extra information"), NULL },
  { "columns", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_cols, N_("What information to show"), N_("FIELD,…")  },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, N_("List installed applications"), NULL },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("List installed runtimes"), NULL },
  { "all", 'a', 0, G_OPTION_ARG_NONE, &opt_all, N_("List all refs (including locale/debug)"), NULL },
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to show"), N_("ARCH") },
  { "app-runtime", 'a', 0, G_OPTION_ARG_STRING, &opt_app_runtime, N_("List all applications using RUNTIME"), N_("RUNTIME") },
  { "match", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_filters, N_("List refs matching FILTER"), N_("FILTER,…") },
  { NULL }
};

static Column all_columns[] = {
  { "description",  N_("Description"),    N_("Show the description"),    1, 1 },
  { "application",  N_("Application"),    N_("Show the application ID"), 0, 1 },
  { "version",      N_("Version"),        N_("Show the version"),        1, 1 },
  { "branch",       N_("Branch"),         N_("Show the branch"),         0, 1 },
  { "arch",         N_("Arch"),           N_("Show the architecture"),   0, 1 },
  { "origin",       N_("Origin"),         N_("Show the origin remote"),  1, 1 },
  { "installation", N_("Installation"),   N_("Show the installation"),   1, 0 },
  { "ref",          N_("Ref"),            N_("Show the ref"),            1, 0 },
  { "active",       N_("Active commit"),  N_("Show the active commit"),  1, 0 },
  { "latest",       N_("Latest commit"),  N_("Show the latest commit"),  1, 0 },
  { "size",         N_("Installed size"), N_("Show the installed size"), 1, 0 },
  { "options",      N_("Options"),        N_("Show options"),            1, 0 },
  { NULL }
};

static gboolean
valid_filter (const char *filter, GError **error)
{
  if (g_str_has_prefix (filter, "runtime="))
    {
      if (!flatpak_split_partial_ref_arg (filter + strlen ("runtime="),
                                          FLATPAK_KINDS_RUNTIME, NULL, NULL,
                                          NULL, NULL, NULL, NULL,
                                          error))
        return FALSE;
      return TRUE;
    }
  else if (g_str_has_prefix (filter, "kind="))
    {
      const char *kinds[] = { "app", "runtime", "all", NULL };
      if (!g_strv_contains (kinds, filter + strlen ("kind=")))
        return flatpak_fail (error, _("kind must be 'app', 'runtime' or 'all'"));
      return TRUE;
    }
  else if (g_str_has_prefix (filter, "origin="))
    return TRUE;
  else if (g_str_has_prefix (filter, "arch="))
    {
      const char **arches = flatpak_get_arches ();
      if (!g_strv_contains (arches, filter + strlen ("arch=")))
        return flatpak_fail (error, _("Not an arch: %s"), filter + strlen ("arch="));
      return TRUE;
    }
  else if (g_str_has_prefix (filter, "permissions="))
    {
      if (!g_str_equal (filter + strlen ("permissions="), "network"))
        flatpak_fail (error, _("Not a permission: %s"), filter + strlen ("permissions="));
      return TRUE;
    }
  else if (g_str_has_prefix (filter, "search="))
    return TRUE;

  return flatpak_fail (error, _("Unknown filter: %s"), filter);
}

static char *
filter_help (void)
{
  GString *s = g_string_new ("");

  g_string_append (s, _("Available filters:\n"));
  g_string_append_printf (s, "  runtime=RUNTIME           %s\n", _("Show apps with this runtime"));
  g_string_append_printf (s, "  kind=[app|runtime|all]    %s\n", _("Show refs of this kind"));
  g_string_append_printf (s, "  origin=REMOTE             %s\n", _("Show refs from this remote"));
  g_string_append_printf (s, "  arch=ARCH                 %s\n", _("Show refs with the given arch"));
  g_string_append_printf (s, "  permissions=PERMISSION    %s\n", _("Show apps with the given permission, e.g. network"));
  g_string_append_printf (s, "  search=TEXT               %s\n", _("Show refs matching TEXT"));
  g_string_append_printf (s, "  help                      %s\n", _("Show available filters"));

  return g_string_free (s, FALSE);
}

static char **
handle_filters (const char **opt_filters, GError **error)
{
  g_autoptr(GPtrArray) filters = NULL;
  int i;

  filters = g_ptr_array_new_with_free_func (g_free);
  if (opt_app_runtime)
    g_ptr_array_add (filters, g_strconcat ("runtime=", opt_app_runtime, NULL));
  if (opt_arch)
    g_ptr_array_add (filters, g_strconcat ("arch=", opt_arch, NULL));

  if (opt_all)
    g_ptr_array_add (filters, g_strdup ("kind=all"));
  else if (opt_app)
    g_ptr_array_add (filters, g_strdup ("kind=app"));
  else if (opt_runtime)
    g_ptr_array_add (filters, g_strdup ("kind=runtime"));

  for (i = 0; opt_filters && opt_filters[i]; i++)
    {
      g_auto(GStrv) strv = g_strsplit (opt_filters[i], ",", 0);
      int j;
      for (j = 0; strv[j]; j++)
        {
	  if (g_str_equal (strv[j], "help"))
            {
              g_autofree char *help = filter_help ();
              g_print ("%s", help);
              return NULL;
            }

	  if (!valid_filter (strv[j], error))
            return NULL;

          g_ptr_array_add (filters, g_strdup (strv[j]));
        }
    }

  g_ptr_array_add (filters, NULL);

  return g_strdupv ((char **)filters->pdata);
}

/* Associates a flatpak installation's directory with
 * the list of references for apps and runtimes */
typedef struct
{
  FlatpakDir *dir;
  GStrv       refs;
} RefsData;

static RefsData *
refs_data_new (FlatpakDir *dir, const GStrv refs)
{
  RefsData *refs_data = g_new0 (RefsData, 1);

  refs_data->dir = g_object_ref (dir);
  refs_data->refs = g_strdupv ((char **) refs);
  return refs_data;
}

static void
refs_data_free (RefsData *refs_data)
{
  g_object_unref (refs_data->dir);
  g_strfreev (refs_data->refs);
  g_free (refs_data);
}

static char **
join_strv (char **a, char **b)
{
  gsize len = 1, i, j;
  char **res;

  if (a)
    len += g_strv_length (a);
  if (b)
    len += g_strv_length (b);

  res = g_new (char *, len);

  i = 0;

  for (j = 0; a != NULL && a[j] != NULL; j++)
    res[i++] = g_strdup (a[j]);

  for (j = 0; b != NULL && b[j] != NULL; j++)
    res[i++] = g_strdup (b[j]);

  res[i++] = NULL;
  return res;
}

static gboolean
find_refs_for_dir (FlatpakDir *dir,
                   GStrv *refs,
                   GCancellable *cancellable,
                   GError **error)
{
  if (flatpak_dir_ensure_repo (dir, cancellable, NULL))
    {
      g_auto(GStrv) apps = NULL;
      g_auto(GStrv) runtimes = NULL;

      if (!flatpak_dir_list_refs (dir, "app", &apps, cancellable, error))
        return FALSE;
      if (!flatpak_dir_list_refs (dir, "runtime", &runtimes, cancellable, error))
        return FALSE;
      *refs = join_strv (apps, runtimes);
    }

  return TRUE;
}

static gboolean
print_table_for_refs (GPtrArray * refs_array,
                      const char **filters,
                      Column *columns,
                      GCancellable *cancellable,
                      GError **error)
{
  FlatpakTablePrinter *printer;
  int i;
  FlatpakKinds match_kinds;
  g_autofree char *match_id = NULL;
  g_autofree char *match_arch = NULL;
  g_autofree char *match_branch = NULL;
  const char *runtime_filter = NULL;
  const char *kind_filter = NULL;
  const char *origin_filter = NULL;
  const char *arch_filter = NULL;
  const char *permissions_filter = NULL;
  const char *search_filter = NULL;
  int rows, cols;

  if (columns[0].name == NULL)
    return TRUE;

  for (i = 0; filters && filters[i]; i++)
    {
      if (g_str_has_prefix (filters[i], "runtime="))
        runtime_filter = filters[i] + strlen ("runtime=");
      else if (g_str_has_prefix (filters[i], "kind="))
        kind_filter = filters[i] + strlen ("kind=");
      else if (g_str_has_prefix (filters[i], "origin="))
        origin_filter = filters[i] + strlen ("origin=");
      else if (g_str_has_prefix (filters[i], "arch="))
        arch_filter = filters[i] + strlen ("arch=");
      else if (g_str_has_prefix (filters[i], "permissions="))
        permissions_filter = filters[i] + strlen ("permissions=");
      else if (g_str_has_prefix (filters[i], "search="))
        search_filter = filters[i] + strlen ("search=");
      else
        return flatpak_fail (error, _("Unknown filter: %s"), filters[i]);
    }

  printer = flatpak_table_printer_new ();

  flatpak_table_printer_set_column_titles (printer, columns);

  for (i = 0; columns[i].name; i++)
    flatpak_table_printer_set_column_expand (printer, i, TRUE);
  flatpak_table_printer_set_column_ellipsize (printer,
                                              find_column (columns, "description", NULL),
                                              FLATPAK_ELLIPSIZE_MODE_END);
  flatpak_table_printer_set_column_ellipsize (printer,
                                              find_column (columns, "application", NULL),
                                              FLATPAK_ELLIPSIZE_MODE_START);

  if (runtime_filter)
    {
      if (!flatpak_split_partial_ref_arg (runtime_filter, FLATPAK_KINDS_RUNTIME, NULL, NULL,
                                          &match_kinds, &match_id, &match_arch, &match_branch, error))
        return FALSE;
    }

  for (i = 0; i < refs_array->len; i++)
    {
      RefsData *refs_data = NULL;
      FlatpakDir *dir = NULL;
      char **dir_refs = NULL;
      g_autoptr(GHashTable) pref_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      int j;

      refs_data = (RefsData *) g_ptr_array_index (refs_array, i);
      dir = refs_data->dir;
      dir_refs = refs_data->refs;

      for (j = 0; dir_refs[j] != NULL; j++)
        {
          char *ref = dir_refs[j];
          char *partial_ref = flatpak_make_valid_id_prefix (strchr (ref, '/') + 1);
          g_hash_table_insert (pref_hash, partial_ref, ref);
        }

      for (j = 0; dir_refs[j] != NULL; j++)
        {
          char *ref, *partial_ref;
          g_auto(GStrv) parts = NULL;
          const char *repo = NULL;
          g_autoptr(FlatpakDeploy) deploy = NULL;
          g_autoptr(GVariant) deploy_data = NULL;
          const char *active;
          const char *alt_id;
          const char *eol;
          const char *eol_rebase;
          const char *appdata_name;
          const char *appdata_summary;
          const char *appdata_version;
          g_autofree char *latest = NULL;
          g_autofree const char **subpaths = NULL;
          int k;

          ref = dir_refs[j];

          parts = g_strsplit (ref, "/", -1);
          partial_ref = strchr (ref, '/') + 1;

          if (arch_filter)
            {
              if (strcmp (arch_filter, parts[2]) != 0)
                continue;
            }

          deploy = flatpak_dir_load_deployed (dir, ref, NULL, cancellable, NULL);
          deploy_data = flatpak_deploy_get_deploy_data (deploy, FLATPAK_DEPLOY_VERSION_CURRENT, cancellable, NULL);
          if (deploy_data == NULL)
            continue;

          if (runtime_filter)
            {
              const char *runtime = flatpak_deploy_data_get_runtime (deploy_data);
              if (runtime)
                {
                  g_auto(GStrv) pref = g_strsplit (runtime, "/", 3);
                  if ((match_id && pref[0] && strcmp (pref[0], match_id) != 0) ||
                      (match_arch && pref[1] && strcmp (pref[1], match_arch) != 0) ||
                      (match_branch && pref[2] && strcmp (pref[2], match_branch) != 0))
                    continue;
                }
            }

          if (kind_filter)
            {
              if (strcmp (kind_filter, "all") != 0 &&
                  strcmp (kind_filter, parts[0]) != 0)
                continue;
            }

          if (g_strcmp0 (kind_filter, "all") != 0 && strcmp (parts[0], "runtime") == 0 &&
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


          repo = flatpak_deploy_data_get_origin (deploy_data);

          if (origin_filter)
            {
              if (strcmp (origin_filter, repo) != 0)
                continue;
            }

          if (permissions_filter)
            {
              g_autoptr(GKeyFile) metadata = NULL;
              g_auto(GStrv) shares = NULL;

              metadata = flatpak_deploy_get_metadata (deploy);
              shares = g_key_file_get_string_list (metadata, FLATPAK_METADATA_GROUP_CONTEXT,
                                                   FLATPAK_METADATA_KEY_SHARED, NULL, NULL);
              if (shares == NULL ||
                  !g_strv_contains ((const char *const *)shares, permissions_filter))
                continue;
            }

          active = flatpak_deploy_data_get_commit (deploy_data);
          alt_id = flatpak_deploy_data_get_alt_id (deploy_data);
          eol = flatpak_deploy_data_get_eol (deploy_data);
          eol_rebase = flatpak_deploy_data_get_eol_rebase (deploy_data);
          appdata_name = flatpak_deploy_data_get_appdata_name (deploy_data);
          appdata_summary = flatpak_deploy_data_get_appdata_summary (deploy_data);
          appdata_version = flatpak_deploy_data_get_appdata_version (deploy_data);

          if (search_filter)
            {
              /* FIXME: this is a bit crude, compared to as_app_search_matches */
              if ((!appdata_name || !g_str_match_string (search_filter, appdata_name, TRUE)) &&
                  (!appdata_summary || !g_str_match_string (search_filter, appdata_summary, TRUE)) &&
                  !g_str_match_string (search_filter, parts[1], TRUE))
                 continue;
            }

          latest = flatpak_dir_read_latest (dir, repo, ref, NULL, NULL, NULL);
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
              if (strcmp (columns[k].name, "description") == 0)
                {
                  g_autofree char *description = NULL;
                  const char *name = appdata_name ? appdata_name : strrchr (parts[1], '.') + 1;

                  if (appdata_summary)
                    description =  g_strconcat (name, " - ", appdata_summary, NULL);
                  else
                    description =  g_strdup (name);
                  flatpak_table_printer_add_column (printer, description);
                }
              else if (strcmp (columns[k].name, "version") == 0)
                flatpak_table_printer_add_column (printer, appdata_version ? appdata_version : "");
              else if (strcmp (columns[k].name, "installation") == 0)
                flatpak_table_printer_add_column (printer, flatpak_dir_get_name_cached (dir));
              else if (strcmp (columns[k].name, "ref") == 0)
                flatpak_table_printer_add_column (printer, partial_ref);
              else if (strcmp (columns[k].name, "application") == 0)
                flatpak_table_printer_add_column (printer, parts[1]);
              else if (strcmp (columns[k].name, "arch") == 0)
                flatpak_table_printer_add_column (printer, parts[2]);
              else if (strcmp (columns[k].name, "branch") == 0)
                flatpak_table_printer_add_column (printer, parts[3]);
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

                  if (strcmp (parts[0], "app") == 0)
                    {
                      g_autofree char *current = flatpak_dir_current_ref (dir, parts[1], cancellable);
                      if (current && strcmp (ref, current) == 0)
                        flatpak_table_printer_append_with_comma (printer, "current");
                    }
                  else
                    {
                      flatpak_table_printer_append_with_comma (printer, "runtime");
                    }

                  subpaths = flatpak_deploy_data_get_subpaths (deploy_data);
                  if (subpaths[0] != NULL)
                    {
                      g_autofree char *paths = g_strjoinv (" ", (char **)subpaths);
                      g_autofree char *value = g_strconcat ("partial (", paths, ")", NULL);
                      flatpak_table_printer_append_with_comma (printer, value);
                    }

                  if (eol)
                    flatpak_table_printer_append_with_comma_printf (printer, "eol=%s", eol);
                  if (eol_rebase)
                    flatpak_table_printer_append_with_comma_printf (printer, "eol-rebase=%s", eol_rebase);
                }
            }

          flatpak_table_printer_set_key (printer, ref);
          flatpak_table_printer_finish_row (printer);
        }
    }

  flatpak_table_printer_sort (printer, (GCompareFunc)flatpak_compare_ref);

  flatpak_get_window_size (&rows, &cols);
  flatpak_table_printer_print_full (printer, 0, cols, NULL, NULL);
  g_print ("\n");

  flatpak_table_printer_free (printer);

  return TRUE;
}

static gboolean
print_installed_refs (GPtrArray *dirs,
                      const char **filters,
                      Column *cols,
                      GCancellable *cancellable,
                      GError **error)
{
  g_autoptr(GPtrArray) refs_array = NULL;
  int i;

  refs_array = g_ptr_array_new_with_free_func ((GDestroyNotify) refs_data_free);

  for (i = 0; i < dirs->len; i++)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, i);
      g_auto(GStrv) refs = NULL;

      if (!find_refs_for_dir (dir, &refs, cancellable, error))
        return FALSE;
      g_ptr_array_add (refs_array, refs_data_new (dir, refs));
    }

  if (!print_table_for_refs (refs_array, filters, cols, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_builtin_list (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autofree char *col_help = NULL;
  g_autofree char *fil_help = NULL;
  g_autofree char *combined_help = NULL;
  g_autofree Column *columns = NULL;
  g_auto(GStrv) filters = NULL;

  context = g_option_context_new (_(" - List installed apps and/or runtimes"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);
  col_help = column_help (all_columns);
  fil_help = filter_help ();
  combined_help = g_strconcat (col_help, "\n\n", fil_help, NULL);
  g_option_context_set_description (context, combined_help);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, cancellable, error))
    return FALSE;

  if (argc > 1)
    return usage_error (context, _("Too many arguments"), error);

  /* Default to showing installation if we're listing multiple installations */
  if (dirs->len > 1)
    {
      int c = find_column (all_columns, "installation", NULL);
      all_columns[c].def = 1;
    }

  columns = handle_column_args (all_columns, opt_show_details, opt_cols, error);
  if (columns == NULL)
    return FALSE;

  filters = handle_filters (opt_filters, error);
  if (filters == NULL)
    return !*error;

  return print_installed_refs (dirs, (const char **)filters, columns, cancellable, error);
}

gboolean
flatpak_complete_list (FlatpakCompletion *completion)
{
  flatpak_complete_options (completion, global_entries);
  flatpak_complete_options (completion, options);
  flatpak_complete_options (completion, user_entries);
  return TRUE;
}
