#include "equipment.h"

#include <engine/macros.h>
#include <engine/renderer/descriptor_set.h>
#include <engine/renderer/vk_images.h>
#include <engine/renderer/vulkan.h>
#include <wowauth/wowdbc.h>

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

//---------------------------------------------------------------------------
//body skin (CharSections.dbc)
//---------------------------------------------------------------------------

#define CHARSECTIONS_DBC_PATH "data/dbc/CharSections.dbc"
#define CHARSECTIONS_SECTION_SKIN 0
#define TAUREN_RACE_ID 6
#define MALE_SEX_ID 0

#define CHARSECTIONS_FIELD_RACE 1
#define CHARSECTIONS_FIELD_SEX 2
#define CHARSECTIONS_FIELD_BASE_SECTION 3
#define CHARSECTIONS_FIELD_COLOR_INDEX 5
#define CHARSECTIONS_FIELD_TEXTURE1 6

//m22gltf's normalise_texture_name, applied by hand to the one texture path
//this needs: CharSections.dbc's own paths ("Character\Tauren\Male\...blp")
//over into what prepare_character.sh actually wrote to data/
//("character/tauren/male/...png")
static void normalise_texture_path(char *name) {
  for (char *c = name; *c; c++)
    *c = *c == '\\' ? '/' : (char)tolower((unsigned char)*c);

  size_t length = strlen(name);
  if (length > 4 && strcmp(name + length - 4, ".blp") == 0)
    strcpy(name + length - 4, ".png");
}

void resolve_tauren_male_skin_path(u8 skin_id, char *out, size_t out_size) {
  const char *fallback = "data/character/tauren/male/taurenmaleskin00_00.png";
  snprintf(out, out_size, "%s", fallback);

  PWowDBC dbc;
  if (!pe_wowdbc_load(CHARSECTIONS_DBC_PATH, &dbc))
    return;

  for (u32 r = 0; r < dbc.record_count; r++) {
    if (pe_wowdbc_get_u32(&dbc, r, CHARSECTIONS_FIELD_RACE) != TAUREN_RACE_ID)
      continue;
    if (pe_wowdbc_get_u32(&dbc, r, CHARSECTIONS_FIELD_SEX) != MALE_SEX_ID)
      continue;
    if (pe_wowdbc_get_u32(&dbc, r, CHARSECTIONS_FIELD_BASE_SECTION) !=
        CHARSECTIONS_SECTION_SKIN)
      continue;
    if (pe_wowdbc_get_u32(&dbc, r, CHARSECTIONS_FIELD_COLOR_INDEX) != skin_id)
      continue;

    char path[512];
    snprintf(path, sizeof(path), "%s",
             pe_wowdbc_get_string(&dbc, r, CHARSECTIONS_FIELD_TEXTURE1));
    if (path[0] == '\0')
      break;

    normalise_texture_path(path);
    snprintf(out, out_size, "data/%s", path);
    break;
  }

  pe_wowdbc_free(&dbc);
}

//---------------------------------------------------------------------------
//ItemDisplayInfo.dbc
//---------------------------------------------------------------------------

//classic (confirmed against WoWee's own dbc_layouts.json for this
//expansion). the model/texture fields for a weapon (LeftModel/RightModel
//and their textures, fields 1-4) aren't read here yet - drawing a weapon
//needs attaching a second model at a bone, not just a texture, and is out
//of scope for the first pass at equipment (TODO.md's "Character rendering
//polish" item 4)
#define ITEMDISPLAYINFO_DBC_PATH "data/dbc/ItemDisplayInfo.dbc"
#define ITEMDISPLAYINFO_FIELD_ID 0
#define ITEMDISPLAYINFO_FIELD_GEOSET_GROUP1 6
#define ITEMDISPLAYINFO_FIELD_GEOSET_GROUP3 8
#define ITEMDISPLAYINFO_FIELD_TEXTURE_ARM_UPPER 14
#define ITEMDISPLAYINFO_FIELD_TEXTURE_ARM_LOWER 15
#define ITEMDISPLAYINFO_FIELD_TEXTURE_HAND 16
#define ITEMDISPLAYINFO_FIELD_TEXTURE_TORSO_UPPER 17
#define ITEMDISPLAYINFO_FIELD_TEXTURE_TORSO_LOWER 18
#define ITEMDISPLAYINFO_FIELD_TEXTURE_LEG_UPPER 19
#define ITEMDISPLAYINFO_FIELD_TEXTURE_LEG_LOWER 20
#define ITEMDISPLAYINFO_FIELD_TEXTURE_FOOT 21

