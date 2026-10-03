//converts frames of the game's FrameXML to a C table. usage:
//  xml2ui <game data> <ui_frames.c> <FrameName>...
//prints "texture <png>" for each texture the frames use, which
//prepare_ui.sh converts
#include <dirent.h>
#include <expat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>

#include "../ui_layout.h"

#define MAX_ATTRIBUTES 24
#define MAX_NODES 4096

typedef struct Element {
  char tag[32];
  char *names[MAX_ATTRIBUTES];
  char *values[MAX_ATTRIBUTES];
  int attribute_count;
  struct Element *parent;
  struct Element **children;
  int child_count;
} Element;

typedef struct Template {
  char *name;
  Element *element;
} Template;

typedef struct Frame {
  Element *element;
  int node;
  int strata;
  int level;
  int sequence;
} Frame;

typedef struct Region {
  int node;
  int layer;
} Region;

static Template *templates;
static int template_count;
static char game_data[512];

static UiNodeDef nodes[MAX_NODES];
static char *node_names[MAX_NODES];
static char *node_relative_names[MAX_NODES][UI_MAX_ANCHORS];
static char *node_textures[MAX_NODES];
static int node_count;

static Frame frames[MAX_NODES];
static int frame_count;
static Region regions[MAX_NODES];
static int region_count;
static int region_frame[MAX_NODES];

static void *checked(void *pointer) {
  if (!pointer) {
    fprintf(stderr, "xml2ui: out of memory\n");
    exit(1);
  }
  return pointer;
}

static char *copy(const char *text) { return checked(strdup(text)); }

typedef struct Parse {
  Element *root;
  Element *current;
} Parse;

static void start_element(void *user, const XML_Char *tag,
                          const XML_Char **attributes) {
  Parse *parse = user;
  Element *element = checked(calloc(1, sizeof(Element)));
  snprintf(element->tag, sizeof(element->tag), "%s", tag);

  for (int i = 0; attributes[i] && element->attribute_count < MAX_ATTRIBUTES;
       i += 2) {
    element->names[element->attribute_count] = copy(attributes[i]);
    element->values[element->attribute_count++] = copy(attributes[i + 1]);
  }

  element->parent = parse->current;
  if (parse->current) {
    Element *parent = parse->current;
    parent->children = checked(realloc(
        parent->children, sizeof(Element *) * (parent->child_count + 1)));
    parent->children[parent->child_count++] = element;
  } else {
    parse->root = element;
  }
  parse->current = element;
}

static void end_element(void *user, const XML_Char *tag) {
  Parse *parse = user;
  (void)tag;
  parse->current = parse->current->parent;
}

static Element *read_xml(const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return NULL;

  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  char *text = checked(malloc(size + 1));
  if (fread(text, 1, size, file) != (size_t)size) {
    fclose(file);
    free(text);
    return NULL;
  }
  fclose(file);

  Parse parse = {0};
  XML_Parser parser = XML_ParserCreate(NULL);
  XML_SetUserData(parser, &parse);
  XML_SetElementHandler(parser, start_element, end_element);
  int ok = XML_Parse(parser, text, size, 1) != XML_STATUS_ERROR;
  if (!ok)
    fprintf(stderr, "xml2ui: %s: %s\n", path,
            XML_ErrorString(XML_GetErrorCode(parser)));
  XML_ParserFree(parser);
  free(text);
  return ok ? parse.root : NULL;
}

static const char *attribute(const Element *element, const char *name) {
  for (int i = 0; i < element->attribute_count; i++)
    if (strcmp(element->names[i], name) == 0)
      return element->values[i];
  return NULL;
}

static void add_template(Element *element) {
  const char *name = attribute(element, "name");
  const char *virtual = attribute(element, "virtual");
  if (!name || !virtual || strcmp(virtual, "true") != 0)
    return;

  templates = checked(realloc(templates, sizeof(Template) * (template_count + 1)));
  templates[template_count++] = (Template){copy(name), element};
}

