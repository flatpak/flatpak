/*
 * Copyright © 2016 Red Hat, Inc
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

#ifndef __FLATPAK_TRANSACTION_H__
#define __FLATPAK_TRANSACTION_H__

#include <glib.h>
#include "libglnx/libglnx.h"

#define FLATPAK_TYPE_TRANSACTION flatpak_transaction_get_type ()

G_DECLARE_FINAL_TYPE (FlatpakTransaction, flatpak_transaction, FLATPAK, TRANSACTION, GObject);

void                flatpak_transaction_set_disable_interaction   (FlatpakTransaction  *self,
                                                                   gboolean             no_interaction);
void                flatpak_transaction_set_no_pull               (FlatpakTransaction  *self,
                                                                   gboolean             no_pull);
void                flatpak_transaction_set_no_deploy             (FlatpakTransaction  *self,
                                                                   gboolean             no_deploy);
void                flatpak_transaction_set_disable_static_deltas (FlatpakTransaction  *self,
                                                                   gboolean             disable_static_deltas);
void                flatpak_transaction_set_disable_dependencies  (FlatpakTransaction  *self,
                                                                   gboolean             disable_dependencies);
void                flatpak_transaction_set_disable_related       (FlatpakTransaction  *self,
                                                                   gboolean             disable_related);
void                flatpak_transaction_set_reinstall             (FlatpakTransaction   *self,
                                                                   gboolean             reinstall);
gboolean            flatpak_transaction_update_metadata           (FlatpakTransaction  *self,
                                                                   gboolean             all_remotes,
                                                                   GCancellable        *cancellable,
                                                                   GError             **error);
gboolean            flatpak_transaction_run                       (FlatpakTransaction  *self,
                                                                   gboolean             stop_on_first_errror,
                                                                   GCancellable        *cancellable,
                                                                   GError             **error);
gboolean            flatpak_transaction_add_install               (FlatpakTransaction  *self,
                                                                   const char          *remote,
                                                                   const char          *ref,
                                                                   const char         **subpaths,
                                                                   GError             **error);
gboolean            flatpak_transaction_add_install_bundle        (FlatpakTransaction  *self,
                                                                   GFile               *file,
                                                                   GBytes              *gpg_data,
                                                                   GError             **error);
gboolean            flatpak_transaction_add_update                (FlatpakTransaction  *self,
                                                                   const char          *ref,
                                                                   const char         **subpaths,
                                                                   const char          *commit,
                                                                   GError             **error);
gboolean            flatpak_transaction_is_empty                  (FlatpakTransaction  *self);



#endif /* __FLATPAK_TRANSACTION_H__ */
