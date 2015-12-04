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
  g_autoptr(GPtrArray) names = NULL;
  int i;
  const char *repository;
  g_autofree char *url = NULL;

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

  names = g_ptr_array_new_with_free_func (g_free);

  g_hash_table_iter_init (&iter, refs);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *ref = key;
      const char *checksum = value;
      char *name = NULL;
      char *p;

      if (opt_only_updates)
        {
          g_autofree char *deployed = NULL;

          deployed = xdg_app_dir_read_active (dir, ref, cancellable);
          if (deployed == NULL)
            continue;

          if (g_strcmp0 (deployed, checksum) == 0)
            continue;
        }

      if (g_str_has_prefix (ref, "runtime/") && !opt_only_apps)
        {
          if (!opt_show_details)
            {
              name = g_strdup (ref + strlen ("runtime/"));
              p = strchr (name, '/');
              if (p)
                *p = 0;
            }
          else
            {
              name = g_strdup (ref);
            }
        }

      if (g_str_has_prefix (ref, "app/") && !opt_only_runtimes)
        {
          if (!opt_show_details)
            {
              name = g_strdup (ref + strlen ("app/"));
              p = strchr (name, '/');
              if (p)
                *p = 0;
            }
          else
            {
              name = g_strdup (ref);
            }
        }

      if (name)
        {
          gboolean found = FALSE;

          for (i = 0; i < names->len; i++)
            {
              if (strcmp (name, g_ptr_array_index (names, i)) == 0)
                found = TRUE;
              break;
            }

          if (!found)
            g_ptr_array_add (names, name);
          else
            g_free (name);
        }
    }

  g_ptr_array_sort (names, (GCompareFunc)strcmp);

  for (i = 0; i < names->len; i++)
    g_print ("%s\n", (char *)g_ptr_array_index (names, i));

  return TRUE;
}