static void load_templates(const char *directory) {
  DIR *dir = opendir(directory);
  if (!dir) {
    fprintf(stderr, "xml2ui: can't open %s\n", directory);
    exit(1);
  }

  for (struct dirent *entry; (entry = readdir(dir));) {
    size_t length = strlen(entry->d_name);
    if (length < 5 || strcasecmp(entry->d_name + length - 4, ".xml") != 0)
      continue;

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
    Element *root = read_xml(path);
    for (int i = 0; root && i < root->child_count; i++)
      add_template(root->children[i]);
  }
  closedir(dir);
}

static Element *find_template(const char *name) {
  for (int i = 0; i < template_count; i++)
    if (strcmp(templates[i].name, name) == 0)
      return templates[i].element;
  return NULL;
}

//an element and what it inherits, the lowest priority first: whatever the last
//one says wins
#define MAX_CONTRIBUTORS 16

typedef struct Contributors {
  Element *elements[MAX_CONTRIBUTORS];
  int count;
} Contributors;

static void gather(const Element *element, Contributors *out) {
  const char *inherits = attribute(element, "inherits");

  if (inherits) {
    char list[256];
    snprintf(list, sizeof(list), "%s", inherits);
    for (char *name = strtok(list, ", "); name; name = strtok(NULL, ", ")) {
      Element *template = find_template(name);
      if (template)
        gather(template, out);
      else
        fprintf(stderr, "xml2ui: no template %s\n", name);
    }
  }

  if (out->count < MAX_CONTRIBUTORS)
    out->elements[out->count++] = (Element *)element;
}

static const char *effective_attribute(const Contributors *c, const char *name) {
  const char *found = NULL;
  for (int i = 0; i < c->count; i++) {
    const char *value = attribute(c->elements[i], name);
    if (value)
      found = value;
  }
  return found;
}

static Element *effective_child(const Contributors *c, const char *tag) {
  Element *found = NULL;
  for (int i = 0; i < c->count; i++)
    for (int j = 0; j < c->elements[i]->child_count; j++)
      if (strcmp(c->elements[i]->children[j]->tag, tag) == 0)
        found = c->elements[i]->children[j];
  return found;
}

static Element *child(const Element *element, const char *tag) {
  for (int i = 0; element && i < element->child_count; i++)
    if (strcmp(element->children[i]->tag, tag) == 0)
      return element->children[i];
  return NULL;
}

static float number(const char *text, float fallback) {
  return text ? (float)atof(text) : fallback;
}

static bool is_true(const char *text) { return text && strcmp(text, "true") == 0; }

static void read_pair(const Element *element, const char *x_name,
                      const char *y_name, float *x, float *y) {
  const Element *dimension = child(element, "AbsDimension");
  const Element *source = dimension ? dimension : element;
  *x = number(attribute(source, x_name), *x);
  *y = number(attribute(source, y_name), *y);
}

static const char *point_names[] = {"TOPLEFT", "TOP",        "TOPRIGHT",
                                    "LEFT",    "CENTER",     "RIGHT",
                                    "BOTTOMLEFT", "BOTTOM", "BOTTOMRIGHT"};

static UiPoint parse_point(const char *name, UiPoint fallback) {
  for (int i = 0; name && i < 9; i++)
    if (strcmp(point_names[i], name) == 0)
      return i;
  return fallback;
}

static void expand_name(const char *name, const char *parent_name, char *out,
                        size_t size) {
  if (strncmp(name, "$parent", 7) == 0)
    snprintf(out, size, "%s%s", parent_name, name + 7);
  else
    snprintf(out, size, "%s", name);
}

static int new_node(UiKind kind, int parent, const char *name) {
  if (node_count == MAX_NODES) {
    fprintf(stderr, "xml2ui: too many nodes\n");
    exit(1);
  }
  int index = node_count++;
  nodes[index].kind = kind;
  nodes[index].parent = parent;
  node_names[index] = copy(name);
  return index;
}

