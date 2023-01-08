# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

executable(
  'flatpak-session-helper',
  dependencies : [
    base_deps,
    threads_dep,
    libglnx_dep,
    libflatpak_common_base_dep,
  ],
  install : true,
  install_dir : get_option('libexecdir'),
  sources : ['flatpak-session-helper.c'],
)

configure_file(
  input : 'flatpak-session-helper.service.in',
  output : 'flatpak-session-helper.service',
  configuration : service_conf_data,
  install_dir : get_option('systemduserunitdir'),
)

configure_file(
  input : 'org.freedesktop.Flatpak.service.in',
  output : 'org.freedesktop.Flatpak.service',
  configuration : service_conf_data,
  install_dir : dbus_service_dir,
)
