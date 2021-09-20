if BUILD_SYSTEM_HELPER

libexec_PROGRAMS += \
	flatpak-system-helper \
	$(NULL)

dbussystemservicedir = $(datadir)/dbus-1/system-services
service_in_files += system-helper/org.freedesktop.Flatpak.SystemHelper.service.in
dbussystemservice_DATA = system-helper/org.freedesktop.Flatpak.SystemHelper.service

dbusconfdir = $(DBUS_CONFIG_DIR)
dist_dbusconf_DATA = system-helper/org.freedesktop.Flatpak.SystemHelper.conf

service_in_files += system-helper/flatpak-system-helper.service.in
systemdsystemunit_DATA += system-helper/flatpak-system-helper.service

flatpak_system_helper_SOURCES = \
	system-helper/flatpak-system-helper.c	\
	system-helper/flatpak-system-helper.h	\
	$(NULL)

flatpak_system_helper_LDADD = $(BASE_LIBS) $(OSTREE_LIBS) $(JSON_LIBS) $(POLKIT_LIBS) libflatpak-common.la libflatpak-common-base.la libglnx.la
flatpak_system_helper_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) $(OSTREE_CFLAGS) $(APPSTREAM_CFLAGS) $(SOUP_CFLAGS) $(JSON_CFLAGS) $(POLKIT_CFLAGS) -DFLATPAK_COMPILATION -DLIBEXECDIR=\"$(libexecdir)\"

system-helper/org.freedesktop.Flatpak.rules: system-helper/org.freedesktop.Flatpak.rules.in
	$(AM_V_GEN) $(SED) -e "s|\@privileged_group\@|$(PRIVILEGED_GROUP)|" $< > $@

polkit_rulesdir = $(datadir)/polkit-1/rules.d
polkit_rules_DATA =					\
	system-helper/org.freedesktop.Flatpak.rules

polkit_policydir = $(datadir)/polkit-1/actions
polkit_policy_DATA =					\
	system-helper/org.freedesktop.Flatpak.policy

%.policy: %.policy.in
	$(AM_V_GEN) $(MSGFMT) --xml -d $(top_srcdir)/po --template $< -o $@ || cp $< $@

sysusers_DATA = system-helper/flatpak.conf

%.conf: %.conf.in
	$(AM_V_GEN) $(SED) \
	  -e "s|\@SYSTEM_HELPER_USER\@|$(SYSTEM_HELPER_USER)|" \
	  $< > $@

DISTCLEANFILES += system-helper/org.freedesktop.Flatpak.policy system-helper/org.freedesktop.Flatpak.rules system-helper/flatpak-system-helper.service system-helper/org.freedesktop.Flatpak.SystemHelper.service system-helper/flatpak.conf

endif

EXTRA_DIST += system-helper/org.freedesktop.Flatpak.policy.in system-helper/org.freedesktop.Flatpak.SystemHelper.conf system-helper/org.freedesktop.Flatpak.rules.in system-helper/org.freedesktop.Flatpak.SystemHelper.service.in system-helper/flatpak-system-helper.service.in system-helper/flatpak.conf.in
