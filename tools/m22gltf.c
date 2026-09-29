#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define M2_MAGIC "MD20"
#define M2_VANILLA_VERSION 256

//byte offsets of the fields of the header a classic model has, each an element
//count followed by the offset of the elements. shared with m22wwb.c, checked
//there against real data
#define HEADER_VERTICES 0x44
#define HEADER_VIEWS 0x4C
#define HEADER_TEXTURES 0x5C
#define HEADER_RENDER_FLAGS 0x84
#define HEADER_TEXTURE_LOOKUP 0x94

//INFO the generic docs for this field (wowdev's M2 page) describe a newer
//version whose header is 8 bytes shorter here: classic still carries a
//"playable_animation_lookup" array right before this one that later versions
//dropped, and views/colors/textures/materials downstream shift too because
//classic's viewCount here is a real M2Array (embedded views) and not the
//4-byte skin profile count later versions use. found by brute-forcing every
//4-aligned offset in the header for one whose record flags only use the
//documented bone flag bits and whose parent_bone is in range: only 0x34 with
//a 108 byte record scores 100% over all 134 bones of taurenmale.m2, and the
//resulting parent chains and pivots are anatomically sane (paired shoulders,
//a spine, foot chains) while the generic-doc offset (0x2C) and stride (88,
//no slack bytes) are not
#define HEADER_BONES 0x34
#define BONE_SIZE 108
#define BONE_KEY_ID 0
#define BONE_FLAGS 4
#define BONE_PARENT 8
#define BONE_TRANSLATION 12
#define BONE_ROTATION 40
#define BONE_SCALE 68
#define BONE_PIVOT (BONE_SIZE - 12)

//INFO each of the 3 tracks above is 28 bytes, also found empirically: the
//generic docs' M2Track (interpolation, global sequence, a nested array of
//per sequence timestamp arrays, a nested array of per sequence value arrays,
//20 bytes) does not fit the 84 bytes free between the confirmed header and
//pivot (84 / 3 is not 20), but 84 / 3 = 28 does, with an extra M2Array
//between global sequence and timestamps. reading it as the classic "ranges"
//field confirms it: timestamps and values here are flat, one array for the
//whole model's every sub animation concatenated, and ranges is a [start,end]
//pair per sub animation (134 bones' worth of these were checked: every pair
//is in bounds, and adjacent pairs are contiguous). rotation's values are
//C4Quaternion (4 floats, magnitude 1.0 checked), not the later compressed
//4x int16, matching the doc's version split
#define TRACK_SIZE 28
#define TRACK_GLOBAL_SEQ 2
#define TRACK_RANGES 4
#define TRACK_TIMESTAMPS 12
#define TRACK_VALUES 20

#define VERTEX_SIZE 48
#define VERTEX_POSITION 0
#define VERTEX_BONE_WEIGHTS 12
#define VERTEX_BONE_INDICES 16
#define VERTEX_NORMAL 20
#define VERTEX_UV 32
#define VERTEX_BONES_PER_VERTEX 4

#define TEXTURE_SIZE 16
#define TEXTURE_TYPE_FILE 0

#define RENDER_FLAGS_SIZE 4

//the first view is the model at its finest, and is all this reads. the others
//are cheaper copies of it for a long way off
#define VIEW_SIZE 44
#define VIEW_INDICES 0
#define VIEW_TRIANGLES 8
#define VIEW_SUBMESHES 24
#define VIEW_BATCHES 32

#define SUBMESH_SIZE 32
#define SUBMESH_GEOSET 0
#define SUBMESH_FIRST_INDEX 8
#define SUBMESH_INDEX_COUNT 10

#define BATCH_SIZE 24
#define BATCH_SUBMESH 4
#define BATCH_MATERIAL 10
#define BATCH_LAYER 12
#define BATCH_TEXTURE_COMBO 16

#define BASE_LAYER 0

#define BLEND_OPAQUE 0
#define BLEND_ALPHA_KEY 1
#define BLEND_ALPHA 2
#define BLEND_ADD 3

#define MATERIAL_TWO_SIDED 0x4

#define TEXTURES_MAX 256
#define MATERIALS_MAX 256
#define BONES_MAX 512
#define PATH_MAX_LENGTH 256

#define NO_TEXTURE 0xFFFFFFFFu

//INFO 68 bytes, once again not what the generic docs give for a newer
//version (they show a single 4 byte "duration" where classic has two 4 byte
//timestamps, an 8 field. this one wasn't found by brute force but by the
//file's own layout: the sequences array (header 0x1C) and the array right
//after it (sequence lookups, header 0x24) are stored back to back in the
//file, 8704 bytes apart for 128 sequences, and 8704 / 128 is exactly 68 --
//which is also exactly what summing the documented old-version fields comes
//to by hand
#define HEADER_SEQUENCES 0x1C
#define SEQUENCE_SIZE 68
#define SEQUENCE_ID 0
#define SEQUENCE_VARIATION 2

#define SEQUENCES_MAX 512
#define ANIMATION_NAMES_MAX 1024
#define ANIMATION_NAME_LENGTH 64

#define EXIT_NOTHING_TO_DRAW 3

typedef struct Material {
  uint32_t texture;
  uint32_t blend;
  uint32_t flags;
} Material;

typedef struct Batch {
  uint32_t first_index;
  uint32_t index_count;
  uint32_t material;
  uint32_t geoset;
} Batch;

//a flat array of keyframes for every sub animation the model has, back to
//back: global_seq is -1 for one driven by the model's own sub animations,
//where ranges holds a [start,end] pair per sub animation into timestamps and
//values, or >= 0 for one that loops on its own, constantly, off one of the
//model's few global sequences, where ranges is empty and the whole of
//timestamps and values is one loop
typedef struct Track {
  int32_t global_seq;
  uint32_t ranges_count;
  uint32_t ranges_offset;
  uint32_t count;
  uint32_t timestamps_offset;
  uint32_t values_offset;
} Track;

