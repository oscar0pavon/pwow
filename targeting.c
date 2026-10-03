#include "targeting.h"

#include <math.h>
#include <stdio.h>

#include "creatures.h"
#include "hud.h"

#define HUMAN_HEIGHT 2.0f
#define MINIMUM_HEIGHT 0.8f
#define BODY_RADIUS_FRACTION 0.25f
#define MINIMUM_RADIUS 0.35f
#define PICK_SLACK 0.15f

static const float HEALTH_COLOR[3] = {0.f, 1.f, 0.f};
static const float POWER_COLORS[][3] = {
    {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}, {1.f, 0.5f, 0.25f}, {1.f, 1.f, 0.f}};

static float creature_scale(const PWowCreature *creature) {
  return creatures_display_scale(creature->display_id) * creature->scale;
}

float targeting_height(const PWowCreature *creature) {
  float height = HUMAN_HEIGHT * creature_scale(creature);
  return height > MINIMUM_HEIGHT ? height : MINIMUM_HEIGHT;
}

void targeting_ray(const mat4 view, const mat4 projection, const vec3 eye, float x, float y,
                   float width, float height, vec3 direction) {
  mat4 view_projection, inverse;
  glm_mat4_mul((vec4 *)projection, (vec4 *)view, view_projection);
  glm_mat4_inv(view_projection, inverse);

  //the projection already turns y over for Vulkan, so the top of the screen is -1
  vec4 far_point = {2.f * x / width - 1.f, 2.f * y / height - 1.f, 1.f, 1.f};
  vec4 world;
  glm_mat4_mulv(inverse, far_point, world);

  vec3 target = {world[0] / world[3], world[1] / world[3], world[2] / world[3]};
  glm_vec3_sub(target, (float *)eye, direction);
  glm_vec3_normalize(direction);
}

bool targeting_project(const mat4 view, const mat4 projection, const vec3 point, float *x, float *y) {
  vec4 world = {point[0], point[1], point[2], 1.f};
  vec4 eye_space, clip;
  glm_mat4_mulv((vec4 *)view, world, eye_space);
  glm_mat4_mulv((vec4 *)projection, eye_space, clip);
  if (clip[3] <= 0.01f)
    return false;

  *x = clip[0] / clip[3] * 0.5f + 0.5f;
  *y = clip[1] / clip[3] * 0.5f + 0.5f;
  return true;
}

//how far the ray, from its start on, passes from the segment, and how far along the ray that is
static float ray_to_segment(const vec3 origin, const vec3 direction, const vec3 start,
                            const vec3 end, float *along) {
  vec3 segment, offset;
  glm_vec3_sub((float *)end, (float *)start, segment);
  glm_vec3_sub((float *)origin, (float *)start, offset);

  float length_squared = glm_vec3_dot(segment, segment);
  float f = glm_vec3_dot(segment, offset);
  float c = glm_vec3_dot((float *)direction, offset);
  float b = glm_vec3_dot((float *)direction, segment);
  float denominator = length_squared - b * b;

  float s = denominator > 1e-6f ? (b * f - c * length_squared) / denominator : 0.f;
  s = fmaxf(s, 0.f);
  float t = length_squared > 1e-6f ? (b * s + f) / length_squared : 0.f;
  if (t < 0.f) {
    t = 0.f;
    s = fmaxf(-c, 0.f);
  } else if (t > 1.f) {
    t = 1.f;
    s = fmaxf(b - c, 0.f);
  }

  vec3 on_ray, on_segment, gap;
  glm_vec3_scale((float *)direction, s, on_ray);
  glm_vec3_add((float *)origin, on_ray, on_ray);
  glm_vec3_scale(segment, t, on_segment);
  glm_vec3_add((float *)start, on_segment, on_segment);
  glm_vec3_sub(on_ray, on_segment, gap);

  *along = s;
  return glm_vec3_norm(gap);
}

u64 targeting_pick(const PWowObjectState *state, const vec3 eye, const vec3 direction) {
  u64 picked = 0;
  float nearest = INFINITY;

  for (int i = 0; i < state->count; i++) {
    const PWowCreature *creature = &state->creatures[i];
    float height = targeting_height(creature);
    float radius = fmaxf(height * BODY_RADIUS_FRACTION, MINIMUM_RADIUS);

    vec3 bottom = {creature->x, creature->y, creature->z + radius};
    vec3 top = {creature->x, creature->y, creature->z + fmaxf(height - radius, radius)};

    float along;
    if (ray_to_segment(eye, direction, bottom, top, &along) <= radius + PICK_SLACK && along < nearest) {
      nearest = along;
      picked = creature->guid;
    }
  }
  return picked;
}

static float fraction_of(u32 value, u32 maximum) {
  return maximum ? (float)value / maximum : 0.f;
}

bool targeting_update_frame(const PWowObjectState *state, u64 selected) {
  const PWowCreature *creature = NULL;
  for (int i = 0; i < state->count && selected; i++)
    if (state->creatures[i].guid == selected)
      creature = &state->creatures[i];

  hud_show("TargetFrame", creature != NULL);
  if (!creature)
    return selected == 0;

  const PWowUnitStats *stats = &creature->stats;
  const PWowName *name = pe_wowdialog_name(&state->names, creature->entry);
  char level[16];
  snprintf(level, sizeof(level), "%u", stats->level);

  hud_set_text("TargetName", name ? name->name : "");
  hud_set_text("TargetLevelText", level);
  hud_set_bar("TargetFrameHealthBar", fraction_of(stats->health, stats->max_health), HEALTH_COLOR);

  int power = stats->power_type < 4 ? stats->power_type : 0;
  hud_set_bar("TargetFrameManaBar", fraction_of(stats->power[power], stats->max_power[power]),
              POWER_COLORS[power]);
  hud_show("TargetDeadText", stats->max_health > 0 && stats->health == 0);
  return true;
}
