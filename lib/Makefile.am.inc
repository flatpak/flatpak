lib_LTLIBRARIES += libxdg-app.la

noinst_PROGRAMS += test-libxdg-app

public_headers = \
	lib/xdg-app.h \
	lib/xdg-app-ref.h \
	lib/xdg-app-installed-ref.h \
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

xdgappincludedir = $(includedir)/xdg-app
xdgappinclude_HEADERS = $(public_headers) $(generated_public_headers)

libxdg_app_la_SOURCES = \
	$(public_headers) \
	$(generated_public_headers) \
	lib/xdg-app.c \
	lib/xdg-app-enum-types.c \
	lib/xdg-app-ref.c \
	lib/xdg-app-installed-ref.c \
	lib/xdg-app-installed-ref-private.h \
	lib/xdg-app-remote-private.h \
	lib/xdg-app-remote.c \
	lib/xdg-app-installation.c \
	$(NULL)

libxdg_app_la_CFLAGS = \
	$(HIDDEN_VISIBILITY_CFLAGS)		\
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
