/*
 * Copyright © 2015 Red Hat, Inc
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

#ifndef __FLATPAK_PROXY_H__
#define __FLATPAK_PROXY_H__

#include <gio/gio.h>
#include "libglnx/libglnx.h"

typedef enum {
  FLATPAK_POLICY_NONE,
  FLATPAK_POLICY_SEE,
  FLATPAK_POLICY_TALK,
  FLATPAK_POLICY_OWN
} FlatpakPolicy;

typedef struct FlatpakProxy FlatpakProxy;

#define FLATPAK_TYPE_PROXY flatpak_proxy_get_type ()
#define FLATPAK_PROXY(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_PROXY, FlatpakProxy))
#define FLATPAK_IS_PROXY(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_PROXY))

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakProxy, g_object_unref)

GType flatpak_proxy_get_type (void);

FlatpakProxy *flatpak_proxy_new (const char *dbus_address,
                                 const char *socket_path);
void         flatpak_proxy_set_log_messages (FlatpakProxy *proxy,
                                             gboolean      log);
void         flatpak_proxy_set_filter (FlatpakProxy *proxy,
                                       gboolean      filter);
void         flatpak_proxy_set_sloppy_names (FlatpakProxy *proxy,
                                             gboolean      sloppy_names);
void         flatpak_proxy_add_policy (FlatpakProxy *proxy,
                                       const char   *name,
                                       gboolean      name_is_subtree,
                                       FlatpakPolicy policy);
void         flatpak_proxy_add_call_rule (FlatpakProxy *proxy,
                                          const char   *name,
                                          gboolean      name_is_subtree,
                                          const char   *rule);
void         flatpak_proxy_add_broadcast_rule (FlatpakProxy *proxy,
                                               const char   *name,
                                               gboolean      name_is_subtree,
                                               const char   *rule);
gboolean     flatpak_proxy_start (FlatpakProxy *proxy,
                                  GError      **error);
void         flatpak_proxy_stop (FlatpakProxy *proxy);

#endif /* __FLATPAK_PROXY_H__ */
