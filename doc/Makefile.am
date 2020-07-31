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

.xml.5:
	$(AM_V_GEN) $(XSLTPROC) $(XSLTPROC_FLAGS) http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl $<

man1 = \
	flatpak.1			\
	flatpak-remotes.1		\
	flatpak-remote-add.1		\
	flatpak-remote-delete.1		\
	flatpak-remote-modify.1		\
	flatpak-remote-ls.1		\
	flatpak-remote-info.1		\
	flatpak-install.1		\
	flatpak-config.1		\
	flatpak-update.1		\
	flatpak-uninstall.1		\
	flatpak-mask.1			\
	flatpak-pin.1			\
	flatpak-list.1			\
	flatpak-info.1			\
	flatpak-make-current.1		\
	flatpak-run.1			\
	flatpak-override.1		\
	flatpak-enter.1			\
	flatpak-ps.1			\
	flatpak-document-export.1	\
	flatpak-document-unexport.1	\
	flatpak-document-info.1		\
	flatpak-documents.1		\
	flatpak-permission-remove.1	\
	flatpak-permissions.1		\
	flatpak-permission-show.1	\
	flatpak-permission-reset.1	\
	flatpak-permission-set.1	\
	flatpak-build-init.1		\
	flatpak-build.1			\
	flatpak-build-bundle.1		\
	flatpak-build-import-bundle.1	\
	flatpak-build-finish.1		\
	flatpak-build-export.1		\
	flatpak-build-update-repo.1	\
	flatpak-build-sign.1		\
	flatpak-build-commit-from.1	\
	flatpak-repo.1			\
	flatpak-search.1		\
	flatpak-create-usb.1 		\
	flatpak-repair.1 		\
	flatpak-kill.1 			\
	flatpak-history.1 		\
	flatpak-spawn.1 		\
	$(NULL)

man5 = \
	flatpak-metadata.5		\
	flatpak-flatpakrepo.5		\
	flatpak-flatpakref.5		\
	flatpak-remote.5		\
	flatpak-installation.5		\
	$(NULL)

man_MANS = \
	$(man1) \
	$(man5) \
	$(NULL)

xml_files = \
	$(man1:.1=.xml) \
	$(man5:.5=.xml) \
	$(NULL)

EXTRA_DIST = \
	$(xml_files) 		\
	docbook.css		\
	flatpak-docs.xml.in	\
	xmlto-config.xsl	\
	$(NULL)

CLEANFILES = $(NULL)
DISTCLEANFILES = \
	$(man_MANS)		\
	flatpak-docs.xml	\
	$(NULL)

if DOCBOOK_DOCS_ENABLED

doc_DATA =                      \
        flatpak-docs.html	\
        docbook.css             \
        $(NULL)

CLEANFILES += flatpak-docs.html

flatpak-docs.html: flatpak-docs.xml $(xml_files) xmlto-config.xsl
	$(AM_V_GEN) $(XMLTO) $(XMLTO_FLAGS) --skip-validation xhtml-nochunks -m $(srcdir)/xmlto-config.xsl $<

endif # DOCBOOK_DOCS_ENABLED
