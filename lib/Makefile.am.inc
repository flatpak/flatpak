lib_LTLIBRARIES += libxdg-app.la
noinst_PROGRAMS += test-libxdg-app

public_headers = \
	lib/xdg-app.h \
	lib/xdg-app-ref.h \
	lib/xdg-app-error.h \
	lib/xdg-app-installed-ref.h \
	lib/xdg-app-remote-ref.h \
	lib/xdg-app-bundle-ref.h \
	lib/xdg-app-installation.h \
	lib/xdg-app-remote.h \
	lib/xdg-app-version-macros.h \
	$(NULL)

generated_public_headers = \
	lib/xdg-app-enum-types.h \
	$(NULL)

BUILT_SOURCES += 		\
	lib/xdg-app-enum-types.c \
	lib/xdg-app-enum-types.h \
	$(NULL)

DISTCLEANFILES += 		\
	lib/xdg-app-enum-types.c \
	lib/xdg-app-enum-types.h \
	$(NULL)


lib/xdg-app-enum-types.h: $(public_headers) lib/xdg-app-enum-types.h.template
	$(AM_V_GEN) $(GLIB_MKENUMS) --template $(filter %.template,$^) $(filter-out %.template,$^) > \
	    lib/xdg-app-enum-types.h.tmp && mv lib/xdg-app-enum-types.h.tmp lib/xdg-app-enum-types.h

lib/xdg-app-enum-types.c: $(public_headers) lib/xdg-app-enum-types.c.template
	$(AM_V_GEN) $(GLIB_MKENUMS) --template $(filter %.template,$^) $(filter-out %.template,$^) > \
	    lib/xdg-app-enum-types.c.tmp && mv lib/xdg-app-enum-types.c.tmp lib/xdg-app-enum-types.c

EXTRA_DIST += lib/xdg-app-enum-types.c.template lib/xdg-app-enum-types.h.template

xdgappincludedir = $(includedir)/xdg-app
xdgappinclude_HEADERS = $(public_headers) $(generated_public_headers)

sources = \
	lib/xdg-app.c \
	lib/xdg-app-enum-types.c \
	lib/xdg-app-ref.c \
	lib/xdg-app-installed-ref.c \
	lib/xdg-app-installed-ref-private.h \
	lib/xdg-app-remote-ref.c \
	lib/xdg-app-bundle-ref.c \
	lib/xdg-app-remote-ref-private.h \
	lib/xdg-app-remote-private.h \
	lib/xdg-app-remote.c \
	lib/xdg-app-error.c \
	lib/xdg-app-installation.c \
	$(NULL)

libxdg_app_la_SOURCES = \
	$(public_headers) \
	$(generated_public_headers) \
	$(sources) \
	$(NULL)

libxdg_app_la_CFLAGS = \
	$(HIDDEN_VISIBILITY_CFLAGS) \
	-DXDG_APP_COMPILATION \
	-I$(top_srcdir)/lib \
	-I$(top_builddir)/lib \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	$(OSTREE_CFLAGS) \
	$(SOUP_CFLAGS) \
	$(NULL)

libxdg_app_la_LDFLAGS = \
	-version-info $(LT_VERSION_INFO)	\
	-export-dynamic				\
	-rpath $(libdir)			\
	$(NULL)

libxdg_app_la_LIBADD = \
        libxdgapp-common.la \
	$(BASE_LIBS)	\
	$(OSTREE_LIBS)	\
	$(SOUP_LIBS)	\
	$(NULL)

test_libxdg_app_SOURCES = \
        lib/test-lib.c      \
	$(NULL)

test_libxdg_app_CFLAGS = \
	$(BASE_CFLAGS) \
	-I$(top_srcdir)/lib \
	-I$(top_builddir)/lib \
	$(NULL)

test_libxdg_app_LDADD = \
	$(BASE_LIBS)	\
        libxdg-app.la \
	$(NULL)

# gobject-introspection rules
-include $(INTROSPECTION_MAKEFILE)

INTROSPECTION_GIRS =

if HAVE_INTROSPECTION
XdgApp-1.0.gir: libxdg-app.la Makefile

introspected_headers = $(public_headers)
introspected_sources = $(filter-out %-private.h,$(sources))

XdgApp_1_0_gir_NAMESPACE = XdgApp
XdgApp_1_0_gir_VERSION = 1.0
XdgApp_1_0_gir_LIBS = libxdg-app.la
XdgApp_1_0_gir_FILES = $(introspected_headers) $(introspected_sources)
XdgApp_1_0_gir_CFLAGS = $(libxdg_app_la_CFLAGS)
XdgApp_1_0_gir_INCLUDES = GObject-2.0 Gio-2.0
XdgApp_1_0_gir_SCANNERFLAGS = \
        --warn-all \
        --c-include='xdg-app.h' \
        --pkg-export=xdg-app

INTROSPECTION_GIRS += XdgApp-1.0.gir

girdir = $(datadir)/gir-1.0
nodist_gir_DATA = $(INTROSPECTION_GIRS)
CLEANFILES += $(nodist_gir_DATA)

typelibdir = $(libdir)/girepository-1.0
nodist_typelib_DATA = $(INTROSPECTION_GIRS:.gir=.typelib)
CLEANFILES += $(nodist_typelib_DATA)

endif # HAVE_INTROSPECTION
