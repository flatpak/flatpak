libexec_PROGRAMS += \
	xdg-app-session-helper \
	$(NULL)

service_in_files += session-helper/xdg-app-session-helper.service.in
systemduserunit_DATA += session-helper/xdg-app-session-helper.service

service_in_files += session-helper/org.freedesktop.XdgApp.service.in
dbus_service_DATA += session-helper/org.freedesktop.XdgApp.service

xdg_app_session_helper_SOURCES = \
	session-helper/xdg-app-session-helper.c	\
	session-helper/xdg-app-permission-store.c	\
	session-helper/xdg-app-permission-store.h	\
	$(NULL)

xdg_app_session_helper_LDADD = $(BASE_LIBS) libxdgapp-common.la
xdg_app_session_helper_CFLAGS = $(BASE_CFLAGS)
