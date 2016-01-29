TEST_PROGS += testdb test-doc-portal
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


tests/services/org.freedesktop.portal.Documents.service: document-portal/org.freedesktop.portal.Documents.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" $< > $@

tests/services/org.freedesktop.XdgApp.service: session-helper/org.freedesktop.XdgApp.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" $< > $@

test_doc_portal_DEPENDENCIES = tests/services/org.freedesktop.portal.Documents.service  tests/services/org.freedesktop.XdgApp.service

check_PROGRAMS = $(TEST_PROGS)

TESTS=testdb test-doc-portal

@VALGRIND_CHECK_RULES@
VALGRIND_SUPPRESSIONS_FILES=tests/xdg-app.supp
EXTRA_DIST += tests/xdg-app.supp tests/dbs/no_tables
CLEANFILES += \
	tests/services/org.freedesktop.XdgApp.service \
	tests/services/org.freedesktop.portal.Documents.service \
	$(NULL)
