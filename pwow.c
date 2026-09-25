#include <engine/engine.h>
#include <engine/terrain/terrain_draw.h>
#include <engine/time.h>
#include <engine/window_manager.h>

#define TILE_PATH "data/azeroth_31_49"
#define TEXTURE_DIRECTORY "data"

//the renderer clears to black, so that is what distant ground has to fade into
#define FOG_COLOR 0.0f, 0.0f, 0.0f, 1.0f

#define MOVE_SPEED 60.0f
#define FAST_MOVE_FACTOR 4.0f
#define TURN_SPEED 1.5f
#define PITCH_LIMIT 1.4f
#define START_HEIGHT_ABOVE_GROUND 80.0f

static PTerrainTile tile;
static PTerrainMesh mesh;
static PTerrainGpuMesh gpu_mesh;
static PTerrainPipeline pipeline;
static PTerrainFrames frames;
static PTerrainMaterials materials;

static float yaw = GLM_PI;
static float pitch = -0.35f;

static void update_camera_direction() {
  main_camera.front[0] = cosf(pitch) * cosf(yaw);
  main_camera.front[1] = cosf(pitch) * sinf(yaw);
  main_camera.front[2] = sinf(pitch);
  camera_update(&main_camera);
}

static void place_camera_above_tile_centre() {
  //the first vertex of the chunk in the middle of the tile is the tile's centre
  const int middle_chunk = 8 * PE_TERRAIN_CHUNKS_PER_SIDE + 8;
  const float *ground =
      mesh.vertices[middle_chunk * PE_TERRAIN_CHUNK_VERTICES].position;

  camera_init(&main_camera);
  glm_vec3_copy(
      (vec3){ground[0], ground[1], ground[2] + START_HEIGHT_ABOVE_GROUND},
      main_camera.position);
  update_camera_direction();
}

static void fill_lighting(PTerrainFrame *frame) {
  glm_vec4_copy((vec4){-0.4f, -0.3f, -0.8f, 0}, frame->light_direction);
  glm_vec4_copy((vec4){0.9f, 0.85f, 0.75f, 1}, frame->light_color);
  glm_vec4_copy((vec4){0.35f, 0.38f, 0.45f, 1}, frame->ambient_color);
  glm_vec4_copy((vec4){FOG_COLOR}, frame->fog_color);
  glm_vec4_copy((vec4){300, 900, 0, 0}, frame->fog_range);
}

static void pwow_draw_scene(PRenderTarget *target, VkCommandBuffer *command,
                            uint32_t image_index) {
  PTerrainFrame frame;
  ZERO(frame);
  pe_terrain_frame_set_camera(&frame, &main_camera);
  fill_lighting(&frame);
  pe_vk_terrain_frame_update(&frames, image_index, &frame);

  PTerrainDrawInfo draw = {.pipeline = &pipeline,
                           .frames = &frames,
                           .mesh = &gpu_mesh,
                           .materials = &materials,
                           .frame = &frame,
                           .command_buffer = *command,
                           .image_index = image_index};
  pe_vk_terrain_draw(&draw);
}

static void pwow_init() {
  if (pe_terrain_load(TILE_PATH, &tile) == false) {
    LOG("pwow: can't load %s, run ./prepare_tile.sh azeroth 31 49\n",
        TILE_PATH);
    exit(1);
  }

  pe_terrain_mesh_build(&tile, &mesh);
  pe_vk_terrain_mesh_upload(&mesh, &gpu_mesh);
  pe_vk_terrain_pipeline_create(&pipeline);
  pe_vk_terrain_frames_create(&pipeline, &frames);
  pe_vk_terrain_materials_create(&pipeline, &tile, TEXTURE_DIRECTORY,
                                 &materials);

  place_camera_above_tile_centre();

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
