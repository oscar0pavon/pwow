//prints, for every "simple" (non-humanoid) row of CreatureDisplayInfo.dbc -
//ExtendedDisplayInfoID == 0, meaning its look comes from its own model and
//texture rather than CreatureDisplayInfoExtra's race/gender/skin/face/hair/
//equipment (the same pipeline a player character uses, which this does not
//handle) - the model to convert and, if the display names a texture
//variation, the texture to convert too. usage: resolve_creatures <dbc dir>,
//where <dbc dir> has CreatureDisplayInfo.dbc and CreatureModelData.dbc.
//prints "creature <model.m2> <texture.blp-or-->" once per unique (model,
//texture) pair, same "print what to do" convention adt2wot/m22wwb use for
//prepare_tile.sh - unlike those, this links pengine to reuse wowdbc.h rather
//than write a third WDBC parser, the same way test_auth links it for wowauth
#include <engine/wowauth/wowdbc.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define CREATURE_DISPLAY_FIELD_ID 0
#define CREATURE_DISPLAY_FIELD_MODEL_ID 1
#define CREATURE_DISPLAY_FIELD_EXTENDED 3
#define CREATURE_DISPLAY_FIELD_TEXTURE0 6

#define CREATURE_MODEL_FIELD_ID 0
#define CREATURE_MODEL_FIELD_NAME 2

//Creature\Tallstrider\TallStrider.mdx -> creature/tallstrider/tallstrider.m2 -
//same normalisation adt2wot.c's resolve_model() applies to doodad paths
static void normalise_model_path(char *name) {
  for (char *c = name; *c; c++)
    *c = *c == '\\' ? '/' : (char)tolower((unsigned char)*c);

  size_t length = strlen(name);
  if (length > 4 && (strcmp(name + length - 4, ".mdx") == 0 ||
                     strcmp(name + length - 4, ".mdl") == 0))
    strcpy(name + length - 4, ".m2");
}

static void lowercase(char *name) {
  for (char *c = name; *c; c++)
    *c = (char)tolower((unsigned char)*c);
}

#define PAIRS_MAX 1024
static char printed[PAIRS_MAX][512];
static int printed_count;

static int already_printed(const char *line) {
  for (int i = 0; i < printed_count; i++) {
    if (strcmp(printed[i], line) == 0)
      return 1;
  }
  if (printed_count < PAIRS_MAX)
    snprintf(printed[printed_count++], sizeof(printed[0]), "%s", line);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <dbc dir>\n", argv[0]);
    return 1;
  }

  char display_path[512], model_path_arg[512];
  snprintf(display_path, sizeof(display_path), "%s/CreatureDisplayInfo.dbc",
          argv[1]);
  snprintf(model_path_arg, sizeof(model_path_arg), "%s/CreatureModelData.dbc",
          argv[1]);

  PWowDBC display, model;
  if (!pe_wowdbc_load(display_path, &display)) {
    fprintf(stderr, "can't read %s\n", display_path);
    return 1;
  }
  if (!pe_wowdbc_load(model_path_arg, &model)) {
    fprintf(stderr, "can't read %s\n", model_path_arg);
    return 1;
  }

  for (u32 r = 0; r < display.record_count; r++) {
    if (pe_wowdbc_get_u32(&display, r, CREATURE_DISPLAY_FIELD_EXTENDED) != 0)
      continue;

    u32 model_id = pe_wowdbc_get_u32(&display, r, CREATURE_DISPLAY_FIELD_MODEL_ID);

    u32 model_record = model.record_count;
    for (u32 m = 0; m < model.record_count; m++) {
      if (pe_wowdbc_get_u32(&model, m, CREATURE_MODEL_FIELD_ID) == model_id) {
        model_record = m;
        break;
      }
    }
    if (model_record == model.record_count)
      continue;

    char model_path[512];
    snprintf(model_path, sizeof(model_path), "%s",
            pe_wowdbc_get_string(&model, model_record, CREATURE_MODEL_FIELD_NAME));
    if (model_path[0] == '\0')
      continue;
    normalise_model_path(model_path);

    char texture_path[512];
    texture_path[0] = '-';
    texture_path[1] = '\0';

    char variation[512];
    snprintf(variation, sizeof(variation), "%s",
            pe_wowdbc_get_string(&display, r, CREATURE_DISPLAY_FIELD_TEXTURE0));
    if (variation[0] != '\0') {
      lowercase(variation);

      char dir[512];
      snprintf(dir, sizeof(dir), "%s", model_path);
      char *slash = strrchr(dir, '/');
      if (slash)
        *slash = '\0';
      else
        dir[0] = '\0';

      snprintf(texture_path, sizeof(texture_path), "%s%s%s.blp", dir,
               dir[0] ? "/" : "", variation);
    }

    //tab separated, not space: a model's own directory can have a space in
    //it (Creature\Lord Kezzak\...), which would otherwise split wrong under
    //a shell script's "read -r kind model texture"
    char line[1100];
    snprintf(line, sizeof(line), "creature\t%s\t%s", model_path, texture_path);
    if (!already_printed(line))
      printf("%s\n", line);
  }

  pe_wowdbc_free(&display);
  pe_wowdbc_free(&model);
  return 0;
}
