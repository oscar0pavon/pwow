#include "questmarks.h"

#include "hud.h"
#include "targeting.h"

#define AVAILABLE_ICON "interface/gossipframe/availablequesticon.png"
#define ACTIVE_ICON "interface/gossipframe/activequesticon.png"
#define MARK_SIZE 26.f
#define MARK_RISE 0.6f
#define MARK_DISTANCE 60.f

static const float INCOMPLETE_TINT[4] = {0.55f, 0.55f, 0.55f, 1.f};

static const char *icon_of(u32 status, const float **tint) {
  *tint = NULL;
  switch (status) {
  case PE_WOWOBJECT_QUEST_AVAILABLE:
    return AVAILABLE_ICON;
  case PE_WOWOBJECT_QUEST_REWARD:
  case PE_WOWOBJECT_QUEST_REWARD_REP:
    return ACTIVE_ICON;
  case PE_WOWOBJECT_QUEST_INCOMPLETE:
    *tint = INCOMPLETE_TINT;
    return ACTIVE_ICON;
  default:
    return NULL;
  }
}

void questmarks_update(const PWowObjectState *state, const mat4 view, const mat4 projection,
                       const vec3 player_position) {
  float width, height;
  hud_screen_size(&width, &height);

  for (int i = 0; i < state->count; i++) {
    const PWowCreature *creature = &state->creatures[i];
    const float *tint;
    const char *icon = icon_of(creature->quest_status, &tint);
    if (!icon)
      continue;

    float dx = creature->x - player_position[0], dy = creature->y - player_position[1];
    if (dx * dx + dy * dy > MARK_DISTANCE * MARK_DISTANCE)
      continue;

    vec3 above = {creature->x, creature->y, creature->z + targeting_height(creature) + MARK_RISE};
    float x, y;
    if (!targeting_project(view, projection, above, &x, &y))
      continue;

    hud_canvas_picture(icon, x * width - MARK_SIZE * 0.5f, y * height - MARK_SIZE, MARK_SIZE,
                       MARK_SIZE, NULL, tint, false);
  }
}
