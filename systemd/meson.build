# Copyright 2026 The Flatpak Authors
# SPDX-License-Identifier: LGPL-2.1-or-later

# Reference unit for OS vendors; not enabled by default (no alias/wants from us).
configure_file(
  input : 'flatpak-preinstall.service.in',
  output : 'flatpak-preinstall.service',
  configuration : service_conf_data,
  install_dir : get_option('systemdsystemunitdir'),
)
