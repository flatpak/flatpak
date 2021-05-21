AM_TESTS_ENVIRONMENT = FLATPAK_TESTS_DEBUG=1 \
	FLATPAK_CONFIG_DIR=/dev/null \
	FLATPAK_PORTAL=$$(cd $(top_builddir) && pwd)/flatpak-portal \
	FLATPAK_TRIGGERSDIR=$$(cd $(top_srcdir) && pwd)/triggers \
	FLATPAK_VALIDATE_ICON=$$(cd $(top_builddir) && pwd)/flatpak-validate-icon \
	FLATPAK_REVOKEFS_FUSE=$$(cd $(top_builddir) && pwd)/revokefs-fuse \
	GI_TYPELIB_PATH=$$(cd $(top_builddir) && pwd)$${GI_TYPELIB_PATH:+:$$GI_TYPELIB_PATH} \
	LD_LIBRARY_PATH=$$(cd $(top_builddir)/.libs && pwd)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	PATH=$$(cd $(top_builddir) && pwd):$${PATH} \
	$(NULL)

if WITH_SYSTEM_DBUS_PROXY
AM_TESTS_ENVIRONMENT += FLATPAK_DBUSPROXY=$(DBUS_PROXY)
else
AM_TESTS_ENVIRONMENT += FLATPAK_DBUSPROXY=$$(cd $(top_builddir) && pwd)/flatpak-dbus-proxy
endif

if WITH_SYSTEM_BWRAP
AM_TESTS_ENVIRONMENT += FLATPAK_BWRAP=$(BWRAP)
else
AM_TESTS_ENVIRONMENT += FLATPAK_BWRAP=$$(cd $(top_builddir) && pwd)/flatpak-bwrap
endif

noinst_LTLIBRARIES += libtestlib.la
libtestlib_la_CFLAGS = \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	-DFLATPAK_COMPILATION \
	$(NULL)
libtestlib_la_SOURCES = \
	tests/testlib.c \
	tests/testlib.h \
	$(NULL)
libtestlib_la_LIBADD = \
	$(AM_LDADD) \
	$(BASE_LIBS) \
	libglnx.la \
	$(NULL)

testlibrary_CFLAGS = \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	$(FUSE_CFLAGS) \
	$(OSTREE_CFLAGS) \
	-DFLATPAK_COMPILATION \
	$(NULL)
testlibrary_LDADD = \
	$(AM_LDADD) \
	$(BASE_LIBS) \
	$(FUSE_LIBS) \
	$(OSTREE_LIBS) \
	libglnx.la \
	libflatpak.la \
	$(NULL)
testlibrary_SOURCES = \
	tests/can-use-fuse.c \
	tests/can-use-fuse.h \
	tests/testlibrary.c \
	$(NULL)

testcommon_CFLAGS = \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	$(OSTREE_CFLAGS) \
	$(SOUP_CFLAGS) \
	$(JSON_CFLAGS) \
	$(APPSTREAM_GLIB_CFLAGS) \
	-DFLATPAK_COMPILATION \
	-DLIBEXECDIR=\"$(libexecdir)\" \
	-I$(srcdir)/app \
	-I$(builddir)/app \
	$(NULL)
testcommon_LDADD = \
	$(AM_LDADD) \
	$(BASE_LIBS) \
	$(OSTREE_LIBS) \
	$(SOUP_LIBS) \
	$(JSON_LIBS) \
	$(APPSTREAM_GLIB_LIBS) \
	libflatpak-app.la \
	libflatpak-common.la \
	libflatpak-common-base.la \
	libglnx.la \
	$(NULL)
testcommon_SOURCES = tests/testcommon.c

test_context_CFLAGS = $(testcommon_CFLAGS)
test_context_LDADD = $(testcommon_LDADD) libtestlib.la
test_context_SOURCES = tests/test-context.c

test_exports_CFLAGS = $(testcommon_CFLAGS)
test_exports_LDADD = $(testcommon_LDADD) libtestlib.la
test_exports_SOURCES = tests/test-exports.c

