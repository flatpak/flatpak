/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014,2015 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include <gio/gio.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/fsuid.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/capability.h>
#include <sched.h>

void glnx_libcontainer_set_not_available (void);
gboolean glnx_libcontainer_get_available (void);

gboolean glnx_libcontainer_bind_mount_readonly (const char *path, GError **error);

int glnx_libcontainer_make_api_mounts (const char *dest);
int glnx_libcontainer_prep_dev (const char  *dest);

pid_t glnx_libcontainer_run_in_root (const char  *dest,
                                     const char  *binary,
                                     char **argv);
