if BUILD_SELINUX_MODULE
selinux_moduledir = ${datadir}/selinux/packages
selinux_module_DATA = $(NULL)

selinux_develdir = ${datadir}/selinux/devel/include/contrib
selinux_devel_DATA = $(NULL)

flatpak.pp.bz2: selinux/flatpak.te selinux/flatpak.fc
	$(eval TMP := $(shell mktemp -d selinux-build-XXXXXX))
	cp $^ $(TMP)/
	cd $(TMP) && make -f /usr/share/selinux/devel/Makefile flatpak.pp
	bzip2 -9 $(TMP)/flatpak.pp
	cp $(TMP)/flatpak.pp.bz2 .
	rm -rf $(TMP)

selinux_module_DATA += flatpak.pp.bz2
selinux_devel_DATA += selinux/flatpak.if
endif

EXTRA_DIST += \
	selinux/flatpak.te \
	selinux/flatpak.fc \
	selinux/flatpak.if \
	$(NULL)

DISTCLEANFILES += flatpak.pp.bz2
