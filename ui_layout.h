#ifndef PWOW_UI_LAYOUT_H
#define PWOW_UI_LAYOUT_H

#include <stdbool.h>

//what tools/xml2ui writes into ui_frames.c: the frames of the game's own
//FrameXML, templates already resolved, in the order they are drawn

typedef enum UiKind {
  UI_FRAME,
  UI_STATUSBAR,
  UI_TEXTURE,
  UI_FONTSTRING,
  UI_BARFILL,
} UiKind;

typedef enum UiPoint {
  UI_TOPLEFT,
  UI_TOP,
  UI_TOPRIGHT,
  UI_LEFT,
  UI_CENTER,
  UI_RIGHT,
  UI_BOTTOMLEFT,
  UI_BOTTOM,
  UI_BOTTOMRIGHT,
} UiPoint;

#define UI_SCREEN (-1)
#define UI_MAX_ANCHORS 3

typedef enum UiJustify {
  UI_JUSTIFY_CENTER,
  UI_JUSTIFY_LEFT,
  UI_JUSTIFY_RIGHT,
} UiJustify;

typedef enum UiState {
  UI_STATE_NONE,
  UI_STATE_PUSHED,
  UI_STATE_HIGHLIGHT,
  UI_STATE_CHECKED,
} UiState;

typedef struct UiAnchorDef {
  UiPoint point;
  int relative;
  UiPoint relative_point;
  float x, y;
} UiAnchorDef;

typedef struct UiNodeDef {
  UiKind kind;
  const char *name;
  int parent;
  const char *texture;
  float width, height;
  int anchor_count;
  UiAnchorDef anchors[UI_MAX_ANCHORS];
  bool set_all_points;
  bool hidden;
  bool additive;
  bool has_tex_coords;
  bool has_color;
  float tex_coords[4];
  float color[4];
  float font_size;
  UiJustify justify;
  bool clickable;
  UiState state;
  bool captures_mouse; //enableMouse: the pointer over it is not the world's
} UiNodeDef;

extern const UiNodeDef ui_nodes[];
extern const int ui_node_count;
extern const int ui_draw_order[];
extern const int ui_draw_count;

#endif
