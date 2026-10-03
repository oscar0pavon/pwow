#include <engine/animation/animation.h>
#include <engine/engine.h>
#include <engine/files.h>
#include <engine/images.h>
#include <engine/model.h>
#include <engine/renderer/descriptor_set.h>
#include <engine/renderer/draw.h>
#include <engine/renderer/pipeline.h>
#include <engine/renderer/shaders.h>
#include <engine/renderer/uniform_buffer.h>
#include <engine/renderer/vk_vertex.h>
#include <engine/renderer/vulkan.h>
#include <engine/skeletal.h>
#include <engine/terrain/terrain_world.h>
#include <engine/text.h>
#include <engine/ui.h>
#include <engine/time.h>
#include <engine/window_manager.h>
#include <wowauth/wowauth.h>
#include <wowauth/wowobject.h>
#include <wowauth/wowworld.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "camera.h"
#include "attached.h"
#include "creatures.h"
#include "equipment.h"
#include "actionbar.h"
#include "hud.h"
#include "input.h"

#define PLAYER_MODEL_PATH "data/character/tauren/male/taurenmale.glb"

//the display a Tauren male is born with, whose size (1.35) the character is
//drawn at
#define PLAYER_DISPLAY_ID 59
#define PLAYER_ANIMATION "Stand"

//stands in for the skin, face, hair style and hair colour SMSG_UPDATE_OBJECT
//would carry in PLAYER_BYTES: live networking doesn't parse the player's own
//object update yet (see TODO.md, "live client" item 1), so there is nothing
//to read this from. all zero is what the test character was created with
#define PLAYER_SKIN_ID 0
static const PAppearance PLAYER_APPEARANCE = {.skin = PLAYER_SKIN_ID};

#define HUD_FONT_PATH "/root/sources/WoWee/Data/expansions/classic/fonts/frizqt__.ttf"
#define HUD_FONT_SIZE 13.0f

#define DATA_DIRECTORY "data"

//with no arguments it starts over the Crossroads, in the Barrens. ./pwow <map>
//<tile x> <tile y> starts over the middle of another tile, which prepare_tile.sh
//has to have converted. the Crossroads stand at X -437, Y 2596, in tile 36, 32
#define DEFAULT_MAP "kalimdor"
#define DEFAULT_TILE_X 36
#define DEFAULT_TILE_Y 32
#define DEFAULT_START_X -437.0f
#define DEFAULT_START_Y 2596.0f

//how far from the camera the tiles are kept loaded, in yards, and how high over
//the ground of a tile, in yards, a camera that starts over one is. the fog hides
//the ground from 250 to 600, so tiles kept 500 yards out are gone from sight
//before their edge shows
#define STREAM_DISTANCE 500.0f
#define START_HEIGHT_OVER_GROUND 40.0f

//the ground fades into the horizon colour and the sky climbs from it to the
//zenith, so the fog and the horizon have to be the same
#define HORIZON_COLOR 0.62f, 0.72f, 0.85f, 1.0f
#define ZENITH_COLOR 0.20f, 0.42f, 0.85f, 1.0f
#define FOG_START 250.0f
#define FOG_END 600.0f

#define MOVE_SPEED 60.0f
#define FAST_MOVE_FACTOR 4.0f

//live mode: the character's own run speed and turn rate, and the camera's
//distance and height behind it - WoW's own WOW_RUN_SPEED/WOW_TURN_SPEED
//(WoWee's CameraController) are 7 yards/s and 180 degrees/s; kept a little
//slower to turn here since there is no mouse-look to correct an overshoot
#define CHARACTER_MOVE_SPEED 7.0f
//WoWee's WOW_BACK_SPEED: backing up is slower than a run, and paced to the
//"Walkbackwards" clip played below - moving at CHARACTER_MOVE_SPEED while
//backpedaling made the feet slide against the ground
#define CHARACTER_BACK_SPEED 4.5f
#define CHARACTER_TURN_SPEED_DEGREES 120.0f
#define CHARACTER_CAMERA_DISTANCE 8.0f
#define CHARACTER_CAMERA_PIVOT_HEIGHT 1.8f
#define CHARACTER_CAMERA_PITCH_DEGREES -12.0f

//on foot: a human's eye height and a run, in yards and yards a second, and how
//quickly the camera settles onto the ground as it moves over it
#define EYE_HEIGHT 2.0f
#define WALK_SPEED 7.0f
#define WALK_FAST_FACTOR 3.0f
#define GROUND_FOLLOW_RATE 12.0f
//the walker's body against buildings: how far out from its middle it reaches, how
//high a step it goes up without being stopped, and the heights above its feet of
//the two spheres that stand in for it, the knee above a step and the head
#define BODY_RADIUS 0.4f
#define STEP_HEIGHT 0.6f
#define BODY_SPHERE_LOW 1.0f
#define BODY_SPHERE_HIGH 1.6f
#define WALL_PUSH_PASSES 3

#define TURN_SPEED 1.5f
#define PITCH_LIMIT 1.4f

//WoW-style: look only while the right button is held, so the cursor is free
//to leave the window otherwise (pway reports absolute position, not a
//pointer-locked delta, so nothing steers the camera while the button is up)
#define MOUSE_LOOK_SENSITIVITY_DEGREES 0.15f

static PTerrainWorld world;

//set by live_login() when pwow was started with --live, and read by
//pwow_init() instead of the ground-height guess start_over_ground() makes
//for the static tile viewer
static bool live_mode;
static char player_name[32] = "Player";
static float live_player_z;
static float live_player_facing_degrees;

//the world connection live_login() makes, kept open (not closed after
//login like it used to be) so pwow_update() can keep polling it every
//frame for SMSG_UPDATE_OBJECT/SMSG_COMPRESSED_UPDATE_OBJECT - the only way
//creature spawns ever reach pwow
static PWowWorld world_conn;
static PWowObjectState npc_state;

//the character's own position/facing in live mode, updated every frame by
//input; the orbit camera that follows it
static vec3 player_position;
static float player_facing;
static PwowOrbitCamera player_camera;

