introspectiondir = $(datadir)/dbus-1/interfaces
introspection_DATA = \
	data/org.freedesktop.portal.Documents.xml \
	data/org.freedesktop.XdgApp.xml \
	$(NULL)

EXTRA_DIST += \
	data/org.freedesktop.portal.Documents.xml \
	data/org.freedesktop.systemd1.xml \
	data/org.freedesktop.XdgApp.xml \
	$(NULL)
