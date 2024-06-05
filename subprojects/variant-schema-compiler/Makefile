PREFIX = /usr/local
CC ?= gcc
CFLAGS ?= -g -Wall
PKG_CONFIG ?= pkg-config
GLIB_CFLAGS = `$(PKG_CONFIG) --cflags glib-2.0`
GLIB_LIBS = `$(PKG_CONFIG) --libs glib-2.0`

all: sample ostree_test performance selftests

sample.h: variant-schema-compiler sample.gv
	./variant-schema-compiler --internal-validation --outfile sample-impl.h --outfile-header=sample.h --prefix=sample sample.gv

sample: sample.c sample.h
	$(CC) $(GLIB_CFLAGS) $(CFLAGS) -o sample sample.c $(GLIB_LIBS)

performance.h: variant-schema-compiler performance.gv
	./variant-schema-compiler --outfile performance.h --prefix=performance performance.gv

performance: performance.c performance.h
	$(CC) $(GLIB_CFLAGS) $(CFLAGS) -O2 -o performance performance.c $(GLIB_LIBS)

ostree_test.h: variant-schema-compiler ostree.gv
	./variant-schema-compiler --outfile ostree_test.h --prefix=ot ostree.gv

ostree_test: ostree_test.c ostree_test.h
	$(CC) $(GLIB_CFLAGS) $(CFLAGS) -o ostree_test ostree_test.c $(GLIB_LIBS)

selftests: selftests.c ostree_test.h
	$(CC) $(GLIB_CFLAGS) $(CFLAGS) -o selftests -O2 selftests.c $(GLIB_LIBS)

clean:
	rm -f sample.h sample performance.h performance ostree_test.h ostree_test

.PHONY: install
install: variant-schema-compiler
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -T $< $(DESTDIR)$(PREFIX)/bin/variant-schema-compiler

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/variant-schema-compiler
