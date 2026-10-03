#ifndef PWOW_QUESTWINDOW_H
#define PWOW_QUESTWINDOW_H

#include <stdbool.h>

#include <cglm/cglm.h>
#include <wowauth/wowobject.h>
#include <wowauth/wowworld.h>

#include "hud.h"

//the window an NPC's gossip and quests are read in: the game's QuestFrame and
//GossipFrame, laid out here and not from their XML (their text scrolls, wraps and
//is filled by Lua), on the hud's canvas. it shows whatever state->dialog holds

//the character's name, race, class and gender (the game's ids), which the quest
//texts name with $N, $R, $C and $g
void quest_window_set_player(const char *name, unsigned race, unsigned character_class,
                             unsigned gender);

//lays the window out on the canvas for this frame; closes it when the player walks
//away from the NPC. call after hud_canvas_clear()
void quest_window_update(PWowObjectState *state, const vec3 player_position);

//what a click on the window did; false for any other
bool quest_window_click(HudClick click, PWowObjectState *state, PWowWorld *world);

//Esc, or taking the NPC away: true if a window was open
bool quest_window_close(PWowObjectState *state);

//the wheel over the window scrolls its text; true when it did
bool quest_window_scroll(int notches);

//the entry of the item under a reward button of the window, 0 for any other name
unsigned quest_window_item_under(const char *name, const PWowObjectState *state);

#endif
