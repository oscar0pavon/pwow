#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define WWB_MAGIC 0x32425757
#define WWC_MAGIC 0x31435757

#define M2_MAGIC "MD20"
#define M2_VANILLA_VERSION 256

//byte offsets of the fields of the header a classic model has, each an element
//count followed by the offset of the elements
#define HEADER_VERTICES 0x44
#define HEADER_VIEWS 0x4C
#define HEADER_TEXTURES 0x5C
#define HEADER_RENDER_FLAGS 0x84
#define HEADER_TEXTURE_LOOKUP 0x94

//what a model is walked into by: a few triangles of its own that are much
//simpler than the ones it is drawn with, a box for a fence or a cylinder for a
//trunk, as indices and then positions. a model with none, a bush or a blade of
//grass, is walked through
#define HEADER_COLLISION_INDICES 0xEC
#define HEADER_COLLISION_VERTICES 0xF4
#define COLLISION_VERTEX_SIZE 12

#define VERTEX_SIZE 48
#define VERTEX_POSITION 0
#define VERTEX_NORMAL 20
#define VERTEX_UV 32

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
#define SUBMESH_FIRST_INDEX 8
#define SUBMESH_INDEX_COUNT 10

#define BATCH_SIZE 24
#define BATCH_SUBMESH 4
#define BATCH_MATERIAL 10
#define BATCH_LAYER 12
#define BATCH_TEXTURE_COMBO 16

//a model draws over its submeshes in layers: the base, then whatever is
//blended over it. a layer above the base that is solid would only cover the
//base, and is left out
#define BASE_LAYER 0

//how the game blends a batch, and the ones the building pipeline draws: as it
//is, cut out at half alpha, blended over what is behind it, and added to it.
//what multiplies the picture, a shadow or a decal, is left out
#define BLEND_OPAQUE 0
#define BLEND_ALPHA_KEY 1
#define BLEND_ALPHA 2
#define BLEND_ADD 3

#define GROUP_EXTERIOR 0x8

#define TEXTURES_MAX 256
#define MATERIALS_MAX 256
#define PATH_MAX_LENGTH 256

#define NO_TEXTURE 0xFFFFFFFFu

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
} Batch;

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

static float box_low[3];
static float box_high[3];

