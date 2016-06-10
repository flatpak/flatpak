NULL =

SUBDIRS = reference

XSLTPROC_FLAGS = \
	--nonet						\
	--stringparam man.output.quietly 1		\
	--stringparam funcsynopsis.style ansi		\
	--stringparam man.th.extra1.suppress 1		\
	--stringparam man.authors.section.enabled 0	\
	--stringparam man.copyright.section.enabled 0	\
	$(NULL)

.xml.1:
	$(AM_V_GEN) $(XSLTPROC) $(XSLTPROC_FLAGS) http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl $<

man_MANS = \
	flatpak.1			\
	flatpak-remote-add.1		\
	flatpak-remote-delete.1		\
	flatpak-remote-list.1		\
	flatpak-remote-modify.1		\
	flatpak-remote-ls.1		\
	flatpak-install.1		\
	flatpak-update.1		\
	flatpak-uninstall.1		\
	flatpak-list.1			\
	flatpak-info.1			\
	flatpak-make-current.1		\
	flatpak-run.1			\
	flatpak-override.1		\
	flatpak-enter.1			\
	flatpak-document-export.1	\
	flatpak-document-unexport.1	\
	flatpak-document-info.1		\
	flatpak-document-list.1		\
	flatpak-build-init.1		\
	flatpak-build.1			\
	flatpak-build-bundle.1		\
	flatpak-build-import-bundle.1	\
	flatpak-build-finish.1		\
	flatpak-build-export.1		\
	flatpak-build-update-repo.1	\
	flatpak-build-sign.1		\
	flatpak-builder.1		\
	$(NULL)

xml_files = $(man_MANS:.1=.xml)

EXTRA_DIST = $(xml_files)

DISTCLEANFILES = $(man_MANS)
