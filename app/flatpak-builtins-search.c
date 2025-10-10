/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright © 2017 Patrick Griffis
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
 *       Patrick Griffis <tingping@tingping.se>
 */

#include "config.h"

#include <glib/gi18n.h>
#include <appstream.h>

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-dir-private.h"
#include "flatpak-table-printer.h"
#include "flatpak-utils-private.h"

static char *opt_arch;
static const char **opt_cols;
static gboolean opt_json;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to search for"), N_("ARCH") },
  { "columns", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_cols, N_("What information to show"), N_("FIELD,…") },
  { "json", 'j', 0, G_OPTION_ARG_NONE, &opt_json, N_("Show output in JSON format"), NULL },
  { NULL}
};

static Column all_columns[] = {
  { "name",        N_("Name"),        N_("Show the name"),               1, FLATPAK_ELLIPSIZE_MODE_END, 1, 1 },
  { "description", N_("Description"), N_("Show the description"),        1, FLATPAK_ELLIPSIZE_MODE_END, 1, 1 },
  { "application", N_("Application ID"), N_("Show the application ID"),     1, FLATPAK_ELLIPSIZE_MODE_START, 1, 1 },
  { "version",     N_("Version"),     N_("Show the version"),            1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "branch",      N_("Branch"),      N_("Show the application branch"), 1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { "remotes",     N_("Remotes"),     N_("Show the remotes"),            1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
  { NULL }
};

static GPtrArray *
get_remote_stores (GPtrArray *dirs, const char *arch, GCancellable *cancellable)
{
  GError *error = NULL;
  GPtrArray *ret = g_ptr_array_new_with_free_func (g_object_unref);
  guint i, j;

  for (i = 0; i < dirs->len; ++i)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, i);
      g_auto(GStrv) remotes = NULL;

      flatpak_log_dir_access (dir);

      remotes = flatpak_dir_list_enumerated_remotes (dir, cancellable, &error);
      if (error)
        {
          g_warning ("%s", error->message);
          g_clear_error (&error);
          continue;
        }
      else if (remotes == NULL)
        continue;

      for (j = 0; remotes[j]; ++j)
        {
          g_autoptr(AsMetadata) mdata = as_metadata_new ();

          flatpak_dir_load_appstream_data (dir, remotes[j], arch, mdata, cancellable, &error);

          if (error)
            {
              g_warning ("%s", error->message);
              g_clear_error (&error);
            }

          g_object_set_data_full (G_OBJECT (mdata), "remote-name", g_strdup (remotes[j]), g_free);
          g_ptr_array_add (ret, g_steal_pointer (&mdata));
        }
    }
  return ret;
}

typedef struct MatchResult
{
  AsComponent *app;
  GPtrArray   *remotes;
  guint        score;
} MatchResult;

static void
match_result_free (MatchResult *result)
{
  g_object_unref (result->app);
  g_ptr_array_unref (result->remotes);
  g_free (result);
}

static MatchResult *
match_result_new (AsComponent *app, guint score)
{
  MatchResult *result = g_new (MatchResult, 1);

  result->app = g_object_ref (app);
  result->remotes = g_ptr_array_new_with_free_func (g_free);
  result->score = score;

  return result;
}

static void
match_result_add_remote (MatchResult *self, const char *remote)
{
  guint i;

  for (i = 0; i < self->remotes->len; ++i)
    {
      const char *remote_entry = g_ptr_array_index (self->remotes, i);
      if (!strcmp (remote, remote_entry))
        return;
    }
  g_ptr_array_add (self->remotes, g_strdup (remote));
}

static int
compare_by_score (MatchResult *a, MatchResult *b, gpointer user_data)
{
  // Reverse order, higher score comes first
  return (int) b->score - (int) a->score;
}

static gboolean
as_app_equal (AsComponent *app1, AsComponent *app2)
{
  if (app1 == app2)
    return TRUE;

  FlatpakDecomposed *app1_decomposed = g_object_get_data (G_OBJECT (app1), "decomposed");
  FlatpakDecomposed *app2_decomposed = g_object_get_data (G_OBJECT (app2), "decomposed");

  /* Ignore arch when comparing since it's not shown in the search output and
   * we don't want duplicate results for the same app with different arches.
   */
  return flatpak_decomposed_equal_except_arch (app1_decomposed, app2_decomposed);
}

static int
compare_apps (MatchResult *a, AsComponent *b)
{
  return !as_app_equal (a->app, b);
}

/* as_component_get_id() returns the appstream component ID which doesn't
 * necessarily match the flatpak app ID (e.g. sometimes there's a .desktop
 * suffix on the appstream ID) so this gets the flatpak app ID via the bundle
 * element
 */
static char *
component_get_flatpak_id (AsComponent *app)
{
  FlatpakDecomposed *app_decomposed = g_object_get_data (G_OBJECT (app), "decomposed");
  return flatpak_decomposed_dup_id (app_decomposed);
}

/* as_component_get_branch() seems to return NULL in practice, so use the
 * bundle id to get the branch
 */
static const char *
component_get_branch (AsComponent *app)
{
  FlatpakDecomposed *app_decomposed = g_object_get_data (G_OBJECT (app), "decomposed");
  return flatpak_decomposed_get_branch (app_decomposed);
}

