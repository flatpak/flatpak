DOC_MODULE = flatpak
DOC_MAIN_SGML_FILE = libflatpak-docs.xml
DOC_SOURCE_DIR = $(top_srcdir)/common $(top_builddir)/common

SCAN_OPTIONS = --rebuild-types \
    --ignore-decorators='FLATPAK_EXTERN'
SCANGOBJ_OPTIONS =
MKDB_OPTIONS = --output-format=xml --name-space=flatpak
FIXXREF_OPTIONS =

HFILE_GLOB =  $(top_srcdir)/common/*.h $(top_builddir)/common/*.h
CFILE_GLOB = $(top_srcdir)/common/*.c

IGNORE_HFILES = \
	valgrind-private.h \
	flatpak-bwrap-private.h \
	flatpak-chain-input-stream-private.h \
	flatpak-common-types-private.h \
	flatpak-context-private.h \
	flatpak-dbus-generated.h \
	flatpak-dir-private.h \
	flatpak-document-dbus-generated.h \
	flatpak-enum-types.h \
	flatpak-exports-private.h \
	flatpak-installed-ref-private.h \
	flatpak-json-oci-private.h \
	flatpak-json-private.h \
	flatpak-oci-registry-private.h \
	flatpak-progress-private.h \
	flatpak-remote-private.h \
	flatpak-remote-ref-private.h \
	flatpak-run-private.h \
	flatpak-systemd-dbus-generated.h \
	flatpak-installation-private.h \
	flatpak-transaction-private.h \
	flatpak-utils-private.h \
	flatpak-utils-base-private.h \
	flatpak-utils-http-private.h \
	flatpak-instance-private.h \
	flatpak-auth-private.h \
	flatpak-parental-controls-private.h \
	flatpak-appdata-private.h \
	flatpak-zstd-decompressor-private.h \
	$(NULL)

EXTRA_HFILES =

HTML_IMAGES =

dbus_stamp_files = \
	dbus-org.freedesktop.Flatpak.Authenticator.stamp \
	dbus-org.freedesktop.Flatpak.stamp \
	dbus-org.freedesktop.impl.portal.PermissionStore.stamp \
	dbus-org.freedesktop.portal.Documents.stamp \
	dbus-org.freedesktop.portal.Flatpak.stamp \
	$(NULL)

# Generated from the above stamps
xml_files = \
	dbus-org.freedesktop.Flatpak.AuthenticatorRequest.xml \
	dbus-org.freedesktop.Flatpak.Authenticator.xml \
	dbus-org.freedesktop.Flatpak.Development.xml \
	dbus-org.freedesktop.Flatpak.SessionHelper.xml \
	dbus-org.freedesktop.Flatpak.SystemHelper.xml \
	dbus-org.freedesktop.impl.portal.PermissionStore.xml \
	dbus-org.freedesktop.portal.Documents.xml \
	dbus-org.freedesktop.portal.Flatpak.UpdateMonitor.xml \
	dbus-org.freedesktop.portal.Flatpak.xml \
	$(NULL)

content_files =
expand_content_files =

AM_CPPFLAGS = -I$(top_srcdir)/common -I$(top_builddir)/common $(BASE_CFLAGS)
GTKDOC_LIBS = $(top_builddir)/libflatpak.la $(BASE_LIBS)

include $(top_srcdir)/gtk-doc.make

CLEANFILES += $(xml_files)

EXTRA_DIST += version.xml.in

if ENABLE_GTK_DOC_CHECK
TESTS_ENVIRONMENT = \
	DOC_MODULE=$(DOC_MODULE) DOC_MAIN_SGML_FILE=$(DOC_MAIN_SGML_FILE) \
	SRCDIR=$(abs_srcdir) BUILDDIR=$(abs_builddir)
TESTS = $(GTKDOC_CHECK)
endif

libflatpak-docs.html: libflatpak-docs.xml docs
	$(AM_V_GEN) $(XMLTO) $(XMLTO_FLAGS) --skip-validation xhtml-nochunks -m $(srcdir)/../xmlto-config.xsl $<

sgml.stamp: $(dbus_stamp_files)
# gdbus-codegen hardcodes DocBook version
	$(AM_V_GEN)$(SED) -i \
		-e 's|http://www.oasis-open.org/docbook/xml/4.1.2/docbookx.dtd|http://www.oasis-open.org/docbook/xml/4.5/docbookx.dtd|' \
		-e 's|-//OASIS//DTD DocBook XML V4.1.2//EN|-//OASIS//DTD DocBook XML V4.5//EN|' $(xml_files)

dbus-%.stamp: $(top_srcdir)/data/%.xml
	$(AM_V_GEN)$(GDBUS_CODEGEN) --generate-docbook=dbus $<
