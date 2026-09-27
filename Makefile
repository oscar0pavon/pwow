WORKDIR := /root/pengine

include $(WORKDIR)/include.make

.PHONY: all clean

all: pwow adt2wot wmo2wwb m22wwb

pwow: pwow.c $(WORKDIR)/lib/libpengine.a
	$(CC) $(CFLAGS) $(GLOBAL_DEFINE) $(CINCLUDES) pwow.c \
		-L$(WORKDIR)/lib -lpengine $(LIBRARIES) -o pwow

adt2wot: tools/adt2wot.c
	$(CC) -O2 -Wall -Wextra tools/adt2wot.c -o adt2wot

wmo2wwb: tools/wmo2wwb.c
	$(CC) -O2 -Wall -Wextra tools/wmo2wwb.c -o wmo2wwb

m22wwb: tools/m22wwb.c
	$(CC) -O2 -Wall -Wextra tools/m22wwb.c -o m22wwb

clean:
	rm -f pwow adt2wot wmo2wwb m22wwb