typedef struct Bone {
  int32_t key_bone_id;
  uint32_t flags;
  int32_t parent;
  float pivot[3];
  Track translation;
  Track rotation;
  Track scale;
  uint32_t *children;
  uint32_t child_count;
  uint32_t child_cap;
} Bone;

static uint8_t *model;
static size_t model_size;

static Material materials[MATERIALS_MAX];
static uint32_t material_count;

static char textures[TEXTURES_MAX][PATH_MAX_LENGTH];
static uint32_t texture_count;

static const uint8_t *vertices;
static uint32_t vertex_count;
static uint32_t *indices;
static uint32_t index_count;
static Batch *batches;
static uint32_t batch_count;

static Bone bones[BONES_MAX];
static uint32_t bone_count;

//one per sub animation, in the same order the bone tracks' ranges count
//them: id looks a name up in AnimationData.dbc, variation is which one of a
//row of animations sharing that id this is (0 for the first or only one)
typedef struct Sequence {
  uint32_t id;
  uint32_t variation;
} Sequence;

static Sequence sequences[SEQUENCES_MAX];
static uint32_t sequence_count;

typedef struct AnimationName {
  uint32_t id;
  char name[ANIMATION_NAME_LENGTH];
} AnimationName;

static AnimationName animation_names[ANIMATION_NAMES_MAX];
static uint32_t animation_name_count;

static float box_low[3];
static float box_high[3];

static int fail(const char *message) {
  fprintf(stderr, "m22gltf: %s\n", message);
  return 1;
}

static uint32_t read_u32(const uint8_t *at) {
  uint32_t value;
  memcpy(&value, at, sizeof(value));
  return value;
}

static uint16_t read_u16(const uint8_t *at) {
  uint16_t value;
  memcpy(&value, at, sizeof(value));
  return value;
}

static uint8_t *read_file(const char *path, size_t *size) {
  FILE *file = fopen(path, "rb");
  if (file == NULL)
    return NULL;

  fseek(file, 0, SEEK_END);
  *size = ftell(file);
  fseek(file, 0, SEEK_SET);

  uint8_t *bytes = malloc(*size ? *size : 1);
  if (bytes != NULL && fread(bytes, 1, *size, file) != *size) {
    free(bytes);
    bytes = NULL;
  }
  fclose(file);
  return bytes;
}

//Tileset\Elwynn\Grass.blp becomes tileset/elwynn/grass.png, the name the
//engine looks a texture up by
static void normalise_texture_name(char *name) {
  for (char *c = name; *c; c++)
    *c = *c == '\\' ? '/' : tolower((unsigned char)*c);

  size_t length = strlen(name);
  if (length > 4 && strcmp(name + length - 4, ".blp") == 0)
    strcpy(name + length - 4, ".png");
}

//whether count records of size bytes, from offset, are inside the file
static int in_file(uint32_t offset, uint32_t count, uint32_t size) {
  return (uint64_t)offset + (uint64_t)count * size <= model_size;
}

//a header field: how many, and where they start, checked to lie in the file
static int read_array(uint32_t header_offset, uint32_t size, uint32_t *count,
                      const uint8_t **elements) {
  *count = read_u32(model + header_offset);
  uint32_t offset = read_u32(model + header_offset + 4);

  if (in_file(offset, *count, size) == 0)
    return fail("an array of the model runs past the end of the file");

  *elements = model + offset;
  return 0;
}

static int find_or_add_texture(const char *name, uint32_t *index) {
  for (uint32_t i = 0; i < texture_count; i++) {
    if (strcmp(textures[i], name) == 0) {
      *index = i;
      return 0;
    }
  }

  if (texture_count == TEXTURES_MAX)
    return fail("the model uses more textures than fit");

  strcpy(textures[texture_count], name);
  *index = texture_count++;
  return 0;
}

//the model's texture i, as a name in textures. one that is not read from a
//file of its own is painted by the game from a creature's or a character's
//look, which a model placed in the world does not have, and is left without
static int model_texture(uint32_t i, uint32_t *texture) {
  uint32_t count;
  const uint8_t *records;
  *texture = NO_TEXTURE;

  if (read_array(HEADER_TEXTURES, TEXTURE_SIZE, &count, &records) != 0)
    return 1;
  if (i >= count)
    return fail("a batch names a texture the model does not have");

  const uint8_t *record = records + i * TEXTURE_SIZE;
  uint32_t length = read_u32(record + 8);
  uint32_t offset = read_u32(record + 12);

  if (read_u32(record) != TEXTURE_TYPE_FILE || length == 0)
    return 0;
  if (length >= PATH_MAX_LENGTH || in_file(offset, length, 1) == 0)
    return fail("a texture path is too long or outside the file");

  char name[PATH_MAX_LENGTH];
  memcpy(name, model + offset, length);
  name[length] = 0;
  normalise_texture_name(name);
  return find_or_add_texture(name, texture);
}

static int find_or_add_material(const Material *wanted, uint32_t *index) {
  for (uint32_t i = 0; i < material_count; i++) {
    if (memcmp(&materials[i], wanted, sizeof(*wanted)) == 0) {
      *index = i;
      return 0;
    }
  }

  if (material_count == MATERIALS_MAX)
    return fail("the model uses more materials than fit");

  materials[material_count] = *wanted;
  *index = material_count++;
  return 0;
}

static int batch_material(const uint8_t *batch, uint32_t layer,
                          uint32_t *material, int *drawn) {
  uint32_t flag_count, lookup_count;
  const uint8_t *flag_records, *lookup;

  *drawn = 0;
  if (read_array(HEADER_RENDER_FLAGS, RENDER_FLAGS_SIZE, &flag_count,
                 &flag_records) != 0 ||
      read_array(HEADER_TEXTURE_LOOKUP, sizeof(uint16_t), &lookup_count,
                 &lookup) != 0)
    return 1;

  uint32_t flags_index = read_u16(batch + BATCH_MATERIAL);
  uint32_t combo = read_u16(batch + BATCH_TEXTURE_COMBO);
  if (flags_index >= flag_count || combo >= lookup_count)
    return fail("a batch names a material or a texture that is not there");

  uint32_t blend = read_u16(flag_records + flags_index * RENDER_FLAGS_SIZE + 2);
  if (blend > BLEND_ADD)
    return 0;
  if (layer != BASE_LAYER && blend < BLEND_ALPHA)
    return 0;

  Material wanted = {
      .blend = blend,
      .flags = read_u16(flag_records + flags_index * RENDER_FLAGS_SIZE)};
  if (model_texture(read_u16(lookup + combo * sizeof(uint16_t)),
                    &wanted.texture) != 0)
    return 1;

  *drawn = 1;
  return find_or_add_material(&wanted, material);
}

