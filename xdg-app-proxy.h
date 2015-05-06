/*
 * Copyright © 2015 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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

#ifndef __XDG_APP_PROXY_H__
#define __XDG_APP_PROXY_H__

#include <gio/gio.h>

typedef enum {
  XDG_APP_POLICY_NONE,
  XDG_APP_POLICY_SEE,
  XDG_APP_POLICY_TALK,
  XDG_APP_POLICY_OWN
} XdgAppPolicy;

typedef struct XdgAppProxy XdgAppProxy;

G_DEFINE_AUTOPTR_CLEANUP_FUNC(XdgAppProxy, g_object_unref)

GType xdg_app_proxy_get_type (void);

XdgAppProxy *xdg_app_proxy_new                   (const char    *dbus_address,
                                                  const char    *socket_path);
void         xdg_app_proxy_set_log_messages      (XdgAppProxy   *proxy,
                                                  gboolean       log);
void         xdg_app_proxy_set_filter            (XdgAppProxy   *proxy,
                                                  gboolean       filter);
void         xdg_app_proxy_add_policy            (XdgAppProxy   *proxy,
                                                  const char    *name,
                                                  XdgAppPolicy   policy);
void         xdg_app_proxy_add_wildcarded_policy (XdgAppProxy   *proxy,
                                                  const char    *name,
                                                  XdgAppPolicy   policy);
gboolean     xdg_app_proxy_start                 (XdgAppProxy   *proxy,
                                                  GError       **error);

#endif /* __XDG_APP_PROXY_H__ */
