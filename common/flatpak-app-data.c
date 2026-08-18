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

#include "flatpak-app-data.h"
#include "flatpak-permission-utils-private.h"
#include "flatpak-ref-utils-private.h"
#include "flatpak-utils-private.h"

/**
 * flatpak_app_data_delete:
 * @app_id: the application ID
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Deletes the data directory for @app_id in
 * <filename>~/.var/app</filename> and resets its permissions in the
 * permission store. The application does not need to be installed.
 *
 * If resetting permissions fails after the data directory has been removed,
 * the function returns %FALSE. It can be called again to retry the permission
 * reset.
 *
 * Returns: %TRUE on success, %FALSE on failure
 *
 * Since: 1.19
 */
gboolean
flatpak_app_data_delete (const char   *app_id,
                         GCancellable *cancellable,
                         GError      **error)
{
  g_autofree char *path = NULL;
  g_autoptr(GFile) file = NULL;

  g_return_val_if_fail (app_id != NULL, FALSE);

  if (!flatpak_is_valid_name (app_id, -1, error))
    return FALSE;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  /* The validated app ID cannot introduce path separators or parent traversal. */
  path = g_build_filename (g_get_home_dir (), ".var", "app", app_id, NULL);
  file = g_file_new_for_path (path);

  if (!flatpak_rm_rf (file, cancellable, error))
    return FALSE;

  return flatpak_reset_permissions_for_app (app_id, cancellable, error);
}