static void copy_dbc_texture_field(const PWowDBC *dbc, u32 record, u32 field,
                                   char *out, size_t out_size) {
  snprintf(out, out_size, "%s", pe_wowdbc_get_string(dbc, record, field));
}

bool resolve_item_display_info(u32 display_info_id, PItemDisplayInfo *out) {
  memset(out, 0, sizeof(*out));

  PWowDBC dbc;
  if (!pe_wowdbc_load(ITEMDISPLAYINFO_DBC_PATH, &dbc))
    return false;

  bool found = false;
  for (u32 r = 0; r < dbc.record_count; r++) {
    if (pe_wowdbc_get_u32(&dbc, r, ITEMDISPLAYINFO_FIELD_ID) != display_info_id)
      continue;

    out->geoset_group1 =
        pe_wowdbc_get_u32(&dbc, r, ITEMDISPLAYINFO_FIELD_GEOSET_GROUP1);
    out->geoset_group3 =
        pe_wowdbc_get_u32(&dbc, r, ITEMDISPLAYINFO_FIELD_GEOSET_GROUP3);
    copy_dbc_texture_field(&dbc, r, ITEMDISPLAYINFO_FIELD_TEXTURE_TORSO_UPPER,
                           out->texture_torso_upper,
                           sizeof(out->texture_torso_upper));
    copy_dbc_texture_field(&dbc, r, ITEMDISPLAYINFO_FIELD_TEXTURE_TORSO_LOWER,
                           out->texture_torso_lower,
                           sizeof(out->texture_torso_lower));
    copy_dbc_texture_field(&dbc, r, ITEMDISPLAYINFO_FIELD_TEXTURE_LEG_UPPER,
                           out->texture_leg_upper,
                           sizeof(out->texture_leg_upper));
    copy_dbc_texture_field(&dbc, r, ITEMDISPLAYINFO_FIELD_TEXTURE_LEG_LOWER,
                           out->texture_leg_lower,
                           sizeof(out->texture_leg_lower));
    copy_dbc_texture_field(&dbc, r, ITEMDISPLAYINFO_FIELD_TEXTURE_ARM_UPPER,
                           out->texture_arm_upper,
                           sizeof(out->texture_arm_upper));
    copy_dbc_texture_field(&dbc, r, ITEMDISPLAYINFO_FIELD_TEXTURE_ARM_LOWER,
                           out->texture_arm_lower,
                           sizeof(out->texture_arm_lower));
    copy_dbc_texture_field(&dbc, r, ITEMDISPLAYINFO_FIELD_TEXTURE_HAND,
                           out->texture_hand, sizeof(out->texture_hand));
    copy_dbc_texture_field(&dbc, r, ITEMDISPLAYINFO_FIELD_TEXTURE_FOOT,
                           out->texture_foot, sizeof(out->texture_foot));
    found = true;
    break;
  }

  pe_wowdbc_free(&dbc);
  return found;
}

//---------------------------------------------------------------------------
//geoset selection - a port of WoWee's entity_spawner_player.cpp
//---------------------------------------------------------------------------

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

//classic InventoryType (vmangos ItemPrototype.h) - which slot a PEquippedItem
//names, independent of where its own data originally came from (a player's
//PLAYER_VISIBLE_ITEM index, or a humanoid creature's EquipDisplay slot)
#define INVTYPE_HEAD 1
#define INVTYPE_BODY 4 //shirt
#define INVTYPE_CHEST 5
#define INVTYPE_WAIST 6
#define INVTYPE_LEGS 7
#define INVTYPE_FEET 8
#define INVTYPE_WRISTS 9
#define INVTYPE_HANDS 10
#define INVTYPE_CLOAK 16
#define INVTYPE_TABARD 19
#define INVTYPE_ROBE 20

//geoset_rules.hpp's own bare/base ids (WoWee) - a group's variant 1 (or the
//named base one) means "none of this", the same convention taurenmale.glb's
//own export carries
#define GEOSET_BARE_FOREARMS 401  //group 4: no gloves
#define GEOSET_BARE_SHINS 501     //group 5: no boots
#define GEOSET_BARE_SLEEVES 801   //group 8: no chest/wrist sleeves
#define GEOSET_BARE_PANTS 1301    //group 13: no leggings, and a robe's kilt
                                 //replaces this same group
