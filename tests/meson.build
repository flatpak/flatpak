# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

installed_testdir = get_option('prefix') / get_option('libexecdir') / 'installed-tests' / 'Flatpak'
installed_tests_metadir = get_option('prefix') / get_option('datadir') / 'installed-tests' / 'Flatpak'

if get_option('system_bubblewrap') == ''
  env_flatpak_bwrap = project_build_root / 'subprojects' / 'bubblewrap' / 'flatpak-bwrap'
else
  env_flatpak_bwrap = get_option('system_bubblewrap')
endif

if get_option('system_dbus_proxy') == ''
  env_flatpak_dbusproxy = project_build_root / 'subprojects' / 'dbus-proxy' / 'flatpak-dbus-proxy'
else
  env_flatpak_dbusproxy = get_option('system_dbus_proxy')
endif

tests_environment = {
  'FLATPAK_CONFIG_DIR' : '/dev/null',
  'FLATPAK_PORTAL' : project_build_root / 'portal' / 'flatpak-portal',
  'FLATPAK_REVOKEFS_FUSE' : project_build_root / 'revokefs' / 'revokefs-fuse',
  'FLATPAK_TESTS_DEBUG' : '1',
  'FLATPAK_TESTS_STRICT_TAP' : '1',
  'FLATPAK_TRIGGERSDIR' : project_source_root / 'triggers',
  'FLATPAK_VALIDATE_ICON' : project_build_root / 'icon-validator' / 'flatpak-validate-icon',
  'FUSERMOUNT' : fusermount,
  'G_TEST_BUILDDIR' : meson.current_build_dir(),
  'G_TEST_SRCDIR' : meson.current_source_dir(),
  'FLATPAK_BWRAP' : env_flatpak_bwrap,
  'FLATPAK_DBUSPROXY' : env_flatpak_dbusproxy,
}

tests_environment_prepend = {
  'GI_TYPELIB_PATH' : project_build_root / 'common',
  'LD_LIBRARY_PATH' : project_build_root / 'common',
  'PATH' : project_build_root / 'app',
}

lsan_options = 'log_threads=1:suppressions=' + meson.current_source_dir() / 'flatpak-asan.supp'

if get_option('installed_tests')
  configure_file(
    input : 'installed-tests.sh.in',
    output : 'installed-tests.sh',
    configuration : {
      'FUSERMOUNT' : fusermount,
    },
    install_dir : installed_testdir,
  )
endif

# Explicitly doing a find_program() for this avoids lots of output with
# older Meson versions
tap_test = find_program(
  files(project_source_root / 'buildutil/tap-test'),
)

if can_run_host_binaries
  runtime_repo = custom_target(
    'runtime-repo',
    build_by_default : false,
    command : [
      files('make-runtime-repos'),
      project_build_root / 'app',
      files('make-test-runtime.sh'),
      project_build_root / 'tests/runtime-repo',
      '@OUTPUT@',
    ],
    depends : [flatpak_exe],
    output : 'runtime-repo.stamp',
  )
endif

libtestlib = static_library(
  'testlib',
  'testlib.c',
  include_directories : [common_include_directories],
  dependencies : [
    base_deps,
    libglnx_dep,
  ],
  install : false,
)
libtestlib_dep = declare_dependency(
  dependencies : [
    base_deps,
    libglnx_dep,
  ],
  include_directories : [common_include_directories],
  link_with : libtestlib,
)

c_tests = [
  ['testapp', {
    'extra_dependencies' : [
      libflatpak_app_dep,
    ],
  }],
  ['testcommon', {}],
  ['testlibrary', {
    'dependencies' : [
      base_deps,
      fuse_dep,
      libflatpak_dep,
      libglnx_dep,
      libostree_dep,
    ],
    'extra_sources' : [
      'can-use-fuse.c',
      'testlib.c',
    ],
    'timeout' : 150,
  }],
  ['test-context', {}],
  ['test-exports', {}],
  ['test-instance', {
    'extra_dependencies' : [
      libglnx_testlib_dep,
    ],
  }],
  ['test-locale-utils', {}],
  ['test-portal', {
    'extra_sources' : [
      portal_gdbus[0],
      portal_gdbus[1],
    ],
  }],
]

foreach testcase : c_tests
  name = testcase[0]
  options = testcase[1]

  exe = executable(
    name,
    dependencies : options.get('dependencies', [
      base_deps,
      appstream_dep,
      json_glib_dep,
      libflatpak_common_dep,
      libflatpak_common_base_dep,
      libglnx_dep,
      libostree_dep,
      libsoup_dep,
      libtestlib_dep,
    ] + options.get('extra_dependencies', [])),
    sources : [name + '.c'] + options.get('extra_sources', []),
    install : get_option('installed_tests'),
    install_dir : installed_testdir,
  )

  if get_option('installed_tests')
    configure_file(
      input : 'tap.test.in',
      output : name + '.test',
      configuration : {
        'basename' : name,
        'installed_testdir' : installed_testdir,
        'wrapper' : '',
      },
      install_dir : installed_tests_metadir,
    )
  endif

  test_env = environment(tests_environment)
  foreach k, v: tests_environment_prepend
    test_env.prepend(k, v)
  endforeach

  lsan_log_path = ':log_path=' + meson.current_build_dir() / \
                  'asan_' + name.underscorify() + '.log'
  test_env.set('LSAN_OPTIONS', lsan_options + lsan_log_path)

  if can_run_host_binaries
    test(
      name,
      tap_test,
      args : [exe],
      depends : runtime_repo,
      env : test_env,
      protocol : 'tap',
      timeout : options.get('timeout', 30),
    )
  endif
