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

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to search for"), N_("ARCH") },
  { NULL}
};

static GPtrArray *
get_remote_stores (GPtrArray *dirs, const char *arch, GCancellable *cancellable)
{
  GError *error = NULL;
  GPtrArray *ret = g_ptr_array_new_with_free_func (g_object_unref);
  guint i, j;

  if (arch == NULL)
    arch = flatpak_get_arch ();

  for (i = 0; i < dirs->len; ++i)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, i);
      g_autofree char *install_path = NULL;
      g_auto(GStrv) remotes = NULL;

      flatpak_log_dir_access (dir);

      install_path = g_file_get_path (flatpak_dir_get_path (dir));
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

          g_autofree char *appstream_path = NULL;

          if (flatpak_dir_get_remote_oci (dir, remotes[j]))
            appstream_path = g_build_filename (install_path, "appstream", remotes[j],
                                               arch, "appstream.xml.gz",
                                               NULL);
          else
            appstream_path = g_build_filename (install_path, "appstream", remotes[j],
                                               arch, "active", "appstream.xml.gz",
                                               NULL);

          g_autoptr(GFile) appstream_file = g_file_new_for_path (appstream_path);

          as_store_from_file (store, appstream_file, NULL, cancellable, &error);
          if (error)
            {
              // We want to ignore this error as it is harmless and valid
              // NOTE: appstream-glib doesn't have granular file-not-found error
              if (!g_str_has_suffix (error->message, "No such file or directory"))
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

static const char *
get_comment_localized (AsApp *app)
{
  const char * const * languages = g_get_language_names ();
  gsize i;

  for (i = 0; languages[i]; ++i)
    {
      const char *comment = as_app_get_comment (app, languages[i]);
      if (comment != NULL)
        return comment;
    }
  return NULL;
}

static void
print_app (MatchResult *res, FlatpakTablePrinter *printer)
{
  AsRelease *release = as_app_get_release_default (res->app);
  const char *version = release ? as_release_get_version (release) : NULL;
  const char *id = as_app_get_id_filename (res->app);
  guint i;

  flatpak_table_printer_add_column (printer, id);
  flatpak_table_printer_add_column (printer, version);
#if AS_CHECK_VERSION (0, 6, 1)
  flatpak_table_printer_add_column (printer, as_app_get_branch (res->app));
#endif
  flatpak_table_printer_add_column (printer, g_ptr_array_index (res->remotes, 0));
  for (i = 1; i < res->remotes->len; ++i)
    flatpak_table_printer_append_with_comma (printer, g_ptr_array_index (res->remotes, i));
  flatpak_table_printer_add_column (printer, get_comment_localized (res->app));
  flatpak_table_printer_finish_row (printer);
}

gboolean
flatpak_builtin_search (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(GOptionContext) context = g_option_context_new (_("TEXT - Search remote apps/runtimes for text"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("TEXT must be specified"), error);

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
              const char *app_id = as_app_get_id_filename (app);
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
      FlatpakTablePrinter *printer = flatpak_table_printer_new ();
      int col = 0;

      flatpak_table_printer_set_column_title (printer, col++, _("Application ID"));
      flatpak_table_printer_set_column_title (printer, col++, _("Version"));
#if AS_CHECK_VERSION (0, 6, 1)
      flatpak_table_printer_set_column_title (printer, col++, _("Branch"));
#endif
      flatpak_table_printer_set_column_title (printer, col++, _("Remotes"));
      flatpak_table_printer_set_column_title (printer, col++, _("Description"));
      g_slist_foreach (matches, (GFunc) print_app, printer);
      flatpak_table_printer_print (printer);
      flatpak_table_printer_free (printer);

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
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS, NULL, NULL, NULL))
    return FALSE;

  flatpak_complete_options (completion, global_entries);
  flatpak_complete_options (completion, user_entries);
  return TRUE;
}
