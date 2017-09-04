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

gboolean
flatpak_builtin_ls_remote (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(FlatpakDir) dir = NULL;
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
  g_auto(GStrv) remotes = NULL;
  gboolean has_remote;
  g_autoptr(GHashTable) pref_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GHashTable) refs_hash = g_hash_table_new_full(g_direct_hash, g_direct_equal, (GDestroyNotify)g_hash_table_unref, g_free);

  context = g_option_context_new (_(" REMOTE - Show available runtimes and applications"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (!opt_app && !opt_runtime)
    opt_app = opt_runtime = TRUE;

  if (argc > 2)
    return usage_error (context, _("Too many arguments"), error);

  if (argc < 2)
    {
      has_remote = FALSE;
      remotes = flatpak_dir_list_remotes (dir, cancellable, error);
      if (remotes == NULL)
        return FALSE;
    }
  else
    {
      has_remote = TRUE;
      remotes = g_new (char *, 2);
      remotes[0] = g_strdup(argv[1]);
      remotes[1] = NULL;
    }

  for (i = 0; remotes[i] != NULL; i++)
    {
      g_autoptr(GHashTable) refs = NULL;
      const char *remote_name = remotes[i];

      if (!flatpak_dir_list_remote_refs (dir,
                                         remote_name,
                                         &refs,
                                         cancellable, error))
        return FALSE;

      g_hash_table_insert (refs_hash, g_steal_pointer (&refs), g_strdup (remote_name));
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

  g_hash_table_iter_init (&refs_iter, refs_hash);
  while (g_hash_table_iter_next (&refs_iter, &refs_key, &refs_value))
    {
      GHashTable *refs = refs_key;
      char *remote = refs_value;
      g_autoptr(GHashTable) names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

      g_hash_table_iter_init (&iter, refs);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          char *ref = key;
          char *partial_ref = flatpak_make_valid_id_prefix (strchr (ref, '/') + 1);
          g_hash_table_insert (pref_hash, partial_ref, ref);
        }

      g_hash_table_iter_init (&iter, refs);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          const char *ref = key;
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
              if (g_hash_table_lookup (refs, alt_arch_ref))
                continue;
            }

          if (g_hash_table_lookup (names, name) == NULL)
            g_hash_table_insert (names, g_strdup (name), g_strdup (checksum));
        }
      keys = (const char **) g_hash_table_get_keys_as_array (names, &n_keys);
      g_qsort_with_data (keys, n_keys, sizeof (char *), (GCompareDataFunc) flatpak_strcmp0_ptr, NULL);

      for (i = 0; i < n_keys; i++)
        {
          flatpak_table_printer_add_column (printer, keys[i]);

          if (!has_remote)
              flatpak_table_printer_add_column (printer, remote);

          if (opt_show_details)
            {
              g_autofree char *value = NULL;
              g_autoptr(GVariant) refdata = NULL;
              g_autoptr(GError) local_error = NULL;
              guint64 installed_size;
              guint64 download_size;
              const char *metadata;

              value = g_strdup ((char *) g_hash_table_lookup (names, keys[i]));
              value[MIN (strlen (value), 12)] = 0;
              flatpak_table_printer_add_column (printer, value);

              if (!flatpak_dir_lookup_repo_metadata (dir, remote, cancellable, &local_error,
                                                     "xa.cache", "v", &refdata))
                {
                  if (local_error == NULL)
                    flatpak_fail (&local_error, _("No ref information available in repository"));
                  g_propagate_error (error, g_steal_pointer (&local_error));
                  return FALSE;
                }

              if (g_variant_lookup (refdata, keys[i], "(tt&s)", &installed_size, &download_size, &metadata))
                {
                  g_autofree char *installed = g_format_size (GUINT64_FROM_BE (installed_size));
                  g_autofree char *download = g_format_size (GUINT64_FROM_BE (download_size));

                  flatpak_table_printer_add_decimal_column (printer, installed);
                  flatpak_table_printer_add_decimal_column (printer, download);
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
  g_autoptr(FlatpakDir) dir = NULL;
  int i;

  context = g_option_context_new ("");

  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv, 0, &dir, NULL, NULL))
    return FALSE;

  switch (completion->argc)
    {
    case 0:
    case 1: /* REMOTE */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      {
        g_auto(GStrv) remotes = flatpak_dir_list_remotes (dir, NULL, NULL);
        if (remotes == NULL)
          return FALSE;
        for (i = 0; remotes[i] != NULL; i++)
          flatpak_complete_word (completion, "%s ", remotes[i]);
      }

      break;
    }

  return TRUE;
}
