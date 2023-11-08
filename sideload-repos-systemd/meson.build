# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

install_data(
  'flatpak-create-sideload-symlinks.sh',
  install_dir : get_option('libexecdir'),
  install_mode : 'rwxr-xr-x',
)

configure_file(
  input : 'flatpak-sideload-usb-repo.service.in',
  output : 'flatpak-sideload-usb-repo.service',
  configuration : service_conf_data,
  install_dir : get_option('systemduserunitdir'),
)

configure_file(
  input : 'flatpak-sideload-usb-repo.path.in',
  output : 'flatpak-sideload-usb-repo.path',
  configuration : service_conf_data,
  install_dir : get_option('systemduserunitdir'),
)

install_data(
  'tmpfiles.d/flatpak-sideload-repos.conf',
  install_dir : get_option('tmpfilesdir'),
)
