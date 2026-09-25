#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNKS 256
#define CHUNKS_PER_SIDE 16
#define VERTICES 145
#define LAYERS_MAX 4
#define TEXTURES_MAX 128
#define TEXTURE_PATH_MAX 128

#define ALPHA_DIM 64
#define ALPHA_SIZE (ALPHA_DIM * ALPHA_DIM)
#define ALPHA_PACKED (ALPHA_SIZE / 2)

#define WHM_MAGIC 0x314D4857

//a chunk's header fields, counted from the byte after its 8 byte chunk header
#define MCNK_INDEX_X 4
#define MCNK_INDEX_Y 8
#define MCNK_LAYERS 12
#define MCNK_OFFSET_HEIGHT 20
#define MCNK_OFFSET_LAYERS 28
#define MCNK_OFFSET_ALPHA 36
#define MCNK_SIZE_ALPHA 40
#define MCNK_HOLES 60
#define MCNK_BASE_HEIGHT 112

#define SUBCHUNK_HEADER 8
#define LAYER_SIZE 16
#define LAYER_USES_ALPHA 0x100
#define LAYER_COMPRESSED 0x200

#define RLE_FILL 0x80
#define RLE_COUNT 0x7F

typedef struct Chunk {
  float base_height;
  float heights[VERTICES];
  uint16_t holes;
  uint32_t layer_count;
  uint32_t layer_textures[LAYERS_MAX];
  uint8_t alpha[LAYERS_MAX - 1][ALPHA_SIZE];
  int found;
} Chunk;

static Chunk chunks[CHUNKS];
static char textures[TEXTURES_MAX][TEXTURE_PATH_MAX];
static int texture_count;