static void read_anchors(const Contributors *c, int node, const char *parent_name) {
  const Element *anchors = effective_child(c, "Anchors");
  UiNodeDef *def = &nodes[node];

  for (int i = 0; anchors && i < anchors->child_count; i++) {
    const Element *anchor = anchors->children[i];
    if (def->anchor_count == UI_MAX_ANCHORS)
      break;

    int slot = def->anchor_count++;
    UiAnchorDef *out = &def->anchors[slot];
    out->point = parse_point(attribute(anchor, "point"), UI_CENTER);
    out->relative_point = parse_point(attribute(anchor, "relativePoint"), out->point);

    const char *relative = attribute(anchor, "relativeTo");
    char expanded[128];
    if (relative)
      expand_name(relative, parent_name, expanded, sizeof(expanded));
    node_relative_names[node][slot] = relative ? copy(expanded) : NULL;

    const Element *offset = child(anchor, "Offset");
    if (offset)
      read_pair(offset, "x", "y", &out->x, &out->y);
  }
}

static void read_size(const Contributors *c, int node) {
  const Element *size = effective_child(c, "Size");
  if (size)
    read_pair(size, "x", "y", &nodes[node].width, &nodes[node].height);
}

static void read_color(const Element *color, float *out, bool *has) {
  if (!color)
    return;
  out[0] = number(attribute(color, "r"), 1.f);
  out[1] = number(attribute(color, "g"), 1.f);
  out[2] = number(attribute(color, "b"), 1.f);
  out[3] = number(attribute(color, "a"), 1.f);
  if (has)
    *has = true;
}

static void texture_path(const char *file, char **out) {
  char path[256];
  size_t length = 0;
  for (const char *p = file; *p && length < sizeof(path) - 5; p++)
    path[length++] = *p == '\\' ? '/' : (char)tolower((unsigned char)*p);
  path[length] = 0;

  char *dot = strrchr(path, '.');
  if (dot && strchr(dot, '/') == NULL)
    *dot = 0;
  strcat(path, ".png");

  char blp[1024];
  snprintf(blp, sizeof(blp), "%s/%.*s.blp", game_data, (int)strlen(path) - 4, path);
  if (access(blp, R_OK) != 0)
    fprintf(stderr, "xml2ui: no %s\n", blp);
  else
    printf("texture %s\n", path);

  *out = copy(path);
}

static UiJustify justification(const Contributors *c) {
  const char *name = effective_attribute(c, "justifyH");
  if (name && strcmp(name, "LEFT") == 0)
    return UI_JUSTIFY_LEFT;
  if (name && strcmp(name, "RIGHT") == 0)
    return UI_JUSTIFY_RIGHT;
  return UI_JUSTIFY_CENTER;
}

static float font_height(const Contributors *c) {
  const char *font_name = effective_attribute(c, "inherits");
  const Element *template = font_name ? find_template(font_name) : NULL;
  if (!template)
    return 0.f;

  Contributors font_contributors = {0};
  gather(template, &font_contributors);
  const Element *height = effective_child(&font_contributors, "FontHeight");
  const Element *value = child(height, "AbsValue");
  return value ? number(attribute(value, "val"), 0.f) : 0.f;
}

static const char *layer_names[] = {"BACKGROUND", "BORDER", "ARTWORK", "OVERLAY",
                                    "HIGHLIGHT"};

static int layer_rank(const char *name) {
  for (int i = 0; name && i < 5; i++)
    if (strcmp(layer_names[i], name) == 0)
      return i;
  return 2;
}

