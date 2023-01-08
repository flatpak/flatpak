EXTRA_DIST += session-helper/meson.build

libexec_PROGRAMS += \
	flatpak-session-helper \
	$(NULL)

service_in_files += session-helper/flatpak-session-helper.service.in
systemduserunit_DATA += session-helper/flatpak-session-helper.service

service_in_files += session-helper/org.freedesktop.Flatpak.service.in
dbus_service_DATA += session-helper/org.freedesktop.Flatpak.service

flatpak_session_helper_SOURCES = \
	session-helper/flatpak-session-helper.c	\
	session-helper/flatpak-session-helper.h	\
	$(NULL)

flatpak_session_helper_LDADD = $(AM_LDADD) $(BASE_LIBS) libflatpak-common-base.la libglnx.la
flatpak_session_helper_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) -DFLATPAK_COMPILATION
