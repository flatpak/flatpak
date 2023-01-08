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
  ['tests', 'org.flatpak.Authenticator.test', {}],
  ['tests', 'org.freedesktop.impl.portal.desktop.test', {}],
]
  directory = triple[0]
  service = triple[1]
  options = triple[2]

  configure_file(
    input : project_source_root / directory / (service + '.service.in'),
    output : service + '.service',
    configuration : {
      'extraargs' : options.get('extraargs', ''),
      'libexecdir' : project_build_root / directory,
    }
  )
endforeach
