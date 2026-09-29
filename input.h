#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>

//one frame's movement intent, read from pengine's own key state. the
//keyboard-only subset of WoWee's CameraController::FrameInput pwow actually
//needs to move a character - no touch, no UI-focus check, since pwow has
//none of those. mouse-look is a camera concern, not movement intent, and is
//read straight off pengine's global `mouse` in main.c instead
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
