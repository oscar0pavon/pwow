#include "hud.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <engine/text.h>
#include <engine/ui.h>
#include <engine/utils.h>
#include <wowauth/wowobject.h>

#include "ui_layout.h"

//the game lays its interface out in units of a screen 768 high
#define UI_HEIGHT 768.f
#define TEXT_MAX 64
#define SHADOW_OFFSET 1.f

typedef struct Rect {
  float left, bottom, width, height;
} Rect;

typedef struct NodeState {
  PUiImage *image;
  bool shown;
  char text[TEXT_MAX];
  float fraction;
  float bar_color[3];
  bool has_bar_color;
  Rect rect;
  bool laid_out;
  bool has_size;
  float width, height;
  bool has_tex_coords;
  float tex_coords[4];
  bool has_anchor;
  UiAnchorDef anchor;
} NodeState;

#define HUD_NODES_MAX 8192

static NodeState states[HUD_NODES_MAX];
static char texture_directory[256];
static float pointer_x, pointer_y;
static PUiImage *cursor_image;
static float ui_width;
static float ui_height = UI_HEIGHT;
static float ui_scale = 1.f;

void hud_draw_tooltip_box();
void hud_draw_tooltip_text();
static void draw_canvas_pictures();
static void draw_canvas_text();
static void draw_notice();

static int find_node(const char *name) {
  for (int i = 0; i < ui_node_count; i++)
    if (ui_nodes[i].name[0] && strcmp(ui_nodes[i].name, name) == 0)
      return i;
  return -1;
}

int hud_init(const char *directory) {
  snprintf(texture_directory, sizeof(texture_directory), "%s", directory);
  for (int i = 0; i < ui_node_count; i++) {
    states[i].shown = !ui_nodes[i].hidden;
    states[i].fraction = 1.f;

    if (!ui_nodes[i].texture)
      continue;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", texture_directory, ui_nodes[i].texture);
    states[i].image = pe_ui_image(path);
  }
  return 1;
}

int hud_node(const char *name) { return find_node(name); }

float hud_text_width(const char *text) { return pe_text_width(text) / ui_scale; }

void hud_set_size(int node, float width, float height) {
  states[node].has_size = true;
  states[node].width = width;
  states[node].height = height;
}

void hud_set_tex_coords(int node, float left, float right, float top, float bottom) {
  states[node].has_tex_coords = true;
  states[node].tex_coords[0] = left;
  states[node].tex_coords[1] = right;
  states[node].tex_coords[2] = top;
  states[node].tex_coords[3] = bottom;
}

//the node's anchors become this one: point of the node on relative_point of
//the relative node, or of the screen for UI_SCREEN
void hud_set_anchor(int node, int point, int relative, int relative_point, float x, float y) {
  states[node].has_anchor = true;
  states[node].anchor = (UiAnchorDef){point, relative, relative_point, x, y};
}

void hud_show_node(int node, bool shown) { states[node].shown = shown; }

void hud_set_node_texture(int node, const char *texture) {
  if (!texture) {
    states[node].image = NULL;
    return;
  }

  char path[512];
  snprintf(path, sizeof(path), "%s/%s", texture_directory, texture);
  PUiImage *image = pe_ui_image(path);
  if (image)
    states[node].image = image;
}

void hud_set_node_text(int node, const char *text) {
  snprintf(states[node].text, TEXT_MAX, "%s", text);
}

void hud_show(const char *name, bool shown) {
  int node = find_node(name);
  if (node >= 0)
    states[node].shown = shown;
}

void hud_show_prefixed(const char *prefix, bool shown) {
  size_t length = strlen(prefix);
  for (int i = 0; i < ui_node_count; i++)
    if (strncmp(ui_nodes[i].name, prefix, length) == 0)
      states[i].shown = shown;
}

void hud_set_texture(const char *name, const char *texture) {
  int node = find_node(name);
  if (node < 0)
    return;

  if (!texture) {
    states[node].image = NULL;
    return;
  }

  char path[512];
  snprintf(path, sizeof(path), "%s/%s", texture_directory, texture);
  PUiImage *image = pe_ui_image(path);
  if (image)
    states[node].image = image;
}

void hud_set_text(const char *name, const char *text) {
  int node = find_node(name);
  if (node >= 0)
    snprintf(states[node].text, TEXT_MAX, "%s", text);
}

