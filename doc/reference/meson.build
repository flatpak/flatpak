# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

toml_conf = configuration_data()
toml_conf.set('VERSION', meson.project_version())

source_toml = configure_file(
  input : 'Flatpak.toml.in',
  output : 'Flatpak.toml',
  configuration : toml_conf,
)

custom_target('flatpak-doc',
  input : [source_toml, flatpak_gir[0]],
  output : 'Flatpak-1.0',
  command : [
    gidocgen,
    'generate',
    '--quiet',
    '--no-namespace-dir',
    '--config=@INPUT0@',
    '--output-dir=@OUTPUT@',
    '--content-dir=@0@'.format(meson.current_source_dir()),
    '@INPUT1@',
  ],
  build_by_default : true,
  install : true,
  install_dir : get_option('datadir') / 'doc',
)
