libexec_PROGRAMS += \
	xdg-dbus-proxy \
	$(NULL)

xdg_dbus_proxy_SOURCES = \
	dbus-proxy/xdg-app-proxy.c	\
	dbus-proxy/xdg-app-proxy.h	\
	dbus-proxy/dbus-proxy.c	        \
	$(NULL)

xdg_dbus_proxy_LDADD = $(BASE_LIBS) libglnx.la
xdg_dbus_proxy_CFLAGS = $(BASE_CFLAGS) -I$(srcdir)/dbus-proxy
