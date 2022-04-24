# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

foreach file : [
  'README',
  'pubring.gpg',
  'secring.gpg',
]
  configure_file(
    input : file,
    output : file,
    copy : true,
    install : get_option('installed_tests'),
    install_dir : installed_testdir / 'test-keyring',
    install_mode : 'rw-r--r--',
  )
endforeach
