#ifndef CAMERA_H
#define CAMERA_H

#include <cglm/cglm.h>
#include <engine/camera.h>

//a third-person camera that orbits a moving target, the way WoWee's
//CameraController::updateOrbitCamera does for a player character - stripped
//down to what pwow needs right now: yaw/pitch/distance around a pivot above
//the target's feet, with the zoom eased in rather than snapped. no wall
//collision sweep, no indoor zoom limit, no mount/taxi/barber special cases -
//those are all real features WoWee has and pwow does not need yet
typedef struct PwowOrbitCamera {
  float yaw;             //degrees, independent of the character's own facing
  float pitch;           //degrees
  float distance;        //current, eased toward target_distance
  float target_distance; //what the camera should settle at
  float pivot_height;    //yards above the target position the camera looks at
} PwowOrbitCamera;

void pwow_camera_init(PwowOrbitCamera *camera, float yaw, float pitch,
                      float distance, float pivot_height);

//turns the camera round the target, independent of the character's own
//facing - the same freedom WoW's mouse-look gives while not steering
void pwow_camera_turn(PwowOrbitCamera *camera, float yaw_delta,
                      float pitch_delta);

//moves the distance the camera settles at by wheel notches (positive zooms
//in), the way WoWee's CameraController::processMouseWheel does: a step of 15%
//of the current distance, at least 0.3 yards, so it stays fine up close
void pwow_camera_zoom(PwowOrbitCamera *camera, int notches);

//orbits around target_position (the character's feet) and writes the result
//into main_camera, including the final camera_update() call
void pwow_camera_update(PwowOrbitCamera *camera, PCamera *main_camera,
                        vec3 target_position, float delta_time);

#endif