static PModel player_model;
static PSkin player_skin;
static PShader player_shader;

//one PLAYER_VISIBLE_ITEM slot's own resolution state, driven by
//sync_player_equipment(). resolved_entry tracks what item_entry was last
//looked up for this slot so a CMSG_ITEM_QUERY_SINGLE round trip only
//happens once per distinct item, not once a frame forever
typedef struct PlayerEquipSlot {
  u32 resolved_entry;
  bool have_info;
  PWowItemInfo info;
  bool have_display;
  PItemDisplayInfo display;
} PlayerEquipSlot;

static PlayerEquipSlot player_equip_slots[PE_WOWOBJECT_PLAYER_EQUIP_SLOTS];

//Player::EquipmentSlots' head, shoulders, main hand, off hand and ranged weapon
#define PLAYER_SLOT_HEAD 0
#define PLAYER_SLOT_SHOULDERS 2
#define PLAYER_SLOT_MAIN_HAND 15
#define PLAYER_SLOT_OFF_HAND 16
#define PLAYER_SLOT_RANGED 17

static PAttachmentPoints player_points;

//one item on the body: where it is held and where it is carried, and how it is
//turned when carried. a weapon is held at drawn while the character attacks and
//carried at sheathed the rest of the time; a pair of shoulders is both, at the
//same place
typedef struct PlayerHeldItem {
  PAttachedItem attached;
  PAttachmentPoint drawn, sheathed;
  mat4 sheathed_local;
} PlayerHeldItem;

static PlayerHeldItem player_held[ATTACHED_ITEMS_MAX];
static int player_held_count;
static bool player_weapons_drawn;

//a two handed weapon and a ranged one ride on the back, the other weapons on
//the right hip (main hand) or the left (off hand), and a shield stays on its
//arm: the client's own places, as WoWee's weaponAttachment() has them
static u32 sheath_point_of(int slot, u32 inventory_type) {
  if (inventory_type == INVTYPE_SHIELD)
    return ATTACHMENT_SHIELD;
  if (inventory_type == INVTYPE_2HWEAPON || slot == PLAYER_SLOT_RANGED)
    return ATTACHMENT_BACK;
  return slot == PLAYER_SLOT_MAIN_HAND ? ATTACHMENT_HIP_RIGHT
                                       : ATTACHMENT_HIP_LEFT;
}

//puts a model on the body at drawn, carried at sheathed turned by how a big
//weapon or a small one is carried. a shoulder is carried where it is held
static void add_player_item(u32 drawn, u32 sheathed, bool big_weapon,
                            const char *folder, const char *suffix,
                            const char *model, const char *texture) {
  if (player_held_count >= ATTACHED_ITEMS_MAX)
    return;

  PlayerHeldItem *held = &player_held[player_held_count];
  if (!attached_item_create(&held->attached, &player_points, drawn, folder,
                            suffix, model, texture))
    return;

  held->drawn = held->attached.point;
  const PAttachmentPoint *carried = attachment_points_find(&player_points, sheathed);
  held->sheathed = carried ? *carried : held->drawn;
  if (sheathed == drawn)
    glm_mat4_identity(held->sheathed_local);
  else
    attached_carry_matrix(big_weapon, held->sheathed_local);
  player_held_count++;
}

//the shoulders on the shoulders, the main hand's item in the right hand, the
//off hand's in the left, or on the arm for a shield, and the ranged weapon in
//the right hand; redone whenever equipment changes
static void sync_player_held_items() {
  for (int i = 0; i < player_held_count; i++)
    attached_release(&player_held[i].attached.model);
  player_held_count = 0;

  //the helm is the Tauren male's own model
  PlayerEquipSlot *head = &player_equip_slots[PLAYER_SLOT_HEAD];
  if (head->have_display)
    add_player_item(ATTACHMENT_HELM, ATTACHMENT_HELM, false, "head", "_tam",
                    head->display.model, head->display.model_texture);

  PlayerEquipSlot *shoulders = &player_equip_slots[PLAYER_SLOT_SHOULDERS];
  if (shoulders->have_display) {
    add_player_item(ATTACHMENT_SHOULDER_LEFT, ATTACHMENT_SHOULDER_LEFT, false,
                    "shoulder", "", shoulders->display.model,
                    shoulders->display.model_texture);
    add_player_item(ATTACHMENT_SHOULDER_RIGHT, ATTACHMENT_SHOULDER_RIGHT, false,
                    "shoulder", "", shoulders->display.model_right,
                    shoulders->display.model_texture_right);
  }

  static const int slots[] = {PLAYER_SLOT_MAIN_HAND, PLAYER_SLOT_OFF_HAND,
                              PLAYER_SLOT_RANGED};
  for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
    int slot = slots[i];
    PlayerEquipSlot *equip = &player_equip_slots[slot];
    if (!equip->have_display)
      continue;

    u32 type = equip->info.inventory_type;
    bool shield = type == INVTYPE_SHIELD;
    u32 drawn = shield ? ATTACHMENT_SHIELD
                       : (slot == PLAYER_SLOT_OFF_HAND ? ATTACHMENT_LEFT_HAND
                                                       : ATTACHMENT_RIGHT_HAND);
    bool big = type == INVTYPE_2HWEAPON || slot == PLAYER_SLOT_RANGED;
    add_player_item(drawn, sheath_point_of(slot, type), big,
                    shield ? "shield" : "weapon", "", equip->display.model,
                    equip->display.model_texture);
  }
}

//what the character swings: a two handed weapon, a one handed one or fists
static const char *player_attack_animation() {
  PlayerEquipSlot *main_hand = &player_equip_slots[PLAYER_SLOT_MAIN_HAND];
  if (!main_hand->have_info)
    return "AttackUnarmed";
  return main_hand->info.inventory_type == INVTYPE_2HWEAPON ? "Attack2H"
                                                            : "Attack1H";
}

