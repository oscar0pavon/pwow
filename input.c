#include "input.h"

#include <engine/input.h>

void pwow_input_read(PwowFrameInput *out) {
  out->forward = input.W.pressed;
  out->backward = input.S.pressed;

  //WoWee's CameraController::update(): A/D turn while the right mouse button
  //is up and strafe while it is held, Q/E always strafe - retail's own
  //dual-purpose A/D. J/L keep turning either way, since they are pwow's own
  //dedicated turn keys (also used by the fly camera)
  bool ad_strafes = mouse.right.pressed;
  out->strafe_left = input.Q.pressed || (ad_strafes && input.D.pressed);
  out->strafe_right = input.E.pressed || (ad_strafes && input.A.pressed);
  out->turn_left = input.J.pressed || (!ad_strafes && input.D.pressed);
  out->turn_right = input.L.pressed || (!ad_strafes && input.A.pressed);
}
