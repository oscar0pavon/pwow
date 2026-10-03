#ifndef PWOW_SELECTION_H
#define PWOW_SELECTION_H

#include <cglm/cglm.h>
#include <engine/terrain/terrain_world.h>
#include <vulkan/vulkan_core.h>
#include <wowauth/wowobject.h>

//what shows which creature is selected: a green ring on the ground under it, and its name over its head

//call once, after pe_ui_init()
int selection_init(void);

//the name over the head, on the hud's canvas; call after hud_canvas_clear()
void selection_label(const PWowObjectState *state, u64 selected, const mat4 view, const mat4 projection);

//the ring, lying on the terrain; call inside the scene's render pass after the models
void selection_draw_ring(const PWowObjectState *state, u64 selected, PTerrainWorld *world,
                         const mat4 view, const mat4 projection, VkCommandBuffer command,
                         uint32_t image_index);

#endif
