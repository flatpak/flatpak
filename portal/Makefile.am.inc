EXTRA_DIST += portal/meson.build

libexec_PROGRAMS += \
	flatpak-portal \
	$(NULL)

service_in_files += portal/flatpak-portal.service.in
systemduserunit_DATA += portal/flatpak-portal.service

service_in_files += portal/org.freedesktop.portal.Flatpak.service.in
dbus_service_DATA += portal/org.freedesktop.portal.Flatpak.service

nodist_flatpak_portal_SOURCES = \
	portal/flatpak-permission-dbus.c \
	portal/flatpak-permission-dbus.h \
	portal/flatpak-portal-dbus.c \
	portal/flatpak-portal-dbus.h \
	$(NULL)

portal/flatpak-permission-dbus.c: data/org.freedesktop.impl.portal.PermissionStore.xml Makefile
	mkdir -p $(builddir)/portal
	$(AM_V_GEN) $(GDBUS_CODEGEN)                            \
		--interface-prefix org.freedesktop.impl.portal  \
		--c-namespace XdpDbus                           \
		--generate-c-code $(builddir)/portal/flatpak-permission-dbus  \
		$(srcdir)/data/org.freedesktop.impl.portal.PermissionStore.xml  \
		$(NULL)


portal/flatpak-portal-dbus.c: data/org.freedesktop.portal.Flatpak.xml Makefile
	mkdir -p $(builddir)/portal
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.portal	\
		--c-namespace Portal				\
		--generate-c-code $(builddir)/portal/flatpak-portal-dbus	\
		$(srcdir)/data/org.freedesktop.portal.Flatpak.xml  \
		$(NULL)

portal/%-dbus.h: portal/%-dbus.c
	@true # Built as a side-effect of the rules for the .c

flatpak_portal_SOURCES = \
	portal/flatpak-portal.c		\
	portal/flatpak-portal.h		\
	portal/flatpak-portal-app-info.c		\
	portal/flatpak-portal-app-info.h		\
	portal/portal-impl.c            \
	portal/portal-impl.h            \
	common/flatpak-portal-error.c	\
	common/flatpak-portal-error.h	\
	$(NULL)

BUILT_SOURCES += $(nodist_flatpak_portal_SOURCES)
CLEANFILES += $(nodist_flatpak_portal_SOURCES)

flatpak_portal_LDADD = $(AM_LDADD) $(BASE_LIBS) $(OSTREE_LIBS) libflatpak-common.la libflatpak-common-base.la libglnx.la
flatpak_portal_CFLAGS = \
        $(AM_CFLAGS) \
        $(BASE_CFLAGS) \
        $(OSTREE_CFLAGS) \
        $(SOUP_CFLAGS) \
        $(JSON_CFLAGS) \
        -DFLATPAK_COMPILATION \
        -DDATADIR=\"$(datadir)\" \
        $(NULL)
flatpak_portal_CPPFLAGS = $(AM_CPPFLAGS) -I$(builddir)/portal
