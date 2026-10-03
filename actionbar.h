#ifndef PWOW_ACTIONBAR_H
#define PWOW_ACTIONBAR_H

#include <stdbool.h>

#include <wowauth/wowobject.h>

#define ACTIONBAR_BUTTONS 12

bool actionbar_init(const char *dbc_directory);

//puts the icons of the slots of the page the player is on into the frame, when
//the server's slots or the player's form changed since the last call
void actionbar_update(const PWowObjectState *state);

//the spell in slot 1 to 12 of the page shown, 0 if it holds none
unsigned actionbar_spell(const PWowObjectState *state, int slot);

#endif
