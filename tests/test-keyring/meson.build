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

configure_file(
  input : 'openpgp-revocs.d/3718EEBEB5740A7AB3D651B7138B31E07B0961FD.rev',
  output : '3718EEBEB5740A7AB3D651B7138B31E07B0961FD.rev',
  copy : true,
  install : get_option('installed_tests'),
  install_dir : installed_testdir / 'test-keyring' / 'openpgp-revocs.d',
  install_mode : 'rw-r--r--',
)