static void
print_app (Column *columns, MatchResult *res, FlatpakTablePrinter *printer)
{
  const char *version = component_get_version_latest (res->app);
  g_autofree char *id = component_get_flatpak_id (res->app);
  const char *name = as_component_get_name (res->app);
  const char *comment = as_component_get_summary (res->app);
  guint i;

  for (i = 0; columns[i].name; i++)
    {
      if (strcmp (columns[i].name, "name") == 0)
        flatpak_table_printer_add_column (printer, name);
      if (strcmp (columns[i].name, "description") == 0)
        flatpak_table_printer_add_column (printer, comment);
      else if (strcmp (columns[i].name, "application") == 0)
        flatpak_table_printer_add_column (printer, id);
      else if (strcmp (columns[i].name, "version") == 0)
        flatpak_table_printer_add_column (printer, version);
      else if (strcmp (columns[i].name, "branch") == 0)
        flatpak_table_printer_add_column (printer, component_get_branch (res->app));
      else if (strcmp (columns[i].name, "remotes") == 0)
        {
          int j;
          flatpak_table_printer_add_column (printer, g_ptr_array_index (res->remotes, 0));
          for (j = 1; j < res->remotes->len; j++)
            flatpak_table_printer_append_with_comma (printer, g_ptr_array_index (res->remotes, j));
        }
    }
  flatpak_table_printer_finish_row (printer);
}

static void
print_matches (Column *columns, GSList *matches)
{
  g_autoptr(FlatpakTablePrinter) printer = NULL;
  GSList *s;

  printer = flatpak_table_printer_new ();

  flatpak_table_printer_set_columns (printer, columns, opt_cols != NULL);

  for (s = matches; s; s = s->next)
    {
      MatchResult *res = s->data;
      print_app (columns, res, printer);
    }

  opt_json ? flatpak_table_printer_print_json (printer) : flatpak_table_printer_print (printer);
}

gboolean
flatpak_builtin_search (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GPtrArray) dirs = NULL;
  g_autofree char *col_help = NULL;
  g_autofree Column *columns = NULL;
  g_autoptr(GOptionContext) context = g_option_context_new (_("TEXT - Search remote apps/runtimes for text"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);
  col_help = column_help (all_columns);
  g_option_context_set_description (context, col_help);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("TEXT must be specified"), error);

  columns = handle_column_args (all_columns, FALSE, opt_cols, error);
  if (columns == NULL)
    return FALSE;

  if (!update_appstream (dirs, NULL, opt_arch, FLATPAK_APPSTREAM_TTL, TRUE, cancellable, error))
    return FALSE;

  const char *search_text = argv[1];
  GSList *matches = NULL;
  guint j;

  // We want a store for each remote so we keep the remote information
  // as AsComponent doesn't currently contain that information
  g_autoptr(GPtrArray) remote_stores = get_remote_stores (dirs, opt_arch, cancellable);
  for (j = 0; j < remote_stores->len; ++j)
    {
      AsMetadata *mdata = g_ptr_array_index (remote_stores, j);
#if AS_CHECK_VERSION(1, 0, 0)
      AsComponentBox *apps = as_metadata_get_components (mdata);
#else
      GPtrArray *apps = as_metadata_get_components (mdata);
#endif

#if AS_CHECK_VERSION(1, 0, 0)
      for (guint i = 0; i < as_component_box_len (apps); ++i)
        {
          AsComponent *app = as_component_box_index (apps, i);
#else
      for (guint i = 0; i < apps->len; ++i)
        {
          AsComponent *app = g_ptr_array_index (apps, i);
#endif
          const char *remote_name = g_object_get_data (G_OBJECT (mdata), "remote-name");
          g_autoptr(FlatpakDecomposed) decomposed = NULL;

          AsBundle *bundle = as_component_get_bundle (app, AS_BUNDLE_KIND_FLATPAK);
          if (bundle == NULL || as_bundle_get_id (bundle) == NULL ||
              (decomposed = flatpak_decomposed_new_from_ref (as_bundle_get_id (bundle), NULL)) == NULL)
            {
              g_info ("Ignoring app %s from remote %s as it lacks a flatpak bundle",
                      as_component_get_id (app), remote_name);
              continue;
            }

          g_object_set_data_full (G_OBJECT (app), "decomposed", g_steal_pointer (&decomposed),
                                  (GDestroyNotify) flatpak_decomposed_unref);

          guint score = as_component_search_matches (app, search_text);
          if (score == 0)
            {
              g_autofree char *app_id = component_get_flatpak_id (app);
              const char *app_name = as_component_get_name (app);
              if (strcasestr (app_id, search_text) != NULL || strcasestr (app_name, search_text) != NULL)
                score = 50;
              else
                continue;
            }

          // Avoid duplicate entries, but show multiple remotes
          GSList *list_entry = g_slist_find_custom (matches, app,
                                                    (GCompareFunc) compare_apps);
          MatchResult *result = NULL;
          if (list_entry != NULL)
            result = list_entry->data;
          else
            {
              result = match_result_new (app, score);
              matches = g_slist_insert_sorted_with_data (matches, result,
                                                         (GCompareDataFunc) compare_by_score, NULL);
            }
          match_result_add_remote (result, remote_name);
        }
    }

  if (matches != NULL)
    {
      print_matches (columns, matches);
      g_slist_free_full (matches, (GDestroyNotify) match_result_free);
    }
  else
    {
      g_print ("%s\n", _("No matches found"));
    }
  return TRUE;
}

gboolean
flatpak_complete_search (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     NULL, NULL, NULL))
    return FALSE;

  flatpak_complete_options (completion, global_entries);
  flatpak_complete_options (completion, options);
  flatpak_complete_options (completion, user_entries);
  flatpak_complete_columns (completion, all_columns);
  return TRUE;
}
