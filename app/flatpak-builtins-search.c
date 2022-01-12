/*
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
#include <appstream-glib.h>

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-dir-private.h"
#include "flatpak-table-printer.h"
#include "flatpak-utils-private.h"

static char *opt_arch;
static const char **opt_cols;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to search for"), N_("ARCH") },
  { "columns", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_cols, N_("What information to show"), N_("FIELD,…") },
  { NULL}
};

static Column all_columns[] = {
  { "name",        N_("Name"),        N_("Show the name"),               1, FLATPAK_ELLIPSIZE_MODE_END, 1, 1 },
  { "description", N_("Description"), N_("Show the description"),        1, FLATPAK_ELLIPSIZE_MODE_END, 1, 1 },
  { "application", N_("Application ID"), N_("Show the application ID"),     1, FLATPAK_ELLIPSIZE_MODE_START, 1, 1 },
  { "version",     N_("Version"),     N_("Show the version"),            1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
#if AS_CHECK_VERSION (0, 6, 1)
  { "branch",      N_("Branch"),      N_("Show the application branch"), 1, FLATPAK_ELLIPSIZE_MODE_NONE, 1, 1 },
#endif
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
          g_autoptr(AsStore) store = as_store_new ();

#if AS_CHECK_VERSION (0, 6, 1)
          // We want to see multiple versions/branches of same app-id's, e.g. org.gnome.Platform
          as_store_set_add_flags (store, as_store_get_add_flags (store) | AS_STORE_ADD_FLAG_USE_UNIQUE_ID);
#endif

          flatpak_dir_load_appstream_store (dir, remotes[j], arch, store, cancellable, &error);

          if (error)
            {
              g_warning ("%s", error->message);
              g_clear_error (&error);
            }

          g_object_set_data_full (G_OBJECT (store), "remote-name", g_strdup (remotes[j]), g_free);
          g_ptr_array_add (ret, g_steal_pointer (&store));
        }
    }
  return ret;
}

static void
clear_app_arches (AsApp *app)
{
  GPtrArray *arches = as_app_get_architectures (app);

  g_ptr_array_set_size (arches, 0);
}

typedef struct MatchResult
{
  AsApp     *app;
  GPtrArray *remotes;
  guint      score;
} MatchResult;

static void
match_result_free (MatchResult *result)
{
  g_object_unref (result->app);
  g_ptr_array_unref (result->remotes);
  g_free (result);
}

static MatchResult *
match_result_new (AsApp *app, guint score)
{
  MatchResult *result = g_new (MatchResult, 1);

  result->app = g_object_ref (app);
  result->remotes = g_ptr_array_new_with_free_func (g_free);
  result->score = score;

  clear_app_arches (result->app);

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

#if !AS_CHECK_VERSION (0, 6, 1)
/* Roughly copied directly from appstream-glib */

static const gchar *
as_app_fix_unique_nullable (const gchar *tmp)
{
  if (tmp == NULL || tmp[0] == '\0')
    return "*";
  return tmp;
}

static char *
as_app_get_unique_id (AsApp *app)
{
  const gchar *id_str = NULL;
  const gchar *kind_str = NULL;
  AsAppKind kind = as_app_get_kind (app);

  if (kind != AS_APP_KIND_UNKNOWN)
    kind_str = as_app_kind_to_string (kind);
  id_str = as_app_get_id_no_prefix (app);
  return g_strdup_printf ("%s/%s",
                          as_app_fix_unique_nullable (kind_str),
                          as_app_fix_unique_nullable (id_str));
}

static gboolean
as_app_equal (AsApp *app1, AsApp *app2)
{
  if (app1 == app2)
    return TRUE;

  g_autofree char *app1_id = as_app_get_unique_id (app1);
  g_autofree char *app2_id = as_app_get_unique_id (app2);
  return strcmp (app1_id, app2_id) == 0;
}
#endif

