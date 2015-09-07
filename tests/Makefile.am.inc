TEST_PROGS += testdb
testdb_CFLAGS = $(BASE_CFLAGS) -DDB_DIR=\"$(abs_srcdir)/tests/dbs\"
testdb_LDADD = \
             $(BASE_LIBS) \
             $(OSTREE_LIBS) \
             libglnx.la \
             libxdgapp.la \
             $(NULL)
testdb_SOURCES = tests/testdb.c


noinst_PROGRAMS = $(TEST_PROGS)

TESTS=testdb

@VALGRIND_CHECK_RULES@
VALGRIND_SUPPRESSIONS_FILES=tests/xdg-app.supp
EXTRA_DIST+=tests/xdg-app.supp
