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
		-e "s|\@media_dir\@|$(RUN_MEDIA_DIR)|" \
		-e "s|\@extraargs\@||" $< > $@

dbus_servicedir = $(DBUS_SERVICE_DIR)
service_in_files = $(NULL)
dbus_service_DATA = $(NULL)
systemdsystemunit_DATA = $(NULL)
systemduserunit_DATA = $(NULL)
CLEANFILES += $(dbus_service_DATA)
CLEANFILES += $(systemduserunit_DATA)
EXTRA_DIST += $(service_in_files)

FLATPAK_BINDIR=$(bindir)

ACLOCAL_AMFLAGS = -I m4 -I subprojects/libglnx ${ACLOCAL_FLAGS}
AM_CPPFLAGS =							\
	-DFLATPAK_BINDIR=\"$(FLATPAK_BINDIR)\"			\
	-DFLATPAK_SYSTEMDIR=\"$(SYSTEM_INSTALL_DIR)\"		\
	-DFLATPAK_CONFIGDIR=\"$(sysconfdir)/flatpak\"		\
	-DFLATPAK_BASEDIR=\"$(pkgdatadir)\"			\
	-DFLATPAK_TRIGGERDIR=\"$(pkgdatadir)/triggers\"		\
	-DSYSTEM_FONTS_DIR=\"$(SYSTEM_FONTS_DIR)\"		\
	-DSYSTEM_HELPER_USER=\"$(SYSTEM_HELPER_USER)\"          \
	-DSYSTEM_FONT_CACHE_DIRS=\"$(SYSTEM_FONT_CACHE_DIRS)\"		\
	-DG_LOG_DOMAIN=\"flatpak\"				\
	-I$(srcdir)/subprojects					\
	-I$(srcdir)/subprojects/libglnx				\
	-I$(srcdir)/common					\
	-I$(builddir)/common					\
	-include "config.h"					\
	$(NULL)

ASAN_FLAGS=
if ENABLE_ASAN
ASAN_FLAGS = -fsanitize=undefined -fsanitize-undefined-trap-on-error -fsanitize=address
endif

AM_CFLAGS = $(WARN_CFLAGS) $(ASAN_FLAGS)
AM_LDFLAGS = $(ASAN_FLAGS)

if WITH_SYSTEM_BWRAP
AM_CPPFLAGS += -DHELPER=\"$(BWRAP)\"
else
AM_CPPFLAGS += -DHELPER=\"$(libexecdir)/flatpak-bwrap\"
endif

if WITH_SYSTEM_DBUS_PROXY
AM_CPPFLAGS += -DDBUSPROXY=\"$(DBUS_PROXY)\"
else
AM_CPPFLAGS += -DDBUSPROXY=\"$(libexecdir)/flatpak-dbus-proxy\"
endif

triggersdir = $(pkgdatadir)/triggers
dist_triggers_SCRIPTS = \
	triggers/gtk-icon-cache.trigger \
	triggers/mime-database.trigger \
	triggers/desktop-database.trigger \
	$(NULL)

if USE_GPGME
# This canonicalizes the PKG_CHECK_MODULES or AM_PATH_GPGME results
INTERNAL_GPGME_CFLAGS = $(DEP_GPGME_CFLAGS) $(GPGME_PTHREAD_CFLAGS)
INTERNAL_GPGME_LIBS = $(DEP_GPGME_LIBS) $(GPGME_PTHREAD_LIBS)
endif

lib_LTLIBRARIES =
noinst_LTLIBRARIES += libglnx.la
libglnx_srcpath := $(srcdir)/subprojects/libglnx
libglnx_cflags := \
	$(BASE_CFLAGS) \
	"-I$(libglnx_srcpath)" \
	-std=gnu99 \
	$(HIDDEN_VISIBILITY_CFLAGS) \
	$(NULL)
libglnx_libs := $(BASE_LIBS)
include subprojects/Makefile-libglnx.am.inc

include common/Makefile.am.inc
include data/Makefile.am.inc
include app/Makefile.am.inc
include session-helper/Makefile.am.inc
include portal/Makefile.am.inc
include system-helper/Makefile.am.inc
include oci-authenticator/Makefile.am.inc
include icon-validator/Makefile.am.inc
include revokefs/Makefile.am.inc
include selinux/Makefile.am.inc
include sideload-repos-systemd/Makefile.am.inc
include tests/Makefile.am.inc

