#ifndef PWOW_CREATURES_H
#define PWOW_CREATURES_H

#include <cglm/cglm.h>
#include <engine/renderer/vulkan.h>
#include <engine/wowauth/wowobject.h>

//renders the "simple" (non-humanoid) creatures pe_wowworld_poll() tracks in
//a PWowObjectState: one whose ExtendedDisplayInfoID is 0 in CreatureDisplayInfo.dbc,
//meaning its look comes from its own model and texture rather than
//CreatureDisplayInfoExtra's race/gender/skin/face/hair/equipment - the same
//pipeline a player character uses, which this does not drive. a humanoid
//creature is tracked in npc_state like any other but is silently never
//given an instance here. models/textures must already be converted by
//prepare_creatures.sh; one that is not is silently skipped too

//creates the shared plain-diffuse shader every creature model draws with.
//call once, after pe_vk_init (same requirement pe_text_init has)
void creatures_init(void);

//call once a frame, after pe_wowworld_poll(): creates a GPU instance for
//each newly-seen simple creature (loading its model/texture the first time
//that species is needed), updates the position/facing of ones already
//tracked, and releases any instance whose creature is no longer in
//npc_state
void creatures_sync(const PWowObjectState *npc_state);

//call once a frame, inside the same render pass player_draw() draws into
void creatures_draw(VkCommandBuffer *command, uint32_t image_index,
                    mat4 view, mat4 projection);

#endif