static int add_region(const Element *element, int frame_node, int layer,
                      const char *frame_name) {
  size_t tag_length = strlen(element->tag);
  bool is_texture = tag_length >= 7 &&
                    strcmp(element->tag + tag_length - 7, "Texture") == 0;

  Contributors c = {0};
  gather(element, &c);

  char name[160] = "";
  const char *raw = effective_attribute(&c, "name");
  if (raw)
    expand_name(raw, frame_name, name, sizeof(name));

  int node = new_node(is_texture ? UI_TEXTURE : UI_FONTSTRING, frame_node, name);
  UiNodeDef *def = &nodes[node];

  read_size(&c, node);
  read_anchors(&c, node, frame_name);
  def->set_all_points = is_true(effective_attribute(&c, "setAllPoints"));
  def->hidden = is_true(effective_attribute(&c, "hidden"));

  if (is_texture) {
    const char *file = effective_attribute(&c, "file");
    def->additive = effective_attribute(&c, "alphaMode") &&
                    strcmp(effective_attribute(&c, "alphaMode"), "ADD") == 0;
    if (file)
      texture_path(file, &node_textures[node]);

    const Element *coords = effective_child(&c, "TexCoords");
    if (coords) {
      def->has_tex_coords = true;
      def->tex_coords[0] = number(attribute(coords, "left"), 0.f);
      def->tex_coords[1] = number(attribute(coords, "right"), 1.f);
      def->tex_coords[2] = number(attribute(coords, "top"), 0.f);
      def->tex_coords[3] = number(attribute(coords, "bottom"), 1.f);
    }
  } else {
    def->font_size = font_height(&c);
    def->justify = justification(&c);
  }

  read_color(effective_child(&c, "Color"), def->color, &def->has_color);

  regions[region_count] = (Region){node, layer};
  region_frame[region_count++] = frame_node;
  return node;
}

static void add_layers(const Element *layers, int frame_node, const char *frame_name) {
  for (int i = 0; i < layers->child_count; i++) {
    const Element *layer = layers->children[i];
    int rank = layer_rank(attribute(layer, "level"));
    for (int j = 0; j < layer->child_count; j++) {
      const char *tag = layer->children[j]->tag;
      if (strcmp(tag, "Texture") == 0 || strcmp(tag, "FontString") == 0)
        add_region(layer->children[j], frame_node, rank, frame_name);
    }
  }
}

//what a button shows only while it is pressed, hovered or checked
static void add_state_texture(const Contributors *c, int frame_node,
                              const char *frame_name, const char *tag,
                              UiState state, int layer) {
  const Element *element = effective_child(c, tag);
  if (!element)
    return;

  int node = add_region(element, frame_node, layer, frame_name);
  nodes[node].state = state;
  nodes[node].hidden = true;
}

static const char *strata_names[] = {"WORLD",  "BACKGROUND", "LOW",
                                     "MEDIUM", "HIGH",       "DIALOG",
                                     "FULLSCREEN", "FULLSCREEN_DIALOG",
                                     "TOOLTIP"};

static int strata_rank(const char *name, int fallback) {
  for (int i = 0; name && i < 9; i++)
    if (strcmp(strata_names[i], name) == 0)
      return i;
  return fallback;
}

static bool is_frame_tag(const char *tag) {
  static const char *tags[] = {"Frame",      "Button",   "CheckButton",
                               "StatusBar",  "Slider",   "ScrollFrame",
                               "EditBox",    "Model",    "Cooldown",
                               "GameTooltip", "MessageFrame"};
  for (size_t i = 0; i < sizeof(tags) / sizeof(*tags); i++)
    if (strcmp(tags[i], tag) == 0)
      return true;
  return false;
}

