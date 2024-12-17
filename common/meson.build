# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

public_headers = [
  'flatpak-bundle-ref.h',
  'flatpak-error.h',
  'flatpak-installation.h',
  'flatpak-installed-ref.h',
  'flatpak-instance.h',
  'flatpak-portal-error.h',
  'flatpak-ref.h',
  'flatpak-related-ref.h',
  'flatpak-remote-ref.h',
  'flatpak-remote.h',
  'flatpak-transaction.h',
  'flatpak.h',
]

install_headers(
  public_headers,
  subdir : 'flatpak',
)

flatpak_version_macros = configure_file(
  input : 'flatpak-version-macros.h.in',
  output : 'flatpak-version-macros.h',
  configuration : {
    'FLATPAK_MAJOR_VERSION' : flatpak_major_version,
    'FLATPAK_MINOR_VERSION' : flatpak_minor_version,
    'FLATPAK_MICRO_VERSION' : flatpak_micro_version,
  },
  install_dir : get_option('includedir') / 'flatpak',
)

# TODO: After the Autotools build system is removed, we can probably
# switch this to gnome.mkenums_simple, but it's easier to keep them
# consistent if we use the same templates
enums = gnome.mkenums(
  'flatpak-enum-types',
  c_template : 'flatpak-enum-types.c.template',
  h_template : 'flatpak-enum-types.h.template',
  install_dir : get_option('includedir') / 'flatpak',
  install_header : true,
  sources : public_headers,
)

flatpak_gdbus = gnome.gdbus_codegen(
  'flatpak-dbus-generated',
  sources : [
    '../data/org.freedesktop.Flatpak.xml',
    '../data/org.freedesktop.Flatpak.Authenticator.xml',
  ],
  interface_prefix : 'org.freedesktop.Flatpak.',
  namespace : 'Flatpak',
)

flatpak_document_gdbus = gnome.gdbus_codegen(
  'flatpak-document-dbus-generated',
  sources: [
    '../data/org.freedesktop.portal.Documents.xml',
  ],
  interface_prefix : 'org.freedesktop.portal.',
  namespace : 'XdpDbus',
)

systemd_gdbus = gnome.gdbus_codegen(
  'flatpak-systemd-dbus-generated',
  sources: [
    '../data/org.freedesktop.systemd1.xml',
  ],
  interface_prefix : 'org.freedesktop.systemd1.',
  namespace : 'Systemd',
)

variant_schema_compiler_command = [
  global_source_root / 'subprojects' / 'variant-schema-compiler' / 'variant-schema-compiler',
]

if get_option('internal_checks')
  variant_schema_compiler_command += ['--internal-validation']
endif

variant_schema_compiler_command += [
  '--outfile', '@OUTPUT0@',
  '--outfile-header', '@OUTPUT1@',
  '--prefix', 'var',
  '@INPUT@',
]

flatpak_variant = custom_target(
  'flatpak-variant-private.h',
  input : [
    '../data/flatpak-variants.gv',
  ],
  output : [
    'flatpak-variant-impl-private.h',
    'flatpak-variant-private.h',
  ],
  build_by_default : true,
  command : variant_schema_compiler_command,
)

libflatpak_common_base_deps = base_deps + [libglnx_dep]

libflatpak_common_base_sources = [
  'flatpak-utils-base.c',
  'flatpak-utils-base-private.h',
] + flatpak_gdbus + flatpak_document_gdbus

built_headers = [
  enums[1],
  flatpak_version_macros,
  flatpak_gdbus[1],
  flatpak_document_gdbus[1],
  systemd_gdbus[1],
  flatpak_variant[1],
]

libflatpak_common_deps = [
  base_deps,
  dconf_dep,
  gpgme_dep,
  json_glib_dep,
  libarchive_dep,
  libcurl_dep,
  libglnx_dep,
  libostree_dep,
  libseccomp_dep,
  libsoup_dep,
  libsystemd_dep,
  libxml_dep,
  libzstd_dep,
  malcontent_dep,
  polkit_agent_dep,
  xau_dep,
]
built_sources = []

if build_wayland_security_context
  wayland_scanner_prog = find_program(wayland_scanner.get_variable(pkgconfig: 'wayland_scanner'))
  wayland_protocols_dir = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir')
  wl_security_context_xml = wayland_protocols_dir / 'staging/security-context/security-context-v1.xml'
  wl_security_context = [
    custom_target(
      'security-context-v1-protocol.c',
      input : wl_security_context_xml,
      output : 'security-context-v1-protocol.c',
      command : [wayland_scanner_prog, 'private-code', '@INPUT@', '@OUTPUT@'],
    ),
    custom_target(
      'security-context-v1-protocol.h',
      input : wl_security_context_xml,
      output : 'security-context-v1-protocol.h',
      command : [wayland_scanner_prog, 'client-header', '@INPUT@', '@OUTPUT@'],
    ),
  ]

  libflatpak_common_deps += [wayland_client]
  built_sources += [wl_security_context]
  built_headers += [wl_security_context[1]]