static void grow_box(const float *position) {
  for (int i = 0; i < 3; i++) {
    if (position[i] < box_low[i])
      box_low[i] = position[i];
    if (position[i] > box_high[i])
      box_high[i] = position[i];
  }
}

//glTF is Y-up, right handed; the model's own axes are Z-up (X north, Y west
//in the game's terms). every tool that reads a .glb assumes Y-up, Blender
//included, so this converts rather than carrying the model's native axes
//through unconverted the way m22wwb does for the terrain's own .wwb format.
//pengine's glTF loader does no axis work of its own (see model.c), so this is
//the only place the conversion can happen
static void remap_axis(const float *in, float *out) {
  out[0] = in[0];
  out[1] = in[2];
  out[2] = -in[1];
}

//a per axis scale factor is not a direction, so unlike a position or a
//normal it is relabelled, not reflected
static void remap_scale(const float *in, float *out) {
  out[0] = in[0];
  out[1] = in[2];
  out[2] = in[1];
}

//INFO remap_axis is a proper rotation (determinant +1: it is a -90 degree
//turn about X), so the same relabelling applied to a quaternion's vector
//part, with w untouched, carries the rotation it represents into the
//remapped axes. checked against a 90 degree turn about the model's Y by
//hand: remapped, it comes out as a 90 degree turn about -Z, which is where
//remap_axis sends the Y axis
static void remap_quat(const float *in, float *out) {
  remap_axis(in, out);
  out[3] = in[3];
}

//appends the triangles of one submesh, which the view lists as positions in
//its own table of vertices
static int append_submesh(const uint8_t *submesh, const uint16_t *lookup,
                          uint32_t lookup_count, const uint8_t *triangles,
                          uint32_t triangle_count, uint32_t material) {
  uint32_t first = read_u16(submesh + SUBMESH_FIRST_INDEX);
  uint32_t count = read_u16(submesh + SUBMESH_INDEX_COUNT);

  if (count == 0)
    return 0;
  if ((uint64_t)first + count > triangle_count)
    return fail("a submesh reaches past the model's triangles");

  indices = realloc(indices, (index_count + count) * sizeof(uint32_t));
  batches = realloc(batches, (batch_count + 1) * sizeof(Batch));
  if (indices == NULL || batches == NULL)
    return fail("out of memory");

  Batch *batch = &batches[batch_count++];
  batch->first_index = index_count;
  batch->index_count = count;
  batch->material = material;
  batch->geoset = read_u16(submesh + SUBMESH_GEOSET);

  for (uint32_t i = 0; i < count; i++) {
    uint32_t position = read_u16(triangles + (first + i) * sizeof(uint16_t));
    if (position >= lookup_count)
      return fail("a triangle names a vertex the view does not list");

    uint32_t vertex = lookup[position];
    if (vertex >= vertex_count)
      return fail("the view names a vertex the model does not have");

    indices[index_count++] = vertex;
  }
  return 0;
}

static int read_track(const uint8_t *record, Track *track,
                      uint32_t value_size) {
  track->global_seq = (int16_t)read_u16(record + TRACK_GLOBAL_SEQ);
  track->ranges_count = read_u32(record + TRACK_RANGES);
  track->ranges_offset = read_u32(record + TRACK_RANGES + 4);
  track->count = read_u32(record + TRACK_TIMESTAMPS);
  track->timestamps_offset = read_u32(record + TRACK_TIMESTAMPS + 4);

  uint32_t value_count = read_u32(record + TRACK_VALUES);
  uint32_t values_offset = read_u32(record + TRACK_VALUES + 4);
  if (value_count != track->count)
    return fail("a bone track's timestamps and values do not agree in count");
  track->values_offset = values_offset;

  if (in_file(track->ranges_offset, track->ranges_count, 8) == 0 ||
      in_file(track->timestamps_offset, track->count, 4) == 0 ||
      in_file(track->values_offset, track->count, value_size) == 0)
    return fail("a bone track's keyframes run past the end of the file");
  return 0;
}

static int read_sequences(void) {
  uint32_t count;
  const uint8_t *records;
  if (read_array(HEADER_SEQUENCES, SEQUENCE_SIZE, &count, &records) != 0)
    return 1;
  if (count > SEQUENCES_MAX)
    return fail("the model has more animation sequences than fit");

  sequence_count = count;
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t *record = records + i * SEQUENCE_SIZE;
    sequences[i].id = read_u16(record + SEQUENCE_ID);
    sequences[i].variation = read_u16(record + SEQUENCE_VARIATION);
  }
  return 0;
}

//AnimationData.dbc, read directly rather than through WoWee's dbc_to_csv:
//a WDBC file is a 20 byte header (magic, record count, field count, record
//size, string block size), then the records, then a block of the strings
//they point into. id and name are always its first two fields, string
//fields being a byte offset into that block, true for every WDBC this old
//and not particular to this one
static int read_animation_names(const char *path) {
  size_t size;
  uint8_t *dbc = read_file(path, &size);
  if (dbc == NULL)
    return fail("can't read the animation names file");
  if (size < 20 || memcmp(dbc, "WDBC", 4) != 0) {
    free(dbc);
    return fail("the animation names file is not a WDBC file");
  }

  uint32_t record_count = read_u32(dbc + 4);
  uint32_t record_size = read_u32(dbc + 12);
  uint32_t string_block_size = read_u32(dbc + 16);
  if ((uint64_t)20 + (uint64_t)record_count * record_size + string_block_size >
      size) {
    free(dbc);
    return fail("the animation names file is smaller than its header says");
  }
  if (record_count > ANIMATION_NAMES_MAX) {
    free(dbc);
    return fail("the animation names file has more rows than fit");
  }

  const uint8_t *records = dbc + 20;
  const uint8_t *strings = records + (size_t)record_count * record_size;
  for (uint32_t i = 0; i < record_count; i++) {
    const uint8_t *record = records + i * record_size;
    uint32_t name_offset = read_u32(record + 4);
    if (name_offset >= string_block_size) {
      free(dbc);
      return fail("the animation names file names a string outside its block");
    }
    animation_names[i].id = read_u32(record);
    snprintf(animation_names[i].name, ANIMATION_NAME_LENGTH, "%s",
            (const char *)(strings + name_offset));
  }
  animation_name_count = record_count;

  free(dbc);
  return 0;
}

