lib_LTLIBRARIES += libflatpak.la
noinst_LTLIBRARIES += libflatpak-common.la
noinst_PROGRAMS += test-libflatpak

flatpakincludedir = $(includedir)/flatpak

flatpakinclude_HEADERS = \
	common/flatpak.h \
	common/flatpak-ref.h \
	common/flatpak-error.h \
	common/flatpak-installed-ref.h \
	common/flatpak-remote-ref.h \
	common/flatpak-related-ref.h \
	common/flatpak-bundle-ref.h \
	common/flatpak-installation.h \
	common/flatpak-remote.h \
	common/flatpak-version-macros.h \
	common/flatpak-portal-error.h \
	common/flatpak-transaction.h \
	common/flatpak-instance.h \
	$(NULL)

nodist_flatpakinclude_HEADERS = \
	common/flatpak-enum-types.h \
	$(NULL)

dbus_built_sources = common/flatpak-dbus-generated.c common/flatpak-dbus-generated.h common/flatpak-document-dbus-generated.c common/flatpak-document-dbus-generated.h
systemd_dbus_built_sources = common/flatpak-systemd-dbus-generated.c common/flatpak-systemd-dbus-generated.h

common/flatpak-enum-types.h: $(flatpakinclude_HEADERS) common/flatpak-enum-types.h.template
	$(AM_V_GEN) $(GLIB_MKENUMS) --template $(filter %.template,$^) $(filter-out %.template,$^) > \
	    common/flatpak-enum-types.h.tmp && mv common/flatpak-enum-types.h.tmp common/flatpak-enum-types.h

common/flatpak-enum-types.c: $(flatpakinclude_HEADERS) common/flatpak-enum-types.c.template
	$(AM_V_GEN) $(GLIB_MKENUMS) --template $(filter %.template,$^) $(filter-out %.template,$^) > \
	    common/flatpak-enum-types.c.tmp && mv common/flatpak-enum-types.c.tmp common/flatpak-enum-types.c

EXTRA_DIST += common/flatpak-enum-types.c.template common/flatpak-enum-types.h.template

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
	$(nodist_flatpakinclude_HEADERS)    \
	$(dbus_built_sources)		\
	$(systemd_dbus_built_sources)	\
	$(xdp_dbus_built_sources) \
	common/flatpak-enum-types.c \
	$(NULL)

BUILT_SOURCES += $(nodist_libflatpak_common_la_SOURCES)
CLEANFILES += $(nodist_libflatpak_common_la_SOURCES)

libflatpak_common_la_SOURCES = \
	$(flatpakinclude_HEADERS) \
	common/flatpak-common-types-private.h \
	common/flatpak-appdata.c \
	common/flatpak-appdata-private.h \
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
	common/flatpak-transaction-private.h \
	common/flatpak-transaction.h \
	common/flatpak-transaction.c \
	common/flatpak-utils.c \
	common/flatpak-utils-http.c \
	common/flatpak-utils-http-private.h \
	common/flatpak-utils-memfd.c \
	common/flatpak-utils-memfd-private.h \
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
	common/flatpak-ref.c \
	common/flatpak-installed-ref.c \
	common/flatpak-installed-ref-private.h \
	common/flatpak-instance-private.h \
	common/flatpak-remote-ref.c \
	common/flatpak-remote-ref-private.h \
	common/flatpak-bundle-ref.c \
	common/flatpak-related-ref.c \
	common/flatpak-related-ref-private.h \
	common/flatpak-remote-private.h \
	common/flatpak-remote.c \
	common/flatpak-error.c \
	common/flatpak-installation-private.h \
	common/flatpak-installation.c \
	common/flatpak-instance.c \
	common/flatpak-syscalls-private.h \
	common/valgrind-private.h \
	$(NULL)