void hud_set_bar(const char *name, float fraction, const float color[3]) {
  int node = find_node(name);
  if (node < 0)
    return;

  states[node].fraction = fraction < 0.f ? 0.f : fraction > 1.f ? 1.f : fraction;
  if (color) {
    memcpy(states[node].bar_color, color, sizeof(states[node].bar_color));
    states[node].has_bar_color = true;
  }
}

static float point_fraction_x(UiPoint point) {
  return (point % 3) * 0.5f;
}

//the game's y runs up, so the top row is the largest fraction
static float point_fraction_y(UiPoint point) {
  return 1.f - (point / 3) * 0.5f;
}

static Rect screen_rect() { return (Rect){0.f, 0.f, ui_width, ui_height}; }

static Rect lay_out(int node);

static Rect relative_rect(int relative) {
  return relative == UI_SCREEN ? screen_rect() : lay_out(relative);
}

typedef struct Axis {
  float fractions[UI_MAX_ANCHORS];
  float positions[UI_MAX_ANCHORS];
  int count;
} Axis;

static void place_on_axis(const Axis *axis, float size, float *start, float *out_size) {
  *out_size = size;
  if (axis->count == 0)
    return;

  float spread = axis->fractions[axis->count - 1] - axis->fractions[0];
  if (axis->count > 1 && fabsf(spread) > 0.01f) {
    *out_size = (axis->positions[axis->count - 1] - axis->positions[0]) / spread;
    *start = axis->positions[0] - axis->fractions[0] * *out_size;
    return;
  }
  *start = axis->positions[0] - axis->fractions[0] * size;
}

static Rect lay_out_anchored(const UiNodeDef *def, Rect parent) {
  Axis horizontal = {0}, vertical = {0};

  for (int i = 0; i < def->anchor_count; i++) {
    const UiAnchorDef *anchor = &def->anchors[i];
    Rect target = relative_rect(anchor->relative);

    horizontal.fractions[horizontal.count] = point_fraction_x(anchor->point);
    horizontal.positions[horizontal.count++] =
        target.left + point_fraction_x(anchor->relative_point) * target.width +
        anchor->x;

    vertical.fractions[vertical.count] = point_fraction_y(anchor->point);
    vertical.positions[vertical.count++] =
        target.bottom + point_fraction_y(anchor->relative_point) * target.height +
        anchor->y;
  }

  Rect rect = {parent.left, parent.bottom, def->width, def->height};
  place_on_axis(&horizontal, def->width, &rect.left, &rect.width);
  place_on_axis(&vertical, def->height, &rect.bottom, &rect.height);
  return rect;
}

static Rect lay_out(int node) {
  NodeState *state = &states[node];
  if (state->laid_out)
    return state->rect;

  UiNodeDef effective = ui_nodes[node];
  if (state->has_size) {
    effective.width = state->width;
    effective.height = state->height;
  }
  if (state->has_anchor) {
    effective.anchor_count = 1;
    effective.anchors[0] = state->anchor;
    effective.set_all_points = false;
  }

  const UiNodeDef *def = &effective;
  Rect parent = def->parent == UI_SCREEN ? screen_rect() : lay_out(def->parent);

  state->rect = def->set_all_points || def->anchor_count == 0
                    ? parent
                    : lay_out_anchored(def, parent);
  state->laid_out = true;
  return state->rect;
}

static bool is_visible(int node) {
  for (; node != UI_SCREEN; node = ui_nodes[node].parent)
    if (!states[node].shown)
      return false;
  return true;
}

static void draw_quad(PUiImage *image, Rect rect, const float *tex_coords,
                      const float color[4], PUiBlend blend) {
  PUiQuad quad = {.image = image,
                  .x = rect.left * ui_scale,
                  .y = (ui_height - rect.bottom - rect.height) * ui_scale,
                  .width = rect.width * ui_scale,
                  .height = rect.height * ui_scale,
                  .u0 = tex_coords[0],
                  .u1 = tex_coords[1],
                  .v0 = tex_coords[2],
                  .v1 = tex_coords[3],
                  .blend = blend};
  memcpy(quad.color, color, sizeof(quad.color));
  pe_ui_quad(&quad);
}

