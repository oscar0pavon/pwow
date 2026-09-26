#include <engine/engine.h>
#include <engine/terrain/terrain_world.h>
#include <engine/time.h>
#include <engine/window_manager.h>

#define DATA_DIRECTORY "data"
#define MAP "azeroth"

//Goldshire, and the tiles around it
#define CENTRE_TILE_X 31
#define CENTRE_TILE_Y 49
#define TILES_AROUND_CENTRE 1

//the middle of the centre tile: the tile size times how far it is from tile
//32, the middle of the map, plus half a tile. the ground there is at 67
#define START_X (-17.5f * PE_TERRAIN_TILE_SIZE)
#define START_Y (0.5f * PE_TERRAIN_TILE_SIZE)
#define START_Z 150.0f

//the ground fades into the horizon colour and the sky climbs from it to the
//zenith, so the fog and the horizon have to be the same
#define HORIZON_COLOR 0.62f, 0.72f, 0.85f, 1.0f
#define ZENITH_COLOR 0.20f, 0.42f, 0.85f, 1.0f
#define FOG_START 500.0f
#define FOG_END 1400.0f

#define MOVE_SPEED 60.0f
#define FAST_MOVE_FACTOR 4.0f
#define TURN_SPEED 1.5f
#define PITCH_LIMIT 1.4f

static PTerrainWorld world;

static float yaw = GLM_PI;
static float pitch = -0.35f;

static void update_camera_direction() {
  main_camera.front[0] = cosf(pitch) * cosf(yaw);
  main_camera.front[1] = cosf(pitch) * sinf(yaw);
  main_camera.front[2] = sinf(pitch);
  camera_update(&main_camera);
}

static void place_camera() {
  camera_init(&main_camera);
  glm_vec3_copy((vec3){START_X, START_Y, START_Z}, main_camera.position);
  update_camera_direction();
}

static void fill_lighting(PTerrainFrame *frame) {
  glm_vec4_copy((vec4){-0.4f, -0.3f, -0.8f, 0}, frame->light_direction);
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
}

static void pwow_init() {
  pe_vk_terrain_world_create(&world);

  if (pe_vk_terrain_world_load_area(&world, DATA_DIRECTORY, MAP, CENTRE_TILE_X,
                                    CENTRE_TILE_Y, TILES_AROUND_CENTRE) ==
      false) {
    LOG("pwow: no tiles in %s, run ./prepare_tile.sh %s %d %d %d\n",
        DATA_DIRECTORY, MAP, CENTRE_TILE_X, CENTRE_TILE_Y, TILES_AROUND_CENTRE);
    exit(1);
  }

  place_camera();
  pe_vk_draw_scene = &pwow_draw_scene;
}

static void move_camera(float distance) {
  vec3 right;
  glm_vec3_cross(main_camera.up, main_camera.front, right);

  vec3 direction = {0, 0, 0};
  if (input.W.pressed)
    glm_vec3_add(direction, main_camera.front, direction);
  if (input.S.pressed)
    glm_vec3_sub(direction, main_camera.front, direction);
  if (input.D.pressed)
    glm_vec3_add(direction, right, direction);
  if (input.A.pressed)
    glm_vec3_sub(direction, right, direction);
  if (input.SPACE.pressed)
    glm_vec3_add(direction, main_camera.up, direction);
  if (input.C.pressed)
    glm_vec3_sub(direction, main_camera.up, direction);

  glm_vec3_muladds(direction, distance, main_camera.position);
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

static void pwow_update() {
  float speed = input.SHIFT.pressed ? MOVE_SPEED * FAST_MOVE_FACTOR : MOVE_SPEED;

  turn_camera(TURN_SPEED * delta_time);
  update_camera_direction();
  move_camera(speed * delta_time);
  camera_update(&main_camera);
}

static void pwow_draw() {}

static void pwow_input() {
  if (key_released(&input.Q))
    exit(0);
}

int main() {
  PGame game;
  ZERO(game);
  game.name = "pwow";
  game.init = &pwow_init;
  game.update = &pwow_update;
  game.draw = &pwow_draw;
  game.input = &pwow_input;

  pe_renderer_type = PEWMVULKAN;
  is_wayland_window = true;

  pengine_run(&game);
  return 0;
}
