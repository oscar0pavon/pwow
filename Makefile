WORKDIR ?= ../pengine

include $(WORKDIR)/include.make

CINCLUDES += -I.

#INFO wowauth needs OpenSSL's bignum/SHA1 for SRP6. named by full path, not
#-lcrypto: this host also has an old OpenSSL 1.1 under /usr/local/lib, and
#the plain flag's default search order picks that one over /usr/lib's 3.x -
#same headers, wrong library, that only shows up at link time
WOWAUTH_LIBRARIES := /usr/lib/libcrypto.so

#INFO wowobject.c decompresses SMSG_COMPRESSED_UPDATE_OBJECT with zlib
WOWAUTH_LIBRARIES += -lz

wowauth_src := $(wildcard wowauth/*.c)
wowauth_objs := $(wowauth_src:%.c=%.o)
wowauth_deps := $(wowauth_objs:.o=.d)

wowauth/%.o: wowauth/%.c
	$(CC) $(CFLAGS) -MMD -MP $(GLOBAL_DEFINE) $(CINCLUDES) -c $< -o $@

-include $(wowauth_deps)

.PHONY: all clean compile_commands

all: pwow adt2wot wmo2wwb m22wwb m22gltf test_auth resolve_creatures

compile_commands:
	make --always-make --dry-run

pwow: main.c camera.c input.c creatures.c $(wowauth_objs) $(WORKDIR)/lib/libpengine.a
	$(CC) $(CFLAGS) $(GLOBAL_DEFINE) $(CINCLUDES) main.c camera.c input.c creatures.c $(wowauth_objs) \
		-L$(WORKDIR)/lib -lpengine $(LIBRARIES) $(WOWAUTH_LIBRARIES) -o pwow

test_auth: tools/test_auth.c $(wowauth_objs) $(WORKDIR)/lib/libpengine.a
	$(CC) $(CFLAGS) $(GLOBAL_DEFINE) $(CINCLUDES) tools/test_auth.c $(wowauth_objs) \
		-L$(WORKDIR)/lib -lpengine $(LIBRARIES) $(WOWAUTH_LIBRARIES) -o test_auth

resolve_creatures: tools/resolve_creatures.c $(wowauth_objs) $(WORKDIR)/lib/libpengine.a
	$(CC) $(CFLAGS) $(GLOBAL_DEFINE) $(CINCLUDES) tools/resolve_creatures.c $(wowauth_objs) \
		-L$(WORKDIR)/lib -lpengine $(LIBRARIES) $(WOWAUTH_LIBRARIES) -o resolve_creatures

adt2wot: tools/adt2wot.c
	$(CC) -O2 -Wall -Wextra tools/adt2wot.c -o adt2wot

wmo2wwb: tools/wmo2wwb.c
	$(CC) -O2 -Wall -Wextra tools/wmo2wwb.c -o wmo2wwb

m22wwb: tools/m22wwb.c
	$(CC) -O2 -Wall -Wextra tools/m22wwb.c -o m22wwb

m22gltf: tools/m22gltf.c
	$(CC) -O2 -Wall -Wextra tools/m22gltf.c -o m22gltf

clean:
	rm -f pwow adt2wot wmo2wwb m22wwb m22gltf test_auth resolve_creatures
	rm -f $(wowauth_objs) $(wowauth_deps)
