/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014,2015 Colin Walters <walters@verbum.org>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include <glnx-dirfd.h>

G_BEGIN_DECLS

gboolean
glnx_shutil_rm_rf_at (int                   dfd,
                      const char           *path,
                      GCancellable         *cancellable,
                      GError              **error);

gboolean
glnx_shutil_mkdir_p_at (int                   dfd,
                        const char           *path,
                        int                   mode,
                        GCancellable         *cancellable,
                        GError              **error);

gboolean
glnx_shutil_mkdir_p_at_open (int            dfd,
                             const char    *path,
                             int            mode,
                             int           *out_dfd,
                             GCancellable  *cancellable,
                             GError       **error);

G_END_DECLS
