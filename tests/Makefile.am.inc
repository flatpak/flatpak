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
