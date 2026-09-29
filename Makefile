WORKDIR := /root/pengine

include $(WORKDIR)/include.make

.PHONY: all clean

all: pwow adt2wot wmo2wwb m22wwb m22gltf test_auth resolve_creatures

pwow: main.c camera.c input.c creatures.c $(WORKDIR)/lib/libpengine.a
	$(CC) $(CFLAGS) $(GLOBAL_DEFINE) $(CINCLUDES) main.c camera.c input.c creatures.c \
		-L$(WORKDIR)/lib -lpengine $(LIBRARIES) -o pwow

test_auth: tools/test_auth.c $(WORKDIR)/lib/libpengine.a
	$(CC) $(CFLAGS) $(GLOBAL_DEFINE) $(CINCLUDES) tools/test_auth.c \
		-L$(WORKDIR)/lib -lpengine $(LIBRARIES) -o test_auth

resolve_creatures: tools/resolve_creatures.c $(WORKDIR)/lib/libpengine.a
	$(CC) $(CFLAGS) $(GLOBAL_DEFINE) $(CINCLUDES) tools/resolve_creatures.c \
		-L$(WORKDIR)/lib -lpengine $(LIBRARIES) -o resolve_creatures

adt2wot: tools/adt2wot.c
	$(CC) -O2 -Wall -Wextra tools/adt2wot.c -o adt2wot

wmo2wwb: tools/wmo2wwb.c
	$(CC) -O2 -Wall -Wextra tools/wmo2wwb.c -o wmo2wwb

m22wwb: tools/m22wwb.c
	$(CC) -O2 -Wall -Wextra tools/m22wwb.c -o m22wwb

m22gltf: tools/m22gltf.c
	$(CC) -O2 -Wall -Wextra tools/m22gltf.c -o m22gltf

clean:
	rm -f pwow adt2wot wmo2wwb m22wwb m22gltf test_auth
