#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define WWB_MAGIC 0x32425757
#define WMO_VERSION 17

//a group's chunk header, then the group header inside it: the flags and the
//box are the first things in it, and its sub-chunks start after 68 bytes
#define SUBCHUNK_HEADER 8
#define MOGP_HEADER_SIZE 68
#define MOGP_FLAGS 8
#define MOGP_BOX 12

#define MOHD_GROUPS 4
#define MATERIAL_SIZE 64
#define MATERIAL_FLAGS 0
#define MATERIAL_BLEND 2
#define MATERIAL_TEXTURE 3

#define BATCH_SIZE 24
#define BATCH_FIRST_INDEX 12
#define BATCH_INDEX_COUNT 16
#define BATCH_LAST_VERTEX 20
#define BATCH_MATERIAL 23

#define GROUPS_MAX 64
#define TEXTURES_MAX 256
#define PATH_MAX_LENGTH 256

#define NO_TEXTURE 0xFFFFFFFFu

typedef struct Batch {
  uint32_t first_index;
  uint32_t index_count;
  uint32_t material;
} Batch;

typedef struct Group {
  uint32_t flags;
  float box_low[3];
  float box_high[3];
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t batch_count;
  float *positions;
  float *normals;
  float *uvs;
  uint8_t *colors;
  uint32_t *indices;
  Batch *batches;
} Group;

typedef struct Material {
  uint32_t texture;
  uint32_t blend;
  uint32_t flags;
} Material;

static Group groups[GROUPS_MAX];
static uint32_t group_count;

static Material *materials;
static uint32_t material_count;

static char textures[TEXTURES_MAX][PATH_MAX_LENGTH];
static uint32_t texture_count;

static float box_low[3];
static float box_high[3];

