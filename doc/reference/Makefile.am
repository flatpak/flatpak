DOC_MODULE = flatpak
DOC_MAIN_SGML_FILE = flatpak-docs.xml
DOC_SOURCE_DIR = $(top_srcdir)/lib $(top_builddir)/lib

SCAN_OPTIONS = --rebuild-types \
    --ignore-decorators='FLATPAK_EXTERN'
SCANGOBJ_OPTIONS =
MKDB_OPTIONS = --output-format=xml --name-space=flatpak
FIXXREF_OPTIONS =

HFILE_GLOB = $(top_srcdir)/lib/*.h $(top_builddir)/lib/*.h
CFILE_GLOB = $(top_srcdir)/lib/*.c

IGNORE_HFILES = \
	flatpak-enum-types.h \
	flatpak-installed-ref-private.h \
	flatpak-remote-ref-private.h \
	flatpak-remote-private.h

EXTRA_HFILES =

HTML_IMAGES =

content_files =
expand_content_files = 

AM_CPPFLAGS = -I$(top_srcdir)/lib -I$(top_builddir)/lib $(BASE_CFLAGS)
GTKDOC_LIBS = $(top_builddir)/libflatpak.la $(BASE_LIBS)

include $(top_srcdir)/gtk-doc.make

EXTRA_DIST += version.xml.in

if ENABLE_GTK_DOC_CHECK
TESTS_ENVIRONMENT = \
	DOC_MODULE=$(DOC_MODULE) DOC_MAIN_SGML_FILE=$(DOC_MAIN_SGML_FILE) \
	SRCDIR=$(abs_srcdir) BUILDDIR=$(abs_builddir)
TESTS = $(GTKDOC_CHECK)
endif