test_instance_CFLAGS = $(testcommon_CFLAGS)
test_instance_LDADD = $(testcommon_LDADD) libtestlib.la
test_instance_SOURCES = \
	subprojects/libglnx/tests/libglnx-testlib.c \
	subprojects/libglnx/tests/libglnx-testlib.h \
	tests/test-instance.c \
	$(NULL)

test_portal_CFLAGS = $(testcommon_CFLAGS)
test_portal_LDADD = $(testcommon_LDADD) libtestlib.la
test_portal_SOURCES = \
	portal/flatpak-portal-dbus.c \
	portal/flatpak-portal-dbus.h \
	tests/test-portal.c \
	$(NULL)

tests_hold_lock_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS)
tests_hold_lock_LDADD = $(AM_LDADD) $(BASE_LIBS) libglnx.la
tests_hold_lock_SOURCES = tests/hold-lock.c

tests_httpcache_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) $(OSTREE_CFLAGS) $(SOUP_CFLAGS) $(JSON_CFLAGS) $(APPSTREAM_GLIB_CFLAGS) \
	-DFLATPAK_COMPILATION \
	-DLOCALEDIR=\"$(localedir)\"
tests_httpcache_LDADD = $(AM_LDADD) $(BASE_LIBS) $(OSTREE_LIBS) $(SOUP_LIBS) $(JSON_LIBS) $(APPSTREAM_GLIB_LIBS) \
	libflatpak-common.la libflatpak-common-base.la libglnx.la

tests_mock_flatpak_CFLAGS = $(testcommon_CFLAGS)
tests_mock_flatpak_LDADD = $(testcommon_LDADD) libtestlib.la
tests_mock_flatpak_SOURCES = tests/mock-flatpak.c

tests_test_update_portal_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) \
	-DFLATPAK_COMPILATION \
	-DLOCALEDIR=\"$(localedir)\"
tests_test_update_portal_LDADD = $(AM_LDADD) $(BASE_LIBS)
tests_test_update_portal_SOURCES = tests/test-update-portal.c
nodist_tests_test_update_portal_SOURCES = portal/flatpak-portal-dbus.c

tests_test_portal_impl_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) \
	-DFLATPAK_COMPILATION \
	-DLOCALEDIR=\"$(localedir)\"
tests_test_portal_impl_LDADD = $(AM_LDADD) $(BASE_LIBS)
tests_test_portal_impl_SOURCES = tests/test-portal-impl.c

tests_test_authenticator_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) \
	-DFLATPAK_COMPILATION \
	-DLOCALEDIR=\"$(localedir)\"
tests_test_authenticator_LDADD = $(AM_LDADD) $(BASE_LIBS) libflatpak-common.la libflatpak-common-base.la libglnx.la
tests_test_authenticator_SOURCES = tests/test-authenticator.c

tests_list_unused_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) $(OSTREE_CFLAGS) $(SOUP_CFLAGS) $(JSON_CFLAGS) $(APPSTREAM_GLIB_CFLAGS) \
	-DFLATPAK_COMPILATION \
	-DLOCALEDIR=\"$(localedir)\"
tests_list_unused_LDADD = $(AM_LDADD) $(BASE_LIBS) $(BASE_LIBS) $(OSTREE_LIBS) libflatpak-common.la libflatpak-common-base.la libglnx.la
tests_list_unused_SOURCES = tests/list-unused.c

tests/services/org.freedesktop.Flatpak.service: session-helper/org.freedesktop.Flatpak.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" $< > $@

tests/services/org.freedesktop.portal.Flatpak.service: portal/org.freedesktop.portal.Flatpak.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" -e "s|\@extraargs\@| --poll-timeout=1|"  $< > $@

tests/services/org.freedesktop.Flatpak.SystemHelper.service: system-helper/org.freedesktop.Flatpak.SystemHelper.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" -e "s|\@extraargs\@| --session --no-idle-exit|" $< > $@

tests/services/org.freedesktop.impl.portal.desktop.test.service: tests/org.freedesktop.impl.portal.desktop.test.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/tests|" $< > $@

