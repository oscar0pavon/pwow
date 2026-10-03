#ifndef PWOW_GAMEDATA_H
#define PWOW_GAMEDATA_H

#include <stdbool.h>

//converts the game's own files into data/ the first time they are needed. an
//item's art is only named once the live server sends it, or a creature's
//CreatureDisplayInfoExtra.dbc row is read, so no prepare_*.sh script could
//have converted it ahead of time. both take the file's path under the game
//data and under data/, without its extension, like "item/texturecomponents/
//foottexture/boot_01_m", and say whether data/ has the converted file now:
//false if the game has no such file or the converter failed

//game/<base>.blp -> data/<base>.png
bool gamedata_ensure_png(const char *base);

//game/<base>.m2 -> data/<base>.glb and data/<base>.att, its clips named
bool gamedata_ensure_model(const char *base);

#endif
