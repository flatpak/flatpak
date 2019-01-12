/*
 * Copyright © 2019 Red Hat, Inc
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include "flatpak-appdata-private.h"
#include <appstream-glib.h>

gboolean
flatpak_parse_appdata (const char  *appdata_xml,
                       const char  *app_id,
                       GHashTable **names,
                       GHashTable **comments,
                       char       **version,
                       char       **license)
{
  g_autoptr(AsStore) store = as_store_new ();
  GPtrArray *apps;

  as_store_from_xml (store, appdata_xml, NULL, NULL);
  apps = as_store_get_apps (store);
  if (apps->len > 0)
    {
      AsApp *app = g_ptr_array_index (apps, 0);
      AsRelease *release = as_app_get_release_default (app);
      GHashTableIter iter;
      char *key, *val;

      *names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      g_hash_table_iter_init (&iter, as_app_get_names (app));
      while (g_hash_table_iter_next (&iter, (gpointer *)&key, (gpointer *)&val))
        g_hash_table_insert (*names, g_strdup (key), g_strdup (val));

      *comments = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      g_hash_table_iter_init (&iter, as_app_get_comments (app));
      while (g_hash_table_iter_next (&iter, (gpointer *)&key, (gpointer *)&val))
        g_hash_table_insert (*comments, g_strdup (key), g_strdup (val));

      if (release)
        *version = g_strdup (as_release_get_version (release));

      *license = g_strdup (as_app_get_project_license (app));

      return TRUE;
    }

  return FALSE;
}
