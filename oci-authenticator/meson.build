# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

executable(
  'flatpak-oci-authenticator',
  'flatpak-oci-authenticator.c',
  dependencies : [
    base_deps,
    json_glib_dep,
    libflatpak_common_base_dep,
    libflatpak_common_dep,
    libglnx_dep,
    libostree_dep,
    libsoup_dep,
  ],
  include_directories : [
    include_directories('.'),
  ],
  install : true,
  install_dir : get_option('libexecdir'),
)

configure_file(
  input : 'flatpak-oci-authenticator.service.in',
  output : 'flatpak-oci-authenticator.service',
  configuration : service_conf_data,
  install_dir : get_option('systemduserunitdir'),
)

configure_file(
  input : 'org.flatpak.Authenticator.Oci.service.in',
  output : 'org.flatpak.Authenticator.Oci.service',
  configuration : service_conf_data,
  install_dir : dbus_service_dir,
)
