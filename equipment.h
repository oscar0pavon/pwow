#ifndef PWOW_EQUIPMENT_H
#define PWOW_EQUIPMENT_H

#include <engine/model.h>
#include <engine/numbers.h>
#include <engine/skeletal.h>

#include <stdbool.h>

//everything about building a Tauren character's on-screen
//appearance from DBC data: the body skin, and equipped items' geosets and
//region textures. shared by the live player (main.c, whose equipped items
//come off the wire) and humanoid creature templates (creatures.c, whose
//come baked into CreatureDisplayInfoExtra.dbc) - the two differ only in
//where a PEquippedItem list comes from, never in what it means once built

//CharSections.dbc's own sex ids
#define SEX_MALE 0
#define SEX_FEMALE 1

//which of CharSections.dbc's rows a character is built from: its sex, the
//skin colour, the face, and the hair style and colour (a Tauren's horns)
typedef struct PAppearance {
  u8 sex, skin, face, hair_style, hair_color;
} PAppearance;

//scans CharSections.dbc for the Tauren skin row of look's sex and colour
//and writes its data/ path into out. falls back to skin 0 - already
//converted by prepare_character.sh - if the DBC is missing or names no
//such row, since a wrong skin tone beats no body texture at all
void resolve_tauren_skin_path(const PAppearance *look, char *out,
                              size_t out_size);

//the data/ pngs a body is composited from, an empty string for a layer the
//character does not have: the skin is the whole atlas and its extra the mane
//and horns sheet, the rest land on it. a female's underwear has a torso as
//well as a pelvis. sex picks the gendered spelling of an item's textures
typedef struct PBodyLayers {
  u8 sex;
  char skin[512], skin_extra[512];
  char face_lower[512], face_upper[512];
  char scalp_lower[512], scalp_upper[512];
  char pelvis[512], torso[512];
} PBodyLayers;

//scans CharSections.dbc for the Tauren rows of look: the skin, the face,
//the scalp of the hair style and the underwear
void resolve_tauren_body(const PAppearance *look, PBodyLayers *out);

//classic InventoryType (vmangos ItemPrototype.h) - which slot a PEquippedItem
//names, independent of where its own data originally came from (a player's
//PLAYER_VISIBLE_ITEM index, or a humanoid creature's EquipDisplay slot)
#define INVTYPE_HEAD 1
#define INVTYPE_BODY 4 //shirt
#define INVTYPE_CHEST 5
#define INVTYPE_WAIST 6
#define INVTYPE_LEGS 7
#define INVTYPE_FEET 8
#define INVTYPE_SHIELD 14
#define INVTYPE_2HWEAPON 17
#define INVTYPE_WRISTS 9
#define INVTYPE_HANDS 10
#define INVTYPE_CLOAK 16
#define INVTYPE_TABARD 19
#define INVTYPE_ROBE 20

//ItemDisplayInfo.dbc's geoset groups and the six body-region texture names
//an equipped item carries - resolved once per display id and cached by
//the caller (PlayerEquipSlot in main.c, HumanoidEquipSlot in creatures.c),
//not re-queried every frame
typedef struct PItemDisplayInfo {
  u32 geoset_group1, geoset_group3;
  //the model an item is drawn as, apart from the body (a helm, a shoulder,
  //a weapon) and its texture, as the game spells them: "Helm_Leather_A_02.mdx"
  //and "Helm_Leather_A_02Blue". empty for an item that is only body art
  char model[64], model_texture[64];
  char texture_torso_upper[64], texture_torso_lower[64];
  char texture_leg_upper[64], texture_leg_lower[64];
  char texture_arm_upper[64], texture_arm_lower[64];
  char texture_hand[64], texture_foot[64];
} PItemDisplayInfo;

//scans ItemDisplayInfo.dbc for display_info_id's row. false if the DBC is
//missing or names no such row - 0 is a real display id here (an empty slot
//never gets this far)
bool resolve_item_display_info(u32 display_info_id, PItemDisplayInfo *out);

//one equipped item, resolved: which slot it's in (a classic InventoryType -
//vmangos's ItemPrototype.h enum) and what ItemDisplayInfo says about it.
//the player builds this from PLAYER_VISIBLE_ITEM entries resolved over
//CMSG_ITEM_QUERY_SINGLE; a humanoid creature builds it directly from
//CreatureDisplayInfoExtra's own baked EquipDisplay ids - no network round
//trip, since those are already ItemDisplayInfo ids, not item entries
typedef struct PEquippedItem {
  u32 inventory_type;
  PItemDisplayInfo display;
} PEquippedItem;

//selects and applies model's active geosets for the given equipped items -
//a port of WoWee's entity_spawner_player.cpp geoset-selection rules
//(eraseGroup/pickGeoset/equippedGeoset and the per-InventoryType group
//mapping). helm/shoulder model attachment, weapons and belt/tabard art are
//out of scope - only the geoset selection itself is ported. safe to call
//more than once on the same model (an equipment change). it replaces and
//destroys the model's index buffer, so one shared with a template or another
//instance (pe_vk_model_instance*()) has to be given a copy of its own first -
//see pe_model_set_active_geosets()'s own doc comment in pengine
void apply_equipment_geosets(PModel *model, const PEquippedItem *items,
                             int count);

//composites body's layers plus every item's own region textures (their
//.blp converted to a data/ png at runtime, the first time a given one is
//needed - see resolve_item_region_texture() in equipment.c) onto one
//256x256 texture and swaps it into model, rewriting skin's descriptor set
//to point at it. it destroys the texture it finds in model->texture, so an
//instance has to hold a texture of its own there first, not its template's
void apply_equipment_texture(PModel *model, PSkin *skin,
                             const PBodyLayers *body,
                             const PEquippedItem *items, int count);

#endif
