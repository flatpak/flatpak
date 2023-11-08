introspectiondir = $(datadir)/dbus-1/interfaces
introspection_DATA = \
	data/org.freedesktop.Flatpak.xml \
	data/org.freedesktop.Flatpak.Authenticator.xml \
	data/org.freedesktop.portal.Flatpak.xml \
	$(NULL)

dist_tmpfiles_DATA += data/tmpfiles.d/flatpak.conf

EXTRA_DIST += \
	data/org.freedesktop.portal.Documents.xml \
	data/org.freedesktop.impl.portal.PermissionStore.xml \
	data/org.freedesktop.systemd1.xml \
	data/org.freedesktop.Flatpak.xml \
	data/org.freedesktop.Flatpak.Authenticator.xml \
	data/org.freedesktop.portal.Flatpak.xml \
	data/flatpak-docker-seccomp.json \
	data/flatpak-variants.gv \
	$(NULL)