static int fail(const char *message) {
  fprintf(stderr, "m22wwb: %s\n", message);
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

  for (uint32_t i = 0; i < count; i++) {
    uint32_t position = read_u16(triangles + (first + i) * sizeof(uint16_t));
    if (position >= lookup_count)
      return fail("a triangle names a vertex the view does not list");

    uint32_t vertex = lookup[position];
    if (vertex >= vertex_count)
      return fail("the view names a vertex the model does not have");

    indices[index_count++] = vertex;
    grow_box((const float *)(vertices + vertex * VERTEX_SIZE));
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
    fprintf(stderr, "m22wwb: skipped: the model has no view or no vertices\n");
    return EXIT_NOTHING_TO_DRAW;
  }

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

  for (int i = 0; i < 3; i++) {
    box_low[i] = 1e30f;
    box_high[i] = -1e30f;
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

static void write_u32(FILE *file, uint32_t value) {
  fwrite(&value, sizeof(value), 1, file);
}

//a model is a building of one group, drawn from every side, whose vertices
//carry no light of their own: white, which leaves the texture as it is
static void write_group(FILE *file) {
  const uint8_t white[4] = {255, 255, 255, 255};

  write_u32(file, GROUP_EXTERIOR);
  fwrite(box_low, sizeof(float), 3, file);
  fwrite(box_high, sizeof(float), 3, file);
  write_u32(file, vertex_count);
  write_u32(file, index_count);
  write_u32(file, batch_count);

  for (uint32_t i = 0; i < vertex_count; i++) {
    const uint8_t *vertex = vertices + i * VERTEX_SIZE;

    fwrite(vertex + VERTEX_POSITION, sizeof(float), 3, file);
    fwrite(vertex + VERTEX_NORMAL, sizeof(float), 3, file);
    fwrite(vertex + VERTEX_UV, sizeof(float), 2, file);
    fwrite(white, 1, sizeof(white), file);
  }
  fwrite(indices, sizeof(uint32_t), index_count, file);

  for (uint32_t i = 0; i < batch_count; i++) {
    write_u32(file, batches[i].first_index);
    write_u32(file, batches[i].index_count);
    write_u32(file, batches[i].material);
  }
}

static int write_building(const char *path) {
  if (make_directories(path) != 0)
    return fail("can't make the output directory");

  FILE *file = fopen(path, "wb");
  if (file == NULL)
    return fail("can't write the .wwb");

  write_u32(file, WWB_MAGIC);
  fwrite(box_low, sizeof(float), 3, file);
  fwrite(box_high, sizeof(float), 3, file);

  write_u32(file, texture_count);
  for (uint32_t i = 0; i < texture_count; i++) {
    uint16_t length = strlen(textures[i]);
    fwrite(&length, sizeof(length), 1, file);
    fwrite(textures[i], 1, length, file);
  }

  write_u32(file, material_count);
  for (uint32_t i = 0; i < material_count; i++) {
    write_u32(file, materials[i].texture);
    write_u32(file, materials[i].blend);
    write_u32(file, materials[i].flags);
  }

  write_u32(file, 1);
  write_group(file);

  fclose(file);
  return 0;
}

//INFO the collision of a model goes in a file beside it, WWC1: a count of
//positions, a count of indices, the positions as three floats each and the
//indices, three to a triangle, as words. a model that has none has no file
static int write_collision(const char *path) {
  uint32_t position_count, index_count;
  const uint8_t *positions, *indices16;

  if (read_array(HEADER_COLLISION_VERTICES, COLLISION_VERTEX_SIZE,
                 &position_count, &positions) != 0 ||
      read_array(HEADER_COLLISION_INDICES, sizeof(uint16_t), &index_count,
                 &indices16) != 0)
    return 1;

  index_count -= index_count % 3;
  if (index_count == 0)
    return 0;

  for (uint32_t i = 0; i < index_count; i++)
    if (read_u16(indices16 + i * sizeof(uint16_t)) >= position_count)
      return fail("the collision names a position the model does not have");

  FILE *file = fopen(path, "wb");
  if (file == NULL)
    return fail("can't write the .wwc");

  write_u32(file, WWC_MAGIC);
  write_u32(file, position_count);
  write_u32(file, index_count);
  fwrite(positions, COLLISION_VERTEX_SIZE, position_count, file);
  for (uint32_t i = 0; i < index_count; i++)
    write_u32(file, read_u16(indices16 + i * sizeof(uint16_t)));

  fclose(file);
  return 0;
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
  if (argc != 4) {
    fprintf(stderr,
            "usage: m22wwb <game data> <model path> <output directory>\n"
            "converts one classic model, given as the game names it in "
            "lowercase with forward slashes and ending in .m2, to "
            "<output>/<path>.wwb, and what it is walked into by to "
            "<output>/<path>.wwc if it has any, and prints the PNG each "
            "texture is to be converted to, as 'texture <png>'. exits with 3, and writes "
            "nothing, for a model with nothing in it to draw\n");
    return 2;
  }

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
    fprintf(stderr, "m22wwb: skipped %s: nothing in it is drawn\n", model_path);
    return EXIT_NOTHING_TO_DRAW;
  }

  char out_path[PATH_MAX];
  snprintf(out_path, sizeof(out_path), "%s/%s.wwb", argv[3], stem);
  if (write_building(out_path) != 0)
    return 1;

  snprintf(out_path, sizeof(out_path), "%s/%s.wwc", argv[3], stem);
  if (write_collision(out_path) != 0)
    return 1;

  for (uint32_t i = 0; i < texture_count; i++)
    printf("texture %s\n", textures[i]);
  return 0;
}
