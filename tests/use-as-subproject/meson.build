# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.0-or-later

project(
  'use-libglnx-as-subproject',
  'c',
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

executable('use-libglnx', 'use-libglnx.c', dependencies : [libglnx_dep, glib_dep])
executable('use-testlib', 'use-testlib.c', dependencies : [libglnx_testlib_dep, glib_dep])
