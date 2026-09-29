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
#include <engine/time.h>
#include <engine/window_manager.h>
#include <engine/wowauth/wowauth.h>
#include <engine/wowauth/wowworld.h>

#include <math.h>
#include <string.h>

#include "pwow_camera.h"
#include "pwow_input.h"

#define PLAYER_MODEL_PATH "data/character/tauren/male/taurenmale.glb"
#define PLAYER_SKIN_PATH "data/character/tauren/male/taurenmaleskin00_00.png"
#define PLAYER_ANIMATION "Stand"

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
//the ground from 500 to 1400, so tiles are kept as far as two tiles allow
#define STREAM_DISTANCE 900.0f
#define START_HEIGHT_OVER_GROUND 40.0f

//the ground fades into the horizon colour and the sky climbs from it to the
//zenith, so the fog and the horizon have to be the same
#define HORIZON_COLOR 0.62f, 0.72f, 0.85f, 1.0f
#define ZENITH_COLOR 0.20f, 0.42f, 0.85f, 1.0f
#define FOG_START 500.0f
#define FOG_END 1400.0f

#define MOVE_SPEED 60.0f
#define FAST_MOVE_FACTOR 4.0f

//live mode: the character's own run speed and turn rate, and the camera's
//distance and height behind it - WoW's own WOW_RUN_SPEED/WOW_TURN_SPEED
//(WoWee's CameraController) are 7 yards/s and 180 degrees/s; kept a little
//slower to turn here since there is no mouse-look to correct an overshoot
#define CHARACTER_MOVE_SPEED 7.0f
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

static PTerrainWorld world;

//set by live_login() when pwow was started with --live, and read by
//pwow_init() instead of the ground-height guess start_over_ground() makes
//for the static tile viewer
static bool live_mode;
static float live_player_z;
static float live_player_facing_degrees;

//the character's own position/facing in live mode, updated every frame by
//input; the orbit camera that follows it
static vec3 player_position;
static float player_facing;
static PwowOrbitCamera player_camera;

static PModel player_model;
static PSkin player_skin;
static PShader player_shader;

static const char *map = DEFAULT_MAP;
static int start_tile_x = DEFAULT_TILE_X;
static int start_tile_y = DEFAULT_TILE_Y;
static float start_x = DEFAULT_START_X;
static float start_y = DEFAULT_START_Y;

static bool walking;

static float yaw = GLM_PI;
static float pitch = -0.2f;

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
  shader_info.fragment_path = file_diffuse_frag_spv;
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
  player_model.shader = player_shader;

  pe_load_texture(PLAYER_SKIN_PATH, &player_model.texture);

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
static void player_place(vec3 position, float facing_degrees) {
  glm_mat4_identity(player_model.model_mat);
  glm_translate(player_model.model_mat, position);
  glm_rotate(player_model.model_mat, glm_rad(facing_degrees), (vec3){0, 0, 1});
  glm_rotate(player_model.model_mat, glm_rad(90.0f), (vec3){1, 0, 0});
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
}

static void fill_lighting(PTerrainFrame *frame) {
  glm_vec4_copy((vec4){-0.4f, 0.3f, -0.8f, 0}, frame->light_direction);
  glm_vec4_copy((vec4){0.9f, 0.85f, 0.75f, 1}, frame->light_color);
  glm_vec4_copy((vec4){0.35f, 0.38f, 0.45f, 1}, frame->ambient_color);
  glm_vec4_copy((vec4){HORIZON_COLOR}, frame->fog_color);
  glm_vec4_copy((vec4){ZENITH_COLOR}, frame->sky_zenith);
  glm_vec4_copy((vec4){FOG_START, FOG_END, 0, 0}, frame->fog_range);
}

static void pwow_draw_scene(PRenderTarget *target, VkCommandBuffer *command,
                            uint32_t image_index) {
  PTerrainFrame frame;
  ZERO(frame);
  pe_terrain_frame_set_camera(&frame, &main_camera);
  fill_lighting(&frame);

  pe_vk_terrain_world_draw(&world, &frame, *command, image_index);
  player_draw(command, image_index);
}

static void pwow_init() {
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

  pitch = glm_clamp(pitch, -PITCH_LIMIT, PITCH_LIMIT);
}

static float movement_speed() {
  float base = walking ? WALK_SPEED : MOVE_SPEED;
  float fast = walking ? WALK_FAST_FACTOR : FAST_MOVE_FACTOR;

  return input.SHIFT.pressed ? base * fast : base;
}

//moves the character relative to its own facing (forward is whichever way it
//is turned, not where the camera looks - WoWee's moveFollowedCharacter()
//does the same split), snaps its feet to the ground, then lets the orbit
//camera catch up. no wall push or building floors yet, unlike the fly
//camera's own keep_out_of_walls()/follow_ground() - the character can walk
//through what the camera cannot
static void update_live_character(float seconds) {
  PwowFrameInput in;
  pwow_input_read(&in);

  if (in.turn_left)
    player_facing += CHARACTER_TURN_SPEED_DEGREES * seconds;
  if (in.turn_right)
    player_facing -= CHARACTER_TURN_SPEED_DEGREES * seconds;

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

  if (glm_vec3_norm(direction) > 0.001f) {
    glm_vec3_normalize(direction);
    glm_vec3_muladds(direction, CHARACTER_MOVE_SPEED * seconds, player_position);
  }

  float ground = 0;
  if (pe_terrain_world_height_at(&world, player_position[0], player_position[1],
                                 &ground))
    player_position[2] = ground;

  player_place(player_position, player_facing);
  pwow_camera_update(&player_camera, &main_camera, player_position, seconds);
}

static void pwow_update() {
  if (live_mode) {
    update_live_character(delta_time);
    stream_world();
    play_animation_list();
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

  play_animation_list();
}

static void pwow_input() {
  if (key_released(&input.Q))
    pe_terminate();
}

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

  PWowWorld world_conn;
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

  PWowLoginResult login;
  if (!pe_wowworld_player_login(&world_conn, characters[0].guid, &login,
                                error, sizeof(error))) {
    fprintf(stderr, "player login failed: %s\n", error);
    exit(1);
  }
  pe_wowworld_close(&world_conn);

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
  //(pe_terrain_point_y makes the same flip for terrain read from disk)
  start_x = login.x;
  start_y = -login.y;
  live_player_z = login.z;
  live_player_facing_degrees = glm_deg(login.o);

  //inverse of read_start_tile()'s own formula: world x comes from tile_y
  //(rows run north-south) and world y from tile_x (columns run east-west),
  //so recovering the tile indices swaps them back the same way
  start_tile_x = (int)lroundf(PE_TERRAIN_MAP_CENTRE_TILE - 0.5f +
                              start_y / PE_TERRAIN_TILE_SIZE);
  start_tile_y = (int)lroundf(PE_TERRAIN_MAP_CENTRE_TILE - 0.5f -
                              start_x / PE_TERRAIN_TILE_SIZE);

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
