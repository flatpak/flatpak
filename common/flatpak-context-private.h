/*
 * Copyright © 2014-2018 Red Hat, Inc
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

#ifndef __FLATPAK_CONTEXT_H__
#define __FLATPAK_CONTEXT_H__

#include "libglnx/libglnx.h"
#include "dbus-proxy/flatpak-proxy.h"
#include <flatpak-common-types-private.h>
#include "flatpak-exports-private.h"

typedef struct FlatpakContext FlatpakContext;

typedef enum {
  FLATPAK_CONTEXT_SHARED_NETWORK   = 1 << 0,
  FLATPAK_CONTEXT_SHARED_IPC       = 1 << 1,
} FlatpakContextShares;

typedef enum {
  FLATPAK_CONTEXT_SOCKET_X11         = 1 << 0,
  FLATPAK_CONTEXT_SOCKET_WAYLAND     = 1 << 1,
  FLATPAK_CONTEXT_SOCKET_PULSEAUDIO  = 1 << 2,
  FLATPAK_CONTEXT_SOCKET_SESSION_BUS = 1 << 3,
  FLATPAK_CONTEXT_SOCKET_SYSTEM_BUS  = 1 << 4,
  FLATPAK_CONTEXT_SOCKET_FALLBACK_X11 = 1 << 5, /* For backwards compat, also set SOCKET_X11 */
  FLATPAK_CONTEXT_SOCKET_SSH_AUTH    = 1 << 6,
} FlatpakContextSockets;

typedef enum {
  FLATPAK_CONTEXT_DEVICE_DRI         = 1 << 0,
  FLATPAK_CONTEXT_DEVICE_ALL         = 1 << 1,
  FLATPAK_CONTEXT_DEVICE_KVM         = 1 << 2,
} FlatpakContextDevices;

typedef enum {
  FLATPAK_CONTEXT_FEATURE_DEVEL        = 1 << 0,
  FLATPAK_CONTEXT_FEATURE_MULTIARCH    = 1 << 1,
  FLATPAK_CONTEXT_FEATURE_BLUETOOTH    = 1 << 2,
} FlatpakContextFeatures;

struct FlatpakContext
{
  FlatpakContextShares   shares;
  FlatpakContextShares   shares_valid;
  FlatpakContextSockets  sockets;
  FlatpakContextSockets  sockets_valid;
  FlatpakContextDevices  devices;
  FlatpakContextDevices  devices_valid;
  FlatpakContextFeatures features;
  FlatpakContextFeatures features_valid;
  GHashTable            *env_vars;
  GHashTable            *persistent;
  GHashTable            *filesystems;
  GHashTable            *session_bus_policy;
  GHashTable            *system_bus_policy;
  GHashTable            *generic_policy;
};

extern const char *flatpak_context_sockets[];
extern const char *flatpak_context_devices[];
extern const char *flatpak_context_features[];
extern const char *flatpak_context_shares[];

FlatpakContext *flatpak_context_new (void);
void           flatpak_context_free (FlatpakContext *context);
void           flatpak_context_merge (FlatpakContext *context,
                                      FlatpakContext *other);
GOptionEntry  *flatpak_context_get_option_entries (void);
GOptionGroup  *flatpak_context_get_options (FlatpakContext *context);
gboolean       flatpak_context_load_metadata (FlatpakContext *context,
                                              GKeyFile       *metakey,
                                              GError        **error);
void           flatpak_context_save_metadata (FlatpakContext *context,
                                              gboolean        flatten,
                                              GKeyFile       *metakey);
void           flatpak_context_allow_host_fs (FlatpakContext *context);
void           flatpak_context_set_session_bus_policy (FlatpakContext *context,
                                                       const char     *name,
                                                       FlatpakPolicy   policy);
GStrv          flatpak_context_get_session_bus_policy_allowed_own_names (FlatpakContext *context);
void           flatpak_context_set_system_bus_policy (FlatpakContext *context,
                                                      const char     *name,
                                                      FlatpakPolicy   policy);
void           flatpak_context_to_args (FlatpakContext *context,
                                        GPtrArray      *args);
FlatpakRunFlags flatpak_context_get_run_flags (FlatpakContext *context);
void           flatpak_context_add_bus_filters (FlatpakContext *context,
                                                const char     *app_id,
                                                gboolean        session_bus,
                                                FlatpakBwrap   *bwrap);

gboolean       flatpak_context_get_needs_session_bus_proxy (FlatpakContext *context);
gboolean       flatpak_context_get_needs_system_bus_proxy (FlatpakContext *context);

void           flatpak_context_reset_permissions (FlatpakContext *context);
void           flatpak_context_make_sandboxed (FlatpakContext *context);

gboolean       flatpak_context_allows_features (FlatpakContext        *context,
                                                FlatpakContextFeatures features);

FlatpakContext *flatpak_context_load_for_deploy (FlatpakDeploy *deploy,
                                                 GError       **error);
FlatpakContext *flatpak_context_load_for_app (const char *app_id,
                                              GError    **error);

FlatpakExports *flatpak_context_get_exports (FlatpakContext *context,
                                             const char     *app_id);

void flatpak_context_append_bwrap_filesystem (FlatpakContext  *context,
                                              FlatpakBwrap    *bwrap,
                                              const char      *app_id,
                                              GFile           *app_id_dir,
                                              FlatpakExports **exports_out);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakContext, flatpak_context_free)

#endif /* __FLATPAK_CONTEXT_H__ */
