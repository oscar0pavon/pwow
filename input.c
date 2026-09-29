#include "input.h"

#include <engine/input.h>

void pwow_input_read(PwowFrameInput *out) {
  out->forward = input.W.pressed;
  out->backward = input.S.pressed;
  out->strafe_left = input.A.pressed;
  out->strafe_right = input.D.pressed;
  out->turn_left = input.J.pressed;
  out->turn_right = input.L.pressed;
}