#define GEOSET_NO_CAPE 1501       //group 15
#define GEOSET_WITH_CAPE 1502
#define GEOSET_DEFAULT_TABARD 1201 //group 12
#define GEOSET_BELT_BASE 1801      //group 18

//ItemDisplayInfo's GeosetGroup columns hold a small number G meaning "the
//Gth variant after the bare one" (geoset_rules.hpp's equippedGeoset) - so a
//chest with G=2 wants group 8 variant 3, the bare-sleeves id plus 2. G of
//zero means the item does not touch that group
static u32 equipped_geoset(u32 bare_id, u32 group_value) {
  return bare_id + group_value;
}

//the first item in items[] with one of the given inventory types, or NULL.
//mirrors WoWee's findDisplayIdByInvType, but returns the whole item since
//the caller also wants its geoset_group1/3
static const PEquippedItem *find_item_by_inv_type(const PEquippedItem *items,
                                                  int count,
                                                  const u32 *wanted,
                                                  int wanted_count) {
  for (int i = 0; i < count; i++)
    for (int w = 0; w < wanted_count; w++)
      if (items[i].inventory_type == wanted[w])
        return &items[i];
  return NULL;
}

//WoWee's pickGeoset: prefer the equipped variant if the model actually
//carries it, else fall back (usually the bare id), else 0 - draw nothing
//for this group. asking pe_model_set_active_geosets() for a variant the
//model does not have would silently empty that group instead of falling
//back, since it takes the given set exactly as given
static u32 pick_geoset(PModel *model, u32 preferred, u32 fallback) {
  if (preferred != 0 && pe_model_has_geoset(model, preferred))
    return preferred;
  if (fallback != 0 && pe_model_has_geoset(model, fallback))
    return fallback;
  return 0;
}

#define ACTIVE_GEOSETS_MAX 64

