lib_LTLIBRARIES += libflatpak.la
noinst_PROGRAMS += test-libflatpak

public_headers = \
	lib/flatpak.h \
	lib/flatpak-ref.h \
	lib/flatpak-error.h \
	lib/flatpak-installed-ref.h \
	lib/flatpak-remote-ref.h \
	lib/flatpak-bundle-ref.h \
	lib/flatpak-installation.h \
	lib/flatpak-remote.h \
	lib/flatpak-version-macros.h \
	$(NULL)

generated_public_headers = \
	lib/flatpak-enum-types.h \
	$(NULL)

BUILT_SOURCES += \
	lib/flatpak-enum-types.c \
	lib/flatpak-enum-types.h \
	$(NULL)

DISTCLEANFILES += \
	lib/flatpak-enum-types.c \
	lib/flatpak-enum-types.h \
	$(NULL)


lib/flatpak-enum-types.h: $(public_headers) lib/flatpak-enum-types.h.template
	$(AM_V_GEN) $(GLIB_MKENUMS) --template $(filter %.template,$^) $(filter-out %.template,$^) > \
	    lib/flatpak-enum-types.h.tmp && mv lib/flatpak-enum-types.h.tmp lib/flatpak-enum-types.h

lib/flatpak-enum-types.c: $(public_headers) lib/flatpak-enum-types.c.template
	$(AM_V_GEN) $(GLIB_MKENUMS) --template $(filter %.template,$^) $(filter-out %.template,$^) > \
	    lib/flatpak-enum-types.c.tmp && mv lib/flatpak-enum-types.c.tmp lib/flatpak-enum-types.c

EXTRA_DIST += lib/flatpak-enum-types.c.template lib/flatpak-enum-types.h.template

flatpakincludedir = $(includedir)/flatpak
flatpakinclude_HEADERS = $(public_headers) $(generated_public_headers)

sources = \
	lib/flatpak.c \
	lib/flatpak-enum-types.c \
	lib/flatpak-ref.c \
	lib/flatpak-installed-ref.c \
	lib/flatpak-installed-ref-private.h \
	lib/flatpak-remote-ref.c \
	lib/flatpak-bundle-ref.c \
	lib/flatpak-remote-ref-private.h \
	lib/flatpak-remote-private.h \
	lib/flatpak-remote.c \
	lib/flatpak-error.c \
	lib/flatpak-installation.c \
	$(NULL)

libflatpak_la_SOURCES = \
	$(public_headers) \
	$(generated_public_headers) \
	$(sources) \
	$(NULL)

libflatpak_la_CFLAGS = \
	$(HIDDEN_VISIBILITY_CFLAGS) \
	-DFLATPAK_COMPILATION \
	-I$(top_srcdir)/lib \
	-I$(top_builddir)/lib \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	$(OSTREE_CFLAGS) \
	$(SOUP_CFLAGS) \
	$(NULL)

libflatpak_la_LDFLAGS = \
	-version-info $(LT_VERSION_INFO)	\
	-export-dynamic				\
	-rpath $(libdir)			\
	$(NULL)

libflatpak_la_LIBADD = \
        libflatpak-common.la \
	$(BASE_LIBS)	\
	$(OSTREE_LIBS)	\
	$(SOUP_LIBS)	\
	$(NULL)

test_libflatpak_SOURCES = \
        lib/test-lib.c      \
	$(NULL)

test_libflatpak_CFLAGS = \
	$(BASE_CFLAGS) \
	-I$(top_srcdir)/lib \
	-I$(top_builddir)/lib \
	$(NULL)

test_libflatpak_LDADD = \
	$(BASE_LIBS)	\
        libflatpak.la \
	$(NULL)

# gobject-introspection rules
-include $(INTROSPECTION_MAKEFILE)

INTROSPECTION_GIRS =

if HAVE_INTROSPECTION
Flatpak-1.0.gir: libflatpak.la Makefile

introspected_headers = $(public_headers)
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