static void draw_texture(int node) {
  const UiNodeDef *def = &ui_nodes[node];
  NodeState *state = &states[node];

  if (!state->image && !(def->has_color && !def->texture))
    return;

  static const float whole[4] = {0.f, 1.f, 0.f, 1.f};
  static const float white[4] = {1.f, 1.f, 1.f, 1.f};

  draw_quad(state->image, lay_out(node),
            state->has_tex_coords ? state->tex_coords
                                  : def->has_tex_coords ? def->tex_coords : whole,
            def->has_color ? def->color : white,
            def->additive ? PE_UI_BLEND_ADD : PE_UI_BLEND_ALPHA);
}

static void draw_bar_fill(int node) {
  const UiNodeDef *def = &ui_nodes[node];
  const NodeState *bar = &states[def->parent];
  Rect rect = lay_out(def->parent);
  rect.width *= bar->fraction;

  float color[4] = {1.f, 1.f, 1.f, 1.f};
  if (def->has_color)
    memcpy(color, def->color, sizeof(color));
  if (bar->has_bar_color)
    memcpy(color, bar->bar_color, sizeof(bar->bar_color));

  float coords[4] = {0.f, bar->fraction, 0.f, 1.f};
  draw_quad(states[node].image, rect, coords, color, PE_UI_BLEND_ALPHA);
}

static void draw_text(int node) {
  const NodeState *state = &states[node];
  if (!state->text[0])
    return;

  const UiNodeDef *def = &ui_nodes[node];
  Rect rect = lay_out(node);
  float width = pe_text_width(state->text);
  float x = (rect.left + rect.width * 0.5f) * ui_scale - width * 0.5f;
  if (def->justify == UI_JUSTIFY_LEFT)
    x = rect.left * ui_scale;
  else if (def->justify == UI_JUSTIFY_RIGHT)
    x = (rect.left + rect.width) * ui_scale - width;
  float y = (ui_height - rect.bottom - rect.height * 0.5f) * ui_scale +
            (pe_text_ascent() - pe_text_cell_height() * 0.5f);

  vec3 color = {def->has_color ? def->color[0] : 1.f,
                def->has_color ? def->color[1] : 1.f,
                def->has_color ? def->color[2] : 1.f};
  pe_text_draw(state->text, (vec3){0.f, 0.f, 0.f}, x + SHADOW_OFFSET, y + SHADOW_OFFSET);
  pe_text_draw(state->text, color, x, y);
}

static void lay_out_all(PRenderTarget *target) {
  ui_scale = target->extent.height / UI_HEIGHT;
  ui_width = target->extent.width / ui_scale;
  for (int i = 0; i < ui_node_count; i++)
    states[i].laid_out = false;
}

#define CURSOR_ICON_SIZE 36.f

void hud_set_cursor(const char *texture) {
  if (!texture) {
    cursor_image = NULL;
    return;
  }

  char path[512];
  snprintf(path, sizeof(path), "%s/%s", texture_directory, texture);
  cursor_image = pe_ui_image(path);
}

//what the pointer carries, centred on it and over everything
static void draw_cursor_icon() {
  if (!cursor_image)
    return;

  float size = CURSOR_ICON_SIZE * ui_scale;
  PUiQuad quad = {.image = cursor_image,
                  .x = pointer_x - size * 0.5f,
                  .y = pointer_y - size * 0.5f,
                  .width = size,
                  .height = size,
                  .u1 = 1.f,
                  .v1 = 1.f,
                  .color = {1.f, 1.f, 1.f, 1.f}};
  pe_ui_quad(&quad);
}

void hud_draw_images(PRenderTarget *target, VkCommandBuffer command,
                     uint32_t image_index) {
  lay_out_all(target);

  pe_ui_begin(command, target, image_index);
  for (int i = 0; i < ui_draw_count; i++) {
    int node = ui_draw_order[i];
    if (!is_visible(node))
      continue;

    if (ui_nodes[node].kind == UI_TEXTURE)
      draw_texture(node);
    else if (ui_nodes[node].kind == UI_BARFILL)
      draw_bar_fill(node);
  }
  draw_canvas_pictures();
  hud_draw_tooltip_box();
  draw_cursor_icon();
  pe_ui_end();
}

void hud_draw_text() {
  hud_draw_tooltip_text();
  for (int i = 0; i < ui_draw_count; i++) {
    int node = ui_draw_order[i];
    if (ui_nodes[node].kind == UI_FONTSTRING && is_visible(node))
      draw_text(node);
  }
  draw_canvas_text();
  draw_notice();
}

