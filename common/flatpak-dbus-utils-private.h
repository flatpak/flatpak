/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 *
 * Utilities for D-Bus services
 *
 * Copyright 2026 Collabora Ltd.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "libglnx.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

typedef struct _FlatpakUniqueFDList FlatpakUniqueFDList;

void flatpak_unique_fd_list_free (FlatpakUniqueFDList *self);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakUniqueFDList, flatpak_unique_fd_list_free)

FlatpakUniqueFDList *flatpak_unique_fd_list_new (GUnixFDList *fd_list,
                                                 GError **error);

int flatpak_unique_fd_list_get (FlatpakUniqueFDList *self,
                                int handle,
                                GError **error);