tests/services/org.flatpak.Authenticator.test.service: tests/org.flatpak.Authenticator.test.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)/tests|" $< > $@

tests/services/org.flatpak.Authenticator.Oci.service: oci-authenticator/org.flatpak.Authenticator.Oci.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" $< > $@

tests/share/xdg-desktop-portal/portals/test.portal: tests/test.portal.in
	mkdir -p tests/share/xdg-desktop-portal/portals
	$(AM_V_GEN) install -m644 $< $@

tests/libtest.sh: tests/services/org.freedesktop.Flatpak.service tests/services/org.freedesktop.Flatpak.SystemHelper.service tests/services/org.freedesktop.portal.Flatpak.service tests/share/xdg-desktop-portal/portals/test.portal tests/services/org.freedesktop.impl.portal.desktop.test.service tests/services/org.flatpak.Authenticator.test.service tests/services/org.flatpak.Authenticator.Oci.service

install-test-data-hook:
if ENABLE_INSTALLED_TESTS
	mkdir -p $(DESTDIR)$(installed_testdir)/services
	ln -sf $(dbus_servicedir)/org.freedesktop.Flatpak.service $(DESTDIR)$(installed_testdir)/services/
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" -e "s|\@extraargs\@| --poll-timeout=1|" $(top_srcdir)/portal/org.freedesktop.portal.Flatpak.service.in > $(DESTDIR)$(installed_testdir)/services/org.freedesktop.portal.Flatpak.service
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" -e "s|\@extraargs\@| --session --no-idle-exit|" $(top_srcdir)/system-helper/org.freedesktop.Flatpak.SystemHelper.service.in > $(DESTDIR)$(installed_testdir)/services/org.freedesktop.Flatpak.SystemHelper.service
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(installed_testdir)|" $(top_srcdir)/tests/org.freedesktop.impl.portal.desktop.test.service.in > $(DESTDIR)$(installed_testdir)/services/org.freedesktop.impl.portal.desktop.test.service
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(installed_testdir)|" $(top_srcdir)/tests/org.flatpak.Authenticator.test.service.in > $(DESTDIR)$(installed_testdir)/services/org.flatpak.Authenticator.test.service.service
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $(top_srcdir)/oci-authenticator/org.flatpak.Authenticator.Oci.service.in > $(DESTDIR)$(installed_testdir)/services/org.flatpak.Authenticator.Oci.service
	mkdir -p $(DESTDIR)$(installed_testdir)/share/xdg-desktop-portal/portals
	install -m644 $(top_srcdir)/tests/test.portal.in $(DESTDIR)$(installed_testdir)/share/xdg-desktop-portal/portals/test.portal
endif

tests/package_version.txt: Makefile
	echo $(PACKAGE_VERSION) > tests/package_version.txt

tests/test-basic.sh: tests/package_version.txt

dist_installed_test_extra_scripts += \
	buildutil/tap-driver.sh \
	tests/http-utils-test-server.py \
	tests/oci-registry-server.py \
	tests/oci-registry-client.py \
	tests/make-multi-collection-id-repo.sh \
	tests/make-test-app.sh \
	tests/make-test-runtime.sh \
	tests/test-webserver.sh \
	tests/test-wrapper.sh \
	tests/web-server.py \
	$(NULL)

dist_installed_test_data = \
	tests/libtest.sh \
	tests/org.test.Hello.png \
	tests/package_version.txt \
	tests/session.conf.in \
	tests/test.filter \
	tests/test.portal.in \
	tests/org.freedesktop.impl.portal.desktop.test.service.in \
	tests/org.flatpak.Authenticator.test.service.in \
	$(NULL)

test_ltlibraries = tests/libpreload.la

tests_libpreload_la_SOURCES = tests/libpreload.c
tests_libpreload_la_LDFLAGS = \
	-avoid-version \
	-module \
	-no-undefined \
	$(NULL)

if ENABLE_INSTALLED_TESTS
tests_libpreload_la_LDFLAGS += -rpath $(installed_testdir)
else
tests_libpreload_la_LDFLAGS += -rpath  ${abs_builddir}
endif

