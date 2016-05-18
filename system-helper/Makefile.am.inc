libexec_PROGRAMS += \
	flatpak-system-helper \
	$(NULL)

dbussystemservicedir = $(datadir)/dbus-1/system-services
service_in_files += system-helper/org.freedesktop.Flatpak.SystemHelper.service.in
dbussystemservice_DATA = system-helper/org.freedesktop.Flatpak.SystemHelper.service

dbusconfdir = ${sysconfdir}/dbus-1/system.d
dbusconf_DATA = system-helper/org.freedesktop.Flatpak.SystemHelper.conf

service_in_files += system-helper/flatpak-system-helper.service.in
systemdsystemunit_DATA = system-helper/flatpak-system-helper.service

flatpak_system_helper_SOURCES = \
	system-helper/flatpak-system-helper.c	\
	lib/flatpak-error.c	\
	$(NULL)

flatpak_system_helper_LDADD = $(BASE_LIBS) $(OSTREE_LIBS) $(POLKIT_LIBS) libflatpak-common.la
flatpak_system_helper_CFLAGS = $(BASE_CFLAGS) $(OSTREE_CFLAGS) $(POLKIT_CFLAGS)

polkit_rulesdir = $(datadir)/polkit-1/rules.d
dist_polkit_rules_DATA =					\
	system-helper/org.freedesktop.Flatpak.rules

polkit_policydir = $(datadir)/polkit-1/actions
dist_polkit_policy_DATA =					\
	system-helper/org.freedesktop.Flatpak.policy

@INTLTOOL_POLICY_RULE@

EXTRA_DIST += system-helper/org.freedesktop.Flatpak.policy.in system-helper/org.freedesktop.Flatpak.SystemHelper.conf
DISTCLEANFILES += system-helper/org.freedesktop.Flatpak.policy system-helper/flatpak-system-helper.service system-helper/org.freedesktop.Flatpak.SystemHelper.service