void apply_equipment_geosets(PModel *model, const PEquippedItem *items,
                             int count) {
  u32 active[ACTIVE_GEOSETS_MAX];
  u32 active_count = pe_model_default_geosets(model, active, ACTIVE_GEOSETS_MAX);
  if (active_count > ACTIVE_GEOSETS_MAX)
    active_count = ACTIVE_GEOSETS_MAX;

  //erase the default member of every group equipment below might replace -
  //WoWee's eraseGroup(), groups 4 gloves, 5 boots, 8 sleeves, 13 pants, 15
  //cape, 18 belt
  static const u32 erase_groups[] = {4, 5, 8, 13, 15, 18};
  u32 kept = 0;
  for (u32 i = 0; i < active_count; i++) {
    bool erase = false;
    for (u32 g = 0; g < COUNT_OF(erase_groups); g++)
      if (active[i] / 100 == erase_groups[g])
        erase = true;
    if (!erase)
      active[kept++] = active[i];
  }
  active_count = kept;

  u32 geoset_gloves = pick_geoset(model, GEOSET_BARE_FOREARMS, GEOSET_BARE_FOREARMS);
  u32 geoset_boots = pick_geoset(model, GEOSET_BARE_SHINS, GEOSET_BARE_SHINS);
  u32 geoset_sleeves = pick_geoset(model, GEOSET_BARE_SLEEVES, GEOSET_BARE_SLEEVES);
  u32 geoset_pants = pick_geoset(model, GEOSET_BARE_PANTS, GEOSET_BARE_PANTS);

  //chest/shirt/robe -> sleeves (group 8); a robe's second geoset column
  //also names its kilt over the legs (group 13) - WoWee's kRobeKiltBare
  {
    static const u32 wanted[] = {INVTYPE_BODY, INVTYPE_CHEST, INVTYPE_ROBE};
    const PEquippedItem *item =
        find_item_by_inv_type(items, count, wanted, COUNT_OF(wanted));
    if (item) {
      u32 gg1 = item->display.geoset_group1;
      if (gg1 > 0)
        geoset_sleeves = pick_geoset(
            model, equipped_geoset(GEOSET_BARE_SLEEVES, gg1), GEOSET_BARE_SLEEVES);
      u32 gg3 = item->display.geoset_group3;
      if (gg3 > 0)
        geoset_pants = pick_geoset(model, equipped_geoset(GEOSET_BARE_PANTS, gg3),
                                   GEOSET_BARE_PANTS);
    }
  }
  //legs -> pants (group 13)
  {
    static const u32 wanted[] = {INVTYPE_LEGS};
    const PEquippedItem *item =
        find_item_by_inv_type(items, count, wanted, COUNT_OF(wanted));
    if (item) {
      u32 gg1 = item->display.geoset_group1;
      if (gg1 > 0)
        geoset_pants = pick_geoset(model, equipped_geoset(GEOSET_BARE_PANTS, gg1),
                                   GEOSET_BARE_PANTS);
    }
  }
  //feet -> shins (group 5)
  {
    static const u32 wanted[] = {INVTYPE_FEET};
    const PEquippedItem *item =
        find_item_by_inv_type(items, count, wanted, COUNT_OF(wanted));
    if (item) {
      u32 gg1 = item->display.geoset_group1;
      if (gg1 > 0)
        geoset_boots = pick_geoset(model, equipped_geoset(GEOSET_BARE_SHINS, gg1),
                                   GEOSET_BARE_SHINS);
    }
  }
  //hands -> forearms (group 4)
  {
    static const u32 wanted[] = {INVTYPE_HANDS};
    const PEquippedItem *item =
        find_item_by_inv_type(items, count, wanted, COUNT_OF(wanted));
    if (item) {
      u32 gg1 = item->display.geoset_group1;
      if (gg1 > 0)
        geoset_gloves = pick_geoset(model, equipped_geoset(GEOSET_BARE_FOREARMS, gg1),
                                    GEOSET_BARE_FOREARMS);
    }
  }
  //wrists -> sleeves (group 8), only if chest/shirt/robe didn't already set it
  {
    static const u32 wanted[] = {INVTYPE_WRISTS};
    const PEquippedItem *item =
        find_item_by_inv_type(items, count, wanted, COUNT_OF(wanted));
    if (item && geoset_sleeves == GEOSET_BARE_SLEEVES) {
      u32 gg1 = item->display.geoset_group1;
      if (gg1 > 0)
        geoset_sleeves = pick_geoset(
            model, equipped_geoset(GEOSET_BARE_SLEEVES, gg1), GEOSET_BARE_SLEEVES);
    }
  }
  //waist -> belt (group 18); the base buckle variant even with nothing
  //equipped, not nothing at all - the group was erased above
  u32 geoset_belt;
  {
    static const u32 wanted[] = {INVTYPE_WAIST};
    const PEquippedItem *item =
        find_item_by_inv_type(items, count, wanted, COUNT_OF(wanted));
    u32 gg1 = item ? item->display.geoset_group1 : 0;
    geoset_belt = pick_geoset(
        model, gg1 > 0 ? equipped_geoset(GEOSET_BELT_BASE, gg1) : 0, GEOSET_BELT_BASE);
  }
  //back/cloak (group 15)
  u32 geoset_cape;
  {
    static const u32 wanted[] = {INVTYPE_CLOAK};
    bool has_cloak =
        find_item_by_inv_type(items, count, wanted, COUNT_OF(wanted)) != NULL;
    geoset_cape = pick_geoset(model, has_cloak ? GEOSET_WITH_CAPE : GEOSET_NO_CAPE,
                             GEOSET_NO_CAPE);
  }
  //tabard - a fixed base variant only; no per-tabard art (emblem texture) yet
  bool has_tabard;
  {
    static const u32 wanted[] = {INVTYPE_TABARD};
    has_tabard =
        find_item_by_inv_type(items, count, wanted, COUNT_OF(wanted)) != NULL;
  }

  u32 overrides[] = {geoset_gloves, geoset_boots,  geoset_sleeves,
                    geoset_pants,  geoset_belt,   geoset_cape,
                    has_tabard ? (u32)GEOSET_DEFAULT_TABARD : 0};
  for (u32 i = 0; i < COUNT_OF(overrides); i++)
    if (overrides[i] != 0 && active_count < ACTIVE_GEOSETS_MAX)
      active[active_count++] = overrides[i];

  pe_model_set_active_geosets(model, active, active_count);
}

//---------------------------------------------------------------------------
//equipment textures: compositing ItemDisplayInfo's six body-region overlays
//onto the base skin (WoWee's compositeWithRegions() is the reference)
//---------------------------------------------------------------------------