endforeach

executable(
  'hold-lock',
  'hold-lock.c',
  dependencies : [
    base_deps,
    libglnx_dep,
  ],
  include_directories : [common_include_directories],
  install : get_option('installed_tests'),
  install_dir : installed_testdir,
)

executable(
  'httpcache',
  'httpcache.c',
  dependencies : [
    base_deps,
    json_glib_dep,
    libflatpak_common_dep,
    libflatpak_common_base_dep,
    libglnx_dep,
    libostree_dep,
    libsoup_dep,
  ],
  include_directories : [common_include_directories],
  install : get_option('installed_tests'),
  install_dir : installed_testdir,
)

executable(
  'mock-flatpak',
  'mock-flatpak.c',
  dependencies : [
    base_deps,
    appstream_dep,
    json_glib_dep,
    libflatpak_app_dep,
    libflatpak_common_dep,
    libflatpak_common_base_dep,
    libtestlib_dep,
    libglnx_dep,
    libostree_dep,
    libsoup_dep,
  ],
  install : get_option('installed_tests'),
  install_dir : installed_testdir,
)

executable(
  'test-update-portal',
  sources : [
    'test-update-portal.c',
  ] + portal_gdbus,
  dependencies : base_deps,
  include_directories : [common_include_directories],
  install : get_option('installed_tests'),
  install_dir : installed_testdir,
)

executable(
  'test-portal-impl',
  'test-portal-impl.c',
  dependencies : base_deps,
  include_directories : [common_include_directories],
  install : get_option('installed_tests'),
  install_dir : installed_testdir,
)

executable(
  'test-authenticator',
  'test-authenticator.c',
  dependencies : [
    base_deps,
    libflatpak_common_dep,
    libflatpak_common_base_dep,
    libglnx_dep,
  ],
  install : get_option('installed_tests'),
  install_dir : installed_testdir,
)

executable(
  'try-syscall',
  'try-syscall.c',
  include_directories : [common_include_directories],
  install : get_option('installed_tests'),
  install_dir : installed_testdir,
  override_options: ['b_sanitize=none'],
)

executable(
  'list-unused',
  'list-unused.c',
  dependencies : [
    base_deps,
    libflatpak_common_dep,
    libflatpak_common_base_dep,
    libglnx_dep,
    libostree_dep,
  ],
  include_directories : [common_include_directories],
  install : get_option('installed_tests'),
  install_dir : installed_testdir,
)

subdir('share/xdg-desktop-portal/portals')
subdir('services')
subdir('test-keyring')
subdir('test-keyring2')

configure_file(
  input : 'package_version.txt.in',
  output : 'package_version.txt',
  configuration : {
    'PACKAGE_VERSION' : meson.project_version(),
  },
  install : get_option('installed_tests'),
  install_dir : installed_testdir,
)

if get_option('installed_tests')
  subdir('installed-services')

  install_data(
    'http-utils-test-server.py',
    'make-multi-collection-id-repo.sh',
    'make-test-app.sh',
    'make-test-runtime.sh',
    'oci-registry-client.py',
    'oci-registry-server.py',
    'test-webserver.sh',
    'test-wrapper.sh',
    'web-server.py',

    install_dir : installed_testdir,
    install_mode : 'rwxr-xr-x',
  )

  install_data(
    'gphoto2-list',
    'libtest.sh',
    'org.flatpak.Authenticator.test.service.in',
    'org.freedesktop.impl.portal.desktop.test.service.in',
    'org.test.Hello.png',
    'session.conf.in',
    'test.filter',
    'test.portal.in',

    install_dir : installed_testdir,
    install_mode : 'rw-r--r--',
  )
endif

shared_module(
  'preload',
  'libpreload.c',
  install : get_option('installed_tests'),
  install_dir : installed_testdir,
  override_options: ['b_sanitize=none'],
)

wrapped_tests = []
subdir('test-matrix')

foreach testcase : wrapped_tests
  name = testcase['name']
  script = testcase.get('script', name)

  timeout = {
    'test-bundle.sh' : 60,
    'test-oci-registry.sh' : 60,
    'test-repo.sh' : 300,
    'test-run.sh' : 90,
    'test-summaries.sh' : 60,
    'test-unused.sh' : 90,
    'test-update-portal.sh' : 90,
  }.get(script, 30)

  is_parallel = {
    'test-history.sh' : false,
  }.get(script, true)

  if get_option('installed_tests')
    if name == script
      wrapper = ''
    else
      wrapper = installed_testdir / 'test-wrapper.sh'
    endif

    install_data(
      script,
      install_dir : installed_testdir,
      install_mode : 'rwxr-xr-x',
    )
    configure_file(
      input : 'tap.test.in',
      output : name + '.test',
      configuration : {
        'basename' : name,
        'installed_testdir' : installed_testdir,
        'wrapper' : wrapper,
      },
      install_dir : installed_tests_metadir,
    )
  endif

  test_env = environment(tests_environment)
  foreach k, v: tests_environment_prepend
    test_env.prepend(k, v)
  endforeach

  lsan_log_path = ':log_path=' + meson.current_build_dir() / \
                  'asan_' + name.underscorify() + '.log'
  test_env.set('LSAN_OPTIONS', lsan_options + lsan_log_path)

  if can_run_host_binaries
    test(
      name,
      tap_test,
      args : [meson.current_source_dir() / name],
      depends : runtime_repo,
      env : test_env,
      is_parallel : is_parallel,
      protocol : 'tap',
      timeout : timeout,
    )
  endif
endforeach