static int fail(const char *message) {
  fprintf(stderr, "wmo2wwb: %s\n", message);
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

static float read_f32(const uint8_t *at) {
  float value;
  memcpy(&value, at, sizeof(value));
  return value;
}

static int has_magic(const uint8_t *at, const char *reversed) {
  return memcmp(at, reversed, 4) == 0;
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

static int find_or_add_texture(const char *name, uint32_t *index) {
  for (uint32_t i = 0; i < texture_count; i++) {
    if (strcmp(textures[i], name) == 0) {
      *index = i;
      return 0;
    }
  }

  if (texture_count == TEXTURES_MAX)
    return fail("the building uses more textures than fit");

  strcpy(textures[texture_count], name);
  *index = texture_count++;
  return 0;
}

//the texture a material names is a byte offset into the block of names. a
//material that names none is left without one and is drawn plain
static int material_texture(const uint8_t *names, uint32_t names_size,
                            uint32_t offset, uint32_t *texture) {
  *texture = NO_TEXTURE;
  if (offset >= names_size || names[offset] == 0)
    return 0;

  size_t length = strnlen((const char *)names + offset, names_size - offset);
  if (length >= PATH_MAX_LENGTH)
    return fail("a texture path is too long");

  char name[PATH_MAX_LENGTH];
  memcpy(name, names + offset, length);
  name[length] = 0;
  normalise_texture_name(name);

  return find_or_add_texture(name, texture);
}

static int read_materials(const uint8_t *data, uint32_t size,
                          const uint8_t *names, uint32_t names_size) {
  material_count = size / MATERIAL_SIZE;
  materials = calloc(material_count ? material_count : 1, sizeof(Material));

  for (uint32_t i = 0; i < material_count; i++) {
    const uint8_t *record = data + i * MATERIAL_SIZE;

    materials[i].flags = read_u32(record + MATERIAL_FLAGS * 4);
    materials[i].blend = read_u32(record + MATERIAL_BLEND * 4);

    if (material_texture(names, names_size,
                         read_u32(record + MATERIAL_TEXTURE * 4),
                         &materials[i].texture) != 0)
      return 1;
  }
  return 0;
}

static int read_root(const char *path, uint32_t *declared_groups) {
  size_t size;
  uint8_t *bytes = read_file(path, &size);
  if (bytes == NULL)
    return fail("can't read the root file");

  const uint8_t *names = NULL;
  uint32_t names_size = 0;
  const uint8_t *material_data = NULL;
  uint32_t material_size = 0;
  int version = 0;
  *declared_groups = 0;

  size_t offset = 0;
  while (offset + SUBCHUNK_HEADER <= size) {
    uint32_t chunk_size = read_u32(bytes + offset + 4);
    if (offset + SUBCHUNK_HEADER + chunk_size > size)
      return fail("the root file ends inside a chunk");

    const uint8_t *data = bytes + offset + SUBCHUNK_HEADER;
    if (has_magic(bytes + offset, "REVM") && chunk_size >= 4)
      version = read_u32(data);
    else if (has_magic(bytes + offset, "DHOM") && chunk_size >= 8)
      *declared_groups = read_u32(data + MOHD_GROUPS);
    else if (has_magic(bytes + offset, "XTOM")) {
      names = data;
      names_size = chunk_size;
    } else if (has_magic(bytes + offset, "TMOM")) {
      material_data = data;
      material_size = chunk_size;
    }
    offset += SUBCHUNK_HEADER + chunk_size;
  }

  if (version != WMO_VERSION)
    return fail("the root file is not version 17");
  if (material_data == NULL || names == NULL)
    return fail("the root file has no materials or no textures");

  int result = read_materials(material_data, material_size, names, names_size);
  free(bytes);
  return result;
}

static void grow_box(const float *low, const float *high) {
  for (int i = 0; i < 3; i++) {
    if (low[i] < box_low[i])
      box_low[i] = low[i];
    if (high[i] > box_high[i])
      box_high[i] = high[i];
  }
}

static int read_batches(const uint8_t *data, uint32_t size, Group *group) {
  group->batch_count = size / BATCH_SIZE;
  group->batches = calloc(group->batch_count ? group->batch_count : 1,
                          sizeof(Batch));

  for (uint32_t i = 0; i < group->batch_count; i++) {
    const uint8_t *record = data + i * BATCH_SIZE;
    Batch *batch = &group->batches[i];

    batch->first_index = read_u32(record + BATCH_FIRST_INDEX);
    batch->index_count = read_u16(record + BATCH_INDEX_COUNT);
    batch->material = record[BATCH_MATERIAL];

    if ((uint64_t)batch->first_index + batch->index_count > group->index_count)
      return fail("a batch reaches past the group's indices");
    if (batch->material >= material_count)
      return fail("a batch names a material the building does not have");
  }
  return 0;
}

static int read_group_chunks(const uint8_t *bytes, uint32_t start, uint32_t end,
                             Group *group) {
  const uint8_t *batch_data = NULL;
  uint32_t batch_size = 0;
  const uint8_t *index_data = NULL;
  const uint8_t *color_data = NULL;
  uint32_t color_count = 0;
  const uint8_t *normal_data = NULL;
  uint32_t normal_count = 0;
  const uint8_t *uv_data = NULL;
  uint32_t uv_count = 0;
  const uint8_t *vertex_data = NULL;

  uint32_t offset = start;
  while ((uint64_t)offset + SUBCHUNK_HEADER <= end) {
    uint32_t size = read_u32(bytes + offset + 4);
    const uint8_t *data = bytes + offset + SUBCHUNK_HEADER;
    if ((uint64_t)offset + SUBCHUNK_HEADER + size > end)
      return fail("a group chunk runs past its end");

    if (has_magic(bytes + offset, "TVOM")) {
      vertex_data = data;
      group->vertex_count = size / 12;
    } else if (has_magic(bytes + offset, "IVOM")) {
      index_data = data;
      group->index_count = size / 2;
    } else if (has_magic(bytes + offset, "RNOM")) {
      normal_data = data;
      normal_count = size / 12;
    } else if (has_magic(bytes + offset, "VTOM")) {
      uv_data = data;
      uv_count = size / 8;
    } else if (has_magic(bytes + offset, "VCOM")) {
      color_data = data;
      color_count = size / 4;
    } else if (has_magic(bytes + offset, "ABOM")) {
      batch_data = data;
      batch_size = size;
    }
    offset += SUBCHUNK_HEADER + size;
  }

  if (vertex_data == NULL || index_data == NULL)
    return fail("a group has no vertices or no indices");

  uint32_t n = group->vertex_count;
  group->positions = malloc(n * 3 * sizeof(float));
  group->normals = malloc(n * 3 * sizeof(float));
  group->uvs = calloc(n * 2, sizeof(float));
  group->colors = malloc(n * 4);
  group->indices = malloc(group->index_count * sizeof(uint32_t));

  memcpy(group->positions, vertex_data, n * 3 * sizeof(float));
  memset(group->colors, 255, n * 4);

  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *normal = normal_data && i < normal_count
                                ? normal_data + i * 12 : NULL;
    group->normals[i * 3 + 0] = normal ? read_f32(normal) : 0;
    group->normals[i * 3 + 1] = normal ? read_f32(normal + 4) : 0;
    group->normals[i * 3 + 2] = normal ? read_f32(normal + 8) : 1;

    if (uv_data && i < uv_count) {
      group->uvs[i * 2 + 0] = read_f32(uv_data + i * 8);
      group->uvs[i * 2 + 1] = read_f32(uv_data + i * 8 + 4);
    }

    //stored blue, green, red, alpha
    if (color_data && i < color_count) {
      const uint8_t *color = color_data + i * 4;
      group->colors[i * 4 + 0] = color[2];
      group->colors[i * 4 + 1] = color[1];
      group->colors[i * 4 + 2] = color[0];
      group->colors[i * 4 + 3] = color[3];
    }
  }

  for (uint32_t i = 0; i < group->index_count; i++) {
    group->indices[i] = read_u16(index_data + i * 2);
    if (group->indices[i] >= n)
      return fail("an index points past the group's vertices");
  }

  return batch_data ? read_batches(batch_data, batch_size, group)
                    : fail("a group has no batches");
}

static int read_group(const char *path, Group *group) {
  size_t size;
  uint8_t *bytes = read_file(path, &size);
  if (bytes == NULL)
    return fail("can't read a group file");

  size_t offset = 0;
  while (offset + SUBCHUNK_HEADER <= size) {
    uint32_t chunk_size = read_u32(bytes + offset + 4);
    if (offset + SUBCHUNK_HEADER + chunk_size > size)
      return fail("a group file ends inside a chunk");

    if (has_magic(bytes + offset, "PGOM")) {
      if (chunk_size < MOGP_HEADER_SIZE)
        return fail("a group header is too short");

      const uint8_t *header = bytes + offset + SUBCHUNK_HEADER;
      group->flags = read_u32(header + MOGP_FLAGS);

      for (int i = 0; i < 3; i++) {
        group->box_low[i] = read_f32(header + MOGP_BOX + i * 4);
        group->box_high[i] = read_f32(header + MOGP_BOX + 12 + i * 4);
      }
      grow_box(group->box_low, group->box_high);

      int result = read_group_chunks(
          bytes, offset + SUBCHUNK_HEADER + MOGP_HEADER_SIZE,
          offset + SUBCHUNK_HEADER + chunk_size, group);
      free(bytes);
      return result;
    }
    offset += SUBCHUNK_HEADER + chunk_size;
  }

  free(bytes);
  return fail("a group file has no group in it");
}

static int make_directories(const char *path) {
  char partial[PATH_MAX_LENGTH * 4];
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

static void write_group(FILE *file, const Group *group) {
  write_u32(file, group->flags);
  fwrite(group->box_low, sizeof(float), 3, file);
  fwrite(group->box_high, sizeof(float), 3, file);
  write_u32(file, group->vertex_count);
  write_u32(file, group->index_count);
  write_u32(file, group->batch_count);

  for (uint32_t i = 0; i < group->vertex_count; i++) {
    fwrite(&group->positions[i * 3], sizeof(float), 3, file);
    fwrite(&group->normals[i * 3], sizeof(float), 3, file);
    fwrite(&group->uvs[i * 2], sizeof(float), 2, file);
    fwrite(&group->colors[i * 4], 1, 4, file);
  }
  fwrite(group->indices, sizeof(uint32_t), group->index_count, file);

  for (uint32_t i = 0; i < group->batch_count; i++) {
    write_u32(file, group->batches[i].first_index);
    write_u32(file, group->batches[i].index_count);
    write_u32(file, group->batches[i].material);
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

  write_u32(file, group_count);
  for (uint32_t i = 0; i < group_count; i++)
    write_group(file, &groups[i]);

  fclose(file);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr,
            "usage: wmo2wwb <game data> <wmo path> <output directory>\n"
            "converts one building, given as the game names it in lowercase "
            "with forward slashes, to <output>/<path>.wwb and prints the PNG "
            "each texture is to be converted to, as 'texture <png>'\n");
    return 2;
  }

  const char *data_directory = argv[1];
  const char *wmo_path = argv[2];
  size_t path_length = strlen(wmo_path);

  if (path_length < 5 || strcmp(wmo_path + path_length - 4, ".wmo") != 0)
    return fail("the building path does not end in .wmo");

  char root_path[PATH_MAX_LENGTH * 4];
  snprintf(root_path, sizeof(root_path), "%s/%s", data_directory, wmo_path);

  uint32_t declared_groups;
  if (read_root(root_path, &declared_groups) != 0)
    return 1;

  if (declared_groups == 0 || declared_groups > GROUPS_MAX) {
    fprintf(stderr, "wmo2wwb: skipped %s: it has %u groups and %d is the most "
                    "this takes\n",
            wmo_path, declared_groups, GROUPS_MAX);
    return 3;
  }

  for (int i = 0; i < 3; i++) {
    box_low[i] = 1e30f;
    box_high[i] = -1e30f;
  }

  group_count = declared_groups;
  for (uint32_t i = 0; i < group_count; i++) {
    char group_path[PATH_MAX_LENGTH * 4];
    snprintf(group_path, sizeof(group_path), "%s/%.*s_%03u.wmo", data_directory,
             (int)(path_length - 4), wmo_path, i);

    if (read_group(group_path, &groups[i]) != 0)
      return 1;
  }

  char out_path[PATH_MAX_LENGTH * 4];
  snprintf(out_path, sizeof(out_path), "%s/%.*s.wwb", argv[3],
           (int)(path_length - 4), wmo_path);
  if (write_building(out_path) != 0)
    return 1;

  for (uint32_t i = 0; i < texture_count; i++)
    printf("texture %s\n", textures[i]);
  return 0;
}
