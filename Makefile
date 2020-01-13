all: sample

sample.h: variant-parse.py sample.gv
	./variant-parse.py --outfile sample.h --prefix=sample sample.gv

sample: sample.c sample.h
	gcc `pkg-config --cflags --libs glib-2.0` -g -Wall -o sample sample.c
