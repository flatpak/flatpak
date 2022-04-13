# Copyright 2022 Collabora Ltd.
# SPDX-License-Identifier: LGPL-2.1-or-later

if build_gtk_doc
  subdir('reference')
endif

xsltproc_flags = [
  '--nonet',
  '--stringparam', 'man.output.quietly', '1',
  '--stringparam', 'funcsynopsis.style', 'ansi',
  '--stringparam', 'man.th.extra1.suppress', '1',
  '--stringparam', 'man.authors.section.enabled', '0',
  '--stringparam', 'man.copyright.section.enabled', '0',
]

man1 = [
  'flatpak',
  'flatpak-remotes',
  'flatpak-remote-add',
  'flatpak-remote-delete',
  'flatpak-remote-modify',
  'flatpak-remote-ls',
  'flatpak-remote-info',
  'flatpak-install',
  'flatpak-config',
  'flatpak-update',
  'flatpak-uninstall',
  'flatpak-mask',
  'flatpak-pin',
  'flatpak-list',
  'flatpak-info',
  'flatpak-make-current',
  'flatpak-run',
  'flatpak-override',
  'flatpak-enter',
  'flatpak-ps',
  'flatpak-document-export',
  'flatpak-document-unexport',
  'flatpak-document-info',
  'flatpak-documents',
  'flatpak-permission-remove',
  'flatpak-permissions',
  'flatpak-permission-show',
  'flatpak-permission-reset',
  'flatpak-permission-set',
  'flatpak-build-init',
  'flatpak-build',
  'flatpak-build-bundle',
  'flatpak-build-import-bundle',
  'flatpak-build-finish',
  'flatpak-build-export',
  'flatpak-build-update-repo',
  'flatpak-build-sign',
  'flatpak-build-commit-from',
  'flatpak-repo',
  'flatpak-search',
  'flatpak-create-usb',
  'flatpak-repair',
  'flatpak-kill',
  'flatpak-history',
  'flatpak-spawn',
]

man5 = [
  'flatpak-metadata',
  'flatpak-flatpakrepo',
  'flatpak-flatpakref',
  'flatpak-remote',
  'flatpak-installation',
]

xml_files = []

foreach pair : [[man1, '1'], [man5, '5']]
  pages = pair[0]
  section = pair[1]

  foreach man : pages
    xml_files += [man + '.xml']

    if build_man_pages
      custom_target(
        man + '.' + section,
        input : [man + '.xml'],
        output : [man + '.' + section],
        command : [
          xsltproc,
          '-o', '@OUTPUT@',
        ] + xsltproc_flags + [
          manpages_xsl,
          '@INPUT@',
        ],
        build_by_default : true,
        install : true,
        install_dir : get_option('mandir') / ('man' + section),
      )
    endif
  endforeach
endforeach

if xmlto.found()
  cdata = configuration_data()
  cdata.set('VERSION', meson.project_version())
  cdata.set('srcdir', meson.current_source_dir())
  flatpak_docs_xml = configure_file(
    input : 'flatpak-docs.xml.in',
    output : 'flatpak-docs.xml',
    configuration : cdata,
  )
  custom_target(
    'flatpak-docs.html',
    input : [
      flatpak_docs_xml,
      'xmlto-config.xsl',
    ],
    output : ['flatpak-docs.html'],
    depend_files : xml_files,
    command : [
      xmlto,
      '-o', meson.current_build_dir(),
    ] + get_option('xmlto_flags') + [
      '--skip-validation',
      'xhtml-nochunks',
      '-m', '@INPUT1@',
      '@INPUT0@',
    ],
    build_by_default : true,
    install : true,
    install_dir : docdir,
  )
  install_data('docbook.css', install_dir : docdir)
endif
