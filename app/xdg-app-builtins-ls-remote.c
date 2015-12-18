/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-builtins.h"
#include "xdg-app-utils.h"

static gboolean opt_show_details;
static gboolean opt_only_runtimes;
static gboolean opt_only_apps;
static gboolean opt_only_updates;

static GOptionEntry options[] = {
  { "show-details", 'd', 0, G_OPTION_ARG_NONE, &opt_show_details, "Show arches and branches", NULL },
  { "runtimes", 0, 0, G_OPTION_ARG_NONE, &opt_only_runtimes, "Show only runtimes", NULL },
  { "apps", 0, 0, G_OPTION_ARG_NONE, &opt_only_apps, "Show only apps", NULL },
  { "updates", 0, 0, G_OPTION_ARG_NONE, &opt_only_updates, "Show only those where updates are available", NULL },
  { NULL }
};

gboolean
xdg_app_builtin_ls_remote (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(XdgAppDir) dir = NULL;
  g_autoptr(GHashTable) refs = NULL;
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  g_autoptr(GHashTable) names = NULL;
  guint n_keys;
  g_autofree const char **keys = NULL;
  int i;
  const char *repository;
  const char *arch;

  context = g_option_context_new (" REMOTE - Show available runtimes and applications");

  if (!xdg_app_option_context_parse (context, options, &argc, &argv, 0, &dir, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, "REMOTE must be specified", error);

  repository = argv[1];


  if (!xdg_app_dir_list_remote_refs (dir,
                                     repository,
                                     &refs,
                                     cancellable, error))
    return FALSE;

  names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  arch = xdg_app_get_arch ();

  g_hash_table_iter_init (&iter, refs);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *ref = key;
      const char *checksum = value;
      const char *name = NULL;
      g_auto(GStrv) parts = NULL;

      parts = xdg_app_decompose_ref (ref, NULL);
      if (parts == NULL)
        {
          g_debug ("Invalid remote ref %s\n", ref);
          continue;
        }

      if (opt_only_updates)
        {
          g_autofree char *deployed = NULL;

          deployed = xdg_app_dir_read_active (dir, ref, cancellable);
          if (deployed == NULL)
            continue;

          if (g_strcmp0 (deployed, checksum) == 0)
            continue;
        }

      if (!opt_show_details)
        {
          if (strcmp (arch, parts[2]) != 0)
            continue;
        }

      if (strcmp (parts[0], "runtime") == 0 && opt_only_apps)
        continue;

      if (strcmp (parts[0], "app") == 0 && opt_only_runtimes)
        continue;

      if (!opt_show_details)
        name = parts[1];
      else
        name = ref;

      if (g_hash_table_lookup (names, name) == NULL)
        g_hash_table_insert (names, g_strdup (name), g_strdup (checksum));
    }

  keys = (const char **)g_hash_table_get_keys_as_array (names, &n_keys);
  g_qsort_with_data (keys, n_keys, sizeof(char *), (GCompareDataFunc)xdg_app_strcmp0_ptr, NULL);

  for (i = 0; i < n_keys; i++)
    {
      if (opt_show_details)
        g_print ("%s %.12s\n", keys[i], (char *)g_hash_table_lookup (names, keys[i]));
      else
        g_print ("%s\n", keys[i]);
    }

  return TRUE;
}
