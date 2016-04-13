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
	xdg-app.1			\
	xdg-app-remote-add.1		\
	xdg-app-remote-delete.1		\
	xdg-app-remote-list.1		\
	xdg-app-remote-modify.1		\
	xdg-app-remote-ls.1		\
	xdg-app-install.1		\
	xdg-app-update.1		\
	xdg-app-uninstall.1		\
	xdg-app-list.1			\
	xdg-app-info.1			\
	xdg-app-make-current.1		\
	xdg-app-run.1			\
	xdg-app-override.1		\
	xdg-app-enter.1			\
	xdg-app-export-file.1		\
	xdg-app-build-init.1		\
	xdg-app-build.1			\
	xdg-app-build-bundle.1		\
	xdg-app-build-finish.1		\
	xdg-app-build-export.1		\
	xdg-app-build-update-repo.1	\
	xdg-app-build-sign.1		\
	xdg-app-builder.1		\
	$(NULL)

xml_files = $(man_MANS:.1=.xml)

EXTRA_DIST = $(xml_files)

DISTCLEANFILES = $(man_MANS)
