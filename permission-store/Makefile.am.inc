libexec_PROGRAMS += \
	xdg-permission-store \
	$(NULL)

service_in_files += permission-store/xdg-permission-store.service.in
systemduserunit_DATA += permission-store/xdg-permission-store.service

service_in_files += permission-store/org.freedesktop.impl.portal.PermissionStore.service.in
dbus_service_DATA += permission-store/org.freedesktop.impl.portal.PermissionStore.service

ps_dbus_built_sources = permission-store/permission-store-dbus.c permission-store/permission-store-dbus.h
BUILT_SOURCES += $(ps_dbus_built_sources)

$(ps_dbus_built_sources) : data/org.freedesktop.impl.portal.PermissionStore.xml
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.impl.portal.	\
		--c-namespace Xdg				\
		--generate-c-code $(builddir)/permission-store/permission-store-dbus	\
		$(srcdir)/data/org.freedesktop.impl.portal.PermissionStore.xml	\
		$(NULL)

xdg_permission_store_SOURCES = \
	$(ps_dbus_built_sources)		\
	permission-store/permission-store.c	\
	permission-store/xdg-permission-store.c	\
	permission-store/xdg-permission-store.h	\
	$(NULL)

xdg_permission_store_LDADD = $(BASE_LIBS) libflatpak-common.la
xdg_permission_store_CFLAGS = $(BASE_CFLAGS) $(SOUP_CFLAGS) $(OSTREE_CFLAGS) $(GSYSTEM_CFLAGS) -I$(srcdir)/permission-store -I$(builddir)/permission-store
