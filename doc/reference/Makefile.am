DOC_MODULE = xdg-app
DOC_MAIN_SGML_FILE = xdg-app-docs.xml
DOC_SOURCE_DIR = $(top_srcdir)/lib $(top_builddir)/lib

SCAN_OPTIONS = --rebuild-types \
    --ignore-decorators='XDG_APP_EXTERN'
SCANGOBJ_OPTIONS =
MKDB_OPTIONS = --output-format=xml --name-space=xdg_app
FIXXREF_OPTIONS =

HFILE_GLOB = $(top_srcdir)/lib/*.h $(top_builddir)/lib/*.h
CFILE_GLOB = $(top_srcdir)/lib/*.c

IGNORE_HFILES = \
	xdg-app-enum-types.h \
	xdg-app-installed-ref-private.h \
	xdg-app-remote-private.h

EXTRA_HFILES =

HTML_IMAGES =

content_files =
expand_content_files = 

AM_CPPFLAGS = -I$(top_srcdir)/lib -I$(top_builddir)/lib $(BASE_CFLAGS)
GTKDOC_LIBS = $(top_builddir)/libxdg-app.la $(BASE_LIBS)

include $(top_srcdir)/gtk-doc.make

if ENABLE_GTK_DOC_CHECK
TESTS_ENVIRONMENT = \
	DOC_MODULE=$(DOC_MODULE) DOC_MAIN_SGML_FILE=$(DOC_MAIN_SGML_FILE) \
	SRCDIR=$(abs_srcdir) BUILDDIR=$(abs_builddir)
TESTS = $(GTKDOC_CHECK)
endif
