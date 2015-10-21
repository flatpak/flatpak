libexec_PROGRAMS += \
	xdg-desktop-portal \
	$(NULL)

xdg_desktop_resource_files = $(shell $(GLIB_COMPILE_RESOURCES) --sourcedir=$(srcdir) --generate-dependencies $(srcdir)/desktop-portal/xdg-desktop-portal.gresource.xml)

desktop-portal/xdg-desktop-resources.h: desktop-portal/xdg-desktop-portal.gresource.xml
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(srcdir) --c-name _xdg_desktop --generate-header

desktop-portal/xdg-desktop-resources.c: desktop-portal/xdg-desktop-portal.gresource.xml $(xdg_desktop_resource_files)
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(srcdir) --c-name _xdg_desktop --generate-source

service_in_files += desktop-portal/org.freedesktop.portal.Desktop.service.in
dbus_service_DATA += desktop-portal/org.freedesktop.portal.Desktop.service
DISTCLEANFILES += desktop-portal/org.freedesktop.portal.Desktop.service

EXTRA_DIST += desktop-portal/xdg-desktop-portal.gresource.xml

xdg_desktop_portal_SOURCES = \
	desktop-portal/xdg-desktop-portal.c	\
	desktop-portal/xdg-desktop-resources.c	\
	$(NULL)

xdg_desktop_portal_LDADD = $(BASE_LIBS) libflatpak-common.la
xdg_desktop_portal_CFLAGS = \
	-DPKGDATADIR=\"$(pkgdatadir)\"	\
	$(BASE_CFLAGS)	$(OSTREE_CFLAGS) $(SOUP_CFLAGS) \
	-I$(srcdir)/desktop-portal -I$(builddir)/desktop-portal \
	$(NULL)