//creates path and every missing parent directory, tolerating "already
//exists" - the only place pwow creates a directory at runtime; every other
//data/ path is a prepare_*.sh script's job, done offline before pwow ever
//runs, but an item's region art is only named once the live server sends
//it (or, for a humanoid creature, once CreatureDisplayInfoExtra.dbc is
//read), so there is no offline step that could have converted it ahead of
//time
static void make_directories(const char *path) {
  char buf[512];
  snprintf(buf, sizeof(buf), "%s", path);
  for (char *p = buf + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(buf, 0755);
      *p = '/';
    }
  }
  mkdir(buf, 0755);
}

static bool copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  if (!in)
    return false;
  FILE *out = fopen(to, "wb");
  if (!out) {
    fclose(in);
    return false;
  }
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    fwrite(buf, 1, n, out);
  fclose(in);
  fclose(out);
  return true;
}

//runs argv[0] with argv, no shell involved - argv[] is built from a fixed
//tool path plus a texture name that ultimately comes off the wire
//(ItemDisplayInfo.dbc, read by entry the local server names), so this
//never goes through a shell to interpolate it into. blocks for the child;
//true if it exited 0
static bool run_tool(char *const argv[]) {
  pid_t pid = fork();
  if (pid < 0)
    return false;
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
    }
    execv(argv[0], argv);
    _exit(127);
  }
  int status;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#define GAME_DATA_PATH_DEFAULT "/root/sources/WoWee/Data/expansions/classic"
#define BLP_CONVERT_PATH_DEFAULT "/root/sources/WoWee/build/bin/blp_convert"

//one of ItemDisplayInfo's 8 texture-region columns: the folder under
//Item\TextureComponents (item_textures.hpp), lowercased and without the
//backslashes - this repo's extracted game data tree is lowercase
//throughout (WoWee's own MPQ extraction convention), unlike the DBC's own
//spelling - and where its art lands on the 256x256 base skin atlas
//(WoWee's compositeWithRegions(), its own 256-base coordinate table; the
//Tauren male skin already is 256x256, so none of its upscaling applies here)
typedef struct PItemRegion {
  const char *folder;
  int dst_x, dst_y, width, height;
} PItemRegion;

#define ITEM_REGION_COUNT 8
static const PItemRegion ITEM_REGIONS[ITEM_REGION_COUNT] = {
    {"armuppertexture", 0, 0, 128, 64},
    {"armlowertexture", 0, 64, 128, 64},
    {"handtexture", 0, 128, 128, 32},
    {"torsouppertexture", 128, 0, 128, 64},
    {"torsolowertexture", 128, 64, 128, 32},
    {"leguppertexture", 128, 96, 128, 64},
    {"leglowertexture", 128, 160, 128, 64},
    {"foottexture", 128, 224, 128, 32},
};

//resolves one ItemDisplayInfo texture name to a data/ png path, converting
//it from the game's own .blp the first time it's needed - tries the
//gendered file first, then unisex, then the bare name (item_textures.hpp's
//own resolution order), and only ever the male one, since taurenmale.glb
//is the only race/gender pwow has converted. false if tex_name is empty or
//none of the three spellings exist on disk
static bool resolve_item_region_texture(const PItemRegion *region,
                                        const char *tex_name, char *out,
                                        size_t out_size) {
  if (tex_name[0] == '\0')
    return false;

  char lower[128];
  snprintf(lower, sizeof(lower), "%s", tex_name);
  for (char *c = lower; *c; c++)
    *c = (char)tolower((unsigned char)*c);

  const char *game_data = getenv("GAME_DATA");
  if (!game_data)
    game_data = GAME_DATA_PATH_DEFAULT;
  const char *blp_convert = getenv("BLP_CONVERT");
  if (!blp_convert)
    blp_convert = BLP_CONVERT_PATH_DEFAULT;

  static const char *suffixes[] = {"_m", "_u", ""};
  for (int s = 0; s < 3; s++) {
    char source_blp[512];
    snprintf(source_blp, sizeof(source_blp),
            "%s/item/texturecomponents/%s/%s%s.blp", game_data,
            region->folder, lower, suffixes[s]);
    if (access(source_blp, F_OK) != 0)
      continue;

    char dest_dir[480];
    snprintf(dest_dir, sizeof(dest_dir), "data/item/texturecomponents/%s",
            region->folder);
    char dest_base[512];
    snprintf(dest_base, sizeof(dest_base), "%s/%s%s", dest_dir, lower,
            suffixes[s]);
    snprintf(out, out_size, "%s.png", dest_base);

    if (access(out, F_OK) == 0)
      return true; //converted already, by an earlier equip of the same item

    make_directories(dest_dir);
    char dest_blp[532];
    snprintf(dest_blp, sizeof(dest_blp), "%s.blp", dest_base);
    if (!copy_file(source_blp, dest_blp))
      return false;

    char *argv[] = {(char *)blp_convert, "--to-png", dest_blp, NULL};
    bool ok = run_tool(argv);
    remove(dest_blp);
    return ok && access(out, F_OK) == 0;
  }
  return false;
}

