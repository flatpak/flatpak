NULL =

XSLTPROC_FLAGS = \
	--nonet 					\
	--stringparam man.output.quietly 1 		\
	--stringparam funcsynopsis.style ansi 		\
	--stringparam man.th.extra1.suppress 1 		\
	--stringparam man.authors.section.enabled 0 	\
	--stringparam man.copyright.section.enabled 0 	\
	$(NULL)

.xml.1:
	$(AM_V_GEN) $(XSLTPROC) $(XSLTPROC_FLAGS) http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl $<

man_MANS = \
	xdg-app.1			\
	xdg-app-add-repo.1 		\
	xdg-app-delete-repo.1 		\
	xdg-app-list-repos.1 		\
	xdg-app-repo-contents.1		\
	xdg-app-install-runtime.1 	\
	xdg-app-update-runtime.1 	\
	xdg-app-list-runtimes.1 	\
	xdg-app-install-app.1	 	\
	xdg-app-update-app.1	 	\
	xdg-app-list-apps.1	 	\
	xdg-app-run.1		 	\
	xdg-app-build-init.1	 	\
	xdg-app-build.1		 	\
	xdg-app-build-finish.1	 	\
	xdg-app-build-export.1	 	\
	$(NULL)

xml_files = $(man_MANS:.1=.xml)

EXTRA_DIST = $(xml_files)

DISTCLEANFILES = $(man_MANS)
