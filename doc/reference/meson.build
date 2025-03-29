# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

cdata = configuration_data()
cdata.set('FLATPAK_MAJOR_VERSION', flatpak_major_version)
cdata.set('FLATPAK_MINOR_VERSION', flatpak_minor_version)
cdata.set('FLATPAK_MICRO_VERSION', flatpak_micro_version)
configure_file(
  configuration : cdata,
  input : 'version.xml.in',
  output : 'version.xml',
)

# We're really only doing this to generate the Docbook XML.
doc_gdbus = gnome.gdbus_codegen(
  'doc-dbus-generated',
  sources : [
    project_source_root / 'data/org.freedesktop.Flatpak.Authenticator.xml',
    project_source_root / 'data/org.freedesktop.Flatpak.xml',
    project_source_root / 'data/org.freedesktop.impl.portal.PermissionStore.xml',
    project_source_root / 'data/org.freedesktop.portal.Documents.xml',
    project_source_root / 'data/org.freedesktop.portal.Flatpak.xml',
  ],
  namespace : 'doc',
  docbook : 'dbus',
)

gnome.gtkdoc(
  'flatpak',
  main_xml : 'libflatpak-docs.xml',
  namespace : 'flatpak',
  src_dir : [
    project_build_root / 'common',
    project_source_root / 'common',
  ],
  content_files : doc_gdbus[2],
  dependencies : base_deps + [libflatpak_dep],
  ignore_headers : [
    'valgrind-private.h',
    'flatpak-bwrap-private.h',
    'flatpak-chain-input-stream-private.h',
    'flatpak-common-types-private.h',
    'flatpak-context-private.h',
    'flatpak-dbus-generated.h',
    'flatpak-dir-private.h',
    'flatpak-document-dbus-generated.h',
    'flatpak-enum-types.h',
    'flatpak-exports-private.h',
    'flatpak-image-source-private.h',
    'flatpak-installed-ref-private.h',
    'flatpak-json-oci-private.h',
    'flatpak-json-private.h',
    'flatpak-oci-registry-private.h',
    'flatpak-progress-private.h',
    'flatpak-remote-private.h',
    'flatpak-remote-ref-private.h',
    'flatpak-run-private.h',
    'flatpak-systemd-dbus-generated.h',
    'flatpak-installation-private.h',
    'flatpak-transaction-private.h',
    'flatpak-utils-private.h',
    'flatpak-utils-base-private.h',
    'flatpak-utils-http-private.h',
    'flatpak-instance-private.h',
    'flatpak-auth-private.h',
    'flatpak-parental-controls-private.h',
    'flatpak-appdata-private.h',
    'flatpak-zstd-decompressor-private.h',
  ],
  install : true,
  scan_args : [
    '--ignore-decorators=FLATPAK_EXTERN',
    '--rebuild-types',
  ],
)
