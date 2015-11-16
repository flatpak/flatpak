NULL =

bin_PROGRAMS = $(NULL)
libexec_PROGRAMS = $(NULL)
DISTCLEANFILES= $(NULL)
BUILT_SOURCES = $(NULL)

if BUILD_DOCUMENTATION
SUBDIRS = doc
endif

%.service: %.service.in config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

servicedir = $(DBUS_SERVICE_DIR)
service_in_files = $(NULL)
service_DATA = $(NULL)

AM_CPPFLAGS =						\
	-DXDG_APP_BINDIR=\"$(bindir)\"			\
	-DXDG_APP_SYSTEMDIR=\"$(localstatedir)/xdg-app\"\
	-DXDG_APP_BASEDIR=\"$(pkgdatadir)\"		\
	-DXDG_APP_TRIGGERDIR=\"$(pkgdatadir)/triggers\" \
	-DSYSTEM_FONTS_DIR=\"$(SYSTEM_FONTS_DIR)\"	\
	-DHELPER=\"$(bindir)/xdg-app-helper\"		\
	-DDBUSPROXY=\"$(libexecdir)/xdg-dbus-proxy\"	\
	-I$(srcdir)/libglnx 				\
	-I$(srcdir)/lib					\
	-I$(builddir)/lib				\
	$(NULL)

triggersdir = $(pkgdatadir)/triggers
dist_triggers_SCRIPTS = \
	triggers/gtk-icon-cache.trigger \
	triggers/mime-database.trigger \
	triggers/desktop-database.trigger \
	$(NULL)

EXTRA_DIST =

noinst_LTLIBRARIES = libglnx.la
libglnx_srcpath := $(srcdir)/libglnx
libglnx_cflags := $(BASE_CFLAGS) "-I$(libglnx_srcpath)"
libglnx_libs := $(BASE_LIBS)
include libglnx/Makefile-libglnx.am.inc

include lib/Makefile.am.inc
include data/Makefile.am.inc
include app/Makefile.am.inc
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
EXTRA_DIST += profile/xdg-app.sh.in $(service_in_files)
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
