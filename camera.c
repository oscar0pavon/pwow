#include "camera.h"

#include <math.h>

//how fast the zoom eases toward target_distance - the same exponential
//settle pwow's own fly-camera ground-follow uses, and roughly WoWee's own
//ZOOM_SMOOTH_SPEED (15/s)
#define ZOOM_SMOOTH_SPEED 15.0f

#define PITCH_LIMIT_DEGREES 85.0f

#define ZOOM_STEP_FRACTION 0.15f
#define ZOOM_STEP_MINIMUM 0.3f
#define MIN_DISTANCE 0.5f
#define MAX_DISTANCE 22.0f

void pwow_camera_init(PwowOrbitCamera *camera, float yaw, float pitch,
                      float distance, float pivot_height) {
  camera->yaw = yaw;
  camera->pitch = pitch;
  camera->distance = distance;
  camera->target_distance = distance;
  camera->pivot_height = pivot_height;
}

void pwow_camera_turn(PwowOrbitCamera *camera, float yaw_delta,
                      float pitch_delta) {
  camera->yaw += yaw_delta;
  camera->pitch += pitch_delta;
  camera->pitch = glm_clamp(camera->pitch, -PITCH_LIMIT_DEGREES, PITCH_LIMIT_DEGREES);
}

void pwow_camera_zoom(PwowOrbitCamera *camera, int notches) {
  float step = fmaxf(camera->target_distance * ZOOM_STEP_FRACTION, ZOOM_STEP_MINIMUM);
  camera->target_distance -= notches * step;
  camera->target_distance =
      glm_clamp(camera->target_distance, MIN_DISTANCE, MAX_DISTANCE);
}

void pwow_camera_update(PwowOrbitCamera *camera, PCamera *main_camera,
                        vec3 target_position, float delta_time) {
  camera->distance +=
      (camera->target_distance - camera->distance) *
      (1 - expf(-ZOOM_SMOOTH_SPEED * delta_time));

  vec3 pivot = {target_position[0], target_position[1],
               target_position[2] + camera->pivot_height};

  float yaw_rad = glm_rad(camera->yaw);
  float pitch_rad = glm_rad(camera->pitch);
  vec3 front = {cosf(pitch_rad) * cosf(yaw_rad),
               cosf(pitch_rad) * sinf(yaw_rad), sinf(pitch_rad)};

  vec3 offset;
  glm_vec3_scale(front, -camera->distance, offset);
  glm_vec3_add(pivot, offset, main_camera->position);
  glm_vec3_copy(front, main_camera->front);

  camera_update(main_camera);
}
