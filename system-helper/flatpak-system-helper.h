/*
 * Copyright © 2021 Collabora Ltd.
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
 */

#ifndef __FLATPAK_SYSTEM_HELPER_H__
#define __FLATPAK_SYSTEM_HELPER_H__

#include <glib.h>
#include "flatpak-ref.h"

#define FLATPAK_SYSTEM_HELPER_BUS_NAME "org.freedesktop.Flatpak.SystemHelper"
#define FLATPAK_SYSTEM_HELPER_PATH "/org/freedesktop/Flatpak/SystemHelper"
#define FLATPAK_SYSTEM_HELPER_INTERFACE FLATPAK_SYSTEM_HELPER_BUS_NAME

typedef enum
{
  FLATPAK_SYSTEM_HELPER_REF_NOT_INSTALLED,
  FLATPAK_SYSTEM_HELPER_REF_INSTALLED,
} FlatpakSystemHelperRefState;

typedef enum
{
  FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_INSTALL,
  FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_UPDATE,
  FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_DOWNGRADE,
  FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_INSTALL,
  FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_UPDATE,
  FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_DOWNGRADE,
} FlatpakSystemHelperDeployAction;

typedef enum
{
  FLATPAK_SYSTEM_HELPER_DEPLOY_AUTHORIZATION_INSTALL,
  FLATPAK_SYSTEM_HELPER_DEPLOY_AUTHORIZATION_UPDATE,
} FlatpakSystemHelperDeployAuthorization;

FlatpakSystemHelperDeployAction flatpak_system_helper_get_deploy_action (guint32                      flags,
                                                                        FlatpakRefKind               ref_kind,
                                                                        FlatpakSystemHelperRefState  ref_state);
const char *flatpak_system_helper_deploy_action_to_polkit_action (FlatpakSystemHelperDeployAction action);
FlatpakSystemHelperDeployAuthorization flatpak_system_helper_get_deploy_authorization (FlatpakSystemHelperDeployAction action);

#endif