if !WITH_SYSTEM_DBUS_PROXY
libexec_PROGRAMS+=flatpak-dbus-proxy
flatpak_dbus_proxy_SOURCES =				\
	subprojects/dbus-proxy/flatpak-proxy.c		\
	subprojects/dbus-proxy/flatpak-proxy.h		\
	subprojects/dbus-proxy/dbus-proxy.c		\
	subprojects/dbus-proxy/backport-autoptr.h	\
	$(NULL)
flatpak_dbus_proxy_LDADD = $(AM_LDADD) $(BASE_LIBS)
flatpak_dbus_proxy_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS)
endif

if !WITH_SYSTEM_BWRAP

bwrap_PROGRAMS = flatpak-bwrap
flatpak_bwrap_SOURCES = $(bwrap_SOURCES)
flatpak_bwrap_CFLAGS = $(AM_CFLAGS) $(bwrap_CFLAGS)
flatpak_bwrap_LDADD = $(AM_LDADD) $(bwrap_LDADD) $(CAP_LIB)
bwrapdir = $(libexecdir)
include subprojects/Makefile-bwrap.am.inc

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

bashcompletiondir = $(datadir)/bash-completion/completions
bashcompletion_DATA = completion/flatpak
EXTRA_DIST += $(bashcompletion_DATA)

zshcompletiondir = $(datadir)/zsh/site-functions
zshcompletion_DATA = completion/_flatpak
EXTRA_DIST += $(zshcompletion_DATA)

fishcompletiondir = $(datadir)/fish/vendor_completions.d
fishcompletion_DATA = completion/flatpak.fish
EXTRA_DIST += $(fishcompletion_DATA)

profiledir = $(PROFILE_DIR)
profile_DATA = profile/flatpak.sh
EXTRA_DIST += $(profile_DATA)

envdir = $(datadir)/gdm/env.d
env_DATA =
if INSTALL_GDM_ENV_FILE
env_DATA += flatpak.env
endif

EXTRA_DIST += env.d/flatpak.env.in env.d/60-flatpak env.d/60-flatpak-system-only
DISTCLEANFILES += flatpak.env

flatpak.env: env.d/flatpak.env.in
	$(AM_V_GEN) $(SED) -e "s|\@localstatedir\@|$(localstatedir)|" \
		-e "s|\@sysconfdir\@|$(sysconfdir)|" $< > $@

systemenvgendir = $(systemdsystemenvgendir)
systemenvgen_SCRIPTS = env.d/60-flatpak-system-only

userenvgendir = $(systemduserenvgendir)
userenvgen_SCRIPTS = env.d/60-flatpak

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = flatpak.pc
EXTRA_DIST += flatpak.pc.in

scriptsdir = $(bindir)
scripts_SCRIPTS = scripts/flatpak-bisect scripts/flatpak-coredumpctl
EXTRA_DIST += scripts/flatpak-bisect scripts/flatpak-coredumpctl

EXTRA_DIST += subprojects/variant-schema-compiler/variant-schema-compiler

EXTRA_DIST += autogen.sh

EXTRA_DIST += README.md

EXTRA_DIST += flatpak.png

AM_DISTCHECK_CONFIGURE_FLAGS =		\
	--enable-documentation		\
	--disable-maintainer-mode	\
	--enable-introspection

.PHONY: coverage lcov-clean genlcov

coverage:
	$(AM_V_GEN) $(MAKE) $(AM_MAKEFLAGS) lcov-clean
	$(AM_V_GEN) $(MAKE) check
	$(AM_V_GEN) $(MAKE) $(AM_MAKEFLAGS) genlcov

lcov-clean:
	$(AM_V_GEN) $(LCOV) --directory $(top_builddir) --zerocounters

genlcov:
	$(AM_V_GEN) $(LCOV) --directory $(top_builddir) --capture --output-file coverage.info
	$(AM_V_GEN) $(GENHTML) --prefix $(top_builddir) --output-directory coverage coverage.info
