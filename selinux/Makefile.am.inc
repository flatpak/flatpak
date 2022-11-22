if BUILD_SELINUX_MODULE
selinux_moduledir = ${datadir}/selinux/packages
selinux_module_DATA = $(NULL)

selinux_develdir = ${datadir}/selinux/devel/include/contrib
selinux_devel_DATA = $(NULL)

flatpak.pp.bz2: selinux/flatpak.te selinux/flatpak.fc selinux/build-selinux.sh
	$(srcdir)/selinux/build-selinux.sh . $^

selinux_module_DATA += flatpak.pp.bz2
selinux_devel_DATA += selinux/flatpak.if
endif

EXTRA_DIST += \
	selinux/build-selinux.sh \
	selinux/flatpak.te \
	selinux/flatpak.fc \
	selinux/flatpak.if \
	selinux/meson.build \
	$(NULL)

DISTCLEANFILES += flatpak.pp.bz2
