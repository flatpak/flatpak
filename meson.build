# Copyright 2019 Endless OS Foundation LLC
# Copyright 2019 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

project(
  'libglnx',
  'c',
  default_options : [
    'c_std=gnu99',
  ],
)

add_project_arguments('-D_GNU_SOURCE', language: 'c')
add_project_arguments('-Wno-unused-local-typedefs', language: 'c')

cc = meson.get_compiler('c')


check_functions = [
  'renameat2',
  'memfd_create',
  'copy_file_range',
]
conf = configuration_data()
foreach check_function : check_functions
  have_it = cc.compiles('''
    #include <sys/types.h>
    #include <unistd.h>
    #include <stdio.h>
    #include <sys/mount.h>
    #include <fcntl.h>
    #include <sched.h>
    #include <linux/loop.h>
    #include <linux/random.h>
    #include <sys/mman.h>

    int func (void) {
      (void) ''' + check_function + ''';
    }
    ''',
    args : '-D_GNU_SOURCE',
    name : check_function + '() is declared',
  )
  conf.set10('HAVE_DECL_' + check_function.underscorify().to_upper(), have_it)
endforeach
config_h = configure_file(
  output : 'libglnx-config.h',
  configuration : conf,
)

libglnx_deps = [
  dependency('gio-2.0'),
  dependency('gio-unix-2.0'),
]
libglnx_inc = include_directories('.')
libglnx_sources = [
  'glnx-backport-autocleanups.h',
  'glnx-backport-autoptr.h',
  'glnx-backports.c',
  'glnx-backports.h',
  'glnx-console.c',
  'glnx-console.h',
  'glnx-dirfd.c',
  'glnx-dirfd.h',
  'glnx-errors.c',
  'glnx-errors.h',
  'glnx-fdio.c',
  'glnx-fdio.h',
  'glnx-local-alloc.c',
  'glnx-local-alloc.h',
  'glnx-lockfile.c',
  'glnx-lockfile.h',
  'glnx-macros.h',
  'glnx-missing.h',
  'glnx-missing-syscall.h',
  'glnx-shutil.c',
  'glnx-shutil.h',
  'glnx-xattrs.c',
  'glnx-xattrs.h',
  'libglnx.h',
]

libglnx = static_library('glnx',
  libglnx_sources,
  dependencies : libglnx_deps,
  include_directories : libglnx_inc,
  install : false)
libglnx_dep = declare_dependency(
  dependencies : libglnx_deps,
  include_directories : libglnx_inc,
  link_with : libglnx)

subdir('tests')

