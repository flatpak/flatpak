# Copyright 2019 Endless OS Foundation LLC
# Copyright 2019 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

test_names = [
  'errors',
  'fdio',
  'macros',
  'shutil',
  'xattrs',
]

foreach test_name : test_names
  exe = executable(test_name,
    [
      'libglnx-testlib.c',
      'libglnx-testlib.h',
      'test-libglnx-' + test_name + '.c',
    ],
    dependencies: [
      libglnx_dep,
      libglnx_deps,
    ],
  )
  test(test_name, exe)
endforeach
