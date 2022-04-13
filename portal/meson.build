# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

permission_gdbus = gnome.gdbus_codegen(
  'flatpak-permission-dbus',
  sources: [
    '../data/org.freedesktop.impl.portal.PermissionStore.xml',
  ],
  interface_prefix : 'org.freedesktop.impl.portal',
  namespace : 'XdpDbus',
)

portal_gdbus = gnome.gdbus_codegen(
  'flatpak-portal-dbus',
  sources: [
    '../data/org.freedesktop.portal.Flatpak.xml',
  ],
  interface_prefix : 'org.freedesktop.portal',
  namespace : 'Portal',
)

executable(
  'flatpak-portal',
  dependencies : [
    threads_dep,
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
  sources : [
    'flatpak-portal.c',
    'flatpak-portal-app-info.c',
    'portal-impl.c',
    '../common/flatpak-portal-error.c',
  ] + permission_gdbus + portal_gdbus,
)

configure_file(
  input : 'flatpak-portal.service.in',
  output : 'flatpak-portal.service',
  configuration : service_conf_data,
  install_dir : get_option('systemduserunitdir'),
)

configure_file(
  input : 'org.freedesktop.portal.Flatpak.service.in',
  output : 'org.freedesktop.portal.Flatpak.service',
  configuration : service_conf_data,
  install_dir : dbus_service_dir,
)
