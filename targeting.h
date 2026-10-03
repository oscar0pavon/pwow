#ifndef PWOW_TARGETING_H
#define PWOW_TARGETING_H

#include <stdbool.h>

#include <cglm/cglm.h>
#include <wowauth/wowobject.h>

//choosing a creature with the pointer, and the target frame that shows the choice

//the direction of the ray from eye through a point of the screen. x and y are in pixels from
//the top left, width and height the size of the screen in pixels
void targeting_ray(const mat4 view, const mat4 projection, const vec3 eye, float x, float y,
                   float width, float height, vec3 direction);

//a point of the world as a fraction of the screen, from the top left; false behind the eye
bool targeting_project(const mat4 view, const mat4 projection, const vec3 point, float *x, float *y);

//the guid of the creature the ray goes through that is nearest to the eye, 0 for none
u64 targeting_pick(const PWowObjectState *state, const vec3 eye, const vec3 direction);

//shows the creature with this guid in the target frame, or hides the frame when it is gone
//or 0. returns false when a guid is no longer tracked, which a caller drops
bool targeting_update_frame(const PWowObjectState *state, u64 selected);

//how tall a creature stands, in yards, as it is drawn
float targeting_height(const PWowCreature *creature);

#endif