//lays src's pixels over dst at (dst_x, dst_y), clamped to whichever of
//src's or the given width/height is smaller - a size mismatch is silently
//truncated rather than refused, since a wrong-sized region texture should
//still draw something close rather than nothing (WoWee's own
//compositeWithRegions() instead rescales; not needed here, since every
//region texture this has seen so far matches its expected size exactly).
//src's alpha decides how much of dst shows through: a shirt or harness is
//straps on bare skin, and copying it would paint its empty areas over the fur
static void blit_region(PImage *dst, PImage *src, int dst_x, int dst_y,
                        int width, int height) {
  int w = width < src->width ? width : src->width;
  int h = height < src->heigth ? height : src->heigth;
  if (dst_x + w > dst->width)
    w = dst->width - dst_x;
  if (dst_y + h > dst->heigth)
    h = dst->heigth - dst_y;

  for (int y = 0; y < h; y++) {
    unsigned char *dst_row =
        dst->pixels_data + ((size_t)(dst_y + y) * dst->width + dst_x) * 4;
    unsigned char *src_row = src->pixels_data + (size_t)y * src->width * 4;
    for (int x = 0; x < w; x++) {
      unsigned char *to = dst_row + x * 4;
      unsigned char *from = src_row + x * 4;
      int alpha = from[3];
      for (int c = 0; c < 3; c++)
        to[c] = (from[c] * alpha + to[c] * (255 - alpha)) / 255;
    }
  }
}

//composites every region texture one equipped item's own ItemDisplayInfo
//names onto base, converting/loading each on demand
static void composite_item_regions(PImage *base, const PItemDisplayInfo *display) {
  struct {
    const PItemRegion *region;
    const char *name;
  } regions[ITEM_REGION_COUNT] = {
      {&ITEM_REGIONS[0], display->texture_arm_upper},
      {&ITEM_REGIONS[1], display->texture_arm_lower},
      {&ITEM_REGIONS[2], display->texture_hand},
      {&ITEM_REGIONS[3], display->texture_torso_upper},
      {&ITEM_REGIONS[4], display->texture_torso_lower},
      {&ITEM_REGIONS[5], display->texture_leg_upper},
      {&ITEM_REGIONS[6], display->texture_leg_lower},
      {&ITEM_REGIONS[7], display->texture_foot},
  };

  for (int i = 0; i < ITEM_REGION_COUNT; i++) {
    char png_path[512];
    if (!resolve_item_region_texture(regions[i].region, regions[i].name,
                                     png_path, sizeof(png_path)))
      continue;

    PImage region_image;
    ZERO(region_image);
    if (pe_load_image(png_path, &region_image) != 0)
      continue;

    blit_region(base, &region_image, regions[i].region->dst_x,
               regions[i].region->dst_y, regions[i].region->width,
               regions[i].region->height);
    free_image(&region_image);
  }
}

#define EQUIPMENT_SKIN_SIZE 256

void apply_equipment_texture(PModel *model, PSkin *skin,
                             const char *base_skin_path,
                             const PEquippedItem *items, int count) {
  PImage base;
  ZERO(base);
  if (pe_load_image(base_skin_path, &base) != 0)
    return;
  if (base.width != EQUIPMENT_SKIN_SIZE || base.heigth != EQUIPMENT_SKIN_SIZE) {
    free_image(&base);
    return;
  }

  for (int i = 0; i < count; i++)
    composite_item_regions(&base, &items[i].display);

  PTexture new_texture;
  ZERO(new_texture);
  pe_vk_create_texture_from_image(&new_texture, &base);
  new_texture.gpu_loaded = true;
  free_image(&base);

  vkDeviceWaitIdle(vk_device);
  pe_vk_clean_image(&model->texture);
  model->texture = new_texture;
  pe_vk_descriptor_skinned_update(model, skin, &main_render_target);
}