static const float POWER_COLORS[][3] = {
    {0.f, 0.f, 1.f},   //mana
    {1.f, 0.f, 0.f},   //rage
    {1.f, 0.5f, 0.25f}, //focus
    {1.f, 1.f, 0.f},   //energy
};
static const float HEALTH_COLOR[3] = {0.f, 1.f, 0.f};
static const float EXPERIENCE_COLOR[3] = {0.58f, 0.f, 0.55f};

static float fraction_of(u32 value, u32 maximum) {
  return maximum ? (float)value / maximum : 0.f;
}

void hud_update_player(const PWowObjectState *state) {
  if (!state->player_valid)
    return;

  const PWowUnitStats *player = &state->player;
  char text[TEXT_MAX];

  hud_set_bar("PlayerFrameHealthBar", fraction_of(player->health, player->max_health), HEALTH_COLOR);

  int power = player->power_type < 4 ? player->power_type : 0;
  hud_set_bar("PlayerFrameManaBar", fraction_of(player->power[power], player->max_power[power]),
              POWER_COLORS[power]);

  snprintf(text, sizeof(text), "%u", player->level);
  hud_set_text("PlayerLevelText", text);

  hud_set_bar("MainMenuExpBar", fraction_of(state->player_xp, state->player_next_level_xp),
              EXPERIENCE_COLOR);
}

//what the code lays out by itself every frame, for the windows the game builds with
//Lua: in the units of the frames with the origin at the top left and y down, as
//text runs. the code calls hud_canvas_clear() and fills it again each update
#define CANVAS_PICTURES_MAX 512
#define CANVAS_TEXTS_MAX 256
#define CANVAS_REGIONS_MAX 64
#define CANVAS_TEXT_MAX 160
#define CANVAS_NAME_MAX 32

typedef struct Box {
  float left, top, width, height;
} Box;

typedef struct CanvasPicture {
  PUiImage *image;
  Box box;
  float tex_coords[4];
  float color[4];
  bool additive;
} CanvasPicture;

typedef struct CanvasText {
  char text[CANVAS_TEXT_MAX];
  float left, top;
  float color[3];
  bool shadow;
} CanvasText;

typedef struct CanvasRegion {
  char name[CANVAS_NAME_MAX];
  Box box;
  bool clickable;
} CanvasRegion;

static struct {
  CanvasPicture pictures[CANVAS_PICTURES_MAX];
  int picture_count;
  CanvasText texts[CANVAS_TEXTS_MAX];
  int text_count;
  CanvasRegion regions[CANVAS_REGIONS_MAX];
  int region_count;
} canvas;

static int hovered_region = -1;

void hud_canvas_clear() {
  canvas.picture_count = 0;
  canvas.text_count = 0;
  canvas.region_count = 0;
}

void hud_canvas_picture(const char *texture, float left, float top, float width, float height,
                        const float tex_coords[4], const float color[4], bool additive) {
  if (canvas.picture_count == CANVAS_PICTURES_MAX)
    return;

  CanvasPicture *picture = &canvas.pictures[canvas.picture_count++];
  picture->image = NULL;
  if (texture) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", texture_directory, texture);
    picture->image = pe_ui_image(path);
    if (!picture->image) {
      canvas.picture_count--;
      return;
    }
  }
  picture->box = (Box){left, top, width, height};
  static const float whole[4] = {0.f, 1.f, 0.f, 1.f};
  memcpy(picture->tex_coords, tex_coords ? tex_coords : whole, sizeof(picture->tex_coords));
  static const float white[4] = {1.f, 1.f, 1.f, 1.f};
  memcpy(picture->color, color ? color : white, sizeof(picture->color));
  picture->additive = additive;
}

void hud_canvas_text(const char *text, float left, float top, const float color[3], bool shadow) {
  if (canvas.text_count == CANVAS_TEXTS_MAX)
    return;

  CanvasText *entry = &canvas.texts[canvas.text_count++];
  snprintf(entry->text, CANVAS_TEXT_MAX, "%s", text);
  entry->left = left;
  entry->top = top;
  memcpy(entry->color, color, sizeof(entry->color));
  entry->shadow = shadow;
}

