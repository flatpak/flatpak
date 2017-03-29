NULL =

bin_PROGRAMS = $(NULL)
dist_installed_test_extra_scripts = $(NULL)
noinst_PROGRAMS = $(NULL)
noinst_LTLIBRARIES = $(NULL)
libexec_PROGRAMS = $(NULL)
CLEANFILES = $(NULL)
MAINTAINERCLEANFILES = $(NULL)
DISTCLEANFILES= $(NULL)
BUILT_SOURCES = $(NULL)
EXTRA_DIST =

include $(top_srcdir)/buildutil/glib-tap.mk

SUBDIRS =


if BUILD_DOCUMENTATION
SUBDIRS += . doc
endif

SUBDIRS += po

%.service: %.service.in config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" \
		-e "s|\@localstatedir\@|$(localstatedir)|" \
		-e "s|\@extraargs\@||" $< > $@

dbus_servicedir = $(DBUS_SERVICE_DIR)
service_in_files = $(NULL)
dbus_service_DATA = $(NULL)
systemduserunit_DATA = $(NULL)
CLEANFILES += $(dbus_service_DATA)
CLEANFILES += $(systemduserunit_DATA)
EXTRA_DIST += $(service_in_files)

FLATPAK_BINDIR=$(bindir)

AM_CPPFLAGS =							\
	-DFLATPAK_BINDIR=\"$(FLATPAK_BINDIR)\"			\
	-DFLATPAK_SYSTEMDIR=\"$(SYSTEM_INSTALL_DIR)\"		\
	-DFLATPAK_CONFIGDIR=\"$(sysconfdir)/flatpak\"		\
	-DFLATPAK_BASEDIR=\"$(pkgdatadir)\"			\
	-DFLATPAK_TRIGGERDIR=\"$(pkgdatadir)/triggers\"		\
	-DSYSTEM_FONTS_DIR=\"$(SYSTEM_FONTS_DIR)\"		\
	-DDBUSPROXY=\"$(libexecdir)/flatpak-dbus-proxy\"	\
	-DG_LOG_DOMAIN=\"flatpak\"				\
	-I$(srcdir)/libglnx					\
	-I$(srcdir)/common					\
	-I$(builddir)/common					\
	-I$(srcdir)/lib						\
	-I$(builddir)/lib					\
	$(NULL)

AM_CFLAGS = $(WARN_CFLAGS)

if WITH_SYSTEM_BWRAP
AM_CPPFLAGS += -DHELPER=\"$(BWRAP)\"
else
AM_CPPFLAGS += -DHELPER=\"$(libexecdir)/flatpak-bwrap\"
endif

triggersdir = $(pkgdatadir)/triggers
dist_triggers_SCRIPTS = \
	triggers/gtk-icon-cache.trigger \
	triggers/mime-database.trigger \
	triggers/desktop-database.trigger \
	$(NULL)

# This canonicalizes the PKG_CHECK_MODULES or AM_PATH_GPGME results
INTERNAL_GPGME_CFLAGS = $(DEP_GPGME_CFLAGS) $(GPGME_PTHREAD_CFLAGS)
INTERNAL_GPGME_LIBS = $(DEP_GPGME_LIBS) $(GPGME_PTHREAD_LIBS)

lib_LTLIBRARIES =
noinst_LTLIBRARIES += libglnx.la
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
include system-helper/Makefile.am.inc
include dbus-proxy/Makefile.am.inc
include permission-store/Makefile.am.inc
include document-portal/Makefile.am.inc
include tests/Makefile.am.inc

if !WITH_SYSTEM_BWRAP

bwrap_PROGRAMS = flatpak-bwrap
flatpak_bwrap_SOURCES = $(bwrap_SOURCES)
flatpak_bwrap_CFLAGS = $(bwrap_CFLAGS)
flatpak_bwrap_LDADD = $(bwrap_LDADD)
bwrapdir = $(libexecdir)
include bubblewrap/Makefile-bwrap.am.inc

endif # !WITH_SYSTEM_BWRAP

# NOTE: bwrap is install-bwrapPROGS which is run from install-data, not install-exec, this data-hook is used
install-data-hook:
	$(MAKE) $(AM_MAKEFLAGS) install-test-data-hook
if !WITH_SYSTEM_BWRAP
if PRIV_MODE_SETUID
	$(SUDO_BIN) chown root $(DESTDIR)$(libexecdir)/flatpak-bwrap
	$(SUDO_BIN) chmod u+s $(DESTDIR)$(libexecdir)/flatpak-bwrap
endif # !PRIV_MODE_SETUID
endif # !WITH_SYSTEM_BWRAP

completiondir = $(datadir)/bash-completion/completions
completion_DATA = completion/flatpak
EXTRA_DIST += $(completion_DATA)

profiledir = $(PROFILE_DIR)
profile_DATA = flatpak.sh
EXTRA_DIST += \
	profile/flatpak.sh.in \
	$(NULL)
DISTCLEANFILES += flatpak.sh

flatpak.sh: profile/flatpak.sh.in
	$(AM_V_GEN) $(SED) -e "s|\@localstatedir\@|$(localstatedir)|" \
		-e "s|\@sysconfdir\@|$(sysconfdir)|" $< > $@

envdir = $(datadir)/gdm/env.d
env_DATA = flatpak.env
EXTRA_DIST += env.d/flatpak.env.in
DISTCLEANFILES += flatpak.env

flatpak.env: env.d/flatpak.env.in
	$(AM_V_GEN) $(SED) -e "s|\@localstatedir\@|$(localstatedir)|" \
		-e "s|\@sysconfdir\@|$(sysconfdir)|" $< > $@

dbussnippetdir = $(systemduserunitdir)/dbus.service.d
dbussnippet_DATA = flatpak.conf
EXTRA_DIST += dbus.service.d/flatpak.conf.in
DISTCLEANFILES += flatpak.conf

flatpak.conf: dbus.service.d/flatpak.conf.in
	$(AM_V_GEN) $(SED) -e "s|\@localstatedir\@|$(localstatedir)|" \
		-e "s|\@sysconfdir\@|$(sysconfdir)|" $< > $@

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = flatpak.pc
EXTRA_DIST += flatpak.pc.in

scriptsdir = $(bindir)
scripts_SCRIPTS = scripts/flatpak-bisect
EXTRA_DIST += scripts/flatpak-bisect

EXTRA_DIST += README.md

AM_DISTCHECK_CONFIGURE_FLAGS =		\
	--enable-documentation		\
	--disable-maintainer-mode	\
	--enable-introspection