libflatpak_common_la_CFLAGS = \
	-DFLATPAK_COMPILATION \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	$(HIDDEN_VISIBILITY_CFLAGS) \
	$(OSTREE_CFLAGS) \
	$(SOUP_CFLAGS) \
	$(JSON_CFLAGS) \
	$(XAUTH_CFLAGS) \
	$(LIBSECCOMP_CFLAGS) \
	$(INTERNAL_GPGME_CFLAGS) \
	$(SYSTEMD_CFLAGS) \
	$(DCONF_CFLAGS) \
	-I$(srcdir)/dbus-proxy \
	$(NULL)
libflatpak_common_la_LIBADD = $(AM_LIBADD) libglnx.la $(BASE_LIBS) $(OSTREE_LIBS) $(SOUP_LIBS) $(JSON_LIBS) $(XAUTH_LIBS) $(LIBSECCOMP_LIBS) $(INTERNAL_GPGME_LIBS) $(SYSTEMD_LIBS) $(DCONF_LIBS)


libflatpak_la_SOURCES = \
	common/flatpak.c \
	$(NULL)

libflatpak_la_CFLAGS = \
	$(HIDDEN_VISIBILITY_CFLAGS) \
	-DFLATPAK_COMPILATION \
	-I$(top_srcdir)/common \
	-I$(top_builddir)/common \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	$(OSTREE_CFLAGS) \
	$(SOUP_CFLAGS) \
	$(JSON_CFLAGS) \
	$(NULL)

libflatpak_la_LDFLAGS = \
	$(AM_LDFLAGS) \
	-version-info $(LT_VERSION_INFO)	\
	-export-dynamic				\
	-rpath $(libdir)			\
	$(NULL)

libflatpak_la_LIBADD = \
	$(AM_LIBADD) \
	libflatpak-common.la \
	$(BASE_LIBS)	\
	$(OSTREE_LIBS)	\
	$(SOUP_LIBS)	\
	$(JSON_LIBS)    \
	$(NULL)

test_libflatpak_SOURCES = \
	common/test-lib.c      \
	$(NULL)

test_libflatpak_CFLAGS = \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	-I$(top_srcdir)/common \
	-I$(top_builddir)/common \
	$(NULL)

test_libflatpak_LDADD = \
	$(AM_LDADD) \
	$(BASE_LIBS)	\
	libflatpak.la \
	$(NULL)

# gobject-introspection rules
-include $(INTROSPECTION_MAKEFILE)

sources = $(libflatpak_common_la_SOURCES) $(libflatpak_la_SOURCES)

INTROSPECTION_GIRS =

if HAVE_INTROSPECTION
Flatpak-1.0.gir: libflatpak.la Makefile

introspected_headers = \
	$(flatpakinclude_HEADERS) \
	$(nodist_flatpakinclude_HEADERS) \
	$(NULL)
introspected_sources = $(filter-out %-private.h,$(sources))

Flatpak_1_0_gir_NAMESPACE = Flatpak
Flatpak_1_0_gir_VERSION = 1.0
Flatpak_1_0_gir_LIBS = libflatpak.la
Flatpak_1_0_gir_FILES = $(introspected_headers) $(introspected_sources)
Flatpak_1_0_gir_CFLAGS = $(libflatpak_la_CFLAGS)
Flatpak_1_0_gir_INCLUDES = GObject-2.0 Gio-2.0
Flatpak_1_0_gir_SCANNERFLAGS = \
	--warn-all \
	--c-include='flatpak.h' \
	--pkg-export=flatpak

INTROSPECTION_GIRS += Flatpak-1.0.gir

girdir = $(datadir)/gir-1.0
nodist_gir_DATA = $(INTROSPECTION_GIRS)
CLEANFILES += $(nodist_gir_DATA)

typelibdir = $(libdir)/girepository-1.0
nodist_typelib_DATA = $(INTROSPECTION_GIRS:.gir=.typelib)
CLEANFILES += $(nodist_typelib_DATA)

endif # HAVE_INTROSPECTION
