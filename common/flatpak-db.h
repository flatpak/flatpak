/* flatpak-db.h
 *
 * Copyright © 2015 Red Hat, Inc
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#ifndef FLATPAK_DB_H
#define FLATPAK_DB_H

#include <string.h>

#include "libglnx/libglnx.h"
#include <glib-object.h>

G_BEGIN_DECLS

typedef struct FlatpakDb       FlatpakDb;
typedef struct _FlatpakDbEntry FlatpakDbEntry;

#define FLATPAK_TYPE_DB (flatpak_db_get_type ())
#define FLATPAK_DB(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_DB, FlatpakDb))
#define FLATPAK_IS_DB(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_DB))

GType flatpak_db_get_type (void);

FlatpakDb *     flatpak_db_new (const char *path,
                                gboolean    fail_if_not_found,
                                GError    **error);
char **        flatpak_db_list_ids (FlatpakDb *self);
char **        flatpak_db_list_apps (FlatpakDb *self);
char **        flatpak_db_list_ids_by_app (FlatpakDb  *self,
                                           const char *app);
char **        flatpak_db_list_ids_by_value (FlatpakDb *self,
                                             GVariant  *data);
FlatpakDbEntry *flatpak_db_lookup (FlatpakDb  *self,
                                   const char *id);
GString *      flatpak_db_print_string (FlatpakDb *self,
                                        GString   *string);
char *         flatpak_db_print (FlatpakDb *self);

gboolean       flatpak_db_is_dirty (FlatpakDb *self);
void           flatpak_db_set_entry (FlatpakDb      *self,
                                     const char     *id,
                                     FlatpakDbEntry *entry);
void           flatpak_db_update (FlatpakDb *self);
GBytes *       flatpak_db_get_content (FlatpakDb *self);
const char *   flatpak_db_get_path (FlatpakDb *self);
gboolean       flatpak_db_save_content (FlatpakDb *self,
                                        GError   **error);
void           flatpak_db_save_content_async (FlatpakDb          *self,
                                              GCancellable       *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer            user_data);
gboolean       flatpak_db_save_content_finish (FlatpakDb    *self,
                                               GAsyncResult *res,
                                               GError      **error);
void           flatpak_db_set_path (FlatpakDb  *self,
                                    const char *path);


FlatpakDbEntry  *flatpak_db_entry_ref (FlatpakDbEntry *entry);
void            flatpak_db_entry_unref (FlatpakDbEntry *entry);
GVariant *      flatpak_db_entry_get_data (FlatpakDbEntry *entry);
const char **   flatpak_db_entry_list_apps (FlatpakDbEntry *entry);
const char **   flatpak_db_entry_list_permissions (FlatpakDbEntry *entry,
                                                   const char     *app);
gboolean        flatpak_db_entry_has_permission (FlatpakDbEntry *entry,
                                                 const char     *app,
                                                 const char     *permission);
gboolean        flatpak_db_entry_has_permissions (FlatpakDbEntry *entry,
                                                  const char     *app,
                                                  const char    **permissions);
GString *       flatpak_db_entry_print_string (FlatpakDbEntry *entry,
                                               GString        *string);

FlatpakDbEntry  *flatpak_db_entry_new (GVariant *data);
FlatpakDbEntry  *flatpak_db_entry_modify_data (FlatpakDbEntry *entry,
                                               GVariant       *data);
FlatpakDbEntry  *flatpak_db_entry_set_app_permissions (FlatpakDbEntry *entry,
                                                       const char     *app,
                                                       const char    **permissions);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakDb, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakDbEntry, flatpak_db_entry_unref)

G_END_DECLS

#endif /* FLATPAK_DB_H */
