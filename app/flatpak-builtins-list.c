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
#include "flatpak-utils.h"

static gboolean opt_show_details;
static gboolean opt_user;
static gboolean opt_system;
static gboolean opt_runtime;
static gboolean opt_app;
static char *opt_arch;

static GOptionEntry options[] = {
  { "user", 0, 0, G_OPTION_ARG_NONE, &opt_user, N_("Show user installations"), NULL },
  { "system", 0, 0, G_OPTION_ARG_NONE, &opt_system, N_("Show system-wide installations"), NULL },
  { "show-details", 'd', 0, G_OPTION_ARG_NONE, &opt_show_details, N_("Show arches and branches"), NULL },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("List installed runtimes"), NULL },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, N_("List installed applications"), NULL },
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to show"), N_("ARCH") },
  { NULL }
};

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
print_installed_refs (gboolean app, gboolean runtime, gboolean print_system, gboolean print_user, const char *arch, GCancellable *cancellable, GError **error)
{
  g_autofree char *last = NULL;

  g_auto(GStrv) system = NULL;
  g_auto(GStrv) system_app = NULL;
  g_auto(GStrv) system_runtime = NULL;
  g_auto(GStrv) user = NULL;
  g_auto(GStrv) user_app = NULL;
  g_auto(GStrv) user_runtime = NULL;
  g_autoptr(FlatpakDir) user_dir = NULL;
  g_autoptr(FlatpakDir) system_dir = NULL;
  int s, u;

  if (print_user)
    {
      user_dir = flatpak_dir_get (TRUE);

      if (flatpak_dir_ensure_repo (user_dir, cancellable, NULL))
        {
          if (app && !flatpak_dir_list_refs (user_dir, "app", &user_app, cancellable, error))
            return FALSE;
          if (runtime && !flatpak_dir_list_refs (user_dir, "runtime", &user_runtime, cancellable, error))
            return FALSE;
        }
    }

  if (print_system)
    {
      system_dir = flatpak_dir_get (FALSE);
      if (flatpak_dir_ensure_repo (system_dir, cancellable, NULL))
        {
          if (app && !flatpak_dir_list_refs (system_dir, "app", &system_app, cancellable, error))
            return FALSE;
          if (runtime && !flatpak_dir_list_refs (system_dir, "runtime", &system_runtime, cancellable, error))
            return FALSE;
        }
    }

  FlatpakTablePrinter *printer = flatpak_table_printer_new ();

  user = join_strv (user_app, user_runtime);
  system = join_strv (system_app, system_runtime);

  for (s = 0, u = 0; system[s] != NULL || user[u] != NULL; )
    {
      char *ref, *partial_ref;
      g_auto(GStrv) parts = NULL;
      const char *repo = NULL;
      gboolean is_user;
      FlatpakDir *dir = NULL;
      g_autoptr(GVariant) deploy_data = NULL;

      if (system[s] == NULL)
        is_user = TRUE;
      else if (user[u] == NULL)
        is_user = FALSE;
      else if (strcmp (system[s], user[u]) <= 0)
        is_user = FALSE;
      else
        is_user = TRUE;

      if (is_user)
        {
          ref = user[u++];
          dir = user_dir;
        }
      else
        {
          ref = system[s++];
          dir = system_dir;
        }

      parts = g_strsplit (ref, "/", -1);
      partial_ref = strchr (ref, '/') + 1;

      if (arch != NULL && strcmp (arch, parts[1]) != 0)
        continue;

      deploy_data = flatpak_dir_get_deploy_data (dir, ref, cancellable, error);
      if (deploy_data == NULL)
        return FALSE;

      repo = flatpak_deploy_data_get_origin (deploy_data);

      if (opt_show_details)
        {
          g_autofree char *active = flatpak_dir_read_active (dir, ref, NULL);
          g_autofree char *latest = NULL;
          g_autofree char *size_s = NULL;
          guint64 size = 0;
          g_autofree const char **subpaths = NULL;

          latest = flatpak_dir_read_latest (dir, repo, ref, NULL, NULL);
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
          flatpak_table_printer_add_column (printer, repo);

          active[MIN (strlen (active), 12)] = 0;
          flatpak_table_printer_add_column (printer, active);
          flatpak_table_printer_add_column (printer, latest);

          size = flatpak_deploy_data_get_installed_size (deploy_data);
          size_s = g_format_size (size);
          flatpak_table_printer_add_column (printer, size_s);

          flatpak_table_printer_add_column (printer, ""); /* Options */

          if (print_user && print_system)
            flatpak_table_printer_append_with_comma (printer, is_user ? "user" : "system");

          if (strcmp (parts[0], "app") == 0)
            {
              g_autofree char *current;

              current = flatpak_dir_current_ref (dir, parts[1], cancellable);
              if (current && strcmp (ref, current) == 0)
                flatpak_table_printer_append_with_comma (printer, "current");
            }
          else
            {
              if (app)
                flatpak_table_printer_append_with_comma (printer, "runtime");
            }

          subpaths = flatpak_deploy_data_get_subpaths (deploy_data);
          if (subpaths[0] == NULL)
            {
              flatpak_table_printer_add_column (printer, "");
            }
          else
            {
              int i;
              flatpak_table_printer_add_column (printer, ""); /* subpaths */
              for (i = 0; subpaths[i] != NULL; i++)
                flatpak_table_printer_append_with_comma (printer, subpaths[i]);
            }
        }
      else
        {
          if (last == NULL || strcmp (last, parts[1]) != 0)
            {
              flatpak_table_printer_add_column (printer, parts[1]);
              g_clear_pointer (&last, g_free);
              last = g_strdup (parts[1]);
            }
        }
      flatpak_table_printer_finish_row (printer);
    }

  flatpak_table_printer_print (printer);
  flatpak_table_printer_free (printer);

  return TRUE;
}

gboolean
flatpak_builtin_list (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new (_(" - List installed apps and/or runtimes"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, FLATPAK_BUILTIN_FLAG_NO_DIR, NULL, cancellable, error))
    return FALSE;

  if (!opt_app && !opt_runtime)
    opt_app = TRUE;

  if (!print_installed_refs (opt_app, opt_runtime,
                             opt_system || (!opt_user && !opt_system),
                             opt_user || (!opt_user && !opt_system),
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
  return TRUE;
}
