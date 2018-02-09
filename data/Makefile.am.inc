introspectiondir = $(datadir)/dbus-1/interfaces
introspection_DATA = \
	data/org.freedesktop.Flatpak.xml \
	$(NULL)

if ENABLE_DOCUMENT_PORTAL
introspection_DATA += \
	data/org.freedesktop.impl.portal.PermissionStore.xml \
	data/org.freedesktop.portal.Documents.xml \
	$(NULL)
endif

EXTRA_DIST += \
	data/org.freedesktop.portal.Documents.xml \
	data/org.freedesktop.impl.portal.PermissionStore.xml \
	data/org.freedesktop.systemd1.xml \
	data/org.freedesktop.Flatpak.xml \
	$(NULL)
