system_helper_resource_files = $(shell $(GLIB_COMPILE_RESOURCES) --sourcedir=$(srcdir) --generate-dependencies $(srcdir)/system-helper/xdg-app.gresource.xml)

libexec_PROGRAMS += \
	xdg-app-system-helper \
	$(NULL)

system-helper/xdg-app-resources.h: system-helper/xdg-app.gresource.xml
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(srcdir) --c-name _gtk --generate-header

system-helper/xdg-app-resources.c: system-helper/xdg-app.gresource.xml $(system_helper_resource_files)
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(srcdir) --c-name _gtk --generate-source

dbussystemservicedir = $(datadir)/dbus-1/system-services
service_in_files += system-helper/org.freedesktop.XdgApp.SystemHelper.service.in
dbussystemservice_DATA = system-helper/org.freedesktop.XdgApp.SystemHelper.service

dbusconfdir = ${sysconfdir}/dbus-1/system.d
dbusconf_DATA = system-helper/org.freedesktop.XdgApp.SystemHelper.conf

service_in_files += system-helper/xdg-app-system-helper.service.in
systemdsystemunit_DATA = system-helper/xdg-app-system-helper.service

EXTRA_DIST += system-helper/xdg-app.gresource.xml

xdg_app_system_helper_SOURCES = \
	system-helper/xdg-app-system-helper.c	\
	system-helper/xdg-app-resources.h	\
	system-helper/xdg-app-resources.c	\
	$(NULL)

xdg_app_system_helper_LDADD = $(BASE_LIBS) libxdgapp-common.la
xdg_app_system_helper_CFLAGS = $(BASE_CFLAGS)
