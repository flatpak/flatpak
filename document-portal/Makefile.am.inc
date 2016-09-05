libexec_PROGRAMS += \
	xdg-document-portal \
	$(NULL)

xdp_dbus_built_sources = document-portal/xdp-dbus.c document-portal/xdp-dbus.h
BUILT_SOURCES += $(xdp_dbus_built_sources)

$(xdp_dbus_built_sources) : data/org.freedesktop.portal.Documents.xml
	mkdir -p $(builddir)/document-portal
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.portal.	\
		--c-namespace XdpDbus				\
		--generate-c-code $(builddir)/document-portal/xdp-dbus	\
		$(srcdir)/data/org.freedesktop.portal.Documents.xml	\
		$(NULL)

service_in_files += document-portal/xdg-document-portal.service.in
systemduserunit_DATA += document-portal/xdg-document-portal.service

service_in_files += document-portal/org.freedesktop.portal.Documents.service.in
dbus_service_DATA += document-portal/org.freedesktop.portal.Documents.service

nodist_xdg_document_portal_SOURCES = \
	$(xdp_dbus_built_sources)		\
	$(ps_dbus_built_sources)		\
	$(NULL)

xdg_document_portal_SOURCES = \
	document-portal/xdp-main.c		\
	document-portal/xdp-enums.h		\
	document-portal/xdp-util.h		\
	document-portal/xdp-util.c		\
	document-portal/xdp-fuse.h		\
	document-portal/xdp-fuse.c		\
	$(NULL)

xdg_document_portal_LDADD = $(BASE_LIBS) $(FUSE_LIBS) libflatpak-common.la
xdg_document_portal_CFLAGS = $(BASE_CFLAGS) $(OSTREE_CFLAGS) $(SOUP_CFLAGS) $(FUSE_CFLAGS) -I$(srcdir)/document-portal -I$(builddir)/document-portal -I$(srcdir)/permission-store -I$(builddir)/permission-store -DFLATPAK_COMPILATION
