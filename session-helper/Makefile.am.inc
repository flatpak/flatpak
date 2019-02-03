libexec_PROGRAMS += \
	flatpak-session-helper \
	$(NULL)

service_in_files += session-helper/flatpak-session-helper.service.in
systemduserunit_DATA += session-helper/flatpak-session-helper.service

service_in_files += session-helper/org.freedesktop.Flatpak.service.in
dbus_service_DATA += session-helper/org.freedesktop.Flatpak.service

flatpak_session_helper_SOURCES = \
	session-helper/flatpak-session-helper.c	\
	session-helper/background-monitor.c \
	session-helper/background-monitor.h \
	app/flatpak-permission-dbus-generated.c \
	app/flatpak-permission-dbus-generated.h \
	app/gnome-shell-introspect-dbus-generated.c \
	app/gnome-shell-introspect-dbus-generated.h \
	$(NULL)

flatpak_session_helper_LDADD = $(AM_LDADD) $(BASE_LIBS) $(NOTIFY_LIBS) libflatpak-common.la
flatpak_session_helper_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) $(SOUP_CFLAGS) $(OSTREE_CFLAGS) $(GSYSTEM_CFLAGS) $(JSON_CFLAGS) $(NOTIFY_CFLAGS) -DFLATPAK_COMPILATION
