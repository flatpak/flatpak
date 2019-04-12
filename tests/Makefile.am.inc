AM_TESTS_ENVIRONMENT = FLATPAK_TESTS_DEBUG=1 \
	FLATPAK_CONFIG_DIR=/dev/null \
	FLATPAK_TRIGGERSDIR=$$(cd $(top_srcdir) && pwd)/triggers \
	FLATPAK_VALIDATE_ICON=$$(cd $(top_builddir) && pwd)/flatpak-validate-icon \
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

testlibrary_CFLAGS = \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	$(OSTREE_CFLAGS) \
	-DFLATPAK_COMPILATION \
	$(NULL)
testlibrary_LDADD = \
	$(AM_LDADD) \
	$(BASE_LIBS) \
	$(OSTREE_LIBS) \
	libglnx.la \
	libflatpak.la \
	$(NULL)
testlibrary_SOURCES = tests/testlibrary.c

testcommon_CFLAGS = \
	$(AM_CFLAGS) \
	$(BASE_CFLAGS) \
	$(OSTREE_CFLAGS) \
	$(SOUP_CFLAGS) \
	$(JSON_CFLAGS) \
	$(APPSTREAM_GLIB_CFLAGS) \
	-DFLATPAK_COMPILATION \
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
	libglnx.la \
	libflatpak-common.la \
	libflatpak-app.la \
	$(NULL)
testcommon_SOURCES = tests/testcommon.c

tests_httpcache_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) $(OSTREE_CFLAGS) $(SOUP_CFLAGS) $(JSON_CFLAGS) $(APPSTREAM_GLIB_CFLAGS) \
	-DFLATPAK_COMPILATION \
        -DLOCALEDIR=\"$(localedir)\"
tests_httpcache_LDADD = $(AM_LDADD) $(BASE_LIBS) $(OSTREE_LIBS) $(SOUP_LIBS) $(JSON_LIBS) $(APPSTREAM_GLIB_LIBS) \
	libglnx.la libflatpak-common.la

tests/services/org.freedesktop.Flatpak.service: session-helper/org.freedesktop.Flatpak.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" $< > $@

tests/services/org.freedesktop.Flatpak.SystemHelper.service: system-helper/org.freedesktop.Flatpak.SystemHelper.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" -e "s|\@extraargs\@| --session --no-idle-exit|" $< > $@

tests/libtest.sh: tests/services/org.freedesktop.Flatpak.service tests/services/org.freedesktop.Flatpak.SystemHelper.service

install-test-data-hook:
if ENABLE_INSTALLED_TESTS
	mkdir -p $(DESTDIR)$(installed_testdir)/services
	ln -sf $(dbus_servicedir)/org.freedesktop.Flatpak.service $(DESTDIR)$(installed_testdir)/services/
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" -e "s|\@extraargs\@| --session --no-idle-exit|" $(top_srcdir)/system-helper/org.freedesktop.Flatpak.SystemHelper.service.in > $(DESTDIR)$(installed_testdir)/services/org.freedesktop.Flatpak.SystemHelper.service
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
	tests/make-test-bundles.sh \
	tests/test-webserver.sh \
	tests/test-wrapper.sh \
	$(NULL)

dist_installed_test_data = \
	tests/libtest.sh \
	tests/org.test.Hello.png \
	tests/package_version.txt \
	tests/session.conf.in \
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
	tests/test-run.sh{{user+system},{nodeltas+deltas}} \
	tests/test-info.sh{user+system} \
	tests/test-repo.sh{user+system+collections+collections-server-only} \
	tests/test-extensions.sh \
	tests/test-bundle.sh{user+system} \
	tests/test-oci.sh \
	tests/test-oci-registry.sh{user+system} \
	tests/test-unsigned-summaries.sh \
	tests/test-update-remote-configuration.sh \
	tests/test-override.sh \
	$(NULL)

update-test-matrix:
	$(srcdir)/tests/expand-test-matrix.sh "$(TEST_MATRIX_SOURCE)" > tests/Makefile-test-matrix.am.inc

tests/test-%.wrap:
	@true

tests/runtime-repo: tests/make-test-runtime.sh flatpak
	rm -rf tests/runtime-repo
	PATH=$(abs_top_builddir):$${PATH} $(top_srcdir)/tests/make-test-runtime.sh tests/runtime-repo org.test.Platform ""

check_DATA += tests/runtime-repo

distclean-local:
	rm -rf tests/runtime-repo

include tests/Makefile-test-matrix.am.inc

test_scripts = ${TEST_MATRIX}
dist_test_scripts = ${TEST_MATRIX_DIST}
dist_installed_test_extra_scripts += ${TEST_MATRIX_EXTRA_DIST}

test_programs = testlibrary testcommon
test_extra_programs = tests/httpcache

@VALGRIND_CHECK_RULES@
VALGRIND_SUPPRESSIONS_FILES=tests/flatpak.supp tests/glib.supp
EXTRA_DIST += tests/flatpak.supp tests/glib.supp tests/Makefile-test-matrix.am.inc tests/expand-test-matrix.sh tests/test-wrapper.sh
DISTCLEANFILES += \
	tests/services/org.freedesktop.Flatpak.service \
	tests/services/org.freedesktop.Flatpak.SystemHelper.service \
	tests/package_version.txt \
	$(NULL)