static const char *find_animation_name(uint32_t id) {
  for (uint32_t i = 0; i < animation_name_count; i++)
    if (animation_names[i].id == id)
      return animation_names[i].name;
  return NULL;
}

static int read_bones(void) {
  uint32_t count;
  const uint8_t *records;
  if (read_array(HEADER_BONES, BONE_SIZE, &count, &records) != 0)
    return 1;
  if (count > BONES_MAX)
    return fail("the model has more bones than fit");

  bone_count = count;
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t *record = records + i * BONE_SIZE;
    Bone *bone = &bones[i];

    bone->key_bone_id = (int32_t)read_u32(record + BONE_KEY_ID);
    bone->flags = read_u32(record + BONE_FLAGS);
    bone->parent = (int16_t)read_u16(record + BONE_PARENT);
    remap_axis((const float *)(record + BONE_PIVOT), bone->pivot);
    bone->children = NULL;
    bone->child_count = 0;
    bone->child_cap = 0;

    if (bone->parent < -1 || bone->parent >= (int32_t)count)
      return fail("a bone names a parent that is not there");

    if (read_track(record + BONE_TRANSLATION, &bone->translation, 12) != 0 ||
        read_track(record + BONE_ROTATION, &bone->rotation, 16) != 0 ||
        read_track(record + BONE_SCALE, &bone->scale, 12) != 0)
      return 1;
  }

  for (uint32_t i = 0; i < count; i++) {
    if (bones[i].parent < 0)
      continue;
    Bone *parent = &bones[bones[i].parent];
    if (parent->child_count == parent->child_cap) {
      parent->child_cap = parent->child_cap ? parent->child_cap * 2 : 4;
      parent->children =
          realloc(parent->children, parent->child_cap * sizeof(uint32_t));
    }
    parent->children[parent->child_count++] = i;
  }
  return 0;
}

static int check_vertex_bones(void) {
  for (uint32_t i = 0; i < vertex_count; i++) {
    const uint8_t *vertex = vertices + i * VERTEX_SIZE;
    for (int j = 0; j < VERTEX_BONES_PER_VERTEX; j++) {
      uint8_t weight = vertex[VERTEX_BONE_WEIGHTS + j];
      uint8_t index = vertex[VERTEX_BONE_INDICES + j];
      if (weight != 0 && index >= bone_count)
        return fail("a vertex is weighted to a bone the model does not have");
    }
  }
  return 0;
}

static int read_model(void) {
  if (model_size < 0x104 || memcmp(model, M2_MAGIC, 4) != 0)
    return fail("not a model file");
  if (read_u32(model + 4) != M2_VANILLA_VERSION)
    return fail("not a classic model, whose version is 256");

  uint32_t view_count;
  const uint8_t *views;
  if (read_array(HEADER_VIEWS, VIEW_SIZE, &view_count, &views) != 0 ||
      read_array(HEADER_VERTICES, VERTEX_SIZE, &vertex_count, &vertices) != 0)
    return 1;
  if (view_count == 0 || vertex_count == 0) {
    fprintf(stderr, "m22gltf: skipped: the model has no view or no vertices\n");
    return EXIT_NOTHING_TO_DRAW;
  }

  if (read_bones() != 0 || read_sequences() != 0 || check_vertex_bones() != 0)
    return 1;

  uint32_t lookup_count, triangle_count, submesh_count, batch_records;
  const uint8_t *lookup, *triangles, *submeshes, *batch_table;
  uint32_t offsets[4] = {VIEW_INDICES, VIEW_TRIANGLES, VIEW_SUBMESHES,
                         VIEW_BATCHES};
  const uint32_t sizes[4] = {2, 2, SUBMESH_SIZE, BATCH_SIZE};
  uint32_t *counts[4] = {&lookup_count, &triangle_count, &submesh_count,
                         &batch_records};
  const uint8_t **tables[4] = {&lookup, &triangles, &submeshes, &batch_table};

  for (int i = 0; i < 4; i++) {
    uint32_t count = read_u32(views + offsets[i]);
    uint32_t offset = read_u32(views + offsets[i] + 4);
    if (in_file(offset, count, sizes[i]) == 0)
      return fail("an array of the view runs past the end of the file");
    *counts[i] = count;
    *tables[i] = model + offset;
  }

  for (uint32_t i = 0; i < batch_records; i++) {
    const uint8_t *batch = batch_table + i * BATCH_SIZE;
    uint32_t submesh = read_u16(batch + BATCH_SUBMESH);
    uint32_t material;
    int drawn;

    if (submesh >= submesh_count)
      return fail("a batch names a submesh that is not there");
    if (batch_material(batch, read_u16(batch + BATCH_LAYER), &material,
                       &drawn) != 0)
      return 1;
    if (drawn == 0)
      continue;

    if (append_submesh(submeshes + submesh * SUBMESH_SIZE,
                       (const uint16_t *)lookup, lookup_count, triangles,
                       triangle_count, material) != 0)
      return 1;
  }
  return 0;
}

static int make_directories(const char *path) {
  char partial[PATH_MAX];
  snprintf(partial, sizeof(partial), "%s", path);

  for (char *c = partial + 1; *c; c++) {
    if (*c != '/')
      continue;
    *c = 0;
    if (mkdir(partial, 0755) != 0 && errno != EEXIST)
      return 1;
    *c = '/';
  }
  return 0;
}

