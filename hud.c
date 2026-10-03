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
static float ui_width;
static float ui_height = UI_HEIGHT;
static float ui_scale = 1.f;

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
  pe_ui_end();
}

void hud_draw_text() {
  for (int i = 0; i < ui_draw_count; i++) {
    int node = ui_draw_order[i];
    if (ui_nodes[node].kind == UI_FONTSTRING && is_visible(node))
      draw_text(node);
  }
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

#define BUTTON_COUNT 2

static int hovered_node = UI_SCREEN;
static int pressed_node[BUTTON_COUNT] = {UI_SCREEN, UI_SCREEN};
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

  hovered_node = node_under(x, y);

  for (int button = 0; button < BUTTON_COUNT; button++) {
    if (down[button] && !was_down[button])
      pressed_node[button] = hovered_node;

    if (!down[button] && was_down[button]) {
      int node = pressed_node[button];
      if (node != UI_SCREEN && node == hovered_node && ui_nodes[node].clickable)
        clicked = (HudClick){ui_nodes[node].name, button + 1};
      pressed_node[button] = UI_SCREEN;
    }
    was_down[button] = down[button];
  }

  show_button_states();
  return clicked;
}

bool hud_mouse_over_ui() {
  return hovered_node != UI_SCREEN || pressed_node[0] != UI_SCREEN ||
         pressed_node[1] != UI_SCREEN;
}
