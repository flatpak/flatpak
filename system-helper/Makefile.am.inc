libexec_PROGRAMS += \
	xdg-app-system-helper \
	$(NULL)

dbussystemservicedir = $(datadir)/dbus-1/system-services
service_in_files += system-helper/org.freedesktop.XdgApp.SystemHelper.service.in
dbussystemservice_DATA = system-helper/org.freedesktop.XdgApp.SystemHelper.service

dbusconfdir = ${sysconfdir}/dbus-1/system.d
dbusconf_DATA = system-helper/org.freedesktop.XdgApp.SystemHelper.conf

service_in_files += system-helper/xdg-app-system-helper.service.in
systemdsystemunit_DATA = system-helper/xdg-app-system-helper.service

xdg_app_system_helper_SOURCES = \
	system-helper/xdg-app-system-helper.c	\
	lib/xdg-app-error.c	\
	$(NULL)

xdg_app_system_helper_LDADD = $(BASE_LIBS) $(OSTREE_LIBS) $(POLKIT_LIBS) libxdgapp-common.la
xdg_app_system_helper_CFLAGS = $(BASE_CFLAGS) $(OSTREE_CFLAGS) $(POLKIT_CFLAGS)

polkit_rulesdir = $(datadir)/polkit-1/rules.d
dist_polkit_rules_DATA =					\
	system-helper/org.freedesktop.XdgApp.rules

polkit_policydir = $(datadir)/polkit-1/actions
dist_polkit_policy_DATA =					\
	system-helper/org.freedesktop.XdgApp.policy

@INTLTOOL_POLICY_RULE@

EXTRA_DIST += system-helper/org.freedesktop.XdgApp.policy.in
DISTCLEANFILES += system-helper/org.freedesktop.XdgApp.policy