void hud_canvas_region(const char *name, float left, float top, float width, float height,
                       bool clickable) {
  if (canvas.region_count == CANVAS_REGIONS_MAX)
    return;

  CanvasRegion *region = &canvas.regions[canvas.region_count++];
  snprintf(region->name, CANVAS_NAME_MAX, "%s", name);
  region->box = (Box){left, top, width, height};
  region->clickable = clickable;
}

void hud_screen_size(float *width, float *height) {
  *width = ui_width;
  *height = ui_height;
}

float hud_scale() { return ui_scale; }

float hud_line_height() { return pe_text_cell_height() / ui_scale; }

static void draw_canvas_pictures() {
  for (int i = 0; i < canvas.picture_count; i++) {
    const CanvasPicture *picture = &canvas.pictures[i];
    PUiQuad quad = {.image = picture->image,
                    .x = picture->box.left * ui_scale,
                    .y = picture->box.top * ui_scale,
                    .width = picture->box.width * ui_scale,
                    .height = picture->box.height * ui_scale,
                    .u0 = picture->tex_coords[0],
                    .u1 = picture->tex_coords[1],
                    .v0 = picture->tex_coords[2],
                    .v1 = picture->tex_coords[3],
                    .blend = picture->additive ? PE_UI_BLEND_ADD : PE_UI_BLEND_ALPHA};
    memcpy(quad.color, picture->color, sizeof(quad.color));
    pe_ui_quad(&quad);
  }
}

static void draw_canvas_text() {
  for (int i = 0; i < canvas.text_count; i++) {
    const CanvasText *entry = &canvas.texts[i];
    float x = entry->left * ui_scale;
    float y = entry->top * ui_scale + pe_text_ascent();
    if (entry->shadow)
      pe_text_draw(entry->text, (vec3){0.f, 0.f, 0.f}, x + SHADOW_OFFSET, y + SHADOW_OFFSET);
    pe_text_draw(entry->text, (vec3){entry->color[0], entry->color[1], entry->color[2]}, x, y);
  }
}

static bool box_contains(Box box, float x, float y) {
  return x >= box.left && x <= box.left + box.width && y >= box.top && y <= box.top + box.height;
}

//the region under the pointer, the one added last when they overlap
static int region_under(float x, float y) {
  for (int i = canvas.region_count - 1; i >= 0; i--)
    if (box_contains(canvas.regions[i].box, x, y))
      return i;
  return -1;
}

bool hud_canvas_hovered(const char *name) {
  return hovered_region >= 0 && hovered_region < canvas.region_count &&
         strcmp(canvas.regions[hovered_region].name, name) == 0;
}

//a notice is one line over the world that fades after a few seconds, the game's UIErrorsFrame
#define NOTICE_SECONDS 3.f
#define NOTICE_TOP_FRACTION 0.22f

static struct {
  char text[CANVAS_TEXT_MAX];
  float color[3];
  float remaining;
} notice;

void hud_notice(const char *text, const float color[3]) {
  snprintf(notice.text, sizeof(notice.text), "%s", text);
  memcpy(notice.color, color, sizeof(notice.color));
  notice.remaining = NOTICE_SECONDS;
}

void hud_notice_tick(float seconds) {
  notice.remaining -= seconds;
  if (notice.remaining < 0.f)
    notice.remaining = 0.f;
}

static void draw_notice() {
  if (notice.remaining <= 0.f)
    return;

  float x = (ui_width * ui_scale - pe_text_width(notice.text)) * 0.5f;
  float y = ui_height * NOTICE_TOP_FRACTION * ui_scale;
  pe_text_draw(notice.text, (vec3){0.f, 0.f, 0.f}, x + SHADOW_OFFSET, y + SHADOW_OFFSET);
  pe_text_draw(notice.text, (vec3){notice.color[0], notice.color[1], notice.color[2]}, x, y);
}

#define BUTTON_COUNT 2

static int hovered_node = UI_SCREEN;
static int pressed_node[BUTTON_COUNT] = {UI_SCREEN, UI_SCREEN};
static char pressed_region[BUTTON_COUNT][CANVAS_NAME_MAX];
static bool was_down[BUTTON_COUNT];

static bool rect_contains(Rect rect, float x, float y) {
  return x >= rect.left && x <= rect.left + rect.width && y >= rect.bottom &&
         y <= rect.bottom + rect.height;
}

