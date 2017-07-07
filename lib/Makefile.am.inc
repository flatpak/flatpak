lib_LTLIBRARIES += libflatpak.la
noinst_PROGRAMS += test-libflatpak

flatpakincludedir = $(includedir)/flatpak

flatpakinclude_HEADERS = \
	lib/flatpak.h \
	lib/flatpak-ref.h \
	lib/flatpak-error.h \
	lib/flatpak-installed-ref.h \
	lib/flatpak-remote-ref.h \
	lib/flatpak-related-ref.h \
	lib/flatpak-bundle-ref.h \
	lib/flatpak-installation.h \
	lib/flatpak-remote.h \
	lib/flatpak-version-macros.h \
	$(NULL)

nodist_flatpakinclude_HEADERS = \
	lib/flatpak-enum-types.h \
	$(NULL)

BUILT_SOURCES += \
	$(nodist_flatpakinclude_HEADERS) \
	$(nodist_libflatpak_la_SOURCES) \
	$(NULL)

DISTCLEANFILES += \
	$(nodist_flatpakinclude_HEADERS) \
	$(nodist_libflatpak_la_SOURCES) \
	$(NULL)


lib/flatpak-enum-types.h: $(flatpakinclude_HEADERS) lib/flatpak-enum-types.h.template
	$(AM_V_GEN) $(GLIB_MKENUMS) --template $(filter %.template,$^) $(filter-out %.template,$^) > \
	    lib/flatpak-enum-types.h.tmp && mv lib/flatpak-enum-types.h.tmp lib/flatpak-enum-types.h

lib/flatpak-enum-types.c: $(flatpakinclude_HEADERS) lib/flatpak-enum-types.c.template
	$(AM_V_GEN) $(GLIB_MKENUMS) --template $(filter %.template,$^) $(filter-out %.template,$^) > \
	    lib/flatpak-enum-types.c.tmp && mv lib/flatpak-enum-types.c.tmp lib/flatpak-enum-types.c

EXTRA_DIST += lib/flatpak-enum-types.c.template lib/flatpak-enum-types.h.template

libflatpak_la_SOURCES = \
	lib/flatpak.c \
	lib/flatpak-ref.c \
	lib/flatpak-installed-ref.c \
	lib/flatpak-installed-ref-private.h \
	lib/flatpak-remote-ref.c \
	lib/flatpak-remote-ref-private.h \
	lib/flatpak-bundle-ref.c \
	lib/flatpak-related-ref.c \
	lib/flatpak-related-ref-private.h \
	lib/flatpak-remote-private.h \
	lib/flatpak-remote.c \
	lib/flatpak-error.c \
	lib/flatpak-installation.c \
	$(NULL)

nodist_libflatpak_la_SOURCES = \
	lib/flatpak-enum-types.c \
	$(NULL)

sources = \
	$(libflatpak_la_SOURCES) \
	$(nodist_libflatpak_la_SOURCES) \
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
        lib/test-lib.c      \
	$(NULL)

test_libflatpak_CFLAGS = \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	-I$(top_srcdir)/lib \
	-I$(top_builddir)/lib \
	$(NULL)

test_libflatpak_LDADD = \
	$(AM_LDADD) \
	$(BASE_LIBS)	\
        libflatpak.la \
	$(NULL)

# gobject-introspection rules
-include $(INTROSPECTION_MAKEFILE)

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
