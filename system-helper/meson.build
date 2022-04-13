# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

executable(
  'flatpak-system-helper',
  dependencies : base_deps + [
    appstream_dep,
    json_glib_dep,
    libflatpak_common_base_dep,
    libflatpak_common_dep,
    libglnx_dep,
    libostree_dep,
    libsoup_dep,
    polkit_agent_dep,
  ],
  install : true,
  install_dir : get_option('libexecdir'),
  sources : ['flatpak-system-helper.c'],
)

configure_file(
  input : 'flatpak-system-helper.service.in',
  output : 'flatpak-system-helper.service',
  configuration : service_conf_data,
  install_dir : get_option('systemdsystemunitdir'),
)

configure_file(
  input : 'org.freedesktop.Flatpak.SystemHelper.service.in',
  output : 'org.freedesktop.Flatpak.SystemHelper.service',
  configuration : service_conf_data,
  install_dir : get_option('datadir') / 'dbus-1' / 'system-services',
)

install_data(
  'org.freedesktop.Flatpak.SystemHelper.conf',
  install_dir : dbus_config_dir,
)

conf_data = configuration_data()
conf_data.set('privileged_group', get_option('privileged_group'))
configure_file(
  input : 'org.freedesktop.Flatpak.rules.in',
  output : 'org.freedesktop.Flatpak.rules',
  configuration : conf_data,
  install_dir : get_option('datadir') / 'polkit-1' / 'rules.d',
)

i18n.merge_file(
  input : 'org.freedesktop.Flatpak.policy.in',
  output : 'org.freedesktop.Flatpak.policy',
  po_dir : '../po',
  install : true,
  install_dir : get_option('datadir') / 'polkit-1' / 'actions',
)

conf_data = configuration_data()
conf_data.set('SYSTEM_HELPER_USER', get_option('system_helper_user'))
configure_file(
  input : 'flatpak.conf.in',
  output : 'flatpak.conf',
  configuration : conf_data,
  install_dir : get_option('sysusersdir'),
)
