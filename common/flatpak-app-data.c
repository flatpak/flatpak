/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
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

#include "flatpak-app-data-private.h"
#include "flatpak-permission-utils-private.h"
#include "flatpak-utils-private.h"

gboolean
flatpak_delete_app_data (const char *app_id,
                         GError    **error)
{
  g_autofree char *path = g_build_filename (g_get_home_dir (), ".var", "app", app_id, NULL);
  g_autoptr(GFile) file = g_file_new_for_path (path);

  if (g_file_query_exists (file, NULL))
    {
      if (!flatpak_rm_rf (file, NULL, error))
        return FALSE;
    }

  if (!flatpak_reset_permissions_for_app (app_id, error))
    return FALSE;

  return TRUE;
}
