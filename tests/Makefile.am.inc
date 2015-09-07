TEST_PROGS += testdb test-doc-portal
testdb_CFLAGS = $(BASE_CFLAGS) -DDB_DIR=\"$(abs_srcdir)/tests/dbs\"
testdb_LDADD = \
             $(BASE_LIBS) \
             $(OSTREE_LIBS) \
             libglnx.la \
             libxdgapp.la \
             $(NULL)
testdb_SOURCES = tests/testdb.c

test_doc_portal_CFLAGS = $(BASE_CFLAGS) -DTEST_SERVICES=\""$(abs_top_builddir)/tests/services"\"
test_doc_portal_LDADD = \
             $(BASE_LIBS) \
             $(OSTREE_LIBS) \
             libglnx.la \
             libxdgapp.la \
             $(NULL)
test_doc_portal_SOURCES = tests/test-doc-portal.c $(xdp_dbus_built_sources)


tests/services/org.freedesktop.portal.Documents.service: document-portal/org.freedesktop.portal.Documents.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" $< > $@

tests/services/xdg-app-session.service: session-helper/xdg-app-session.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" $< > $@

test_doc_portal_DEPENDENCIES = tests/services/org.freedesktop.portal.Documents.service  tests/services/xdg-app-session.service

noinst_PROGRAMS = $(TEST_PROGS)

TESTS=testdb test-doc-portal

@VALGRIND_CHECK_RULES@
VALGRIND_SUPPRESSIONS_FILES=tests/xdg-app.supp
EXTRA_DIST+=tests/xdg-app.supp
