libexec_PROGRAMS += \
	flatpak-dbus-proxy \
	$(NULL)

flatpak_dbus_proxy_SOURCES = \
	dbus-proxy/flatpak-proxy.c	\
	dbus-proxy/flatpak-proxy.h	\
	dbus-proxy/dbus-proxy.c		\
	$(NULL)

flatpak_dbus_proxy_LDADD = \
	libglnx.la \
	libflatpak-common.la \
	$(AM_LDADD) \
	$(BASE_LIBS) \
	$(NULL)
flatpak_dbus_proxy_CFLAGS = \
	-I$(srcdir)/dbus-proxy \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	$(OSTREE_CFLAGS) \
	$(SOUP_CFLAGS) \
	$(JSON_CFLAGS) \
	$(NULL)