//call every live-mode frame, after pe_wowworld_poll(): for any equip slot
//whose PLAYER_VISIBLE_ITEM entry has changed since last seen, resolves it
//to a display id (CMSG_ITEM_QUERY_SINGLE), looks that up in
//ItemDisplayInfo.dbc, and re-applies the player's active geosets and skin
//texture if anything actually changed
static void sync_player_equipment() {
  if (!npc_state.player_equipment.valid)
    return;

  bool changed = false;

  for (int slot = 0; slot < PE_WOWOBJECT_PLAYER_EQUIP_SLOTS; slot++) {
    u32 entry = npc_state.player_equipment.item_entry[slot];
    PlayerEquipSlot *equip = &player_equip_slots[slot];
    if (entry == equip->resolved_entry)
      continue;

    equip->resolved_entry = entry;
    equip->have_info = false;
    equip->have_display = false;
    changed = true;
    if (entry == 0)
      continue;

    char error[PE_WOWWORLD_ERROR_MAX];
    if (!pe_wowworld_query_item(&world_conn, &npc_state, entry, &equip->info,
                               error, sizeof(error))) {
      LOG("pwow: equip slot %d entry=%u: %s\n", slot, entry, error);
      continue;
    }
    equip->have_info = true;

    LOG("pwow: equip slot %d entry=%u -> displayInfo=%u invType=%u\n", slot,
        entry, equip->info.display_info_id, equip->info.inventory_type);

    if (!resolve_item_display_info(equip->info.display_info_id,
                                   &equip->display)) {
      LOG("pwow: displayInfo=%u: no ItemDisplayInfo.dbc row\n",
          equip->info.display_info_id);
      continue;
    }
    equip->have_display = true;

    LOG("pwow:   geosetGroup1=%u geosetGroup3=%u torso=%s/%s legs=%s/%s "
       "arm=%s/%s hand=%s foot=%s\n",
       equip->display.geoset_group1, equip->display.geoset_group3,
       equip->display.texture_torso_upper, equip->display.texture_torso_lower,
       equip->display.texture_leg_upper, equip->display.texture_leg_lower,
       equip->display.texture_arm_upper, equip->display.texture_arm_lower,
       equip->display.texture_hand, equip->display.texture_foot);
  }

  if (!changed)
    return;

  PEquippedItem items[PE_WOWOBJECT_PLAYER_EQUIP_SLOTS];
  int item_count = 0;
  for (int slot = 0; slot < PE_WOWOBJECT_PLAYER_EQUIP_SLOTS; slot++) {
    PlayerEquipSlot *equip = &player_equip_slots[slot];
    if (!equip->have_display)
      continue;
    items[item_count].inventory_type = equip->info.inventory_type;
    items[item_count].display = equip->display;
    item_count++;
  }

  apply_equipment_geosets(&player_model, &PLAYER_APPEARANCE, items, item_count);

  PBodyLayers body;
  resolve_tauren_body(&PLAYER_APPEARANCE, &body);
  apply_equipment_texture(&player_model, &player_skin, &body, items,
                          item_count);

  sync_player_held_items();
}

static const char *map = DEFAULT_MAP;
static int start_tile_x = DEFAULT_TILE_X;
static int start_tile_y = DEFAULT_TILE_Y;
static float start_x = DEFAULT_START_X;
static float start_y = DEFAULT_START_Y;

static bool walking;

static float yaw = GLM_PI;
static float pitch = -0.2f;

//the cursor position at the start of the current drag, and
//whether one is in progress - reset on button-up so the next press starts
//from wherever the cursor lands rather than jumping from the last drag's end.
//either button drags the camera; only the right one also turns the character
static float mouse_look_x;
static float mouse_look_y;
static bool mouse_look_active;

//dx/dy in screen pixels since the last call, while either button is held;
//false and untouched otherwise. shared by both camera modes, which is safe
//since only one runs per frame
static bool mouse_look_delta(float *dx, float *dy) {
  if (!mouse.left.pressed && !mouse.right.pressed) {
    mouse_look_active = false;
    return false;
  }

  if (!mouse_look_active && hud_mouse_over_ui())
    return false;

  if (!mouse_look_active) {
    mouse_look_x = mouse.x;
    mouse_look_y = mouse.y;
    mouse_look_active = true;
  }

  *dx = mouse.x - mouse_look_x;
  *dy = mouse.y - mouse_look_y;
  mouse_look_x = mouse.x;
  mouse_look_y = mouse.y;
  return true;
}

static void update_camera_direction() {
  main_camera.front[0] = cosf(pitch) * cosf(yaw);
  main_camera.front[1] = cosf(pitch) * sinf(yaw);
  main_camera.front[2] = sinf(pitch);
  camera_update(&main_camera);
}

static void stream_world() {
  pe_vk_terrain_world_stream(&world, DATA_DIRECTORY, map,
                             main_camera.position[0], main_camera.position[1],
                             STREAM_DISTANCE);
}

static void fill_world_around(const vec3 position) {
  glm_vec3_copy((float *)position, main_camera.position);
  while (pe_vk_terrain_world_stream(&world, DATA_DIRECTORY, map, position[0],
                                    position[1], STREAM_DISTANCE))
    ;
}

//over the start, once the ground there is loaded
static void start_over_ground(vec3 position) {
  position[0] = start_x;
  position[1] = start_y;
  position[2] = 0;

  fill_world_around(position);

  float ground = 0;
  pe_terrain_world_height_at(&world, position[0], position[1], &ground);
  position[2] = ground + START_HEIGHT_OVER_GROUND;
}

static void place_camera(const vec3 position) {
  camera_init(&main_camera);
  glm_vec3_copy((float *)position, main_camera.position);
  update_camera_direction();
}

