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

pwow_src := main.c camera.c input.c creatures.c equipment.c gamedata.c attached.c hud.c actionbar.c bags.c tooltip.c targeting.c questmarks.c questwindow.c ui_frames.c

wowauth_src := $(wildcard wowauth/*.c)
wowauth_objs := $(wowauth_src:%.c=%.o)
wowauth_deps := $(wowauth_objs:.o=.d)

wowauth/%.o: wowauth/%.c
	$(CC) $(CFLAGS) -MMD -MP $(GLOBAL_DEFINE) $(CINCLUDES) -c $< -o $@

-include $(wowauth_deps)

.PHONY: all clean compile_commands

all: pwow adt2wot wmo2wwb m22wwb m22gltf xml2ui test_auth resolve_creatures

compile_commands:
	make --always-make --dry-run

pwow: $(pwow_src) $(wowauth_objs) $(WORKDIR)/lib/libpengine.a
	$(CC) $(CFLAGS) $(GLOBAL_DEFINE) $(CINCLUDES) $(pwow_src) $(wowauth_objs) \
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

xml2ui: tools/xml2ui.c ui_layout.h
	$(CC) -O2 -Wall -Wextra tools/xml2ui.c -lexpat -o xml2ui

#INFO the layout comes from the user's own FrameXML, so the table is generated
#here and not committed, like data/
GAME_DATA ?= /root/sources/WoWee/Data/expansions/classic
UI_FRAMES := PlayerFrame TargetFrame MainMenuBar $(shell seq -f ActionButton%g 1 12)
UI_FRAMES += MainMenuBarBackpackButton $(shell seq -f CharacterBag%gSlot 0 3)
UI_FRAMES += $(shell seq -f ContainerFrame%g 1 5)

.PHONY: print-ui-frames
print-ui-frames:
	@echo $(UI_FRAMES)

ui_frames.c: xml2ui
	./xml2ui $(GAME_DATA) $@ $(UI_FRAMES) > /dev/null

m22gltf: tools/m22gltf.c
	$(CC) -O2 -Wall -Wextra tools/m22gltf.c -o m22gltf

clean:
	rm -f pwow adt2wot wmo2wwb m22wwb m22gltf xml2ui ui_frames.c test_auth resolve_creatures
	rm -f $(wowauth_objs) $(wowauth_deps)
