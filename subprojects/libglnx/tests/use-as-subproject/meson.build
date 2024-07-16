# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.0-or-later

project(
  'use-libglnx-as-subproject',
  'c',
  default_options : [
    'c_std=gnu99',
    'warning_level=3',
  ],
  version : '0',
  meson_version : '>=0.49.0',
)

configure_file(
  copy : true,
  input : 'dummy-config.h.in',
  output : 'config.h',
)

glib_dep = dependency('glib-2.0')

libglnx = subproject('libglnx')
libglnx_dep = libglnx.get_variable('libglnx_dep')
libglnx_testlib_dep = libglnx.get_variable('libglnx_testlib_dep')

# This executable is compiled at warning_level=3 by default
executable(
  'trivial',
  'trivial.c',
  dependencies : [glib_dep],
)

# These can't be compiled at warning_level=3 because they use non-ISO
# compiler features in the libglnx headers, which would be warnings or
# errors with -Wpedantic
executable(
  'use-libglnx',
  'use-libglnx.c',
  dependencies : [libglnx_dep, glib_dep],
  override_options : ['warning_level=2'],
)
executable(
  'use-testlib',
  'use-testlib.c',
  dependencies : [libglnx_testlib_dep, glib_dep],
  override_options : ['warning_level=2'],
)