//loads the player's own character model, skinned so it can stand in a
//normal pose rather than the flat T-pose its raw vertices would give: the
//descriptor sets pe_vk_load_skin() makes are plain-layout ones (the same
//call pe_vk_load_model() always makes), so they are remade here against the
//skinned layout, which is the one skinned.vert's joint matrix buffer and
//diffuse_frag's texture actually need
static void player_load() {
  PCreateShaderInfo shader_info;
  ZERO(shader_info);
  shader_info.out_shader = &player_shader;
  shader_info.vertex_path = file_skinned_spv;
  shader_info.fragment_path = file_diffuse_cutout_frag_spv;
  shader_info.layout = pe_vk_pipeline_layout_skinned;

  //the engine's own default vertex input (used when this is left NULL) only
  //describes position and uv - fine for the plain "color"/"diffuse" shaders,
  //but skinned.vert also declares color, normal, joint and weight inputs at
  //locations 1, 2, 4 and 5. leaving those undeclared here isn't a silent
  //no-op: validation reports it outright (VUID-VkGraphicsPipelineCreateInfo-
  //Input-07904), and without it the pipeline has no idea how to find joint/
  //weight in the vertex buffer, so every vertex skins against whatever
  //garbage ends up in those shader inputs instead of the real data
  PVertexAtrributes skinned_attributes;
  ZERO(skinned_attributes);
  skinned_attributes.has_attributes = true;
  skinned_attributes.position = true;
  skinned_attributes.color = true;
  skinned_attributes.normal = true;
  skinned_attributes.uv = true;
  skinned_attributes.joint = true;
  skinned_attributes.weight = true;
  VkPipelineVertexInputStateCreateInfo skinned_vertex_input =
      pe_vk_pipeline_get_default_vertex_input(&skinned_attributes);
  shader_info.vertex_input = &skinned_vertex_input;

  pe_vk_create_shader(&shader_info);

  pe_vk_load_skin(&player_skin, &player_model, PLAYER_MODEL_PATH);
  attachment_points_load(PLAYER_MODEL_PATH, &player_points);
  player_model.shader = player_shader;

  char player_skin_path[512];
  resolve_tauren_skin_path(&PLAYER_APPEARANCE, player_skin_path,
                           sizeof(player_skin_path));
  pe_load_texture(player_skin_path, &player_model.texture);

  pe_vk_create_descriptor_sets(&player_model, pe_vk_descriptor_set_layout_skinned,
                               &main_render_target);
  pe_vk_skin_create_storage_buffers(&player_skin);
  pe_vk_descriptor_skinned_update(&player_model, &player_skin,
                                  &main_render_target);

  play_animation_by_name(&player_skin, PLAYER_ANIMATION, true);
}

//m22gltf bakes a -90 degree turn about X into every position, normal and
//bone (remap_axis, tools/m22gltf.c) to go from the model's own Z-up axes to
//glTF's Y-up convention, since every glTF consumer assumes Y-up. Undoing
//that means the inverse, +90 about X, here - using -90 again looked close
//(the model stood in-plane instead of lying flat) but was 180 degrees off
//from upright, standing the tauren on its head
//facing has to turn the model round its own now-vertical axis, which only
//exists after the up-axis fix runs - pe_model_transform() then pe_model_rotate()
//would append the facing turn on the right of the existing matrix instead,
//rotating the point round the glTF file's own Z axis (m22gltf's negated
//native Y, not up at all) before the up-axis fix ever touches it. built
//directly instead, in the order that actually matches how a point is meant
//to move: turn to face first, stand it upright second, then place it
//taurenmale.glb's own lowest vertex, in its Stand pose, sits this far below
//its root bone's local origin (measured by simulating the pose's actual
//skinning - forward kinematics through inverse bind matrices - rather than
//trusting the unposed mesh's own bounding box, which a keyframed animation
//is free to sit well above or below; see tools/m22gltf.c's animation
//translation fix for why those two used to disagree by over a yard).
//rotated into pwow's Z-up world by the fix above, a Y-up "lowest" carries
//straight into a Z-up "lowest", unchanged
#define PLAYER_FOOT_OFFSET 0.011f

static void player_place(vec3 position, float facing_degrees) {
  static float scale;
  if (scale == 0.0f)
    scale = creatures_display_scale(PLAYER_DISPLAY_ID);

  vec3 render_position = {position[0], position[1],
                          position[2] + PLAYER_FOOT_OFFSET * scale};
  glm_mat4_identity(player_model.model_mat);
  glm_translate(player_model.model_mat, render_position);
  glm_rotate(player_model.model_mat, glm_rad(facing_degrees), (vec3){0, 0, 1});
  glm_rotate(player_model.model_mat, glm_rad(90.0f), (vec3){1, 0, 0});
  //the model's axes are right handed and this world is left handed: reflect the
  //axis that carries left and right, or the character is its own mirror image
  //and what it holds is in the wrong hand (see creatures_sync())
  glm_scale(player_model.model_mat, (vec3){1, 1, -1});
  glm_scale_uni(player_model.model_mat, scale);
  glm_vec3_copy(position, player_model.position);
}

static void player_draw(VkCommandBuffer *command, uint32_t image_index) {
  PUniformBufferObject *ubo = &player_model.uniform_buffer_object;
  glm_mat4_copy(player_model.model_mat, ubo->model);
  glm_mat4_copy(main_camera.view, ubo->view);
  glm_mat4_copy(main_camera.projection, ubo->projection);

  //a point far along the terrain's own sun direction, since skinned.vert
  //treats this as a point to shade towards rather than a direction
  vec3 light_position;
  glm_vec3_copy(player_model.model_mat[3], light_position);
  glm_vec3_muladds((vec3){0.4f, -0.3f, 0.8f}, 5000.0f, light_position);
  glm_vec4(light_position, 1, ubo->light_position);

  pe_vk_send_uniform_buffer(&player_model, image_index);
  pe_vk_skin_send_storage_buffer(&player_skin, image_index);

  PDrawModelCommand draw;
  ZERO(draw);
  draw.model = &player_model;
  draw.layout = pe_vk_pipeline_layout_skinned;
  draw.command_buffer = *command;
  draw.image_index = image_index;
  pe_vk_draw_model(&draw);

  for (int i = 0; i < player_held_count; i++) {
    PlayerHeldItem *held = &player_held[i];
    held->attached.point = player_weapons_drawn ? held->drawn : held->sheathed;
    if (player_weapons_drawn)
      glm_mat4_identity(held->attached.local);
    else
      glm_mat4_copy(held->sheathed_local, held->attached.local);
    attached_item_draw(&held->attached, &player_skin, player_model.model_mat,
                       command, image_index, main_camera.view,
                       main_camera.projection);
  }
}