installed_test_keyringdir = $(installed_testdir)/test-keyring
installed_test_keyring2dir = $(installed_testdir)/test-keyring2

if ENABLE_INSTALLED_TESTS
dist_installed_test_keyring_DATA = \
	tests/test-keyring/README \
	tests/test-keyring/pubring.gpg \
	tests/test-keyring/secring.gpg \
	$(NULL)
dist_installed_test_keyring2_DATA = \
	tests/test-keyring2/README \
	tests/test-keyring2/pubring.gpg \
	tests/test-keyring2/secring.gpg \
	$(NULL)
endif

# Run make update-test-matrix after changing this
TEST_MATRIX_SOURCE = \
	tests/test-basic.sh \
	tests/test-completion.sh \
	tests/test-config.sh \
	tests/test-build-update-repo.sh \
	tests/test-http-utils.sh \
	tests/test-run.sh{{user+system+system-norevokefs},{nodeltas+deltas}} \
	tests/test-info.sh{user+system} \
	tests/test-repo.sh{{user+system+system-norevokefs}+{{user+system},oldsummary}} \
	tests/test-sideload.sh{user+system} \
	tests/test-default-remotes.sh \
	tests/test-extensions.sh \
	tests/test-bundle.sh{user+system+system-norevokefs} \
	tests/test-oci.sh \
	tests/test-oci-registry.sh{user+system} \
	tests/test-update-remote-configuration.sh{newsummary+oldsummary} \
	tests/test-override.sh \
	tests/test-update-portal.sh{user+system} \
	tests/test-auth.sh \
	tests/test-unused.sh \
	tests/test-summaries.sh{user+system} \
	tests/test-subset.sh{user+system} \
	tests/test-prune.sh \
	$(NULL)

update-test-matrix:
	$(srcdir)/tests/expand-test-matrix.sh "$(TEST_MATRIX_SOURCE)" > tests/Makefile-test-matrix.am.inc

tests/test-%.wrap:
	@true

tests/runtime-repo: tests/make-test-runtime.sh flatpak
	rm -rf tests/runtime-repo
	PATH=$(abs_top_builddir):$${PATH} $(top_srcdir)/tests/make-test-runtime.sh tests/runtime-repo org.test.Platform master "" ""
	PATH=$(abs_top_builddir):$${PATH} $(top_srcdir)/tests/make-test-runtime.sh tests/runtime-repo org.test.Platform stable "" ""

check_DATA += tests/runtime-repo

distclean-local:
	rm -rf tests/runtime-repo

include tests/Makefile-test-matrix.am.inc

test_scripts = ${TEST_MATRIX}
dist_test_scripts = ${TEST_MATRIX_DIST}
dist_installed_test_extra_scripts += ${TEST_MATRIX_EXTRA_DIST}

test_programs = \
	test-context \
	test-exports \
	test-instance \
	test-portal \
	testcommon \
	testlibrary \
	$(NULL)

test_extra_programs = \
	tests/hold-lock \
	tests/httpcache \
	tests/list-unused \
	tests/mock-flatpak \
	tests/test-authenticator \
	tests/test-portal-impl \
	tests/test-update-portal \
	$(NULL)

@VALGRIND_CHECK_RULES@
VALGRIND_SUPPRESSIONS_FILES=tests/flatpak.supp tests/glib.supp
EXTRA_DIST += tests/flatpak.supp tests/glib.supp tests/Makefile-test-matrix.am.inc tests/expand-test-matrix.sh tests/test-wrapper.sh
DISTCLEANFILES += \
	tests/services/org.freedesktop.Flatpak.service \
	tests/services/org.freedesktop.Flatpak.SystemHelper.service \
	tests/services/org.freedesktop.portal.Flatpak.service \
	tests/services/org.freedesktop.impl.portal.desktop.test.service \
	tests/services/org.flatpak.Authenticator.test.service \
	tests/services/org.flatpak.Authenticator.Oci.service \
	tests/share/xdg-desktop-portal/portals/test.portal \
	tests/package_version.txt \
	$(NULL)
