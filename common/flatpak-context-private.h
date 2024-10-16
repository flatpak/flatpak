/*
 * Copyright © 2014-2018 Red Hat, Inc
 * Copyright © 2024 GNOME Foundation, Inc.
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
 *       Hubert Figuière <hub@figuiere.net>
 */

#ifndef __FLATPAK_CONTEXT_H__
#define __FLATPAK_CONTEXT_H__

#include "libglnx.h"
#include <flatpak-common-types-private.h>
#include "flatpak-exports-private.h"

typedef enum {
  FLATPAK_SESSION_BUS,
  FLATPAK_SYSTEM_BUS,
  FLATPAK_A11Y_BUS,
} FlatpakBus;

typedef enum {
  FLATPAK_POLICY_NONE,
  FLATPAK_POLICY_SEE,
  FLATPAK_POLICY_TALK,
  FLATPAK_POLICY_OWN
} FlatpakPolicy;

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
  FLATPAK_CONTEXT_SOCKET_PCSC        = 1 << 7,
  FLATPAK_CONTEXT_SOCKET_CUPS        = 1 << 8,
  FLATPAK_CONTEXT_SOCKET_GPG_AGENT   = 1 << 9,
  FLATPAK_CONTEXT_SOCKET_INHERIT_WAYLAND_SOCKET = 1 << 10,
} FlatpakContextSockets;

typedef enum {
  FLATPAK_CONTEXT_DEVICE_DRI         = 1 << 0,
  FLATPAK_CONTEXT_DEVICE_ALL         = 1 << 1,
  FLATPAK_CONTEXT_DEVICE_KVM         = 1 << 2,
  FLATPAK_CONTEXT_DEVICE_SHM         = 1 << 3,
  FLATPAK_CONTEXT_DEVICE_INPUT       = 1 << 4,
  FLATPAK_CONTEXT_DEVICE_USB         = 1 << 5,
} FlatpakContextDevices;

typedef enum {
  FLATPAK_CONTEXT_FEATURE_DEVEL        = 1 << 0,
  FLATPAK_CONTEXT_FEATURE_MULTIARCH    = 1 << 1,
  FLATPAK_CONTEXT_FEATURE_BLUETOOTH    = 1 << 2,
  FLATPAK_CONTEXT_FEATURE_CANBUS       = 1 << 3,
  FLATPAK_CONTEXT_FEATURE_PER_APP_DEV_SHM = 1 << 4,
} FlatpakContextFeatures;

typedef enum {
  FLATPAK_CONTEXT_CONDITION_TRUE          = 1 << 0,
  FLATPAK_CONTEXT_CONDITION_FALSE         = 1 << 1,
  FLATPAK_CONTEXT_CONDITION_HAS_INPUT_DEV = 1 << 2,
  FLATPAK_CONTEXT_CONDITION_HAS_WAYLAND   = 1 << 3,
} FlatpakContextConditions;

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
  GHashTable            *a11y_bus_policy;
  GHashTable            *generic_policy;
  GHashTable            *conditional_sockets;
  GHashTable            *conditional_devices;
};

typedef gboolean (*FlatpakContextConditionEvaluator) (FlatpakContextConditions conditions);

extern const char *flatpak_context_sockets[];
extern const char *flatpak_context_devices[];
extern const char *flatpak_context_features[];
extern const char *flatpak_context_shares[];

gboolean       flatpak_context_parse_filesystem (const char             *filesystem_and_mode,
                                                 gboolean                negated,
                                                 char                  **filesystem_out,
                                                 FlatpakFilesystemMode  *mode_out,
                                                 GError                **error);

FlatpakContext *flatpak_context_new (void);
void           flatpak_context_free (FlatpakContext *context);
void           flatpak_context_dump (FlatpakContext *context,
                                     const char     *title);
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
void           flatpak_context_set_a11y_bus_policy (FlatpakContext *context,
                                                    const char     *name,
                                                    FlatpakPolicy   policy);
void           flatpak_context_to_args (FlatpakContext *context,
                                        GPtrArray      *args);
FlatpakRunFlags flatpak_context_get_run_flags (FlatpakContext *context);
void           flatpak_context_add_bus_filters (FlatpakContext *context,
                                                const char     *app_id,
                                                FlatpakBus      bus,
                                                gboolean        sandboxed,
                                                FlatpakBwrap   *bwrap);

gboolean       flatpak_context_get_needs_session_bus_proxy (FlatpakContext *context);
gboolean       flatpak_context_get_needs_system_bus_proxy (FlatpakContext *context);
gboolean       flatpak_context_adds_permissions (FlatpakContext *old_context,
                                                 FlatpakContext *new_context);

void           flatpak_context_reset_permissions (FlatpakContext *context);
void           flatpak_context_reset_non_permissions (FlatpakContext *context);
void           flatpak_context_make_sandboxed (FlatpakContext *context);

gboolean       flatpak_context_allows_features (FlatpakContext        *context,
                                                FlatpakContextFeatures features);

FlatpakContext *flatpak_context_load_for_deploy (FlatpakDeploy *deploy,
                                                 GError       **error);

FlatpakExports *flatpak_context_get_exports (FlatpakContext *context,
                                             const char     *app_id);
FlatpakExports *flatpak_context_get_exports_full (FlatpakContext *context,
                                                  GFile          *app_id_dir,
                                                  GPtrArray      *extra_app_id_dirs,
                                                  gboolean        do_create,
                                                  gboolean        include_default_dirs,
                                                  gchar         **xdg_dirs_conf,
                                                  gboolean       *home_access_out);

void flatpak_context_append_bwrap_filesystem (FlatpakContext  *context,
                                              FlatpakBwrap    *bwrap,
                                              const char      *app_id,
                                              GFile           *app_id_dir,
                                              FlatpakExports  *exports,
                                              const char      *xdg_dirs_conf,
                                              gboolean         home_access);

gboolean flatpak_context_parse_env_block (FlatpakContext *context,
                                          const char *data,
                                          gsize length,
                                          GError **error);
gboolean flatpak_context_parse_env_fd (FlatpakContext *context,
                                       int fd,
                                       GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakContext, flatpak_context_free)

GFile *flatpak_get_user_base_dir_location (void);
GFile *flatpak_get_data_dir (const char *app_id);

gboolean flatpak_context_get_allowed_exports (FlatpakContext *context,
                                              const char     *source_path,
                                              const char     *app_id,
                                              char         ***allowed_extensions_out,
                                              char         ***allowed_prefixes_out,
                                              gboolean       *require_exact_match_out);

FlatpakContextSockets flatpak_context_compute_allowed_sockets (FlatpakContext                   *context,
                                                               FlatpakContextConditionEvaluator  evaluator);

FlatpakContextDevices flatpak_context_compute_allowed_devices (FlatpakContext                   *context,
                                                               FlatpakContextConditionEvaluator  evaluator);

void flatpak_context_load_device (FlatpakContext *context,
                                  const char     *device_expr);

#endif /* __FLATPAK_CONTEXT_H__ */
