#include "config.h"

#include "flatpak-dir-private.h"
#include "flatpak-system-helper.h"

static FlatpakSystemHelperDeployAction
deploy_action_for_ref_kind(FlatpakRefKind ref_kind,
                           FlatpakSystemHelperDeployAction app_action,
                           FlatpakSystemHelperDeployAction runtime_action)
{
  switch (ref_kind)
  {
  case FLATPAK_REF_KIND_APP:
    return app_action;
  case FLATPAK_REF_KIND_RUNTIME:
    return runtime_action;
  }

  g_assert_not_reached();
  return app_action;
}

FlatpakSystemHelperDeployAction
flatpak_system_helper_get_deploy_action(guint32 flags,
                                        FlatpakRefKind ref_kind,
                                        FlatpakSystemHelperRefState ref_state)
{
  if ((flags & (FLATPAK_HELPER_DEPLOY_FLAGS_INSTALL_HINT |
                FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL)) != 0)
    return deploy_action_for_ref_kind(ref_kind,
                                      FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_INSTALL,
                                      FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_INSTALL);

  switch (ref_state)
  {
  case FLATPAK_SYSTEM_HELPER_REF_NOT_INSTALLED:
    return deploy_action_for_ref_kind(ref_kind,
                                      FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_INSTALL,
                                      FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_INSTALL);
  case FLATPAK_SYSTEM_HELPER_REF_INSTALLED:
    break;
  }
  g_assert(ref_state == FLATPAK_SYSTEM_HELPER_REF_INSTALLED);

  if ((flags & FLATPAK_HELPER_DEPLOY_FLAGS_ALLOW_DOWNGRADE) != 0)
    return deploy_action_for_ref_kind(ref_kind,
                                      FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_DOWNGRADE,
                                      FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_DOWNGRADE);

  return deploy_action_for_ref_kind(ref_kind,
                                    FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_UPDATE,
                                    FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_UPDATE);
}

const char *
flatpak_system_helper_deploy_action_to_polkit_action(FlatpakSystemHelperDeployAction action)
{
  switch (action)
  {
  case FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_INSTALL:
    return "org.freedesktop.Flatpak.app-install";
  case FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_UPDATE:
    return "org.freedesktop.Flatpak.app-update";
  case FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_DOWNGRADE:
    return "org.freedesktop.Flatpak.app-downgrade";
  case FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_INSTALL:
    return "org.freedesktop.Flatpak.runtime-install";
  case FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_UPDATE:
    return "org.freedesktop.Flatpak.runtime-update";
  case FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_DOWNGRADE:
    return "org.freedesktop.Flatpak.runtime-downgrade";
  }

  g_assert_not_reached();
  return NULL;
}

FlatpakSystemHelperDeployAuthorization
flatpak_system_helper_get_deploy_authorization(FlatpakSystemHelperDeployAction action)
{
  switch (action)
  {
  case FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_INSTALL:
  case FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_INSTALL:
    return FLATPAK_SYSTEM_HELPER_DEPLOY_AUTHORIZATION_INSTALL;

  case FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_UPDATE:
  case FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_DOWNGRADE:
  case FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_UPDATE:
  case FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_DOWNGRADE:
    return FLATPAK_SYSTEM_HELPER_DEPLOY_AUTHORIZATION_UPDATE;
  }

  g_assert_not_reached();
  return FLATPAK_SYSTEM_HELPER_DEPLOY_AUTHORIZATION_INSTALL;
}
