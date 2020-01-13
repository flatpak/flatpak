all: sample ostree_test

sample.h: variant-schema-compiler sample.gv
	./variant-schema-compiler --outfile sample.h --prefix=sample sample.gv

sample: sample.c sample.h
	gcc `pkg-config --cflags --libs glib-2.0` -g -Wall -o sample sample.c

ostree_test.h: variant-schema-compiler ostree.gv
	./variant-schema-compiler --outfile ostree_test.h --prefix=ot ostree.gv

ostree_test: ostree_test.c ostree_test.h
	gcc `pkg-config --cflags --libs glib-2.0` -g -Wall -o ostree_test ostree_test.c

clean:
	rm -f sample.h sample ostree_test.h ostree_test
