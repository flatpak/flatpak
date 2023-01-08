# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

executable(
  'revokefs-fuse',
  dependencies : [
    base_deps,
    fuse_dep,
    libglnx_dep,
  ],
  install : true,
  install_dir : get_option('libexecdir'),
  sources : [
    'main.c',
    'writer.c',
  ],
)

executable(
  'revokefs-demo',
  install : false,
  sources : ['demo.c'],
  dependencies : base_deps,
)
