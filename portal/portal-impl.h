/*
 * Copyright © 2016 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
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
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#ifndef __PORTAL_IMPL_H__
#define __PORTAL_IMPL_H__

#include <glib.h>

typedef struct {
  char *source;
  char *dbus_name;
  char **interfaces;
  char **use_in;
  int priority;
} PortalImplementation;

void                  load_installed_portals     (gboolean opt_verbose);
PortalImplementation *find_portal_implementation (const char *interface);

#endif  /* __PORTAL_IMPL_H__ */
