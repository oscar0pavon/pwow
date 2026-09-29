#ifndef PWOW_INPUT_H
#define PWOW_INPUT_H

#include <stdbool.h>

//one frame's movement intent, read from pengine's own key state. the
//keyboard-only subset of WoWee's CameraController::FrameInput pwow actually
//needs to move a character - no mouse-look, no touch, no UI-focus check,
//since pwow has none of those
typedef struct PwowFrameInput {
  bool forward;
  bool backward;
  bool strafe_left;
  bool strafe_right;
  bool turn_left;
  bool turn_right;
} PwowFrameInput;

void pwow_input_read(PwowFrameInput *out);

#endif
