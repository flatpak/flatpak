TESTS_ENVIRONMENT += FLATPAK_TESTS_DEBUG=1 \
	FLATPAK_TRIGGERSDIR=$$(cd $(top_srcdir) && pwd)/triggers \
	FLATPAK_DBUSPROXY=$$(cd $(top_builddir) && pwd)/flatpak-dbus-proxy \
	GI_TYPELIB_PATH=$$(cd $(top_builddir) && pwd) \
	LD_LIBRARY_PATH=$$(cd $(top_builddir)/.libs && pwd) \
	PATH=$$(cd $(top_builddir) && pwd):$${PATH} \
	$(NULL)

if !WITH_SYSTEM_BWRAP
TESTS_ENVIRONMENT += FLATPAK_BWRAP=$$(cd $(top_builddir) && pwd)/flatpak-bwrap
endif

testdb_CFLAGS = $(BASE_CFLAGS)
testdb_LDADD = \
             $(BASE_LIBS) \
             $(OSTREE_LIBS) \
             libglnx.la \
             libflatpak-common.la \
             $(NULL)
testdb_SOURCES = tests/testdb.c

test_doc_portal_CFLAGS = $(BASE_CFLAGS)
test_doc_portal_LDADD = \
             $(BASE_LIBS) \
             $(OSTREE_LIBS) \
             libglnx.la \
             libflatpak-common.la \
             $(NULL)
test_doc_portal_SOURCES = tests/test-doc-portal.c $(xdp_dbus_built_sources)

EXTRA_test_doc_portal_DEPENDENCIES = tests/services/org.freedesktop.impl.portal.PermissionStore.service tests/services/org.freedesktop.portal.Documents.service  tests/services/org.freedesktop.Flatpak.service tests/services/org.freedesktop.Flatpak.SystemHelper.service

tests/services/org.freedesktop.portal.Documents.service: document-portal/org.freedesktop.portal.Documents.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" $< > $@

tests/services/org.freedesktop.impl.portal.PermissionStore.service: permission-store/org.freedesktop.impl.portal.PermissionStore.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" $< > $@

tests/services/org.freedesktop.Flatpak.service: session-helper/org.freedesktop.Flatpak.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" $< > $@

tests/services/org.freedesktop.Flatpak.SystemHelper.service: system-helper/org.freedesktop.Flatpak.SystemHelper.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" -e "s|\@extraargs\@| --session --no-idle-exit|" $< > $@

tests/libtest.sh: tests/services/org.freedesktop.impl.portal.PermissionStore.service tests/services/org.freedesktop.portal.Documents.service  tests/services/org.freedesktop.Flatpak.service

install-test-data-hook:
if ENABLE_INSTALLED_TESTS
	mkdir -p $(DESTDIR)$(installed_testdir)/services
	ln -sf $(dbus_servicedir)/org.freedesktop.Flatpak.service $(DESTDIR)$(installed_testdir)/services/
	ln -sf $(dbus_servicedir)/org.freedesktop.portal.Documents.service $(DESTDIR)$(installed_testdir)/services/
	ln -sf $(dbus_servicedir)/org.freedesktop.impl.portal.PermissionStore.service $(DESTDIR)$(installed_testdir)/services/
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" -e "s|\@extraargs\@| --session --no-idle-exit|" $(top_srcdir)/system-helper/org.freedesktop.Flatpak.SystemHelper.service.in > $(DESTDIR)$(installed_testdir)/services/org.freedesktop.Flatpak.SystemHelper.service
endif

tests/package_version.txt: Makefile
	echo $(PACKAGE_VERSION) > tests/package_version.txt

tests/test-basic.sh: tests/package_version.txt

dist_installed_test_extra_scripts += \
	buildutil/tap-driver.sh \
	tests/test-configure \
	tests/make-test-app.sh \
	tests/make-test-runtime.sh \
	tests/make-test-bundles.sh \
	$(NULL)

installed_test_data = \
	tests/libtest.sh \
	tests/org.test.Hello.png \
	tests/package_version.txt \
	tests/test.json \
	tests/session.conf.in \
	tests/0001-Add-test-logo.patch \
	$(NULL)

installed_test_keyringdir = $(installed_testdir)/test-keyring
installed_test_dbsdir = $(installed_testdir)/dbs

if ENABLE_INSTALLED_TESTS
dist_installed_test_keyring_DATA = \
	tests/test-keyring/README \
	tests/test-keyring/pubring.gpg \
	tests/test-keyring/secring.gpg \
	$(NULL)
dist_installed_test_dbs_DATA = tests/dbs/no_tables
endif

EXTRA_DIST += $(installed_test_data)

dist_test_scripts = \
	tests/test-basic.sh \
	tests/test-run.sh \
	tests/test-run-system.sh \
	tests/test-bundle.sh \
	tests/test-bundle-system.sh \
	tests/test-builder.sh \
	$(NULL)

test_programs = testdb test-doc-portal

@VALGRIND_CHECK_RULES@
VALGRIND_SUPPRESSIONS_FILES=tests/flatpak.supp tests/glib.supp
EXTRA_DIST += tests/flatpak.supp tests/glib.supp
DISTCLEANFILES += \
	tests/services/org.freedesktop.Flatpak.service \
	tests/services/org.freedesktop.portal.Documents.service \
	tests/services/org.freedesktop.impl.portal.PermissionStore.service \
	tests/services/org.freedesktop.Flatpak.SystemHelper.service \
	tests/package_version.txt \
	$(NULL)
