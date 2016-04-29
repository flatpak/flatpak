noinst_LTLIBRARIES += libxdgapp-common.la

dbus_built_sources = common/xdg-app-dbus.c common/xdg-app-dbus.h
systemd_dbus_built_sources = common/xdg-app-systemd-dbus.c common/xdg-app-systemd-dbus.h
BUILT_SOURCES += $(dbus_built_sources) $(systemd_dbus_built_sources)

$(dbus_built_sources) : data/org.freedesktop.XdgApp.xml
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.XdgApp.	\
		--c-namespace XdgApp				\
		--generate-c-code $(builddir)/common/xdg-app-dbus	\
		$(srcdir)/data/org.freedesktop.XdgApp.xml	\
		$(NULL)

$(systemd_dbus_built_sources) : data/org.freedesktop.systemd1.xml
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.systemd1.	\
		--c-namespace Systemd				\
		--generate-c-code $(builddir)/common/xdg-app-systemd-dbus	\
		$(srcdir)/data/org.freedesktop.systemd1.xml	\
		$(NULL)

libxdgapp_common_la_SOURCES = \
	common/xdg-app-common-types.h \
	common/xdg-app-dir.c \
	common/xdg-app-dir.h \
	common/xdg-app-run.c \
	common/xdg-app-run.h \
	common/xdg-app-portal-error.c \
	common/xdg-app-portal-error.h \
	common/xdg-app-utils.c \
	common/xdg-app-utils.h \
	common/xdg-app-chain-input-stream.c \
	common/xdg-app-chain-input-stream.h \
	common/gvdb/gvdb-reader.h	\
	common/gvdb/gvdb-format.h	\
	common/gvdb/gvdb-reader.c	\
	common/gvdb/gvdb-builder.h	\
	common/gvdb/gvdb-builder.c	\
	common/xdg-app-db.c \
	common/xdg-app-db.h \
	$(dbus_built_sources)		\
	$(systemd_dbus_built_sources)	\
	$(NULL)

libxdgapp_common_la_CFLAGS = \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	$(HIDDEN_VISIBILITY_CFLAGS) \
	$(OSTREE_CFLAGS) \
	$(SOUP_CFLAGS) \
	$(XAUTH_CFLAGS) \
	$(LIBSECCOMP_CFLAGS) \
	-I$(srcdir)/dbus-proxy \
	$(NULL)
libxdgapp_common_la_LIBADD = libglnx.la $(BASE_LIBS) $(OSTREE_LIBS) $(SOUP_LIBS) $(XAUTH_LIBS) $(LIBSECCOMP_LIBS)