static void fill_lighting(PTerrainFrame *frame) {
  glm_vec4_copy((vec4){-0.4f, 0.3f, -0.8f, 0}, frame->light_direction);
  glm_vec4_copy((vec4){0.9f, 0.85f, 0.75f, 1}, frame->light_color);
  glm_vec4_copy((vec4){0.35f, 0.38f, 0.45f, 1}, frame->ambient_color);
  glm_vec4_copy((vec4){HORIZON_COLOR}, frame->fog_color);
  glm_vec4_copy((vec4){ZENITH_COLOR}, frame->sky_zenith);
  glm_vec4_copy((vec4){FOG_START, FOG_END, 0, 0}, frame->fog_range);
}

//right of the unit frames, which take the top left
#define DEBUG_TEXT_X 480
#define NPC_HUD_LINES 5

//lists the nearest few tracked creatures below the position line, nearest
//first - proves pe_wowworld_poll()/pe_wowobject_handle_packet() are parsing
//real spawns without needing any model rendering yet. must run inside a
//pe_text_begin()/pe_text_end() pair
static void draw_npc_hud(float y) {
  int best_idx[NPC_HUD_LINES];
  float best_dist[NPC_HUD_LINES];
  int best_count = 0;

  for (int i = 0; i < npc_state.count; i++) {
    PWowCreature *c = &npc_state.creatures[i];
    float dx = c->x - player_position[0];
    float dy = c->y - player_position[1];
    float dz = c->z - player_position[2];
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);

    if (best_count == NPC_HUD_LINES && dist >= best_dist[NPC_HUD_LINES - 1])
      continue;

    int pos = best_count < NPC_HUD_LINES ? best_count : NPC_HUD_LINES - 1;
    while (pos > 0 && best_dist[pos - 1] > dist) {
      best_dist[pos] = best_dist[pos - 1];
      best_idx[pos] = best_idx[pos - 1];
      pos--;
    }
    best_dist[pos] = dist;
    best_idx[pos] = i;
    if (best_count < NPC_HUD_LINES)
      best_count++;
  }

  char line[64];
  vec3 color = {0.7f, 0.9f, 1.f};
  snprintf(line, sizeof(line), "npcs: %d", npc_state.count);
  pe_text_draw(line, color, DEBUG_TEXT_X, y);

  for (int i = 0; i < best_count; i++) {
    PWowCreature *c = &npc_state.creatures[best_idx[i]];
    y += pe_text_cell_height();
    snprintf(line, sizeof(line), "  entry=%u display=%u %.0fy", c->entry,
             c->display_id, best_dist[i]);
    pe_text_draw(line, color, DEBUG_TEXT_X, y);
  }
}

static void pwow_draw_scene(PRenderTarget *target, VkCommandBuffer *command,
                            uint32_t image_index) {
  PTerrainFrame frame;
  ZERO(frame);
  pe_terrain_frame_set_camera(&frame, &main_camera);
  fill_lighting(&frame);

  pe_vk_terrain_world_draw(&world, &frame, *command, image_index);
  player_draw(command, image_index);
  if (live_mode)
    creatures_draw(command, image_index, main_camera.view, main_camera.projection);

  char hud_line[128];
  snprintf(hud_line, sizeof(hud_line), "%s  (%.0f, %.0f, %.0f)", map,
           player_position[0], player_position[1], player_position[2]);

  hud_draw_images(target, *command, image_index);

  pe_text_begin(*command, target, image_index);
  hud_draw_text();
  pe_text_draw(hud_line, (vec3){1.f, 1.f, 1.f}, DEBUG_TEXT_X, pe_text_ascent() + 10);
  if (live_mode)
    draw_npc_hud(pe_text_ascent() + 10 + pe_text_cell_height());
  pe_text_end();
}

#define ACTION_BUTTONS 12

static const char *const action_hotkeys[ACTION_BUTTONS] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "="};

static void init_action_bar() {
  char name[64];

  for (int i = 0; i < ACTION_BUTTONS; i++) {
    snprintf(name, sizeof(name), "ActionButton%dHotKey", i + 1);
    hud_set_text(name, action_hotkeys[i]);
    snprintf(name, sizeof(name), "ActionButton%dNormalTexture", i + 1);
    hud_set_texture(name, "interface/buttons/ui-quickslot.png");
  }
}

static void pwow_init() {
  if (!pe_text_init(HUD_FONT_PATH, HUD_FONT_SIZE))
    LOG("pwow: can't load HUD font %s\n", HUD_FONT_PATH);

  if (!pe_ui_init() || !hud_init(DATA_DIRECTORY))
    LOG("pwow: can't start the interface\n");
  hud_set_text("PlayerName", player_name);
  hud_set_bar("PlayerFrameHealthBar", 1.f, (float[]){0.f, 1.f, 0.f});
  hud_show("PlayerRestIcon", false);
  hud_show("PlayerAttackIcon", false);
  hud_show_prefixed("TargetFrameBuff", false);
  hud_show_prefixed("TargetFrameDebuff", false);
  hud_show("TargetHighLevelTexture", false);
  hud_show("TargetFrame", false);
  init_action_bar();
  if (!actionbar_init("data/dbc"))
    LOG("pwow: can't read the spell dbc files, run ./prepare_ui.sh\n");
  hud_set_bar("MainMenuExpBar", 0.f, NULL);
  hud_set_text("TargetName", "Target");
  hud_set_bar("TargetFrameHealthBar", 0.6f, (float[]){0.f, 1.f, 0.f});

  pe_vk_terrain_world_create(&world);

  vec3 position;
  if (live_mode) {
    position[0] = start_x;
    position[1] = start_y;
    position[2] = live_player_z;
    fill_world_around(position);
  } else {
    start_over_ground(position);
  }

  if (world.tile_count == 0) {
    LOG("pwow: no tiles in %s, run ./prepare_tile.sh %s %d %d 2\n",
        DATA_DIRECTORY, map, start_tile_x, start_tile_y);
    exit(1);
  }

  player_load();

  if (live_mode) {
    creatures_init();

    glm_vec3_copy(position, player_position);
    player_facing = live_player_facing_degrees;
    player_place(player_position, player_facing);

    camera_init(&main_camera);
    pwow_camera_init(&player_camera, player_facing, CHARACTER_CAMERA_PITCH_DEGREES,
                     CHARACTER_CAMERA_DISTANCE, CHARACTER_CAMERA_PIVOT_HEIGHT);
    pwow_camera_update(&player_camera, &main_camera, player_position, 1.0f / 60.0f);
  } else {
    place_camera(position);

    player_position[0] = position[0] - 15.0f;
    player_position[1] = position[1];
    float ground = 0;
    pe_terrain_world_height_at(&world, player_position[0], player_position[1],
                              &ground);
    player_position[2] = ground;
    player_facing = 0.0f;
    player_place(player_position, player_facing);
  }

  pe_vk_draw_scene = &pwow_draw_scene;
}

