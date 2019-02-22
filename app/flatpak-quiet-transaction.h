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

#ifndef __FLATPAK_QUIET_TRANSACTION_H__
#define __FLATPAK_QUIET_TRANSACTION_H__

#include "flatpak-transaction.h"
#include "flatpak-dir-private.h"

#define FLATPAK_TYPE_QUIET_TRANSACTION flatpak_quiet_transaction_get_type ()
G_DECLARE_FINAL_TYPE (FlatpakQuietTransaction, flatpak_quiet_transaction, FLATPAK, QUIET_TRANSACTION, FlatpakTransaction)

FlatpakTransaction * flatpak_quiet_transaction_new (FlatpakDir  * dir,
                                                    GError     **error);

#endif /* __FLATPAK_QUIET_TRANSACTION_H__ */
