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

#include <glnx-backport-autocleanups.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/xattr.h>

G_BEGIN_DECLS

gboolean
glnx_dfd_name_get_all_xattrs (int                    dfd,
                              const char            *name,
                              GVariant             **out_xattrs,
                              GCancellable          *cancellable,
                              GError               **error);

gboolean
glnx_fd_get_all_xattrs (int                    fd,
                        GVariant             **out_xattrs,
                        GCancellable          *cancellable,
                        GError               **error);

gboolean
glnx_dfd_name_set_all_xattrs (int            dfd,
                              const char    *name,
                              GVariant      *xattrs,
                              GCancellable  *cancellable,
                              GError       **error);

gboolean
glnx_fd_set_all_xattrs (int            fd,
                        GVariant      *xattrs,
                        GCancellable  *cancellable,
                        GError       **error);

GBytes *
glnx_lgetxattrat (int            dfd,
                  const char    *subpath,
                  const char    *attribute,
                  GError       **error);

GBytes *
glnx_fgetxattr_bytes (int            fd,
                      const char    *attribute,
                      GError       **error);

gboolean
glnx_lsetxattrat (int            dfd,
                  const char    *subpath,
                  const char    *attribute,
                  const guint8  *value,
                  gsize          len,
                  int            flags,
                  GError       **error);

G_END_DECLS
