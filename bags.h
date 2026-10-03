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

//puts down what the pointer carries without dropping it anywhere
void bags_cancel_cursor(const PWowInventory *inventory);

//the entry of the item under a container frame's item button or a bag button of the bar, 0 for an
//empty one or for any other name; is_bag_button says it was one of the bar's
u32 bags_item_under(const char *name, const PWowInventory *inventory, bool *is_bag_button);

//the icon of an item kind, interface/icons/<name>.png, once its template is known
bool bags_item_icon(const PWowInventory *inventory, u32 entry, char *out, size_t size);

#endif