//on foot, forward is along the ground, not along where the camera looks, so
//looking down does not slow the walk and looking up does not lift off
static void movement_direction(vec3 direction) {
  vec3 forward;
  glm_vec3_copy(main_camera.front, forward);
  if (walking) {
    forward[2] = 0;
    glm_vec3_normalize(forward);
  }

  vec3 right;
  glm_vec3_cross(main_camera.up, forward, right);
  glm_vec3_normalize(right);

  glm_vec3_zero(direction);
  if (input.W.pressed)
    glm_vec3_add(direction, forward, direction);
  if (input.S.pressed)
    glm_vec3_sub(direction, forward, direction);
  if (input.D.pressed)
    glm_vec3_add(direction, right, direction);
  if (input.A.pressed)
    glm_vec3_sub(direction, right, direction);

  if (walking)
    return;

  if (input.SPACE.pressed)
    glm_vec3_add(direction, main_camera.up, direction);
  if (input.C.pressed)
    glm_vec3_sub(direction, main_camera.up, direction);
}

static void move_camera(float distance) {
  vec3 direction;
  movement_direction(direction);
  glm_vec3_muladds(direction, distance, main_camera.position);
}

//a wall pushes the body out of it, sideways only, and a corner needs more than
//one push
static void keep_out_of_walls() {
  const float heights[] = {BODY_SPHERE_LOW, BODY_SPHERE_HIGH};
  vec3 feet = {main_camera.position[0], main_camera.position[1],
               main_camera.position[2] - EYE_HEIGHT};

  for (int pass = 0; pass < WALL_PUSH_PASSES; pass++) {
    bool moved = false;

    for (int i = 0; i < 2; i++) {
      vec3 centre = {feet[0], feet[1], feet[2] + heights[i]};
      vec3 before;
      glm_vec3_copy(centre, before);

      if (pe_terrain_world_push_out(&world, centre, BODY_RADIUS) == false)
        continue;

      feet[0] += centre[0] - before[0];
      feet[1] += centre[1] - before[1];
      moved = true;
    }
    if (moved == false)
      break;
  }

  main_camera.position[0] = feet[0];
  main_camera.position[1] = feet[1];
}

//the floor is the ground or the highest floor of a building that is no more than
//a step above the feet, so a roof or an upper floor overhead is not stood on.
//where there is none under the camera, over a hole or past the loaded tiles, it
//stays as high as it was
static void follow_ground(float seconds) {
  keep_out_of_walls();

  float floor;
  float feet = main_camera.position[2] - EYE_HEIGHT;
  if (pe_terrain_world_floor_at(&world, main_camera.position[0],
                                main_camera.position[1], feet + STEP_HEIGHT,
                                &floor) == false)
    return;

  float target = floor + EYE_HEIGHT;
  main_camera.position[2] +=
      (target - main_camera.position[2]) *
      (1 - expf(-GROUND_FOLLOW_RATE * seconds));
}

static void turn_camera(float angle) {
  if (input.L.pressed)
    yaw += angle;
  if (input.J.pressed)
    yaw -= angle;
  if (input.I.pressed)
    pitch += angle;
  if (input.K.pressed)
    pitch -= angle;

  float dx, dy;
  if (mouse_look_delta(&dx, &dy)) {
    yaw += glm_rad(dx * MOUSE_LOOK_SENSITIVITY_DEGREES);
    pitch -= glm_rad(dy * MOUSE_LOOK_SENSITIVITY_DEGREES);
  }

  pitch = glm_clamp(pitch, -PITCH_LIMIT, PITCH_LIMIT);
}

static float movement_speed() {
  float base = walking ? WALK_SPEED : MOVE_SPEED;
  float fast = walking ? WALK_FAST_FACTOR : FAST_MOVE_FACTOR;

  return input.SHIFT.pressed ? base * fast : base;
}

//the character's own wall push and floor snap - the same
//pe_terrain_world_push_out()/pe_terrain_world_floor_at() the fly camera's
//keep_out_of_walls()/follow_ground() already use, just built around
//player_position directly rather than main_camera.position - the character
//has no eye-height offset to subtract first, since player_position already
//is its feet, not its eyes
static void character_keep_out_of_walls() {
  const float heights[] = {BODY_SPHERE_LOW, BODY_SPHERE_HIGH};
  vec3 feet;
  glm_vec3_copy(player_position, feet);

  for (int pass = 0; pass < WALL_PUSH_PASSES; pass++) {
    bool moved = false;

    for (int i = 0; i < 2; i++) {
      vec3 centre = {feet[0], feet[1], feet[2] + heights[i]};
      vec3 before;
      glm_vec3_copy(centre, before);

      if (pe_terrain_world_push_out(&world, centre, BODY_RADIUS) == false)
        continue;

      feet[0] += centre[0] - before[0];
      feet[1] += centre[1] - before[1];
      moved = true;
    }
    if (moved == false)
      break;
  }

  player_position[0] = feet[0];
  player_position[1] = feet[1];
}

