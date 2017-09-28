/* flatpak-transaction-log.h
 *
 * Copyright © 2017 Endless Mobile, Inc
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
 *       Sam Spilsbury <sam@endlessm.com>
 */

#ifndef FLATPAK_TRANSACTION_LOG_H
#define FLATPAK_TRANSACTION_LOG_H

#include <string.h>

#include <gio/gio.h>
#include <glib-object.h>

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE (FlatpakTransactionLog, flatpak_transaction_log, FLATPAK, TRANSACTION_LOG, GObject)

#define FLATPAK_TYPE_TRANSACTION_LOG flatpak_transaction_log_get_type ()

FlatpakTransactionLog *     flatpak_transaction_log_new (GFile *path);

gboolean flatpak_transaction_log_commit_deploy_event (FlatpakTransactionLog *log,
                                                      const gchar           *ref,
                                                      const gchar           *remote,
                                                      const gchar           *commit,
                                                      GCancellable          *cancellable,
                                                      GError                **error);

gboolean flatpak_transaction_log_commit_uninstall_event (FlatpakTransactionLog *log,
                                                         const gchar           *ref,
                                                         GCancellable          *cancellable,
                                                         GError                **error);

G_END_DECLS

#endif /* FLATPAK_TRANSACTION_LOG_H */