static char *
_app_get_id_no_suffix (AsApp *app)
{
  const char *id_stripped = NULL;
#if AS_CHECK_VERSION (0, 5, 15)
  GPtrArray *bundles = NULL;

  /* First try using the <bundle> ID which is unambiguously the flatpak ref */
  bundles = as_app_get_bundles (app);
  for (guint i = 0; i < bundles->len; i++)
    {
      g_autoptr(FlatpakDecomposed) decomposed = NULL;
      AsBundle *bundle = g_ptr_array_index (bundles, i);
      if (as_bundle_get_kind (bundle) != AS_BUNDLE_KIND_FLATPAK)
        continue;

      decomposed = flatpak_decomposed_new_from_ref (as_bundle_get_id (bundle), NULL);
      if (decomposed != NULL)
        return flatpak_decomposed_dup_id (decomposed);
    }
#endif

  /* Fall back to using the <id> field, which is required by appstream spec,
   * but make sure the .desktop suffix isn't stripped overzealously
   * https://github.com/hughsie/appstream-glib/issues/420
   */
  id_stripped = as_app_get_id_filename (app);
  if (flatpak_is_valid_name (id_stripped, -1, NULL))
    return g_strdup (id_stripped);
  else
    {
      g_autofree char *id_with_desktop = g_strconcat (id_stripped, ".desktop", NULL);
      const char *id_with_suffix = as_app_get_id_no_prefix (app);
      if (flatpak_is_valid_name (id_with_desktop, -1, NULL) &&
          g_strcmp0 (id_with_suffix, id_with_desktop) == 0)
        return g_strdup (id_with_suffix);
      else
        return g_strdup (id_stripped);
    }
}

static int
compare_apps (MatchResult *a, AsApp *b)
{
  /* For now we want to ignore arch when comparing applications
   * It may be valuable to show runtime arches in the future though.
   * This is a naughty hack but for our purposes totally fine.
   */
  clear_app_arches (b);

  return !as_app_equal (a->app, b);
}

static void
print_app (Column *columns, MatchResult *res, FlatpakTablePrinter *printer)
{
  const char *version = as_app_get_version (res->app);
  g_autofree char *id = _app_get_id_no_suffix (res->app);
  const char *name = as_app_get_localized_name (res->app);
  const char *comment = as_app_get_localized_comment (res->app);
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
#if AS_CHECK_VERSION (0, 6, 1)
      else if (strcmp (columns[i].name, "branch") == 0)
        flatpak_table_printer_add_column (printer, as_app_get_branch (res->app));
#endif
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
  int rows, cols;
  GSList *s;

  printer = flatpak_table_printer_new ();

  flatpak_table_printer_set_columns (printer, columns, opt_cols != NULL);

  for (s = matches; s; s = s->next)
    {
      MatchResult *res = s->data;
      print_app (columns, res, printer);
    }

  flatpak_get_window_size (&rows, &cols);
  flatpak_table_printer_print_full (printer, 0, cols, NULL, NULL);
  g_print ("\n");
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
  // as AsApp doesn't currently contain that information
  g_autoptr(GPtrArray) remote_stores = get_remote_stores (dirs, opt_arch, cancellable);
  for (j = 0; j < remote_stores->len; ++j)
    {
      AsStore *store = g_ptr_array_index (remote_stores, j);
      GPtrArray *apps = as_store_get_apps (store);
      guint i;

      for (i = 0; i < apps->len; ++i)
        {
          AsApp *app = g_ptr_array_index (apps, i);
          guint score = as_app_search_matches (app, search_text);
          if (score == 0)
            {
              g_autofree char *app_id = _app_get_id_no_suffix (app);
              if (strcasestr (app_id, search_text) != NULL)
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
          match_result_add_remote (result,
                                   g_object_get_data (G_OBJECT (store), "remote-name"));
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
  flatpak_complete_options (completion, user_entries);
  return TRUE;
}
