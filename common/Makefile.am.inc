noinst_LTLIBRARIES += libflatpak-common.la

dbus_built_sources = common/flatpak-dbus-generated.c common/flatpak-dbus-generated.h common/flatpak-document-dbus-generated.c common/flatpak-document-dbus-generated.h
systemd_dbus_built_sources = common/flatpak-systemd-dbus-generated.c common/flatpak-systemd-dbus-generated.h

common/flatpak-dbus-generated.c: data/org.freedesktop.Flatpak.xml Makefile
	mkdir -p $(builddir)/common
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.Flatpak.	\
		--c-namespace Flatpak				\
		--generate-c-code $(builddir)/common/flatpak-dbus-generated	\
		$(srcdir)/data/org.freedesktop.Flatpak.xml  \
		$(NULL)

common/flatpak-document-dbus-generated.c: data/org.freedesktop.portal.Documents.xml Makefile
	mkdir -p $(builddir)/common
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.portal.	\
		--c-namespace XdpDbus				\
		--generate-c-code $(builddir)/common/flatpak-document-dbus-generated	\
		$(srcdir)/data/org.freedesktop.portal.Documents.xml  \
		$(NULL)

common/flatpak-systemd-dbus-generated.c: data/org.freedesktop.systemd1.xml Makefile
	mkdir -p $(builddir)/common
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.systemd1.	\
		--c-namespace Systemd				\
		--generate-c-code $(builddir)/common/flatpak-systemd-dbus-generated	\
		$(srcdir)/data/org.freedesktop.systemd1.xml	\
		$(NULL)

common/%-dbus-generated.h: common/%-dbus-generated.c
	@true # Built as a side-effect of the rules for the .c

nodist_libflatpak_common_la_SOURCES = \
	$(dbus_built_sources)		\
	$(systemd_dbus_built_sources)	\
	$(xdp_dbus_built_sources) \
	$(NULL)

BUILT_SOURCES += $(nodist_libflatpak_common_la_SOURCES)
CLEANFILES += $(nodist_libflatpak_common_la_SOURCES)

libflatpak_common_la_SOURCES = \
	common/flatpak-common-types-private.h \
	common/flatpak-bwrap.c \
	common/flatpak-bwrap-private.h \
	common/flatpak-dir.c \
	common/flatpak-dir-private.h \
	common/flatpak-run.c \
	common/flatpak-run-private.h \
	common/flatpak-context.c \
	common/flatpak-context-private.h \
	common/flatpak-exports.c \
	common/flatpak-exports-private.h \
	common/flatpak-utils.c \
	common/flatpak-utils-private.h \
	common/flatpak-chain-input-stream.c \
	common/flatpak-chain-input-stream-private.h \
	common/flatpak-portal-error.c \
	common/flatpak-portal-error.h \
	common/flatpak-json.c \
	common/flatpak-json-private.h \
	common/flatpak-json-oci.c \
	common/flatpak-json-oci-private.h \
	common/flatpak-oci-registry.c \
	common/flatpak-oci-registry-private.h \
	$(NULL)

libflatpak_common_la_CFLAGS = \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	$(HIDDEN_VISIBILITY_CFLAGS) \
	$(OSTREE_CFLAGS) \
	$(SOUP_CFLAGS) \
	$(JSON_CFLAGS) \
	$(XAUTH_CFLAGS) \
	$(LIBSECCOMP_CFLAGS) \
	$(INTERNAL_GPGME_CFLAGS) \
	-I$(srcdir)/dbus-proxy \
	$(NULL)
libflatpak_common_la_LIBADD = $(AM_LIBADD) libglnx.la $(BASE_LIBS) $(OSTREE_LIBS) $(SOUP_LIBS) $(JSON_LIBS) $(XAUTH_LIBS) $(LIBSECCOMP_LIBS) $(INTERNAL_GPGME_LIBS)
