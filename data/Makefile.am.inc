org.freedesktop.impl.portal.%.xml: org.freedesktop.portal.%.xml data/to_backend.xsl
	xsltproc --novalid --nonet data/to_backend.xsl $< > $@

introspectiondir = $(datadir)/dbus-1/interfaces
introspection_DATA = \
	data/org.freedesktop.impl.portal.PermissionStore.xml \
	data/org.freedesktop.portal.FileChooser.xml \
	data/org.freedesktop.impl.portal.FileChooser.xml \
	data/org.freedesktop.portal.Documents.xml \
	data/org.freedesktop.Flatpak.xml \
	$(NULL)

EXTRA_DIST += \
	data/org.freedesktop.portal.Documents.xml \
	data/org.freedesktop.impl.portal.PermissionStore.xml \
	data/org.freedesktop.systemd1.xml \
	data/org.freedesktop.Flatpak.xml \
	$(NULL)