//
// glTF / GLB writing
//

typedef struct Buf {
  uint8_t *data;
  size_t size;
  size_t cap;
} Buf;

typedef struct TrackViews {
  uint32_t val_view;
  int has_data;
} TrackViews;

static void buf_reserve(Buf *buf, size_t extra) {
  if (buf->size + extra <= buf->cap)
    return;
  buf->cap = (buf->size + extra) * 2 + 256;
  buf->data = realloc(buf->data, buf->cap);
}

static void buf_bytes(Buf *buf, const void *source, size_t count) {
  buf_reserve(buf, count);
  memcpy(buf->data + buf->size, source, count);
  buf->size += count;
}

static void buf_str(Buf *buf, const char *s) { buf_bytes(buf, s, strlen(s)); }

static void buf_fmt(Buf *buf, const char *format, ...) {
  char text[1024];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  buf_bytes(buf, text, length);
}

static void buf_pad4(Buf *buf, uint8_t fill) {
  while (buf->size % 4 != 0) {
    buf_reserve(buf, 1);
    buf->data[buf->size++] = fill;
  }
}

static void write_gltf(const char *path) {
  Buf bin = {0};
  Buf json = {0};
  Buf views = {0};
  Buf accessors = {0};

  uint32_t view_count = 0;
  uint32_t accessor_count = 0;

  //positions
  float(*positions)[3] = malloc(vertex_count * sizeof(*positions));
  float(*normals)[3] = malloc(vertex_count * sizeof(*normals));
  float(*uvs)[2] = malloc(vertex_count * sizeof(*uvs));
  uint8_t(*joints)[4] = malloc(vertex_count * sizeof(*joints));
  float(*weights)[4] = malloc(vertex_count * sizeof(*weights));

  for (int i = 0; i < 3; i++) {
    box_low[i] = 1e30f;
    box_high[i] = -1e30f;
  }

  for (uint32_t i = 0; i < vertex_count; i++) {
    const uint8_t *vertex = vertices + i * VERTEX_SIZE;
    remap_axis((const float *)(vertex + VERTEX_POSITION), positions[i]);
    remap_axis((const float *)(vertex + VERTEX_NORMAL), normals[i]);
    memcpy(uvs[i], vertex + VERTEX_UV, sizeof(uvs[i]));
    for (int j = 0; j < 4; j++) {
      joints[i][j] = vertex[VERTEX_BONE_INDICES + j];
      weights[i][j] = vertex[VERTEX_BONE_WEIGHTS + j] / 255.0f;
    }
    grow_box(positions[i]);
  }

  uint32_t position_view = view_count++;
  uint32_t position_offset = (uint32_t)bin.size;
  buf_bytes(&bin, positions, vertex_count * sizeof(*positions));
  buf_pad4(&bin, 0);

  uint32_t normal_view = view_count++;
  uint32_t normal_offset = (uint32_t)bin.size;
  buf_bytes(&bin, normals, vertex_count * sizeof(*normals));
  buf_pad4(&bin, 0);

  uint32_t uv_view = view_count++;
  uint32_t uv_offset = (uint32_t)bin.size;
  buf_bytes(&bin, uvs, vertex_count * sizeof(*uvs));
  buf_pad4(&bin, 0);

  uint32_t joints_view = view_count++;
  uint32_t joints_offset = (uint32_t)bin.size;
  buf_bytes(&bin, joints, vertex_count * sizeof(*joints));
  buf_pad4(&bin, 0);

  uint32_t weights_view = view_count++;
  uint32_t weights_offset = (uint32_t)bin.size;
  buf_bytes(&bin, weights, vertex_count * sizeof(*weights));
  buf_pad4(&bin, 0);

  uint32_t indices_view = view_count++;
  uint32_t indices_offset = (uint32_t)bin.size;
  buf_bytes(&bin, indices, index_count * sizeof(uint32_t));
  buf_pad4(&bin, 0);

  float *ibm = malloc(bone_count * 16 * sizeof(float));
  for (uint32_t i = 0; i < bone_count; i++) {
    float *m = ibm + i * 16;
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    m[12] = -bones[i].pivot[0];
    m[13] = -bones[i].pivot[1];
    m[14] = -bones[i].pivot[2];
  }
  uint32_t ibm_view = view_count++;
  uint32_t ibm_offset = (uint32_t)bin.size;
  buf_bytes(&bin, ibm, bone_count * 16 * sizeof(float));
  buf_pad4(&bin, 0);

  buf_fmt(&views,
         "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962}",
         position_offset, (uint32_t)(vertex_count * sizeof(*positions)));
  buf_fmt(&views,
         ",{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962}",
         normal_offset, (uint32_t)(vertex_count * sizeof(*normals)));
  buf_fmt(&views,
         ",{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962}",
         uv_offset, (uint32_t)(vertex_count * sizeof(*uvs)));
  buf_fmt(&views,
         ",{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962}",
         joints_offset, (uint32_t)(vertex_count * sizeof(*joints)));
  buf_fmt(&views,
         ",{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962}",
         weights_offset, (uint32_t)(vertex_count * sizeof(*weights)));
  buf_fmt(&views,
         ",{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34963}",
         indices_offset, (uint32_t)(index_count * sizeof(uint32_t)));
  buf_fmt(&views, ",{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}",
         ibm_offset, (uint32_t)(bone_count * 16 * sizeof(float)));

  uint32_t position_accessor = accessor_count++;
  buf_fmt(&accessors,
         "{\"bufferView\":%u,\"componentType\":5126,\"count\":%u,"
         "\"type\":\"VEC3\",\"min\":[%.9g,%.9g,%.9g],"
         "\"max\":[%.9g,%.9g,%.9g]}",
         position_view, vertex_count, box_low[0], box_low[1], box_low[2],
         box_high[0], box_high[1], box_high[2]);

  uint32_t normal_accessor = accessor_count++;
  buf_fmt(&accessors,
         ",{\"bufferView\":%u,\"componentType\":5126,\"count\":%u,"
         "\"type\":\"VEC3\"}",
         normal_view, vertex_count);

  uint32_t uv_accessor = accessor_count++;
  buf_fmt(&accessors,
         ",{\"bufferView\":%u,\"componentType\":5126,\"count\":%u,"
         "\"type\":\"VEC2\"}",
         uv_view, vertex_count);

  uint32_t joints_accessor = accessor_count++;
  buf_fmt(&accessors,
         ",{\"bufferView\":%u,\"componentType\":5121,\"count\":%u,"
         "\"type\":\"VEC4\"}",
         joints_view, vertex_count);

  uint32_t weights_accessor = accessor_count++;
  buf_fmt(&accessors,
         ",{\"bufferView\":%u,\"componentType\":5126,\"count\":%u,"
         "\"type\":\"VEC4\"}",
         weights_view, vertex_count);

  uint32_t *batch_accessor = malloc(batch_count * sizeof(uint32_t));
  for (uint32_t i = 0; i < batch_count; i++) {
    batch_accessor[i] = accessor_count++;
    buf_fmt(&accessors,
           ",{\"bufferView\":%u,\"byteOffset\":%u,\"componentType\":5125,"
           "\"count\":%u,\"type\":\"SCALAR\"}",
           indices_view, (uint32_t)(batches[i].first_index * sizeof(uint32_t)),
           batches[i].index_count);
  }

  uint32_t ibm_accessor = accessor_count++;
  buf_fmt(&accessors,
         ",{\"bufferView\":%u,\"componentType\":5126,\"count\":%u,"
         "\"type\":\"MAT4\"}",
         ibm_view, bone_count);

  //one flat, already remapped VALUE buffer per bone per track, written once
  //and sliced by byteOffset/count per clip below: the model's own ranges
  //already give exact [start,end] slices into these, so the values are never
  //duplicated per clip. the timestamps can't be shared the same way: each
  //clip needs its own copy shifted to start at time 0 (the model stores
  //every clip's keyframes back to back on one shared multi-minute timeline,
  //which is what the model calls them by but is not a usable per-clip time
  //base in Blender), so those are written fresh per clip below instead
  TrackViews *translation_views = calloc(bone_count, sizeof(TrackViews));
  TrackViews *rotation_views = calloc(bone_count, sizeof(TrackViews));
  TrackViews *scale_views = calloc(bone_count, sizeof(TrackViews));
  uint32_t anim_count = 0;

  for (uint32_t i = 0; i < bone_count; i++) {
    Track *tracks[3] = {&bones[i].translation, &bones[i].rotation,
                        &bones[i].scale};
    TrackViews *outs[3] = {&translation_views[i], &rotation_views[i],
                           &scale_views[i]};
    int floats_per[3] = {3, 4, 3};

    for (int t = 0; t < 3; t++) {
      Track *track = tracks[t];
      TrackViews *out = outs[t];
      out->has_data = track->global_seq == -1 && track->count > 0;
      if (!out->has_data)
        continue;

      if (track->ranges_count > anim_count)
        anim_count = track->ranges_count;

      //INFO the model's translation track stores each keyframe as a delta
      //off the bone's own pivot, not the node-space (parent-relative)
      //translation a glTF channel replaces the node's rest translation
      //with. the rest translation two bones down encodes both the pivot
      //offset (bone->pivot - parent->pivot) and this delta; leaving the
      //offset out here, as this did before, threw it away every keyframe
      //and left an animated bone's local translation as just the delta -
      //a few hundredths of a unit - collapsing the whole chain below it
      //toward its parent. played back, taurenmale.glb's Stand alone (one
      //keyframed bone, the pelvis) pulled the model from a 2.38 unit tall
      //standing pose down to a -1.21..0.71 heap: the animation looked like
      //it had thrown the character on the ground because every one of its
      //bones effectively had.
      float pivot_offset[3] = {0, 0, 0};
      if (t == 0) {
        if (bones[i].parent >= 0) {
          Bone *parent = &bones[bones[i].parent];
          for (int a = 0; a < 3; a++)
            pivot_offset[a] = bones[i].pivot[a] - parent->pivot[a];
        } else {
          memcpy(pivot_offset, bones[i].pivot, sizeof(pivot_offset));
        }
      }

      float *values = malloc((size_t)track->count * floats_per[t] * sizeof(float));
      for (uint32_t k = 0; k < track->count; k++) {
        const float *raw = (const float *)(model + track->values_offset +
                                           (size_t)k * floats_per[t] * sizeof(float));
        float *dst = values + (size_t)k * floats_per[t];
        if (t == 1)
          remap_quat(raw, dst);
        else if (t == 2)
          remap_scale(raw, dst);
        else {
          remap_axis(raw, dst);
          for (int a = 0; a < 3; a++)
            dst[a] += pivot_offset[a];
        }
      }
      uint32_t val_offset = (uint32_t)bin.size;
      buf_bytes(&bin, values, (size_t)track->count * floats_per[t] * sizeof(float));
      buf_pad4(&bin, 0);
      free(values);
      out->val_view = view_count++;
      buf_fmt(&views, ",{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}",
             val_offset, (uint32_t)((size_t)track->count * floats_per[t] * sizeof(float)));
    }
  }

  //one glTF animation per sub animation (the model's own "ranges" slot),
  //skipping the handful of tracks driven by a global sequence instead (a few
  //bones' idle jiggle, looping independently of any sub animation) to keep
  //this to the motion that actually matters: walking, standing, and the rest
  Buf animations_json = {0};
  int any_animation = 0;
  for (uint32_t k = 0; k < anim_count; k++) {
    Buf channels = {0};
    Buf samplers = {0};
    uint32_t sampler_index = 0;

    for (uint32_t i = 0; i < bone_count; i++) {
      struct {
        Track *track;
        TrackViews *views;
        const char *path;
        int floats_per;
      } entries[3] = {
          {&bones[i].translation, &translation_views[i], "translation", 3},
          {&bones[i].rotation, &rotation_views[i], "rotation", 4},
          {&bones[i].scale, &scale_views[i], "scale", 3},
      };

      for (int t = 0; t < 3; t++) {
        Track *track = entries[t].track;
        TrackViews *tv = entries[t].views;
        if (!tv->has_data || k >= track->ranges_count)
          continue;

        uint32_t start = read_u32(model + track->ranges_offset + k * 8);
        uint32_t end = read_u32(model + track->ranges_offset + k * 8 + 4);
        if (end < start || end >= track->count)
          continue;
        uint32_t length = end - start + 1;
        int floats_per = entries[t].floats_per;

        uint32_t clip_start_ms = read_u32(model + track->timestamps_offset + start * 4);
        float *clip_times = malloc(length * sizeof(float));
        for (uint32_t j = 0; j < length; j++) {
          uint32_t ms = read_u32(model + track->timestamps_offset + (start + j) * 4);
          clip_times[j] = (ms - clip_start_ms) / 1000.0f;
        }
        uint32_t ts_offset = (uint32_t)bin.size;
        buf_bytes(&bin, clip_times, length * sizeof(float));
        buf_pad4(&bin, 0);
        free(clip_times);
        uint32_t ts_view = view_count++;
        buf_fmt(&views, ",{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}",
               ts_offset, (uint32_t)(length * sizeof(float)));

        uint32_t input_accessor = accessor_count++;
        buf_fmt(&accessors,
               ",{\"bufferView\":%u,\"componentType\":5126,"
               "\"count\":%u,\"type\":\"SCALAR\"}",
               ts_view, length);

        uint32_t output_accessor = accessor_count++;
        buf_fmt(&accessors,
               ",{\"bufferView\":%u,\"byteOffset\":%u,\"componentType\":5126,"
               "\"count\":%u,\"type\":\"%s\"}",
               tv->val_view, (uint32_t)(start * floats_per * sizeof(float)),
               length, floats_per == 4 ? "VEC4" : "VEC3");

        buf_fmt(&samplers, "%s{\"input\":%u,\"output\":%u,\"interpolation\":\"LINEAR\"}",
               sampler_index > 0 ? "," : "", input_accessor, output_accessor);
        buf_fmt(&channels, "%s{\"sampler\":%u,\"target\":{\"node\":%u,\"path\":\"%s\"}}",
               sampler_index > 0 ? "," : "", sampler_index, i, entries[t].path);
        sampler_index++;
      }
    }

    if (sampler_index > 0) {
      char clip_name[ANIMATION_NAME_LENGTH + 8];
      const char *found = k < sequence_count
                             ? find_animation_name(sequences[k].id)
                             : NULL;
      if (found == NULL)
        snprintf(clip_name, sizeof(clip_name), "anim_%u", k);
      else if (sequences[k].variation > 0)
        snprintf(clip_name, sizeof(clip_name), "%s_%u", found,
                sequences[k].variation);
      else
        snprintf(clip_name, sizeof(clip_name), "%s", found);

      buf_fmt(&animations_json, "%s{\"name\":\"%s\",\"channels\":[",
             any_animation ? "," : "", clip_name);
      buf_bytes(&animations_json, channels.data, channels.size);
      buf_str(&animations_json, "],\"samplers\":[");
      buf_bytes(&animations_json, samplers.data, samplers.size);
      buf_str(&animations_json, "]}");
      any_animation = 1;
    }
    free(channels.data);
    free(samplers.data);
  }

  buf_str(&json, "{\"asset\":{\"version\":\"2.0\",\"generator\":\"m22gltf\"},");
  buf_str(&json, "\"scene\":0,\"scenes\":[{\"nodes\":[");

  int first = 1;
  for (uint32_t i = 0; i < bone_count; i++) {
    if (bones[i].parent >= 0)
      continue;
    if (!first)
      buf_str(&json, ",");
    buf_fmt(&json, "%u", i);
    first = 0;
  }
  buf_fmt(&json, "%s%u]}],", first ? "" : ",", bone_count);

  buf_str(&json, "\"nodes\":[");
  for (uint32_t i = 0; i < bone_count; i++) {
    Bone *bone = &bones[i];
    float translation[3];
    if (bone->parent >= 0) {
      Bone *parent = &bones[bone->parent];
      for (int a = 0; a < 3; a++)
        translation[a] = bone->pivot[a] - parent->pivot[a];
    } else {
      memcpy(translation, bone->pivot, sizeof(translation));
    }

    if (i > 0)
      buf_str(&json, ",");
    buf_fmt(&json, "{\"name\":\"bone_%u\",\"translation\":[%.9g,%.9g,%.9g]",
           i, translation[0], translation[1], translation[2]);
    if (bone->child_count > 0) {
      buf_str(&json, ",\"children\":[");
      for (uint32_t c = 0; c < bone->child_count; c++)
        buf_fmt(&json, "%s%u", c > 0 ? "," : "", bone->children[c]);
      buf_str(&json, "]");
    }
    buf_str(&json, "}");
  }
  buf_fmt(&json, ",{\"name\":\"taurenmale\",\"mesh\":0,\"skin\":0}],");

  buf_str(&json, "\"skins\":[{\"inverseBindMatrices\":");
  buf_fmt(&json, "%u,\"joints\":[", ibm_accessor);
  for (uint32_t i = 0; i < bone_count; i++)
    buf_fmt(&json, "%s%u", i > 0 ? "," : "", i);
  buf_str(&json, "]}],");

  buf_str(&json, "\"materials\":[");
  for (uint32_t i = 0; i < material_count; i++) {
    Material *material = &materials[i];
    const char *alpha_mode = material->blend == BLEND_OPAQUE ? "OPAQUE"
                             : material->blend == BLEND_ALPHA_KEY ? "MASK"
                                                                   : "BLEND";
    if (i > 0)
      buf_str(&json, ",");
    buf_fmt(&json,
           "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1],"
           "\"metallicFactor\":0,\"roughnessFactor\":1");
    if (material->texture != NO_TEXTURE)
      buf_fmt(&json, ",\"baseColorTexture\":{\"index\":%u}",
             material->texture);
    buf_fmt(&json, "},\"alphaMode\":\"%s\"", alpha_mode);
    if (material->blend == BLEND_ALPHA_KEY)
      buf_str(&json, ",\"alphaCutoff\":0.5");
    if (material->flags & MATERIAL_TWO_SIDED)
      buf_str(&json, ",\"doubleSided\":true");
    buf_str(&json, "}");
  }
  buf_str(&json, "],");

  if (texture_count > 0) {
    buf_str(&json, "\"images\":[");
    for (uint32_t i = 0; i < texture_count; i++)
      buf_fmt(&json, "%s{\"uri\":\"%s\"}", i > 0 ? "," : "", textures[i]);
    buf_str(&json, "],\"samplers\":[{}],\"textures\":[");
    for (uint32_t i = 0; i < texture_count; i++)
      buf_fmt(&json, "%s{\"sampler\":0,\"source\":%u}", i > 0 ? "," : "", i);
    buf_str(&json, "],");
  }

  buf_str(&json, "\"meshes\":[{\"primitives\":[");
  for (uint32_t i = 0; i < batch_count; i++) {
    if (i > 0)
      buf_str(&json, ",");
    buf_fmt(&json,
           "{\"attributes\":{\"POSITION\":%u,\"NORMAL\":%u,\"TEXCOORD_0\":%u,"
           "\"JOINTS_0\":%u,\"WEIGHTS_0\":%u},\"indices\":%u,\"material\":%u,"
           "\"extras\":{\"geoset\":%u}}",
           position_accessor, normal_accessor, uv_accessor, joints_accessor,
           weights_accessor, batch_accessor[i], batches[i].material,
           batches[i].geoset);
  }
  buf_str(&json, "]}],");

  buf_str(&json, "\"animations\":[");
  buf_bytes(&json, animations_json.data, animations_json.size);
  buf_str(&json, "],");
  free(animations_json.data);

  buf_fmt(&json, "\"buffers\":[{\"byteLength\":%u}],", (uint32_t)bin.size);
  buf_str(&json, "\"bufferViews\":[");
  buf_bytes(&json, views.data, views.size);
  buf_str(&json, "],\"accessors\":[");
  buf_bytes(&json, accessors.data, accessors.size);
  buf_str(&json, "]}");

  buf_pad4(&json, ' ');

  FILE *file = fopen(path, "wb");
  uint32_t total = 12 + 8 + (uint32_t)json.size + 8 + (uint32_t)bin.size;
  uint32_t magic = 0x46546C67, version = 2, json_type = 0x4E4F534A,
          bin_type = 0x004E4942;

  fwrite(&magic, 4, 1, file);
  fwrite(&version, 4, 1, file);
  fwrite(&total, 4, 1, file);

  uint32_t json_size = (uint32_t)json.size;
  fwrite(&json_size, 4, 1, file);
  fwrite(&json_type, 4, 1, file);
  fwrite(json.data, 1, json.size, file);

  uint32_t bin_size = (uint32_t)bin.size;
  fwrite(&bin_size, 4, 1, file);
  fwrite(&bin_type, 4, 1, file);
  fwrite(bin.data, 1, bin.size, file);

  fclose(file);

  free(positions);
  free(normals);
  free(uvs);
  free(joints);
  free(weights);
  free(ibm);
  free(translation_views);
  free(rotation_views);
  free(scale_views);
  free(batch_accessor);
  free(bin.data);
  free(json.data);
  free(views.data);
  free(accessors.data);
}

