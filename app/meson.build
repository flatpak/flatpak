# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

sources = [
  'flatpak-builtins-utils.c',
  'flatpak-table-printer.c',
  'flatpak-tty-utils.c',
]

parse_datetime = custom_target(
  'parse-datetime.c',
  input : [
    'parse-datetime.y',
  ],
  output : [
    'parse-datetime.c',
  ],
  build_by_default : true,
  command : [
    bison,
    '@INPUT@',
    '-o', '@OUTPUT@',
  ],
)

flatpak_permission_gdbus = gnome.gdbus_codegen(
  'flatpak-permission-dbus-generated',
  sources: [
    '../data/org.freedesktop.impl.portal.PermissionStore.xml',
  ],
  interface_prefix : 'org.freedesktop.impl.portal',
  namespace : 'XdpDbus',
)

libflatpak_app = static_library(
  'libflatpak-app',
  sources : sources + [parse_datetime[0]],
  dependencies : base_deps + [
    appstream_dep,
    json_glib_dep,
    libflatpak_common_base_dep,
    libglnx_dep,
    libostree_dep,
    libsystemd_dep,
    polkit_agent_dep,
  ],
  include_directories : [
    common_include_directories,
    include_directories('.'),
  ],
  install : false,
)
libflatpak_app_dep = declare_dependency(
  dependencies : [
    base_deps,
    appstream_dep,
    json_glib_dep,
    libglnx_dep,
    libostree_dep,
    libsystemd_dep,
    polkit_agent_dep,
  ],
  include_directories : [
    common_include_directories,
    include_directories('.'),
  ],
  link_with : [
    libflatpak_app,
  ],
)

sources = [
  'flatpak-builtins-bind-mount.c',
  'flatpak-builtins-build-bundle.c',
  'flatpak-builtins-build-commit-from.c',
  'flatpak-builtins-build-export.c',
  'flatpak-builtins-build-finish.c',
  'flatpak-builtins-build-import-bundle.c',
  'flatpak-builtins-build-init.c',
  'flatpak-builtins-build-sign.c',
  'flatpak-builtins-build-update-repo.c',
  'flatpak-builtins-build.c',
  'flatpak-builtins-config.c',
  'flatpak-builtins-create-usb.c',
  'flatpak-builtins-document-export.c',
  'flatpak-builtins-document-info.c',
  'flatpak-builtins-document-list.c',
  'flatpak-builtins-document-unexport.c',
  'flatpak-builtins-enter.c',
  'flatpak-builtins-history.c',
  'flatpak-builtins-info.c',
  'flatpak-builtins-install.c',
  'flatpak-builtins-kill.c',
  'flatpak-builtins-list.c',
  'flatpak-builtins-make-current.c',
  'flatpak-builtins-mask.c',
  'flatpak-builtins-override.c',
  'flatpak-builtins-permission-list.c',
  'flatpak-builtins-permission-remove.c',
  'flatpak-builtins-permission-reset.c',
  'flatpak-builtins-permission-set.c',
  'flatpak-builtins-permission-show.c',
  'flatpak-builtins-pin.c',
  'flatpak-builtins-ps.c',
  'flatpak-builtins-remote-add.c',
  'flatpak-builtins-remote-delete.c',
  'flatpak-builtins-remote-info.c',
  'flatpak-builtins-remote-list.c',
  'flatpak-builtins-remote-ls.c',
  'flatpak-builtins-remote-modify.c',
  'flatpak-builtins-repair.c',
  'flatpak-builtins-repo.c',
  'flatpak-builtins-run.c',
  'flatpak-builtins-search.c',
  'flatpak-builtins-uninstall.c',
  'flatpak-builtins-update.c',
  'flatpak-cli-transaction.c',
  'flatpak-complete.c',
  'flatpak-main.c',
  'flatpak-quiet-transaction.c',
]

if build_system_helper
  sources += [
    'flatpak-polkit-agent-text-listener.c',
  ]
endif

flatpak_exe = executable(
  'flatpak',
  dependencies : base_deps + [
    appstream_dep,
    json_glib_dep,
    libflatpak_app_dep,
    libflatpak_common_dep,
    libflatpak_common_base_dep,
    libglnx_dep,
    libostree_dep,
    libsystemd_dep,
    polkit_agent_dep,
  ],
  install : true,
  install_dir : get_option('bindir'),
  sources : sources + flatpak_gdbus + flatpak_permission_gdbus,
)
