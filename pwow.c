#include <engine/engine.h>
#include <engine/terrain/terrain_world.h>
#include <engine/time.h>
#include <engine/window_manager.h>

#define DATA_DIRECTORY "data"

//with no arguments it starts at Goldshire. ./pwow <map> <tile x> <tile y> starts
//over the ground of another tile, which prepare_tile.sh has to have converted
#define DEFAULT_MAP "azeroth"
#define DEFAULT_TILE_X 31
#define DEFAULT_TILE_Y 49

//how far from the camera the tiles are kept loaded, in yards, and how high over
//the ground of a tile, in yards, a camera that starts over one is. the fog hides
//the ground from 500 to 1400, so tiles are kept as far as two tiles allow
#define STREAM_DISTANCE 900.0f
#define START_HEIGHT_OVER_GROUND 40.0f

//in front of the Goldshire inn, sixty yards north of it and looking south. the
//inn stands at X -9464, Y -24, on ground at 56. the trees along the road reach
//past 80, so the camera is kept low, under the canopy
#define START_X -9404.0f
#define START_Y -24.0f
#define START_Z 66.0f

//the ground fades into the horizon colour and the sky climbs from it to the
//zenith, so the fog and the horizon have to be the same
#define HORIZON_COLOR 0.62f, 0.72f, 0.85f, 1.0f
#define ZENITH_COLOR 0.20f, 0.42f, 0.85f, 1.0f
#define FOG_START 500.0f
#define FOG_END 1400.0f

#define MOVE_SPEED 60.0f
#define FAST_MOVE_FACTOR 4.0f

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

static const char *map = DEFAULT_MAP;
static int start_tile_x = DEFAULT_TILE_X;
static int start_tile_y = DEFAULT_TILE_Y;
static bool start_at_inn = true;

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

//the middle of the tile, over its ground once that is loaded
static void start_over_tile(vec3 position) {
  position[0] = (PE_TERRAIN_MAP_CENTRE_TILE - start_tile_y - 0.5f) *
                PE_TERRAIN_TILE_SIZE;
  position[1] = (start_tile_x - PE_TERRAIN_MAP_CENTRE_TILE + 0.5f) *
                PE_TERRAIN_TILE_SIZE;
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
}

static void pwow_init() {
  pe_vk_terrain_world_create(&world);

  vec3 position = {START_X, START_Y, START_Z};
  if (start_at_inn)
    fill_world_around(position);
  else
    start_over_tile(position);

  if (world.tile_count == 0) {
    LOG("pwow: no tiles in %s, run ./prepare_tile.sh %s %d %d 2\n",
        DATA_DIRECTORY, map, start_tile_x, start_tile_y);
    exit(1);
  }

  place_camera(position);
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

static void pwow_update() {
  if (key_released(&input.TAB))
    walking = !walking;

  turn_camera(TURN_SPEED * delta_time);
  update_camera_direction();
  move_camera(movement_speed() * delta_time);
  stream_world();

  if (walking)
    follow_ground(delta_time);

  camera_update(&main_camera);
}

static void pwow_draw() {}

static void pwow_input() {
  if (key_released(&input.Q))
    exit(0);
}

static void read_start_tile(char **arguments) {
  map = arguments[0];
  start_tile_x = atoi(arguments[1]);
  start_tile_y = atoi(arguments[2]);
  start_at_inn = false;
}

int main(int argc, char **argv) {
  if (argc == 4)
    read_start_tile(argv + 1);
  else if (argc != 1) {
    fprintf(stderr, "usage: %s [map tile_x tile_y]\n", argv[0]);
    return 1;
  }

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