//the game's map files name a model .mdx and the files are .m2, or now and
//then still .mdx
static uint8_t *read_model_file(const char *directory, const char *stem) {
  const char *extensions[] = {".m2", ".mdx"};
  char path[PATH_MAX];

  for (int i = 0; i < 2; i++) {
    snprintf(path, sizeof(path), "%s/%s%s", directory, stem, extensions[i]);
    uint8_t *bytes = read_file(path, &model_size);
    if (bytes != NULL)
      return bytes;
  }
  return NULL;
}

int main(int argc, char **argv) {
  if (argc != 4 && argc != 5) {
    fprintf(stderr,
            "usage: m22gltf <game data> <model path> <output directory> "
            "[AnimationData.dbc]\n"
            "converts one classic model, given as the game names it in "
            "lowercase with forward slashes and ending in .m2, to "
            "<output>/<path>.glb: its mesh, its bones, and every sub "
            "animation's keyframes as a separate glTF animation. with "
            "AnimationData.dbc given, a clip is named after the sequence it "
            "came from (with the sequence's own variation number appended if "
            "it is not the first of that name); without it, or for a clip "
            "past the sequences the model names, it is just 'anim_<n>'. "
            "prints the PNG path each texture is to be converted to, as "
            "'texture <png>', same as m22wwb. exits with 3, and writes "
            "nothing, for a model with nothing in it to draw\n");
    return 2;
  }

  if (argc == 5 && read_animation_names(argv[4]) != 0)
    return 1;

  const char *model_path = argv[2];
  size_t path_length = strlen(model_path);

  if (path_length < 4 || strcmp(model_path + path_length - 3, ".m2") != 0)
    return fail("the model path does not end in .m2");

  char stem[PATH_MAX_LENGTH * 4];
  snprintf(stem, sizeof(stem), "%.*s", (int)(path_length - 3), model_path);

  model = read_model_file(argv[1], stem);
  if (model == NULL)
    return fail("can't read the model");

  int status = read_model();
  if (status != 0)
    return status;

  if (batch_count == 0) {
    fprintf(stderr, "m22gltf: skipped %s: nothing in it is drawn\n", model_path);
    return EXIT_NOTHING_TO_DRAW;
  }

  char out_path[PATH_MAX];
  snprintf(out_path, sizeof(out_path), "%s/%s.glb", argv[3], stem);
  if (make_directories(out_path) != 0)
    return fail("can't make the output directory");

  write_gltf(out_path);

  for (uint32_t i = 0; i < texture_count; i++)
    printf("texture %s\n", textures[i]);
  return 0;
}