static void add_frame(const Element *element, int parent, const char *parent_name,
                      int parent_strata, int parent_level) {
  Contributors c = {0};
  gather(element, &c);

  char name[160] = "";
  const char *raw = effective_attribute(&c, "name");
  if (raw)
    expand_name(raw, parent_name, name, sizeof(name));

  bool bar = strcmp(element->tag, "StatusBar") == 0;
  int node = new_node(bar ? UI_STATUSBAR : UI_FRAME, parent, name);
  UiNodeDef *def = &nodes[node];

  read_size(&c, node);
  read_anchors(&c, node, parent_name);
  def->set_all_points = is_true(effective_attribute(&c, "setAllPoints"));
  def->hidden = is_true(effective_attribute(&c, "hidden"));

  int strata = strata_rank(effective_attribute(&c, "frameStrata"), parent_strata);
  const char *level_text = effective_attribute(&c, "frameLevel");
  int level = level_text ? atoi(level_text) : parent_level + 1;
  frames[frame_count++] = (Frame){(Element *)element, node, strata, level, frame_count};

  for (int i = 0; i < c.count; i++) {
    const Element *layers = child(c.elements[i], "Layers");
    if (layers)
      add_layers(layers, node, name);
  }

  const Element *normal = effective_child(&c, "NormalTexture");
  if (normal)
    add_region(normal, node, 2, name);

  def->clickable = strcmp(element->tag, "Button") == 0 ||
                   strcmp(element->tag, "CheckButton") == 0;
  add_state_texture(&c, node, name, "PushedTexture", UI_STATE_PUSHED, 3);
  add_state_texture(&c, node, name, "HighlightTexture", UI_STATE_HIGHLIGHT, 4);
  add_state_texture(&c, node, name, "CheckedTexture", UI_STATE_CHECKED, 4);

  const Element *bar_texture = bar ? effective_child(&c, "BarTexture") : NULL;
  if (bar_texture) {
    int fill = new_node(UI_BARFILL, node, "");
    const char *file = attribute(bar_texture, "file");
    if (file)
      texture_path(file, &node_textures[fill]);
    nodes[fill].set_all_points = true;
    read_color(effective_child(&c, "BarColor"), nodes[fill].color,
               &nodes[fill].has_color);
    regions[region_count] = (Region){fill, 2};
    region_frame[region_count++] = node;
  }

  for (int i = 0; i < c.count; i++) {
    const Element *children = child(c.elements[i], "Frames");
    for (int j = 0; children && j < children->child_count; j++)
      if (is_frame_tag(children->children[j]->tag))
        add_frame(children->children[j], node, name, strata, level);
  }
}

static int find_node(const char *name);

//a frame the XML hangs under a parent of another file: its own parent
//attribute names a frame that was added before it
static void add_top_level_frame(const Element *element) {
  const char *parent_name = attribute(element, "parent");
  int parent = parent_name && strcmp(parent_name, "UIParent") != 0
                   ? find_node(parent_name)
                   : UI_SCREEN;
  if (parent == -2) {
    fprintf(stderr, "xml2ui: %s needs %s added first\n",
            attribute(element, "name"), parent_name);
    exit(1);
  }

  int strata = 3, level = 0;
  for (int i = 0; parent != UI_SCREEN && i < frame_count; i++)
    if (frames[i].node == parent) {
      strata = frames[i].strata;
      level = frames[i].level;
    }

  add_frame(element, parent, parent == UI_SCREEN ? "" : node_names[parent],
            strata, level);
}

static int find_node(const char *name) {
  for (int i = 0; i < node_count; i++)
    if (node_names[i][0] && strcmp(node_names[i], name) == 0)
      return i;
  return -2;
}

static void resolve_anchors() {
  for (int node = 0; node < node_count; node++)
    for (int slot = 0; slot < nodes[node].anchor_count; slot++) {
      const char *name = node_relative_names[node][slot];
      int relative = nodes[node].parent;
      if (name)
        relative = strcmp(name, "UIParent") == 0 ? UI_SCREEN : find_node(name);
      if (relative == -2) {
        fprintf(stderr, "xml2ui: %s anchors to unknown %s\n", node_names[node], name);
        relative = nodes[node].parent;
      }
      nodes[node].anchors[slot].relative = relative;
    }
}

static int compare_frames(const void *a, const void *b) {
  const Frame *left = a, *right = b;
  if (left->strata != right->strata)
    return left->strata - right->strata;
  if (left->level != right->level)
    return left->level - right->level;
  return left->sequence - right->sequence;
}

static void write_string(FILE *out, const char *text) {
  if (!text) {
    fputs("NULL", out);
    return;
  }
  fprintf(out, "\"%s\"", text);
}

static void write_float(FILE *out, float value) {
  char text[32];
  snprintf(text, sizeof(text), "%.9g", value);
  fputs(text, out);
  if (!strpbrk(text, ".e"))
    fputs(".0", out);
  fputc('f', out);
}

static void write_floats(FILE *out, const float *values, int count) {
  for (int i = 0; i < count; i++) {
    if (i)
      fputs(", ", out);
    write_float(out, values[i]);
  }
}

