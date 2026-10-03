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
//the width of a text in the units the frames are laid out in
float hud_text_width(const char *text);
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

//a tooltip is a few lines, drawn by the pointer until it is cleared; the
//caller fills it for what the pointer is over, every frame
void hud_tooltip_clear();
void hud_tooltip_line(const char *text, const float color[3]);

//the name of the button or frame the pointer is over, NULL if it is over none
const char *hud_hovered_name();

//an icon the pointer carries, drawn on it over everything; NULL for none
void hud_set_cursor(const char *texture);

//the pointer is over a button, or holds one: not the world's
bool hud_mouse_over_ui();

//what the code lays out by itself, for the windows the game builds with Lua at
//run time: pictures, text and the regions that take the pointer, in the units of
//the frames with the origin at the top left and y down. the code clears it and
//fills it again every update; it is drawn over the frames and under the tooltip
void hud_canvas_clear();
//texture NULL for a plain colour. tex_coords left, right, top, bottom, NULL for the whole picture
void hud_canvas_picture(const char *texture, float left, float top, float width, float height,
                        const float tex_coords[4], const float color[4], bool additive);
//top is the top of the line; one size for all text. a shadow is for text over the world, not over paper
void hud_canvas_text(const char *text, float left, float top, const float color[3], bool shadow);
//a region takes the pointer, and a clickable one answers a click with its name
void hud_canvas_region(const char *name, float left, float top, float width, float height,
                       bool clickable);
bool hud_canvas_hovered(const char *name);
//the left button is down on it and the pointer still over it
bool hud_canvas_held(const char *name);
//the size of the screen in the units of the frames, and the pixels of one unit
void hud_screen_size(float *width, float *height);
float hud_scale();
float hud_line_height();

//a line over the world for a few seconds, the game's UIErrorsFrame
void hud_notice(const char *text, const float color[3]);
void hud_notice_tick(float seconds);

//the pictures first, then the text from inside the frame's one
//pe_text_begin()/pe_text_end() pair, so the text lies over them
void hud_draw_images(PRenderTarget *target, VkCommandBuffer command,
                     uint32_t image_index);
void hud_draw_text();

#endif
