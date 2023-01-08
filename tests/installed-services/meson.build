# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

foreach triple : [
  ['oci-authenticator', 'org.flatpak.Authenticator.Oci', {}],
  ['portal', 'org.freedesktop.portal.Flatpak', {
    'extraargs' : ' --poll-timeout=1',
  }],
  ['session-helper', 'org.freedesktop.Flatpak', {}],
  ['system-helper', 'org.freedesktop.Flatpak.SystemHelper', {
    'extraargs' : ' --session --no-idle-exit',
  }],
]
  directory = triple[0]
  service = triple[1]
  options = triple[2]

  configure_file(
    input : project_source_root / directory / (service + '.service.in'),
    output : service + '.service',
    configuration : {
      'extraargs' : options.get('extraargs', ''),
      'libexecdir' : get_option('prefix') / get_option('libexecdir'),
    },
    install_dir : installed_testdir / 'services',
  )
endforeach

foreach service : [
  'org.flatpak.Authenticator.test',
  'org.freedesktop.impl.portal.desktop.test',
]
  configure_file(
    input : project_source_root / 'tests' / (service + '.service.in'),
    output : service + '.service',
    configuration : {
      'libexecdir' : installed_testdir,
    },
    install_dir : installed_testdir / 'services',
  )
endforeach

install_data(
  project_source_root / 'tests/test.portal.in',
  install_dir : installed_testdir / 'share/xdg-desktop-portal/portals',
  rename : 'test.portal',
)
