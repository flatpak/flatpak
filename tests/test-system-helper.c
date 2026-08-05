#include "config.h"

#include <glib.h>

#include "flatpak-dir-private.h"
#include "../system-helper/flatpak-system-helper.h"

typedef struct
{
  guint32 flags;
  FlatpakRefKind ref_kind;
  FlatpakSystemHelperRefState ref_state;
  FlatpakSystemHelperDeployAction expected_action;
} DeployActionTest;

static void
assert_deploy_action(DeployActionTest test)
{
  FlatpakSystemHelperDeployAction action;

  action = flatpak_system_helper_get_deploy_action(test.flags,
                                                   test.ref_kind,
                                                   test.ref_state);

  g_assert_cmpint(action, ==, test.expected_action);
}

static void
test_deploy_action_selection(void)
{
  assert_deploy_action((DeployActionTest){
      .flags = FLATPAK_HELPER_DEPLOY_FLAGS_NONE,
      .ref_kind = FLATPAK_REF_KIND_APP,
      .ref_state = FLATPAK_SYSTEM_HELPER_REF_INSTALLED,
      .expected_action = FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_UPDATE,
  });
  assert_deploy_action((DeployActionTest){
      .flags = FLATPAK_HELPER_DEPLOY_FLAGS_ALLOW_DOWNGRADE,
      .ref_kind = FLATPAK_REF_KIND_APP,
      .ref_state = FLATPAK_SYSTEM_HELPER_REF_INSTALLED,
      .expected_action = FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_DOWNGRADE,
  });
  assert_deploy_action((DeployActionTest){
      .flags = FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL |
               FLATPAK_HELPER_DEPLOY_FLAGS_ALLOW_DOWNGRADE,
      .ref_kind = FLATPAK_REF_KIND_APP,
      .ref_state = FLATPAK_SYSTEM_HELPER_REF_INSTALLED,
      .expected_action = FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_INSTALL,
  });
  assert_deploy_action((DeployActionTest){
      .flags = FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL,
      .ref_kind = FLATPAK_REF_KIND_RUNTIME,
      .ref_state = FLATPAK_SYSTEM_HELPER_REF_INSTALLED,
      .expected_action = FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_INSTALL,
  });
  assert_deploy_action((DeployActionTest){
      .flags = FLATPAK_HELPER_DEPLOY_FLAGS_INSTALL_HINT,
      .ref_kind = FLATPAK_REF_KIND_APP,
      .ref_state = FLATPAK_SYSTEM_HELPER_REF_INSTALLED,
      .expected_action = FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_INSTALL,
  });
  assert_deploy_action((DeployActionTest){
      .flags = FLATPAK_HELPER_DEPLOY_FLAGS_NONE,
      .ref_kind = FLATPAK_REF_KIND_RUNTIME,
      .ref_state = FLATPAK_SYSTEM_HELPER_REF_NOT_INSTALLED,
      .expected_action = FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_INSTALL,
  });
  assert_deploy_action((DeployActionTest){
      .flags = FLATPAK_HELPER_DEPLOY_FLAGS_ALLOW_DOWNGRADE,
      .ref_kind = FLATPAK_REF_KIND_APP,
      .ref_state = FLATPAK_SYSTEM_HELPER_REF_NOT_INSTALLED,
      .expected_action = FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_INSTALL,
  });
  assert_deploy_action((DeployActionTest){
      .flags = FLATPAK_HELPER_DEPLOY_FLAGS_NONE,
      .ref_kind = FLATPAK_REF_KIND_RUNTIME,
      .ref_state = FLATPAK_SYSTEM_HELPER_REF_INSTALLED,
      .expected_action = FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_UPDATE,
  });
  assert_deploy_action((DeployActionTest){
      .flags = FLATPAK_HELPER_DEPLOY_FLAGS_ALLOW_DOWNGRADE,
      .ref_kind = FLATPAK_REF_KIND_RUNTIME,
      .ref_state = FLATPAK_SYSTEM_HELPER_REF_INSTALLED,
      .expected_action = FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_DOWNGRADE,
  });
}

static void
test_deploy_action_polkit_names(void)
{
  g_assert_cmpstr(flatpak_system_helper_deploy_action_to_polkit_action(FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_INSTALL),
                  ==, "org.freedesktop.Flatpak.app-install");
  g_assert_cmpstr(flatpak_system_helper_deploy_action_to_polkit_action(FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_UPDATE),
                  ==, "org.freedesktop.Flatpak.app-update");
  g_assert_cmpstr(flatpak_system_helper_deploy_action_to_polkit_action(FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_DOWNGRADE),
                  ==, "org.freedesktop.Flatpak.app-downgrade");
  g_assert_cmpstr(flatpak_system_helper_deploy_action_to_polkit_action(FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_INSTALL),
                  ==, "org.freedesktop.Flatpak.runtime-install");
  g_assert_cmpstr(flatpak_system_helper_deploy_action_to_polkit_action(FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_UPDATE),
                  ==, "org.freedesktop.Flatpak.runtime-update");
  g_assert_cmpstr(flatpak_system_helper_deploy_action_to_polkit_action(FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_DOWNGRADE),
                  ==, "org.freedesktop.Flatpak.runtime-downgrade");
}

static void
test_deploy_action_authorization(void)
{
  g_assert_cmpint(flatpak_system_helper_get_deploy_authorization(FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_INSTALL),
                  ==, FLATPAK_SYSTEM_HELPER_DEPLOY_AUTHORIZATION_INSTALL);
  g_assert_cmpint(flatpak_system_helper_get_deploy_authorization(FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_INSTALL),
                  ==, FLATPAK_SYSTEM_HELPER_DEPLOY_AUTHORIZATION_INSTALL);
  g_assert_cmpint(flatpak_system_helper_get_deploy_authorization(FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_UPDATE),
                  ==, FLATPAK_SYSTEM_HELPER_DEPLOY_AUTHORIZATION_UPDATE);
  g_assert_cmpint(flatpak_system_helper_get_deploy_authorization(FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_APP_DOWNGRADE),
                  ==, FLATPAK_SYSTEM_HELPER_DEPLOY_AUTHORIZATION_UPDATE);
  g_assert_cmpint(flatpak_system_helper_get_deploy_authorization(FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_UPDATE),
                  ==, FLATPAK_SYSTEM_HELPER_DEPLOY_AUTHORIZATION_UPDATE);
  g_assert_cmpint(flatpak_system_helper_get_deploy_authorization(FLATPAK_SYSTEM_HELPER_DEPLOY_ACTION_RUNTIME_DOWNGRADE),
                  ==, FLATPAK_SYSTEM_HELPER_DEPLOY_AUTHORIZATION_UPDATE);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/system-helper/deploy-action/selection", test_deploy_action_selection);
  g_test_add_func("/system-helper/deploy-action/polkit-names", test_deploy_action_polkit_names);
  g_test_add_func("/system-helper/deploy-action/authorization", test_deploy_action_authorization);

  return g_test_run();
}