//no eye-height/step lookahead needed either: the ray already starts a step
//above the feet, which are what player_position is
static void character_follow_ground() {
  character_keep_out_of_walls();

  float floor;
  if (pe_terrain_world_floor_at(&world, player_position[0], player_position[1],
                                player_position[2] + STEP_HEIGHT, &floor))
    player_position[2] = floor;
}

static void update_live_character(float seconds) {
  PwowFrameInput in;
  pwow_input_read(&in);

  float dx, dy;
  if (mouse_look_delta(&dx, &dy))
    pwow_camera_turn(&player_camera, dx * MOUSE_LOOK_SENSITIVITY_DEGREES,
                     -dy * MOUSE_LOOK_SENSITIVITY_DEGREES);

  pwow_camera_zoom(&player_camera, mouse.wheel);
  mouse.wheel = 0;

  //WoWee's CameraController::update() calls this cameraDrivesFacing: held,
  //the right mouse button turns the character to face wherever the camera
  //looks, so walking forward goes where the view points, the way it does in
  //retail. Released, J/L turn the character directly and the camera comes
  //along with it rather than staying at a fixed offset behind a facing that
  //just moved out from under it
  if (mouse.right.pressed) {
    player_facing = player_camera.yaw;
  } else {
    //facing = (cos f, sin f, 0), so increasing it swings the nose toward
    //+right (right = cross(Z, forward)) - a turn to the character's own
    //right, not its left. turn_right adds, turn_left subtracts, to match.
    //the camera takes the same turn rather than snapping to the facing, so
    //a left-button orbit survives it
    float turn = 0;
    if (in.turn_left)
      turn -= CHARACTER_TURN_SPEED_DEGREES * seconds;
    if (in.turn_right)
      turn += CHARACTER_TURN_SPEED_DEGREES * seconds;
    player_facing += turn;
    player_camera.yaw += turn;
  }

  float facing_rad = glm_rad(player_facing);
  vec3 forward = {cosf(facing_rad), sinf(facing_rad), 0};
  vec3 right;
  glm_vec3_cross((vec3){0, 0, 1}, forward, right);
  glm_vec3_normalize(right);

  vec3 direction;
  glm_vec3_zero(direction);
  if (in.forward)
    glm_vec3_add(direction, forward, direction);
  if (in.backward)
    glm_vec3_sub(direction, forward, direction);
  if (in.strafe_right)
    glm_vec3_add(direction, right, direction);
  if (in.strafe_left)
    glm_vec3_sub(direction, right, direction);

  bool backward_only = in.backward && !in.forward;
  float speed = backward_only ? CHARACTER_BACK_SPEED : CHARACTER_MOVE_SPEED;

  bool moving = glm_vec3_norm(direction) > 0.001f;
  if (moving) {
    glm_vec3_normalize(direction);
    glm_vec3_muladds(direction, speed * seconds, player_position);
  }

  //CHARACTER_MOVE_SPEED (7 yd/s) is the game's own running speed, not its
  //walk speed, so forward/diagonal motion plays "Run"; there is no
  //"Runbackwards" clip in taurenmale.glb, so backing up plays "Walkbackwards"
  //at CHARACTER_BACK_SPEED instead. WoWee's CameraController only calls a
  //strafe "anyStrafeLeft/Right" when it is the only input held - forward or
  //backward wins the animation the same way here - and taurenmale.glb does
  //carry "ShuffleLeft"/"ShuffleRight" for that pure-strafe case
  bool strafe_only = moving && !in.forward && !in.backward;
  const char *locomotion_animation = "Stand";
  if (backward_only)
    locomotion_animation = "Walkbackwards";
  else if (strafe_only)
    locomotion_animation = in.strafe_left ? "ShuffleLeft" : "ShuffleRight";
  else if (moving)
    locomotion_animation = "Run";
  else if (in.attack)
    locomotion_animation = player_attack_animation();
  player_weapons_drawn = in.attack;
  play_animation_by_name(&player_skin, locomotion_animation, true);

  character_follow_ground();

  player_place(player_position, player_facing);

  pwow_camera_update(&player_camera, &main_camera, player_position, seconds);
}

#define SPELL_ATTACK 6603

static void use_action_slot(int slot) {
  unsigned spell = actionbar_spell(&npc_state, slot);
  if (!live_mode || !spell || spell == SPELL_ATTACK)
    return;

  pe_wowworld_cast_spell(&world_conn, spell, 0);
  LOG("pwow: cast spell %u from slot %d\n", spell, slot);
}

//ActionButton1 to 12 of the bar, by the click on one
static void use_clicked_button(const char *name) {
  int slot;
  if (name && sscanf(name, "ActionButton%d", &slot) == 1)
    use_action_slot(slot);
}

//the keys 1 to 9 and 0 are slots 1 to 10, acting once as they go down
static void use_action_keys() {
  Key *keys[] = {&input.KEY_1, &input.KEY_2, &input.KEY_3, &input.KEY_4,
                 &input.KEY_5, &input.KEY_6, &input.KEY_7, &input.KEY_8,
                 &input.KEY_9, &input.KEY_0};
  static bool was_down[10];

  for (int i = 0; i < 10; i++) {
    if (keys[i]->pressed && !was_down[i])
      use_action_slot(i + 1);
    was_down[i] = keys[i]->pressed;
  }
}

static void pwow_update() {
  use_clicked_button(hud_update_mouse(mouse.x, mouse.y, mouse.left.pressed));
  use_action_keys();

  if (live_mode) {
    pe_wowworld_poll(&world_conn, &npc_state);
    hud_update_player(&npc_state);
    actionbar_update(&npc_state);
    sync_player_equipment();
    pe_wowobject_state_tick(&npc_state, delta_time);
    creatures_sync(&npc_state);
    update_live_character(delta_time);
    stream_world();
    play_animation_list(delta_time);
    return;
  }

  if (key_released(&input.TAB))
    walking = !walking;

  turn_camera(TURN_SPEED * delta_time);
  update_camera_direction();
  move_camera(movement_speed() * delta_time);
  stream_world();

  if (walking)
    follow_ground(delta_time);

  camera_update(&main_camera);

  play_animation_list(delta_time);
}

