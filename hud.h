#ifndef PWOW_HUD_H
#define PWOW_HUD_H

#include <stdbool.h>
#include <vulkan/vulkan_core.h>

#include <engine/renderer/render_target.h>

//the game's own unit frames, drawn from the table tools/xml2ui made out of
//FrameXML (ui_frames.c). a frame or a region is named as the XML names it

int hud_init(const char *texture_directory);

void hud_show(const char *name, bool shown);
//the nodes by index, for what changes often: -1 if there is no such name.
//size, texture coordinates and the anchor are what the game's Lua changes at run
//time, as a bag's frame does to fit its slots
int hud_node(const char *name);
void hud_set_size(int node, float width, float height);
void hud_set_tex_coords(int node, float left, float right, float top, float bottom);
void hud_set_anchor(int node, int point, int relative, int relative_point, float x, float y);
void hud_show_node(int node, bool shown);
void hud_set_node_texture(int node, const char *texture);
void hud_set_node_text(int node, const char *text);

//every node whose name starts with prefix, for the buff slots and the like
void hud_show_prefixed(const char *prefix, bool shown);
//a texture of a node, for what Lua swaps at run time
void hud_set_texture(const char *name, const char *texture);
void hud_set_text(const char *name, const char *text);

//fraction of a status bar that is filled, 0 to 1, and its colour
void hud_set_bar(const char *name, float fraction, const float color[3]);

struct PWowObjectState;

//the player frame and the XP bar from what the server last said, once it said
//anything: health, the bar of its kind of power, level and experience
void hud_update_player(const struct PWowObjectState *state);

typedef struct HudClick {
  const char *name; //NULL for no click
  int button;       //1 left, 2 right
} HudClick;

//the pointer in window pixels. shows the highlight of the button under it and
//the pushed look of the one held with the left button, and returns the button
//that was clicked (pressed and let go over it) since the last call
HudClick hud_update_mouse(float mouse_x, float mouse_y, bool left_down, bool right_down);

//the pointer is over a button, or holds one: not the world's
bool hud_mouse_over_ui();

//the pictures first, then the text from inside the frame's one
//pe_text_begin()/pe_text_end() pair, so the text lies over them
void hud_draw_images(PRenderTarget *target, VkCommandBuffer command,
                     uint32_t image_index);
void hud_draw_text();

#endif