//the node under the pointer that takes the mouse, the one defined last when
//they overlap: a button, or a frame that has the mouse enabled
static int node_under(float x, float y) {
  int found = UI_SCREEN;
  for (int i = 0; i < ui_node_count; i++)
    if ((ui_nodes[i].clickable || ui_nodes[i].captures_mouse) && is_visible(i) &&
        rect_contains(states[i].rect, x, y))
      found = i;
  return found;
}

static void show_button_states() {
  for (int i = 0; i < ui_node_count; i++) {
    UiState state = ui_nodes[i].state;
    if (state == UI_STATE_NONE)
      continue;

    int button = ui_nodes[i].parent;
    bool pressed = pressed_node[0] == button && hovered_node == button;
    states[i].shown = (state == UI_STATE_HIGHLIGHT && hovered_node == button) ||
                      (state == UI_STATE_PUSHED && pressed);
  }
}

HudClick hud_update_mouse(float mouse_x, float mouse_y, bool left_down, bool right_down) {
  float x = mouse_x / ui_scale;
  float y = ui_height - mouse_y / ui_scale;
  bool down[BUTTON_COUNT] = {left_down, right_down};
  HudClick clicked = {NULL, 0};

  pointer_x = mouse_x;
  pointer_y = mouse_y;
  hovered_region = region_under(x, mouse_y / ui_scale);
  hovered_node = hovered_region >= 0 ? UI_SCREEN : node_under(x, y);

  for (int button = 0; button < BUTTON_COUNT; button++) {
    if (down[button] && !was_down[button]) {
      pressed_node[button] = hovered_node;
      snprintf(pressed_region[button], CANVAS_NAME_MAX, "%s",
               hovered_region >= 0 ? canvas.regions[hovered_region].name : "");
    }

    if (!down[button] && was_down[button]) {
      int node = pressed_node[button];
      if (node != UI_SCREEN && node == hovered_node && ui_nodes[node].clickable)
        clicked = (HudClick){ui_nodes[node].name, button + 1};
      pressed_node[button] = UI_SCREEN;

      if (hovered_region >= 0 && canvas.regions[hovered_region].clickable &&
          strcmp(pressed_region[button], canvas.regions[hovered_region].name) == 0)
        clicked = (HudClick){canvas.regions[hovered_region].name, button + 1};
      pressed_region[button][0] = 0;
    }
    was_down[button] = down[button];
  }

  show_button_states();
  return clicked;
}

bool hud_mouse_over_ui() {
  return hovered_node != UI_SCREEN || hovered_region >= 0 || pressed_node[0] != UI_SCREEN ||
         pressed_node[1] != UI_SCREEN || pressed_region[0][0] || pressed_region[1][0];
}

bool hud_canvas_held(const char *name) {
  return pressed_region[0][0] && strcmp(pressed_region[0], name) == 0 && hud_canvas_hovered(name);
}

#define TOOLTIP_LINES_MAX 8
#define TOOLTIP_LINE_MAX 96
#define TOOLTIP_PADDING 10.f
#define TOOLTIP_EDGE 16.f
#define TOOLTIP_BACKGROUND_INSET 5.f
#define TOOLTIP_POINTER_GAP 14.f
#define TOOLTIP_BORDER "interface/tooltips/ui-tooltip-border.png"

static struct {
  int count;
  char text[TOOLTIP_LINES_MAX][TOOLTIP_LINE_MAX];
  float color[TOOLTIP_LINES_MAX][3];
  Rect box; //where the last draw put it, in the units of the frames
} tooltip;

void hud_tooltip_clear() { tooltip.count = 0; }

void hud_tooltip_line(const char *text, const float color[3]) {
  if (tooltip.count == TOOLTIP_LINES_MAX)
    return;

  snprintf(tooltip.text[tooltip.count], TOOLTIP_LINE_MAX, "%s", text);
  memcpy(tooltip.color[tooltip.count], color, sizeof(tooltip.color[0]));
  tooltip.count++;
}

const char *hud_hovered_name() {
  if (hovered_region >= 0)
    return canvas.regions[hovered_region].name;
  return hovered_node == UI_SCREEN ? NULL : ui_nodes[hovered_node].name;
}

static float tooltip_line_height() { return pe_text_cell_height() / ui_scale; }

