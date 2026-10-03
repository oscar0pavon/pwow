#ifndef PWOW_QUESTMARKS_H
#define PWOW_QUESTMARKS_H

#include <cglm/cglm.h>
#include <wowauth/wowobject.h>

//the sign over the head of an NPC that has a quest: a yellow ! for one to take, a ? for one to
//hand in, a grey ? for one under way. puts them on the canvas, so call after hud_canvas_clear()
void questmarks_update(const PWowObjectState *state, const mat4 view, const mat4 projection,
                       const vec3 player_position);

#endif
