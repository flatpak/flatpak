TESTS_ENVIRONMENT += OT_TESTS_DEBUG=1 \
	XDG_APP_TRIGGERSDIR=$$(cd $(top_srcdir) && pwd)/triggers \
	XDG_APP_BWRAP=$$(cd $(top_builddir) && pwd)/bwrap \
	XDG_APP_DBUSPROXY=$$(cd $(top_builddir) && pwd)/xdg-dbus-proxy \
	GI_TYPELIB_PATH=$$(cd $(top_builddir) && pwd) \
	LD_LIBRARY_PATH=$$(cd $(top_builddir)/.libs && pwd) \
	PATH=$$(cd $(top_builddir) && pwd):$${PATH} \
	$(NULL)

testdb_CFLAGS = $(BASE_CFLAGS) -DDB_DIR=\"$(abs_srcdir)/tests/dbs\"
testdb_LDADD = \
             $(BASE_LIBS) \
             $(OSTREE_LIBS) \
             libglnx.la \
             libxdgapp-common.la \
             $(NULL)
testdb_SOURCES = tests/testdb.c

test_doc_portal_CFLAGS = $(BASE_CFLAGS) -DTEST_SERVICES=\""$(abs_top_builddir)/tests/services"\" -DDOC_PORTAL=\""$(abs_top_builddir)/xdg-document-portal"\"
test_doc_portal_LDADD = \
             $(BASE_LIBS) \
             $(OSTREE_LIBS) \
             libglnx.la \
             libxdgapp-common.la \
             $(NULL)
test_doc_portal_SOURCES = tests/test-doc-portal.c $(xdp_dbus_built_sources)

EXTRA_test_doc_portal_DEPENDENCIES = tests/services/org.freedesktop.portal.Documents.service  tests/services/org.freedesktop.XdgApp.service

tests/services/org.freedesktop.portal.Documents.service: document-portal/org.freedesktop.portal.Documents.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" $< > $@

tests/services/org.freedesktop.XdgApp.service: session-helper/org.freedesktop.XdgApp.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" $< > $@

tests/libtest.sh: tests/services/org.freedesktop.portal.Documents.service  tests/services/org.freedesktop.XdgApp.service

install-test-data-hook:
if ENABLE_INSTALLED_TESTS
	mkdir -p $(DESTDIR)$(installed_testdir)/services
	ln -sf $(dbus_servicedir)/org.freedesktop.XdgApp.service $(DESTDIR)$(installed_testdir)/services/
	ln -sf $(dbus_servicedir)/org.freedesktop.portal.Documents.service $(DESTDIR)$(installed_testdir)/services/
endif

tests/package_version.txt: Makefile
	echo $(PACKAGE_VERSION) > tests/package_version.txt

tests/test-basic.sh: tests/package_version.txt

installed_test_SCRIPTS += \
	buildutil/tap-driver.sh \
	tests/test-configure \
	tests/package_version.txt \
	tests/make-test-app.sh \
	tests/make-test-runtime.sh \
	tests/make-test-bundles.sh \
	tests/libtest.sh \
	$(NULL)

installed_test_data = \
	tests/org.test.Hello.png \
	tests/test.json \
	tests/session.conf.in \
	$(NULL)

EXTRA_DIST += $(installed_test_SCRIPTS) $(installed_test_data)

test_scripts = \
	tests/test-basic.sh \
	tests/test-run.sh \
	tests/test-builder.sh \
	$(NULL)

test_programs = testdb test-doc-portal

@VALGRIND_CHECK_RULES@
VALGRIND_SUPPRESSIONS_FILES=tests/xdg-app.supp
EXTRA_DIST += tests/xdg-app.supp tests/dbs/no_tables
CLEANFILES += \
	tests/services/org.freedesktop.XdgApp.service \
	tests/services/org.freedesktop.portal.Documents.service \
	$(NULL)
