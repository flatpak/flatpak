noinst_LTLIBRARIES += libflatpak-common.la

dbus_built_sources = common/flatpak-dbus.c common/flatpak-dbus.h
systemd_dbus_built_sources = common/flatpak-systemd-dbus.c common/flatpak-systemd-dbus.h

$(dbus_built_sources) : data/org.freedesktop.Flatpak.xml
	mkdir -p $(builddir)/common
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.Flatpak.	\
		--c-namespace Flatpak				\
		--generate-c-code $(builddir)/common/flatpak-dbus	\
		$(srcdir)/data/org.freedesktop.Flatpak.xml	\
		$(NULL)

$(systemd_dbus_built_sources) : data/org.freedesktop.systemd1.xml
	mkdir -p $(builddir)/common
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.systemd1.	\
		--c-namespace Systemd				\
		--generate-c-code $(builddir)/common/flatpak-systemd-dbus	\
		$(srcdir)/data/org.freedesktop.systemd1.xml	\
		$(NULL)

nodist_libflatpak_common_la_SOURCES = \
	$(dbus_built_sources)		\
	$(systemd_dbus_built_sources)	\
	$(NULL)

BUILT_SOURCES += $(nodist_libflatpak_common_la_SOURCES)
CLEANFILES += $(nodist_libflatpak_common_la_SOURCES)

libflatpak_common_la_SOURCES = \
	common/flatpak-common-types.h \
	common/flatpak-dir.c \
	common/flatpak-dir.h \
	common/flatpak-run.c \
	common/flatpak-run.h \
	common/flatpak-portal-error.c \
	common/flatpak-portal-error.h \
	common/flatpak-utils.c \
	common/flatpak-utils.h \
	common/flatpak-chain-input-stream.c \
	common/flatpak-chain-input-stream.h \
	common/gvdb/gvdb-reader.h	\
	common/gvdb/gvdb-format.h	\
	common/gvdb/gvdb-reader.c	\
	common/gvdb/gvdb-builder.h	\
	common/gvdb/gvdb-builder.c	\
	common/flatpak-db.c \
	common/flatpak-db.h \
	$(NULL)

libflatpak_common_la_CFLAGS = \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	$(HIDDEN_VISIBILITY_CFLAGS) \
	$(OSTREE_CFLAGS) \
	$(SOUP_CFLAGS) \
	$(XAUTH_CFLAGS) \
	$(LIBSECCOMP_CFLAGS) \
	-I$(srcdir)/dbus-proxy \
	$(NULL)
libflatpak_common_la_LIBADD = libglnx.la $(BASE_LIBS) $(OSTREE_LIBS) $(SOUP_LIBS) $(XAUTH_LIBS) $(LIBSECCOMP_LIBS)
