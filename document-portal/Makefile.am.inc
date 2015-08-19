libexec_PROGRAMS += \
	xdg-document-portal \
	$(NULL)

xdp_dbus_built_sources = document-portal/xdp-dbus.c document-portal/xdp-dbus.h
BUILT_SOURCES += $(xdp_dbus_built_sources)

$(xdp_dbus_built_sources) : data/org.freedesktop.portal.documents.xml
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.portal.	\
		--c-namespace XdpDbus				\
		--generate-c-code $(builddir)/document-portal/xdp-dbus	\
		$(srcdir)/data/org.freedesktop.portal.documents.xml	\
		$(NULL)

xdp_resource_files = $(shell $(GLIB_COMPILE_RESOURCES) --sourcedir=$(srcdir) --generate-dependencies $(srcdir)/document-portal/xdg-document-portal.gresource.xml)

document-portal/xdp-resources.h: document-portal/xdg-document-portal.gresource.xml
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(srcdir) --c-name _xdg_document --generate-header

document-portal/xdp-resources.c: document-portal/xdg-document-portal.gresource.xml $(xdp_resource_files)
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(srcdir) --c-name _xdg_document --generate-source

service_in_files += document-portal/org.freedesktop.portal.Documents.service.in
service_DATA += document-portal/org.freedesktop.portal.Documents.service
DISTCLEANFILES += document-portal/org.freedesktop.portal.Documents.service

xdg_document_portal_SOURCES = \
	document-portal/xdp-main.c		\
	document-portal/xdp-error.c		\
	document-portal/xdp-error.h		\
	document-portal/xdp-enums.h		\
	document-portal/xdp-doc-db.h		\
	document-portal/xdp-doc-db.c		\
	lib/gvdb/gvdb-reader.c	\
	lib/gvdb/gvdb-builder.c	\
	$(xdp_dbus_built_sources)	\
	document-portal/xdp-resources.h		\
	document-portal/xdp-resources.c		\
	document-portal/xdp-util.h		\
	document-portal/xdp-util.c		\
	document-portal/xdp-fuse.h		\
	document-portal/xdp-fuse.c		\
	$(NULL)

xdg_document_portal_LDADD = $(BASE_LIBS) $(FUSE_LIBS)
xdg_document_portal_CFLAGS = $(BASE_CFLAGS) $(FUSE_CFLAGS) -I$(src)/document-portal
