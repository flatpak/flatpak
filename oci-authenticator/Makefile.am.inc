EXTRA_DIST += oci-authenticator/meson.build

libexec_PROGRAMS += \
	flatpak-oci-authenticator \
	$(NULL)

service_in_files += oci-authenticator/flatpak-oci-authenticator.service.in
systemduserunit_DATA += oci-authenticator/flatpak-oci-authenticator.service

service_in_files += oci-authenticator/org.flatpak.Authenticator.Oci.service.in
dbus_service_DATA += oci-authenticator/org.flatpak.Authenticator.Oci.service

flatpak_oci_authenticator_SOURCES = \
	oci-authenticator/flatpak-oci-authenticator.c		\
	$(NULL)

BUILT_SOURCES += $(nodist_flatpak_oci_authenticator_SOURCES)
CLEANFILES += $(nodist_flatpak_oci_authenticator_SOURCES)

flatpak_oci_authenticator_LDADD = $(AM_LDADD) $(BASE_LIBS) $(OSTREE_LIBS) libflatpak-common.la libflatpak-common-base.la libglnx.la
flatpak_oci_authenticator_CFLAGS = \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	$(OSTREE_CFLAGS) \
	$(SOUP_CFLAGS) \
	$(JSON_CFLAGS) \
	-DFLATPAK_COMPILATION \
	-DDATADIR=\"$(datadir)\" \
	$(NULL)
flatpak_oci_authenticator_CPPFLAGS = $(AM_CPPFLAGS) -I$(builddir)/oci-authenticator