//Q used to quit here, but it is now WoWee's own dedicated strafe-left key
//(input.c's pwow_input_read()), so it no longer doubles as an exit
static void pwow_input() {}

static void read_start_tile(char **arguments) {
  map = arguments[0];
  start_tile_x = atoi(arguments[1]);
  start_tile_y = atoi(arguments[2]);
  start_x = (PE_TERRAIN_MAP_CENTRE_TILE - start_tile_y - 0.5f) *
            PE_TERRAIN_TILE_SIZE;
  start_y = (start_tile_x - PE_TERRAIN_MAP_CENTRE_TILE + 0.5f) *
            PE_TERRAIN_TILE_SIZE;
}

//vmangos's own map ids for the two continents prepare_tile.sh knows how to
//convert. any other map logs in fine but has no data pwow can stream
static const char *map_name_for_id(u32 map_id) {
  switch (map_id) {
  case 0:
    return "azeroth";
  case 1:
    return "kalimdor";
  default:
    return NULL;
  }
}

//logs into a real vmangos realmd/mangosd and pulls the live character
//position out of SMSG_LOGIN_VERIFY_WORLD, converting it into the axes and
//map name pwow already streams terrain in. exits the process on any
//failure, same as the "no tiles converted" check pwow_init already makes -
//there is nothing useful pwow can render without a real position here
static void live_login(const char *host, int port, const char *account,
                       const char *password) {
  PWowAuthResult auth;
  if (!pe_wowauth_login(host, port, account, password, &auth)) {
    fprintf(stderr, "auth failed: %s\n", auth.error);
    exit(1);
  }
  if (auth.realm_count == 0) {
    fprintf(stderr, "no realms on this auth server\n");
    exit(1);
  }

  PWowRealm *realm = &auth.realms[0];
  char realm_host[64];
  char *colon = strchr(realm->address, ':');
  if (!colon) {
    fprintf(stderr, "realm address '%s' has no port\n", realm->address);
    exit(1);
  }
  snprintf(realm_host, sizeof(realm_host), "%.*s",
          (int)(colon - realm->address), realm->address);
  int realm_port = atoi(colon + 1);

  char error[PE_WOWWORLD_ERROR_MAX];
  if (!pe_wowworld_connect(realm_host, realm_port, account, auth.session_key,
                           5875, realm->id, &world_conn, error,
                           sizeof(error))) {
    fprintf(stderr, "world connect failed: %s\n", error);
    exit(1);
  }

  PWowCharacter characters[PE_WOWWORLD_CHARACTERS_MAX];
  int char_count = 0;
  if (!pe_wowworld_char_enum(&world_conn, characters,
                             PE_WOWWORLD_CHARACTERS_MAX, &char_count, error,
                             sizeof(error))) {
    fprintf(stderr, "char enum failed: %s\n", error);
    exit(1);
  }
  if (char_count == 0) {
    fprintf(stderr, "account '%s' has no characters\n", account);
    exit(1);
  }

  //before player_login, not after: the player's own first CREATE_OBJECT -
  //the one PLAYER_VISIBLE_ITEM_1_0..19_0 actually arrives in - is read
  //inside that call's own wait loop, and npc_state can't tell it apart
  //from anyone else's object update without local_player_guid set first
  pe_wowobject_set_local_player_guid(&npc_state, characters[0].guid);

  PWowLoginResult login;
  if (!pe_wowworld_player_login(&world_conn, &npc_state, characters[0].guid,
                                &login, error, sizeof(error))) {
    fprintf(stderr, "player login failed: %s\n", error);
    exit(1);
  }
  //left open on purpose: pwow_update() polls it every frame from here on
  //for SMSG_UPDATE_OBJECT, the only way a creature spawn ever reaches pwow

  const char *login_map = map_name_for_id(login.map);
  if (!login_map) {
    fprintf(stderr,
           "'%s' is on map id %u, which pwow has no converter output for "
           "(only 0=azeroth and 1=kalimdor)\n",
           characters[0].name, login.map);
    exit(1);
  }
  map = login_map;

  //game data is X north, Y west; pwow's world is X north, Y east
  //(pe_terrain_point_y makes the same flip for terrain read from disk).
  //the mirror reverses which way a turn goes, so the facing is negated too
  start_x = login.x;
  start_y = -login.y;
  live_player_z = login.z;
  live_player_facing_degrees = -glm_deg(login.o);

  //inverse of read_start_tile()'s own formula: world x comes from tile_y
  //(rows run north-south) and world y from tile_x (columns run east-west),
  //so recovering the tile indices swaps them back the same way
  start_tile_x = (int)lroundf(PE_TERRAIN_MAP_CENTRE_TILE - 0.5f +
                              start_y / PE_TERRAIN_TILE_SIZE);
  start_tile_y = (int)lroundf(PE_TERRAIN_MAP_CENTRE_TILE - 0.5f -
                              start_x / PE_TERRAIN_TILE_SIZE);

  snprintf(player_name, sizeof(player_name), "%s", characters[0].name);
  LOG("pwow: logged in as %s, map=%s position=(%.2f, %.2f, %.2f)\n",
      characters[0].name, map, start_x, start_y, live_player_z);
}

int main(int argc, char **argv) {
  if (argc == 6 && strcmp(argv[1], "--live") == 0) {
    live_mode = true;
    live_login(argv[2], atoi(argv[3]), argv[4], argv[5]);
  } else if (argc == 4) {
    read_start_tile(argv + 1);
  } else if (argc != 1) {
    fprintf(stderr,
           "usage: %s [map tile_x tile_y]\n"
           "       %s --live <host> <port> <account> <password>\n",
           argv[0], argv[0]);
    return 1;
  }

  PGame game;
  ZERO(game);
  game.name = "pwow";
  game.init = &pwow_init;
  game.update = &pwow_update;
  game.input = &pwow_input;

  pe_renderer_type = PEWMVULKAN;
  is_wayland_window = true;

  pengine_run(&game);
  return 0;
}
