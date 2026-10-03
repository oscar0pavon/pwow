#ifndef PWOW_TOOLTIP_H
#define PWOW_TOOLTIP_H

#include <wowauth/wowobject.h>

//fills the tooltip for what the pointer is over, or clears it: a spell of the action bar, an
//item of a bag, a bag button. call every frame, after hud_update_mouse()
void tooltip_update(const PWowObjectState *state);

#endif
