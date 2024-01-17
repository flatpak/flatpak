/*
 * Copyright © 2024 Red Hat, Inc
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
 *       Lukas Tyrychtr <ltyrycht@redhat.com>
 */

#pragma once

#include "libglnx.h"

#include "flatpak-bwrap-private.h"

G_BEGIN_DECLS

void flatpak_run_add_speech_dispatcher_args (FlatpakBwrap *bwrap);

G_END_DECLS