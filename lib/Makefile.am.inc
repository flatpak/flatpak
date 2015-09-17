noinst_LTLIBRARIES += libxdgapp.la

dbus_built_sources = lib/xdg-app-dbus.c lib/xdg-app-dbus.h
systemd_dbus_built_sources = lib/xdg-app-systemd-dbus.c lib/xdg-app-systemd-dbus.h
BUILT_SOURCES += $(dbus_built_sources) $(systemd_dbus_built_sources)

$(dbus_built_sources) : data/org.freedesktop.XdgApp.xml
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.XdgApp.	\
		--c-namespace XdgApp				\
		--generate-c-code $(builddir)/lib/xdg-app-dbus	\
		$(srcdir)/data/org.freedesktop.XdgApp.xml	\
		$(NULL)

$(systemd_dbus_built_sources) : data/org.freedesktop.systemd1.xml
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.systemd1.	\
		--c-namespace Systemd				\
		--generate-c-code $(builddir)/lib/xdg-app-systemd-dbus	\
		$(srcdir)/data/org.freedesktop.systemd1.xml	\
		$(NULL)

libxdgapp_la_SOURCES = \
	lib/xdg-app-dir.c \
	lib/xdg-app-dir.h \
	lib/xdg-app-run.c \
	lib/xdg-app-run.h \
	lib/xdg-app-error.c \
	lib/xdg-app-error.h \
	lib/xdg-app-utils.c \
	lib/xdg-app-utils.h \
	lib/xdg-app-chain-input-stream.c \
	lib/xdg-app-chain-input-stream.h \
	lib/gvdb/gvdb-reader.c	\
	lib/gvdb/gvdb-builder.c	\
	lib/xdg-app-db.c \
	lib/xdg-app-db.h \
	$(dbus_built_sources)		\
	$(systemd_dbus_built_sources)	\
	$(NULL)

libxdgapp_la_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) $(OSTREE_CFLAGS) $(SOUP_CFLAGS) -I$(srcdir)/dbus-proxy
libxdgapp_la_LIBADD = libglnx.la $(BASE_LIBS) $(OSTREE_LIBS) $(SOUP_LIBS)

bin_PROGRAMS += \
	xdg-app-helper \
	$(NULL)

xdg_app_helper_SOURCES = lib/xdg-app-helper.c
xdg_app_helper_LDADD = $(LIBSECCOMP_LIBS)
xdg_app_helper_CFLAGS = $(LIBSECCOMP_CFLAGS)

install-exec-hook:
if DISABLE_USERNS
if PRIV_MODE_SETUID
	$(SUDO_BIN) chown root $(DESTDIR)$(bindir)/xdg-app-helper
	$(SUDO_BIN) chmod u+s $(DESTDIR)$(bindir)/xdg-app-helper
else
if PRIV_MODE_FILECAPS
	$(SUDO_BIN) setcap cap_sys_admin+ep $(DESTDIR)$(bindir)/xdg-app-helper
endif
endif
endif