endif

libflatpak_common_base = static_library(
  'flatpak-common-base',
  dependencies : libflatpak_common_base_deps,
  gnu_symbol_visibility : 'hidden',
  include_directories : [common_include_directories],
  install : false,
  sources : libflatpak_common_base_sources,
)
libflatpak_common_base_dep = declare_dependency(
  dependencies : base_deps + [libglnx_dep],
  include_directories : [common_include_directories],
  link_with : [
    libflatpak_common_base,
  ],
  sources : built_headers,
)

sources = [
  'flatpak-appdata.c',
  'flatpak-auth.c',
  'flatpak-bundle-ref.c',
  'flatpak-bwrap.c',
  'flatpak-chain-input-stream.c',
  'flatpak-context.c',
  'flatpak-dir.c',
  'flatpak-dir-utils.c',
  'flatpak-docker-reference.c',
  'flatpak-error.c',
  'flatpak-exports.c',
  'flatpak-glib-backports.c',
  'flatpak-image-source.c',
  'flatpak-installation.c',
  'flatpak-installed-ref.c',
  'flatpak-instance.c',
  'flatpak-json-oci.c',
  'flatpak-json.c',
  'flatpak-locale-utils.c',
  'flatpak-oci-registry.c',
  'flatpak-portal-error.c',
  'flatpak-progress.c',
  'flatpak-prune.c',
  'flatpak-ref-utils.c',
  'flatpak-ref.c',
  'flatpak-related-ref.c',
  'flatpak-remote-ref.c',
  'flatpak-remote.c',
  'flatpak-repo-utils.c',
  'flatpak-run.c',
  'flatpak-run-cups.c',
  'flatpak-run-dbus.c',
  'flatpak-run-pulseaudio.c',
  'flatpak-run-sockets.c',
  'flatpak-run-wayland.c',
  'flatpak-run-x11.c',
  'flatpak-transaction.c',
  'flatpak-utils-http.c',
  'flatpak-utils.c',
  'flatpak-uri.c',
  'flatpak-usb.c',
  'flatpak-xml-utils.c',
  'flatpak-zstd-decompressor.c',
]

if malcontent_dep.found()
  sources += ['flatpak-parental-controls.c']
endif

libflatpak_common = static_library(
  'flatpak-common',
  dependencies : [libflatpak_common_base_dep] + libflatpak_common_deps,
  gnu_symbol_visibility : 'hidden',
  include_directories : [common_include_directories],
  install : false,
  sources : enums + public_headers + sources + built_sources + systemd_gdbus + [
    flatpak_variant[0],
    flatpak_variant[1],
  ],
)
libflatpak_common_dep = declare_dependency(
  dependencies : [
    base_deps,
    libflatpak_common_base_dep,
    libglnx_dep,
  ],
  include_directories : [common_include_directories],
  link_with : [
    libflatpak_common,
  ],
  sources : built_headers,
)

libflatpak = library(
  'flatpak',
  'flatpak.c',
  gnu_symbol_visibility : 'hidden',
  include_directories : [common_include_directories],
  install : true,
  link_args : ['-Wl,--export-dynamic'],
  link_whole : [
    libflatpak_common_base,
    libflatpak_common,
  ],
  soversion : '0',
  version : '0.@0@.0'.format(flatpak_binary_age),
)
libflatpak_dep = declare_dependency(
  dependencies : libflatpak_common_deps,
  include_directories : [common_include_directories],
  link_with : [
    libflatpak,
  ],
  sources : built_headers,
)

test_libflatpak = executable(
  'test-libflatpak',
  'test-lib.c',
  dependencies : base_deps + [libglnx_dep, libflatpak_dep],
  install : false,
)

if gir_dep.found()
  gnome.generate_gir(
    libflatpak,
    export_packages : 'flatpak',
    extra_args : [
      '-DFLATPAK_EXTERN=__attribute__((visibility("default"))) extern',
      '-DFLATPAK_COMPILATION=1',
      '--warn-all',
    ],
    header : 'flatpak.h',
    identifier_prefix : 'Flatpak',
    includes : ['GObject-2.0', 'Gio-2.0'],
    install : true,
    namespace : 'Flatpak',
    nsversion : '1.0',
    sources : [
      enums,
      flatpak_version_macros,
      public_headers,
      sources,
    ],
    symbol_prefix : 'flatpak',
  )
endif