//above and to the right of the pointer, kept on the screen
static void place_tooltip() {
  float width = 0.f;
  for (int i = 0; i < tooltip.count; i++) {
    float line = hud_text_width(tooltip.text[i]);
    width = line > width ? line : width;
  }

  float height = tooltip.count * tooltip_line_height();
  Rect box = {0.f, 0.f, width + 2.f * TOOLTIP_PADDING, height + 2.f * TOOLTIP_PADDING};

  box.left = pointer_x / ui_scale + TOOLTIP_POINTER_GAP;
  box.bottom = ui_height - pointer_y / ui_scale + TOOLTIP_POINTER_GAP;
  if (box.left + box.width > ui_width)
    box.left = pointer_x / ui_scale - TOOLTIP_POINTER_GAP - box.width;
  if (box.bottom + box.height > ui_height)
    box.bottom = ui_height - box.height;
  tooltip.box = box;
}

//turned: the piece is stored a quarter turn, as the border's top and bottom are
static void draw_tooltip_piece(PUiImage *image, float x, float y, float width, float height,
                               float u0, float u1, bool turned) {
  PUiQuad quad = {.image = image,
                  .x = x * ui_scale,
                  .y = (ui_height - y - height) * ui_scale,
                  .width = width * ui_scale,
                  .height = height * ui_scale,
                  .u0 = u0,
                  .u1 = u1,
                  .v1 = 1.f,
                  .color = {1.f, 1.f, 1.f, 1.f},
                  .transpose_uv = turned};
  pe_ui_quad(&quad);
}

//the backdrop of the game's tooltips: a dark fill, and a border of eight 16 pixel
//pieces of one strip: left, right, top, bottom, then the four corners
static void draw_tooltip_box() {
  Rect box = tooltip.box;
  static const float fill[4] = {0.01f, 0.01f, 0.04f, 0.9f};
  static const float whole[4] = {0.f, 1.f, 0.f, 1.f};

  Rect inner = {box.left + TOOLTIP_BACKGROUND_INSET, box.bottom + TOOLTIP_BACKGROUND_INSET,
                box.width - 2.f * TOOLTIP_BACKGROUND_INSET,
                box.height - 2.f * TOOLTIP_BACKGROUND_INSET};
  draw_quad(NULL, inner, whole, fill, PE_UI_BLEND_ALPHA);

  char path[512];
  snprintf(path, sizeof(path), "%s/%s", texture_directory, TOOLTIP_BORDER);
  PUiImage *border = pe_ui_image(path);
  if (!border)
    return;

  const float e = TOOLTIP_EDGE, piece = 0.125f;
  float right = box.left + box.width - e, top = box.bottom + box.height - e;
  float middle_width = box.width - 2.f * e, middle_height = box.height - 2.f * e;

  draw_tooltip_piece(border, box.left, box.bottom + e, e, middle_height, 0 * piece, 1 * piece, false);
  draw_tooltip_piece(border, right, box.bottom + e, e, middle_height, 1 * piece, 2 * piece, false);
  draw_tooltip_piece(border, box.left + e, top, middle_width, e, 2 * piece, 3 * piece, true);
  draw_tooltip_piece(border, box.left + e, box.bottom, middle_width, e, 3 * piece, 4 * piece, true);
  draw_tooltip_piece(border, box.left, top, e, e, 4 * piece, 5 * piece, false);
  draw_tooltip_piece(border, right, top, e, e, 5 * piece, 6 * piece, false);
  draw_tooltip_piece(border, box.left, box.bottom, e, e, 6 * piece, 7 * piece, false);
  draw_tooltip_piece(border, right, box.bottom, e, e, 7 * piece, 8 * piece, false);
}

void hud_draw_tooltip_box() {
  if (tooltip.count == 0)
    return;

  place_tooltip();
  draw_tooltip_box();
}

void hud_draw_tooltip_text() {
  float line_height = tooltip_line_height();

  for (int i = 0; i < tooltip.count; i++) {
    float x = (tooltip.box.left + TOOLTIP_PADDING) * ui_scale;
    float top = tooltip.box.bottom + tooltip.box.height - TOOLTIP_PADDING - i * line_height;
    float y = (ui_height - top) * ui_scale + pe_text_ascent();

    pe_text_draw(tooltip.text[i], (vec3){0.f, 0.f, 0.f}, x + SHADOW_OFFSET, y + SHADOW_OFFSET);
    pe_text_draw(tooltip.text[i], (vec3){tooltip.color[i][0], tooltip.color[i][1], tooltip.color[i][2]}, x, y);
  }
}
