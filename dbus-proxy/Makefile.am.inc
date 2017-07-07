libexec_PROGRAMS += \
	flatpak-dbus-proxy \
	$(NULL)

flatpak_dbus_proxy_SOURCES = \
	dbus-proxy/flatpak-proxy.c	\
	dbus-proxy/flatpak-proxy.h	\
	dbus-proxy/dbus-proxy.c		\
	$(NULL)

flatpak_dbus_proxy_LDADD = $(AM_LDADD) $(BASE_LIBS) libglnx.la
flatpak_dbus_proxy_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) -I$(srcdir)/dbus-proxy
