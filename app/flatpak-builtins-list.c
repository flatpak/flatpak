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
#include "flatpak-utils-private.h"
#include "flatpak-table-printer.h"

static gboolean opt_show_details;
static gboolean opt_runtime;
static gboolean opt_app;
static gboolean opt_all;
static char *opt_arch;

static GOptionEntry options[] = {
  { "show-details", 'd', 0, G_OPTION_ARG_NONE, &opt_show_details, N_("Show extra information"), NULL },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("List installed runtimes"), NULL },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, N_("List installed applications"), NULL },
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to show"), N_("ARCH") },
  { "all", 'a', 0, G_OPTION_ARG_NONE, &opt_all, N_("List all refs (including locale/debug)"), NULL },
  { NULL }
};

/* Associates a flatpak installation's directory with
 * the list of references for apps and runtimes */
typedef struct
{
  FlatpakDir *dir;
  GStrv       app_refs;
  GStrv       runtime_refs;
} RefsData;

static RefsData *
refs_data_new (FlatpakDir *dir, const GStrv app_refs, const GStrv runtime_refs)
{
  RefsData *refs_data = g_new0 (RefsData, 1);

  refs_data->dir = g_object_ref (dir);
  refs_data->app_refs = g_strdupv ((char **) app_refs);
  refs_data->runtime_refs = g_strdupv ((char **) runtime_refs);
  return refs_data;
}

static void
refs_data_free (RefsData *refs_data)
{
  g_object_unref (refs_data->dir);
  g_strfreev (refs_data->app_refs);
  g_strfreev (refs_data->runtime_refs);
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
find_refs_for_dir (FlatpakDir *dir, GStrv *apps, GStrv *runtimes, GCancellable *cancellable, GError **error)
{
  if (flatpak_dir_ensure_repo (dir, cancellable, NULL))
    {
      if (apps != NULL && !flatpak_dir_list_refs (dir, "app", apps, cancellable, error))
        return FALSE;
      if (runtimes != NULL && !flatpak_dir_list_refs (dir, "runtime", runtimes, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
print_table_for_refs (gboolean print_apps, GPtrArray * refs_array, const char *arch, GCancellable *cancellable, GError **error)
{
  FlatpakTablePrinter *printer = flatpak_table_printer_new ();
  int i;

  i = 0;
  flatpak_table_printer_set_column_title (printer, i++, _("Ref"));
  if (opt_show_details)
    {
      flatpak_table_printer_set_column_title (printer, i++, _("Origin"));
      flatpak_table_printer_set_column_title (printer, i++, _("Active commit"));
      flatpak_table_printer_set_column_title (printer, i++, _("Latest commit"));
      flatpak_table_printer_set_column_title (printer, i++, _("Installed size"));
    }
  flatpak_table_printer_set_column_title (printer, i++, _("Options"));

  for (i = 0; i < refs_array->len; i++)
    {
      RefsData *refs_data = NULL;
      FlatpakDir *dir = NULL;
      g_auto(GStrv) dir_refs = NULL;
      g_autoptr(GHashTable) pref_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      int j;

      refs_data = (RefsData *) g_ptr_array_index (refs_array, i);
      dir = refs_data->dir;
      dir_refs = join_strv (refs_data->app_refs, refs_data->runtime_refs);

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
          g_autoptr(GVariant) deploy_data = NULL;
          const char *active;
          const char *alt_id;
          const char *eol;
          const char *eol_rebase;
          g_autofree char *latest = NULL;
          g_autofree char *size_s = NULL;
          guint64 size = 0;
          g_autofree const char **subpaths = NULL;

          ref = dir_refs[j];

          parts = g_strsplit (ref, "/", -1);
          partial_ref = strchr (ref, '/') + 1;

          if (arch != NULL && strcmp (arch, parts[1]) != 0)
            continue;

          deploy_data = flatpak_dir_get_deploy_data (dir, ref, cancellable, NULL);
          if (deploy_data == NULL)
            continue;

          if (!opt_all && strcmp (parts[0], "runtime") == 0 &&
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

          active = flatpak_deploy_data_get_commit (deploy_data);
          alt_id = flatpak_deploy_data_get_alt_id (deploy_data);
          eol = flatpak_deploy_data_get_eol (deploy_data);
          eol_rebase = flatpak_deploy_data_get_eol_rebase (deploy_data);

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

          flatpak_table_printer_add_column (printer, partial_ref);

          if (opt_show_details)
            {
              flatpak_table_printer_add_column (printer, repo);

              flatpak_table_printer_add_column_len (printer, active, 12);
              flatpak_table_printer_add_column_len (printer, latest, 12);

              size = flatpak_deploy_data_get_installed_size (deploy_data);
              size_s = g_format_size (size);
              flatpak_table_printer_add_decimal_column (printer, size_s);
            }

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
              g_autofree char *current = NULL;

              current = flatpak_dir_current_ref (dir, parts[1], cancellable);
              if (current && strcmp (ref, current) == 0)
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
              flatpak_table_printer_append_with_comma (printer, "partial");
            }

          if (eol)
            flatpak_table_printer_append_with_comma_printf (printer, "eol=%s", eol);
          if (eol_rebase)
            flatpak_table_printer_append_with_comma_printf (printer, "eol-rebase=%s", eol_rebase);

          flatpak_table_printer_finish_row (printer);
        }
    }

  flatpak_table_printer_print (printer);
  flatpak_table_printer_free (printer);

  return TRUE;
}

static gboolean
print_installed_refs (gboolean app, gboolean runtime, GPtrArray *dirs, const char *arch, GCancellable *cancellable, GError **error)
{
  g_autoptr(GPtrArray) refs_array = NULL;
  int i;

  refs_array = g_ptr_array_new_with_free_func ((GDestroyNotify) refs_data_free);

  for (i = 0; i < dirs->len; i++)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, i);
      g_auto(GStrv) apps = NULL;
      g_auto(GStrv) runtimes = NULL;

      if (!find_refs_for_dir (dir, app ? &apps : NULL, runtime ? &runtimes : NULL, cancellable, error))
        return FALSE;
      g_ptr_array_add (refs_array, refs_data_new (dir, apps, runtimes));
    }

  if (!print_table_for_refs (app, refs_array, arch, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_builtin_list (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;

  context = g_option_context_new (_(" - List installed apps and/or runtimes"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS | FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, cancellable, error))
    return FALSE;

  if (argc > 1)
    return usage_error (context, _("Too many arguments"), error);

  if (!opt_app && !opt_runtime)
    {
      opt_app = TRUE;
      opt_runtime = TRUE;
    }

  if (!print_installed_refs (opt_app, opt_runtime,
                             dirs,
                             opt_arch,
                             cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
flatpak_complete_list (FlatpakCompletion *completion)
{
  flatpak_complete_options (completion, global_entries);
  flatpak_complete_options (completion, options);
  flatpak_complete_options (completion, user_entries);
  return TRUE;
}
