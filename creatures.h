#ifndef PWOW_CREATURES_H
#define PWOW_CREATURES_H

#include <cglm/cglm.h>
#include <engine/renderer/vulkan.h>
#include <wowauth/wowobject.h>

//renders the creatures pe_wowworld_poll() tracks in a PWowObjectState. a
//"simple" one has ExtendedDisplayInfoID 0 in CreatureDisplayInfo.dbc, so its
//look is its own model and texture. a humanoid has a CreatureDisplayInfoExtra
//row instead (race/gender/skin/face/hair/equipment) and is dressed through
//equipment.h like the player; only a Tauren is drawn so far, any other
//humanoid is tracked in npc_state but never given an instance. models and
//textures must already be converted by prepare_creatures.sh and
//prepare_character.sh; one that is not is silently skipped

//the size CreatureDisplayInfo.dbc asks for a display to be drawn at, 1 for one
//it does not know
float creatures_display_scale(u32 display_id);

//creates the shared skinned shader every creature model draws with. call
//once, after pe_vk_init (same requirement pe_text_init has)
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