static int fail(const char *message) {
  fprintf(stderr, "adt2wot: %s\n", message);
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

static int has_magic(const uint8_t *at, const char *reversed_magic) {
  return memcmp(at, reversed_magic, 4) == 0;
}

static uint8_t *read_file(const char *path, size_t *size) {
  FILE *file = fopen(path, "rb");
  if (file == NULL)
    return NULL;

  fseek(file, 0, SEEK_END);
  *size = ftell(file);
  fseek(file, 0, SEEK_SET);

  uint8_t *bytes = malloc(*size);
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

static int read_textures(const uint8_t *data, uint32_t size) {
  uint32_t offset = 0;

  while (offset < size && data[offset] != 0) {
    size_t length = strnlen((const char *)data + offset, size - offset);

    if (texture_count == TEXTURES_MAX || length >= TEXTURE_PATH_MAX)
      return fail("a texture name is too long or there are too many");

    memcpy(textures[texture_count], data + offset, length);
    normalise_texture_name(textures[texture_count]);
    texture_count++;
    offset += length + 1;
  }
  return 0;
}

//the last row and column of a four bit map are not painted; the client
//repeats the row and column before them
static void repeat_last_row_and_column(uint8_t *alpha) {
  const int last = ALPHA_DIM - 1;

  for (int i = 0; i < ALPHA_DIM; i++)
    alpha[last * ALPHA_DIM + i] = alpha[(last - 1) * ALPHA_DIM + i];
  for (int i = 0; i < ALPHA_DIM; i++)
    alpha[i * ALPHA_DIM + last] = alpha[i * ALPHA_DIM + last - 1];
}

static void unpack_four_bit(const uint8_t *packed, uint8_t *alpha) {
  for (int i = 0; i < ALPHA_PACKED; i++) {
    alpha[i * 2] = (packed[i] & 0x0F) * 17;
    alpha[i * 2 + 1] = (packed[i] >> 4) * 17;
  }
  repeat_last_row_and_column(alpha);
}

static void unpack_run_length(const uint8_t *packed, uint32_t size,
                              uint8_t *alpha) {
  uint32_t read = 0;
  uint32_t written = 0;

  while (written < ALPHA_SIZE && read < size) {
    uint8_t command = packed[read++];
    int count = (command & RLE_COUNT) + 1;

    if (command & RLE_FILL) {
      if (read == size)
        break;
      uint8_t value = packed[read++];
      for (int i = 0; i < count && written < ALPHA_SIZE; i++)
        alpha[written++] = value;
    } else {
      for (int i = 0; i < count && written < ALPHA_SIZE && read < size; i++)
        alpha[written++] = packed[read++];
    }
  }
}

//a layer's map runs to where the next layer that has one starts, and that
//length is what tells the eight bit form from the four bit one
static uint32_t alpha_length(const uint8_t *layers, uint32_t layer_count,
                             uint32_t layer, uint32_t blob_size) {
  uint32_t offset = read_u32(layers + layer * LAYER_SIZE + 8);

  for (uint32_t next = layer + 1; next < layer_count; next++) {
    if ((read_u32(layers + next * LAYER_SIZE + 4) & LAYER_USES_ALPHA) == 0)
      continue;

    uint32_t next_offset = read_u32(layers + next * LAYER_SIZE + 8);
    return next_offset > offset ? next_offset - offset : 0;
  }

  return blob_size - offset;
}

//a layer above the base without a map covers everything under it
static void decode_layer_alpha(const uint8_t *layers, uint32_t layer_count,
                               uint32_t layer, const uint8_t *blob,
                               uint32_t blob_size, uint8_t *alpha) {
  uint32_t flags = read_u32(layers + layer * LAYER_SIZE + 4);
  uint32_t offset = read_u32(layers + layer * LAYER_SIZE + 8);

  if ((flags & LAYER_USES_ALPHA) == 0) {
    memset(alpha, 255, ALPHA_SIZE);
    return;
  }
  if (offset >= blob_size)
    return;

  uint32_t length = alpha_length(layers, layer_count, layer, blob_size);

  if (flags & LAYER_COMPRESSED)
    unpack_run_length(blob + offset, blob_size - offset, alpha);
  else if (length >= ALPHA_SIZE && offset + ALPHA_SIZE <= blob_size)
    memcpy(alpha, blob + offset, ALPHA_SIZE);
  else if (length >= ALPHA_PACKED && offset + ALPHA_PACKED <= blob_size)
    unpack_four_bit(blob + offset, alpha);
}

static int read_chunk(const uint8_t *chunk_start, uint32_t size) {
  const uint8_t *header = chunk_start + SUBCHUNK_HEADER;
  uint32_t total = size + SUBCHUNK_HEADER;

  uint32_t index_x = read_u32(header + MCNK_INDEX_X);
  uint32_t index_y = read_u32(header + MCNK_INDEX_Y);
  uint32_t layer_count = read_u32(header + MCNK_LAYERS);
  uint32_t offset_height = read_u32(header + MCNK_OFFSET_HEIGHT);
  uint32_t offset_layers = read_u32(header + MCNK_OFFSET_LAYERS);
  uint32_t offset_alpha = read_u32(header + MCNK_OFFSET_ALPHA);
  uint32_t size_alpha = read_u32(header + MCNK_SIZE_ALPHA);

  if (index_x >= CHUNKS_PER_SIDE || index_y >= CHUNKS_PER_SIDE)
    return fail("a chunk has an index outside the tile");
  if (layer_count > LAYERS_MAX)
    return fail("a chunk has more than four layers");
  if (offset_height + SUBCHUNK_HEADER + VERTICES * sizeof(float) > total ||
      offset_layers + SUBCHUNK_HEADER + layer_count * LAYER_SIZE > total)
    return fail("a chunk's heights or layers run past its end");
  if (!has_magic(chunk_start + offset_height, "TVCM") ||
      !has_magic(chunk_start + offset_layers, "YLCM"))
    return fail("a chunk's heights or layers are not where it says");

  Chunk *chunk = &chunks[index_y * CHUNKS_PER_SIDE + index_x];
  chunk->found = 1;
  chunk->layer_count = layer_count;
  chunk->holes = read_u16(header + MCNK_HOLES);
  memcpy(&chunk->base_height, header + MCNK_BASE_HEIGHT, sizeof(float));
  memcpy(chunk->heights, chunk_start + offset_height + SUBCHUNK_HEADER,
         sizeof(chunk->heights));

  const uint8_t *layers = chunk_start + offset_layers + SUBCHUNK_HEADER;
  for (uint32_t i = 0; i < layer_count; i++) {
    chunk->layer_textures[i] = read_u32(layers + i * LAYER_SIZE);
    if (chunk->layer_textures[i] >= (uint32_t)texture_count)
      return fail("a layer names a texture the tile does not list");
  }

  int has_alpha = offset_alpha > 0 && size_alpha > SUBCHUNK_HEADER &&
                  offset_alpha + size_alpha <= total &&
                  has_magic(chunk_start + offset_alpha, "LACM");
  if (has_alpha == 0)
    return 0;

  const uint8_t *blob = chunk_start + offset_alpha + SUBCHUNK_HEADER;
  for (uint32_t i = 1; i < layer_count; i++)
    decode_layer_alpha(layers, layer_count, i, blob,
                       size_alpha - SUBCHUNK_HEADER, chunk->alpha[i - 1]);
  return 0;
}

static int read_tile(const uint8_t *bytes, size_t size) {
  size_t offset = 0;

  while (offset + SUBCHUNK_HEADER <= size) {
    uint32_t chunk_size = read_u32(bytes + offset + 4);
    if (offset + SUBCHUNK_HEADER + chunk_size > size)
      return fail("the file ends inside a chunk");

    const uint8_t *data = bytes + offset + SUBCHUNK_HEADER;
    int result = 0;

    if (has_magic(bytes + offset, "XETM"))
      result = read_textures(data, chunk_size);
    else if (has_magic(bytes + offset, "KNCM"))
      result = read_chunk(bytes + offset, chunk_size);

    if (result != 0)
      return result;
    offset += SUBCHUNK_HEADER + chunk_size;
  }

  for (int i = 0; i < CHUNKS; i++)
    if (chunks[i].found == 0)
      return fail("the tile is missing a chunk");
  return 0;
}

//the tile is named for where it sits: map_<x>_<y>.adt
static int tile_from_path(const char *path, int *tile_x, int *tile_y) {
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;

  const char *last = strrchr(name, '_');
  if (last == NULL || last == name)
    return 0;

  const char *before = last - 1;
  while (before > name && before[-1] != '_')
    before--;
  if (before == name)
    return 0;

  return sscanf(before, "%d_%d.", tile_x, tile_y) == 2;
}

static int write_metadata(const char *path, int tile_x, int tile_y) {
  FILE *file = fopen(path, "w");
  if (file == NULL)
    return fail("can't write the .wot");

  fprintf(file, "{\"format\":\"wot-1.0\",\"tileX\":%d,\"tileY\":%d,", tile_x,
          tile_y);

  fprintf(file, "\"textures\":[");
  for (int i = 0; i < texture_count; i++)
    fprintf(file, "%s\"%s\"", i ? "," : "", textures[i]);

  fprintf(file, "],\"chunkLayers\":[");
  for (int i = 0; i < CHUNKS; i++) {
    fprintf(file, "%s{\"layers\":[", i ? "," : "");
    for (uint32_t l = 0; l < chunks[i].layer_count; l++)
      fprintf(file, "%s%u", l ? "," : "", chunks[i].layer_textures[l]);
    fprintf(file, "],\"holes\":%u}", chunks[i].holes);
  }
  fprintf(file, "]}\n");

  fclose(file);
  return 0;
}

static int write_heightmap(const char *path) {
  FILE *file = fopen(path, "wb");
  if (file == NULL)
    return fail("can't write the .whm");

  uint32_t header[3] = {WHM_MAGIC, CHUNKS, VERTICES};
  fwrite(header, sizeof(uint32_t), 3, file);

  for (int i = 0; i < CHUNKS; i++) {
    uint32_t maps = chunks[i].layer_count > 1 ? chunks[i].layer_count - 1 : 0;
    uint32_t alpha_size = maps * ALPHA_SIZE;

    fwrite(&chunks[i].base_height, sizeof(float), 1, file);
    fwrite(chunks[i].heights, sizeof(float), VERTICES, file);
    fwrite(&alpha_size, sizeof(uint32_t), 1, file);
    for (uint32_t m = 0; m < maps; m++)
      fwrite(chunks[i].alpha[m], 1, ALPHA_SIZE, file);
  }

  fclose(file);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr,
            "usage: adt2wot <map_x_y.adt> <output base>\n"
            "writes <output base>.wot and .whm, and prints the PNG each of "
            "the tile's textures is to be converted to\n");
    return 2;
  }

  int tile_x, tile_y;
  if (tile_from_path(argv[1], &tile_x, &tile_y) == 0)
    return fail("the file name is not map_<x>_<y>.adt");

  size_t size;
  uint8_t *bytes = read_file(argv[1], &size);
  if (bytes == NULL)
    return fail("can't read the .adt");

  char metadata_path[4096];
  char heightmap_path[4096];
  snprintf(metadata_path, sizeof(metadata_path), "%s.wot", argv[2]);
  snprintf(heightmap_path, sizeof(heightmap_path), "%s.whm", argv[2]);

  if (read_tile(bytes, size) != 0 ||
      write_metadata(metadata_path, tile_x, tile_y) != 0 ||
      write_heightmap(heightmap_path) != 0)
    return 1;

  for (int i = 0; i < texture_count; i++)
    puts(textures[i]);

  free(bytes);
  return 0;
}
