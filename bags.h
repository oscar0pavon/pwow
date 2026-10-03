#ifndef PWOW_BAGS_H
#define PWOW_BAGS_H

#include <stdbool.h>

#include <wowauth/wowobject.h>
#include <wowauth/wowworld.h>

#include "hud.h"

//the backpack and the four bags of the bar, as the game's ContainerFrame.lua
//and MainMenuBarBagButtons.lua lay them out and toggle them, on the frames
//xml2ui made

bool bags_init(const char *dbc_directory);

//the bag buttons' icons, and the contents of the open bags, once the
//inventory changed
void bags_update(const PWowInventory *inventory);

//opens the bags that are shut and shuts them if any is open, as the key B
void bags_toggle_all(const PWowInventory *inventory);

//what a click on a bag button, a bag's close button or an item did; false for
//anything else. a right click on an item uses it, or equips it
bool bags_click(HudClick click, const PWowInventory *inventory, PWowWorld *world);

#endif
