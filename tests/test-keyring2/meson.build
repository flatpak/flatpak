# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

if get_option('installed_tests')
  install_sub = installed_testdir / 'test-keyring2'
else
  install_sub = false
endif

foreach file : [
  'README',
  'pubring.gpg',
  'secring.gpg',
]
  # TODO: When we depend on Meson 0.50.0, we can use
  # install : get_option('installed_tests')
  # instead of
  # install_dir : false
  configure_file(
    input : file,
    output : file,
    copy : true,
    install_dir : install_sub,
    install_mode : 'rw-r--r--',
  )
endforeach
