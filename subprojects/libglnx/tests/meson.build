# Copyright 2019 Endless OS Foundation LLC
# Copyright 2019 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

libglnx_testlib = static_library(
  'glnx-testlib',
  'libglnx-testlib.c',
  'libglnx-testlib.h',
  dependencies : [
    libglnx_dep,
    libglnx_deps,
  ],
  install : false,
)
libglnx_testlib_dep = declare_dependency(
  dependencies : [
    libglnx_dep,
    libglnx_deps,
  ],
  include_directories : include_directories('.'),
  link_with : libglnx_testlib,
)

if get_option('tests')
  testing_helper = executable(
    'testing-helper',
    'testing-helper.c',
    dependencies : [
      libglnx_dep,
      libglnx_deps,
    ],
    install : false,
  )

  test_names = [
    'backports',
    'errors',
    'fdio',
    'macros',
    'shutil',
    'testing',
    'xattrs',
  ]

  foreach test_name : test_names
    exe = executable(test_name,
      [
        'test-libglnx-' + test_name + '.c',
      ],
      dependencies: [
        libglnx_dep,
        libglnx_deps,
        libglnx_testlib_dep,
      ],
    )
    test(test_name, exe, depends: testing_helper)
  endforeach
endif
