libexec_PROGRAMS += \
	xdg-permission-store \
	$(NULL)

service_in_files += permission-store/xdg-permission-store.service.in
systemduserunit_DATA += permission-store/xdg-permission-store.service

service_in_files += permission-store/org.freedesktop.impl.portal.PermissionStore.service.in
dbus_service_DATA += permission-store/org.freedesktop.impl.portal.PermissionStore.service

nodist_xdg_permission_store_SOURCES = permission-store/permission-store-dbus.c permission-store/permission-store-dbus.h
BUILT_SOURCES += $(nodist_xdg_permission_store_SOURCES)
CLEANFILES += $(nodist_xdg_permission_store_SOURCES)

permission-store/permission-store-dbus.c: data/org.freedesktop.impl.portal.PermissionStore.xml Makefile
	mkdir -p $(builddir)/permission-store
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.impl.portal.	\
		--c-namespace Xdg				\
		--generate-c-code $(builddir)/permission-store/permission-store-dbus	\
		$(srcdir)/data/org.freedesktop.impl.portal.PermissionStore.xml	\
		$(NULL)

permission-store/%-dbus.h: permission-store/%-dbus.c
	@true # Built as a side-effect of the rules for the .c

# also used by the document portal
ps_dbus_built_sources = $(nodist_xdg_permission_store_SOURCES)

xdg_permission_store_SOURCES = \
	permission-store/permission-store.c	\
	permission-store/xdg-permission-store.c	\
	permission-store/xdg-permission-store.h	\
	$(NULL)

xdg_permission_store_LDADD = $(AM_LDADD) $(BASE_LIBS) libflatpak-common.la
xdg_permission_store_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) $(SOUP_CFLAGS) $(JSON_CFLAGS) $(OSTREE_CFLAGS) $(GSYSTEM_CFLAGS) -I$(srcdir)/permission-store -I$(builddir)/permission-store