static void write_node(FILE *out, int index) {
  const UiNodeDef *n = &nodes[index];
  static const char *kinds[] = {"UI_FRAME", "UI_STATUSBAR", "UI_TEXTURE",
                                "UI_FONTSTRING", "UI_BARFILL"};

  fprintf(out, "  {%s, ", kinds[n->kind]);
  write_string(out, node_names[index]);
  fprintf(out, ", %d, ", n->parent);
  write_string(out, node_textures[index]);
  fputs(", ", out);
  write_floats(out, &n->width, 2);
  fprintf(out, ", %d, {", n->anchor_count);
  for (int i = 0; i < n->anchor_count; i++) {
    const UiAnchorDef *a = &n->anchors[i];
    fprintf(out, "{%d, %d, %d, ", a->point, a->relative, a->relative_point);
    write_floats(out, &a->x, 2);
    fputs("}, ", out);
  }
  fprintf(out, "}, %d, %d, %d, %d, %d, {", n->set_all_points, n->hidden,
          n->additive, n->has_tex_coords, n->has_color);
  write_floats(out, n->tex_coords, 4);
  fputs("}, {", out);
  write_floats(out, n->color, 4);
  fputs("}, ", out);
  write_float(out, n->font_size);
  fprintf(out, ", %d, %d, %d},\n", n->justify, n->clickable, n->state);
}

static int compare_regions(const void *a, const void *b) {
  const Region *left = a, *right = b;
  if (left->layer != right->layer)
    return left->layer - right->layer;
  return left->node - right->node;
}

static void write_draw_order(FILE *out) {
  qsort(frames, frame_count, sizeof(Frame), compare_frames);

  int order[MAX_NODES];
  int count = 0;
  for (int f = 0; f < frame_count; f++) {
    Region mine[MAX_NODES];
    int mine_count = 0;
    for (int r = 0; r < region_count; r++)
      if (region_frame[r] == frames[f].node)
        mine[mine_count++] = regions[r];
    qsort(mine, mine_count, sizeof(Region), compare_regions);
    for (int r = 0; r < mine_count; r++)
      order[count++] = mine[r].node;
  }

  fputs("const int ui_draw_order[] = {", out);
  for (int i = 0; i < count; i++)
    fprintf(out, "%d, ", order[i]);
  fprintf(out, "};\nconst int ui_draw_count = %d;\n", count);
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: xml2ui <game data> <out.c> <FrameName>...\n");
    return 1;
  }

  snprintf(game_data, sizeof(game_data), "%s", argv[1]);
  char directory[1024];
  snprintf(directory, sizeof(directory), "%s/interface/framexml", argv[1]);
  load_templates(directory);

  for (int i = 3; i < argc; i++) {
    bool found = false;
    DIR *dir = opendir(directory);
    for (struct dirent *entry; !found && (entry = readdir(dir));) {
      size_t length = strlen(entry->d_name);
      if (length < 5 || strcasecmp(entry->d_name + length - 4, ".xml") != 0)
        continue;
      char path[1024];
      snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
      Element *root = read_xml(path);
      for (int j = 0; root && j < root->child_count; j++) {
        const char *name = attribute(root->children[j], "name");
        if (name && strcmp(name, argv[i]) == 0 &&
            !is_true(attribute(root->children[j], "virtual"))) {
          add_top_level_frame(root->children[j]);
          found = true;
          break;
        }
      }
    }
    closedir(dir);
    if (!found) {
      fprintf(stderr, "xml2ui: no frame %s\n", argv[i]);
      return 1;
    }
  }

  resolve_anchors();

  FILE *out = fopen(argv[2], "w");
  if (!out) {
    fprintf(stderr, "xml2ui: can't write %s\n", argv[2]);
    return 1;
  }

  fputs("//generated by tools/xml2ui, do not edit\n#include <stddef.h>\n#include \"ui_layout.h\"\n\n"
        "const UiNodeDef ui_nodes[] = {\n", out);
  for (int i = 0; i < node_count; i++)
    write_node(out, i);
  fprintf(out, "};\nconst int ui_node_count = %d;\n\n", node_count);
  write_draw_order(out);
  fclose(out);

  fprintf(stderr, "xml2ui: %d nodes, %d frames\n", node_count, frame_count);
  return 0;
}
