NULL =

bin_PROGRAMS = $(NULL)
noinst_PROGRAMS = $(NULL)
libexec_PROGRAMS = $(NULL)
CLEANFILES = $(NULL)
DISTCLEANFILES= $(NULL)
BUILT_SOURCES = $(NULL)
EXTRA_DIST =

if BUILD_DOCUMENTATION
SUBDIRS = . doc
endif

%.service: %.service.in config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

dbus_servicedir = $(DBUS_SERVICE_DIR)
service_in_files = $(NULL)
dbus_service_DATA = $(NULL)
systemduserunit_DATA = $(NULL)
CLEANFILES += $(dbus_service_DATA)
CLEANFILES += $(systemduserunit_DATA)
EXTRA_DIST += $(service_in_files)

AM_CPPFLAGS =						\
	-DXDG_APP_BINDIR=\"$(bindir)\"			\
	-DXDG_APP_SYSTEMDIR=\"$(localstatedir)/lib/xdg-app\"\
	-DXDG_APP_BASEDIR=\"$(pkgdatadir)\"		\
	-DXDG_APP_TRIGGERDIR=\"$(pkgdatadir)/triggers\" \
	-DSYSTEM_FONTS_DIR=\"$(SYSTEM_FONTS_DIR)\"	\
	-DHELPER=\"$(bindir)/xdg-app-helper\"		\
	-DDBUSPROXY=\"$(libexecdir)/xdg-dbus-proxy\"	\
	-I$(srcdir)/libglnx 				\
	-I$(srcdir)/common				\
	-I$(builddir)/common				\
	$(NULL)

triggersdir = $(pkgdatadir)/triggers
dist_triggers_SCRIPTS = \
	triggers/gtk-icon-cache.trigger \
	triggers/mime-database.trigger \
	triggers/desktop-database.trigger \
	$(NULL)

lib_LTLIBRARIES =
noinst_LTLIBRARIES = libglnx.la
libglnx_srcpath := $(srcdir)/libglnx
libglnx_cflags := \
	$(BASE_CFLAGS) \
	"-I$(libglnx_srcpath)" \
	$(HIDDEN_VISIBILITY_CFLAGS) \
	$(NULL)
libglnx_libs := $(BASE_LIBS)
include libglnx/Makefile-libglnx.am.inc

include common/Makefile.am.inc
include data/Makefile.am.inc
include app/Makefile.am.inc
include lib/Makefile.am.inc
include builder/Makefile.am.inc
include session-helper/Makefile.am.inc
include dbus-proxy/Makefile.am.inc
include document-portal/Makefile.am.inc
include Makefile.gtester
include tests/Makefile.am.inc

completiondir = $(datadir)/bash-completion/completions
completion_DATA = completion/xdg-app
EXTRA_DIST += $(completion_DATA)

profiledir = $(sysconfdir)/profile.d
profile_DATA = xdg-app.sh
EXTRA_DIST += \
	profile/xdg-app.sh.in \
	$(NULL)
DISTCLEANFILES += xdg-app.sh

xdg-app.sh: profile/xdg-app.sh.in
	$(AM_V_GEN) $(SED) -e "s|\@localstatedir\@|$(localstatedir)|" \
		-e "s|\@sysconfdir\@|$(sysconfdir)|" $< > $@

envdir = $(datadir)/gdm/env.d
env_DATA = xdg-app.env
EXTRA_DIST += env.d/xdg-app.env.in
DISTCLEANFILES += xdg-app.env

xdg-app.env: env.d/xdg-app.env.in
	$(AM_V_GEN) $(SED) -e "s|\@localstatedir\@|$(localstatedir)|" \
		-e "s|\@sysconfdir\@|$(sysconfdir)|" $< > $@

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = xdg-app.pc
EXTRA_DIST += xdg-app.pc.in

DISTCHECK_CONFIGURE_FLAGS =		\
	--enable-documentation		\
	--enable-libxdgapp		\
	--disable-maintainer-mode	\
	--enable-introspection
