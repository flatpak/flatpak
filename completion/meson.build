# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

install_data(
  'flatpak',
  install_dir : get_option('datadir') / 'bash-completion' / 'completions',
)
install_data(
  '_flatpak',
  install_dir : get_option('datadir') / 'zsh' / 'site-functions',
)

# Do not install a fish completion here: fish ships a maintained
# share/completions/flatpak.fish that is preferred over vendor_completions.d.

install_data(
  'flatpak-coredumpctl',
  install_dir : get_option('datadir') / 'bash-completion' / 'completions',
)
