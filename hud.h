#ifndef PWOW_HUD_H
#define PWOW_HUD_H

#include <stdbool.h>
#include <vulkan/vulkan_core.h>

#include <engine/renderer/render_target.h>

//the game's own unit frames, drawn from the table tools/xml2ui made out of
//FrameXML (ui_frames.c). a frame or a region is named as the XML names it

int hud_init(const char *texture_directory);

void hud_show(const char *name, bool shown);
//every node whose name starts with prefix, for the buff slots and the like
void hud_show_prefixed(const char *prefix, bool shown);
void hud_set_text(const char *name, const char *text);

//fraction of a status bar that is filled, 0 to 1, and its colour
void hud_set_bar(const char *name, float fraction, const float color[3]);

//the pictures first, then the text from inside the frame's one
//pe_text_begin()/pe_text_end() pair, so the text lies over them
void hud_draw_images(PRenderTarget *target, VkCommandBuffer command,
                     uint32_t image_index);
void hud_draw_text();

#endif
