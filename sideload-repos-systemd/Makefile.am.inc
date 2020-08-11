if BUILD_AUTO_SIDELOADING

dist_libexec_SCRIPTS = sideload-repos-systemd/flatpak-create-sideload-symlinks.sh

service_in_files += sideload-repos-systemd/flatpak-sideload-usb-repo.service.in
systemduserunit_DATA += sideload-repos-systemd/flatpak-sideload-usb-repo.service

path_in_files = sideload-repos-systemd/flatpak-sideload-usb-repo.path.in
systemduserunit_DATA += sideload-repos-systemd/flatpak-sideload-usb-repo.path

systemdsystemunit_DATA += sideload-repos-systemd/flatpak-sideload-repos-dir.service

%.path: %.path.in config.log
	$(AM_V_GEN) $(SED) -e "s|\@media_dir\@|$(RUN_MEDIA_DIR)|" $< > $@

endif

EXTRA_DIST += sideload-repos-systemd/flatpak-sideload-usb-repo.path.in sideload-repos-systemd/flatpak-sideload-usb-repo.service.in sideload-repos-systemd/flatpak-sideload-repos-dir.service
