# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

install_data(
  '60-flatpak',
  install_dir : get_option('systemduserenvgendir'),
)
install_data(
  '60-flatpak-system-only',
  install_dir : get_option('systemdsystemenvgendir'),
)

if get_option('gdm_env_file')
  conf_data = configuration_data()
  conf_data.set('localstatedir', get_option('prefix') / get_option('localstatedir'))
  conf_data.set('sysconfdir', get_option('prefix') / get_option('sysconfdir'))
  configure_file(
    input : 'flatpak.env.in',
    output : 'flatpak.env',
    configuration : conf_data,
    install_dir : get_option('datadir') / 'gdm' / 'env.d',
  )
endif
