#include "selection.h"

#include <string.h>

#include <engine/decal.h>
#include <engine/ui.h>

#include "creatures.h"
#include "hud.h"
#include "targeting.h"

#define RING_TEXTURE "data/spells/whiteringthin128.png"
#define RING_RADIUS_PER_SCALE 1.0f
#define RING_MINIMUM_RADIUS 0.7f
#define RING_LIFT 0.08f
#define RING_CELLS 12

static const float GREEN[3] = {0.1f, 1.f, 0.1f};
static const float RING_COLOR[4] = {0.2f, 1.f, 0.2f, 1.f};

static PUiImage *ring_image;

int selection_init(void) {
  ring_image = pe_ui_image(RING_TEXTURE);
  return pe_decal_init() && ring_image != NULL;
}

static const PWowCreature *find(const PWowObjectState *state, u64 guid) {
  for (int i = 0; i < state->count; i++)
    if (state->creatures[i].guid == guid)
      return &state->creatures[i];
  return NULL;
}

void selection_label(const PWowObjectState *state, u64 selected, const mat4 view, const mat4 projection) {
  const PWowCreature *creature = selected ? find(state, selected) : NULL;
  const PWowName *name = creature ? pe_wowdialog_name(&state->names, creature->entry) : NULL;
  if (!name)
    return;

  vec3 above = {creature->x, creature->y, creature->z + targeting_height(creature) + 0.3f};
  float x, y;
  if (!targeting_project(view, projection, above, &x, &y))
    return;

  float width, height;
  hud_screen_size(&width, &height);
  hud_canvas_text(name->name, x * width - hud_text_width(name->name) * 0.5f,
                  y * height - hud_line_height(), GREEN, true);
}

//the point of the ring's square at cell corner (i, j), on the ground
static PDecalVertex corner(const PWowCreature *creature, PTerrainWorld *world, float radius, int i, int j) {
  float u = (float)i / RING_CELLS, v = (float)j / RING_CELLS;
  float x = creature->x + (u * 2.f - 1.f) * radius;
  float y = creature->y + (v * 2.f - 1.f) * radius;

  float ground = creature->z;
  pe_terrain_world_height_at(world, x, y, &ground);

  PDecalVertex vertex = {.position = {x, y, ground + RING_LIFT}, .u = u, .v = v};
  memcpy(vertex.color, RING_COLOR, sizeof(vertex.color));
  return vertex;
}

void selection_draw_ring(const PWowObjectState *state, u64 selected, PTerrainWorld *world,
                         const mat4 view, const mat4 projection, VkCommandBuffer command,
                         uint32_t image_index) {
  const PWowCreature *creature = selected ? find(state, selected) : NULL;
  if (!creature || !ring_image)
    return;

  float scale = creatures_display_scale(creature->display_id) * creature->scale;
  float radius = scale * RING_RADIUS_PER_SCALE;
  if (radius < RING_MINIMUM_RADIUS)
    radius = RING_MINIMUM_RADIUS;

  PDecalVertex vertices[RING_CELLS * RING_CELLS * 6];
  int count = 0;
  for (int i = 0; i < RING_CELLS; i++) {
    for (int j = 0; j < RING_CELLS; j++) {
      PDecalVertex a = corner(creature, world, radius, i, j);
      PDecalVertex b = corner(creature, world, radius, i + 1, j);
      PDecalVertex c = corner(creature, world, radius, i + 1, j + 1);
      PDecalVertex d = corner(creature, world, radius, i, j + 1);
      vertices[count++] = a;
      vertices[count++] = b;
      vertices[count++] = c;
      vertices[count++] = a;
      vertices[count++] = c;
      vertices[count++] = d;
    }
  }

  mat4 view_projection;
  glm_mat4_mul((vec4 *)projection, (vec4 *)view, view_projection);

  pe_decal_begin(command, image_index, view_projection);
  //the texture's ring is thin, so it is added twice
  for (int pass = 0; pass < 2; pass++)
    pe_decal_triangles(ring_image, PE_UI_BLEND_ADD, vertices, count);
  pe_decal_end();
}
