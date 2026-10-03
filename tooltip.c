#include "tooltip.h"

#include <stdio.h>
#include <string.h>

#include "actionbar.h"
#include "bags.h"
#include "hud.h"
#include "questwindow.h"

#define INVENTORY_TYPES 27
#define BACKPACK_BUTTON "MainMenuBarBackpackButton"

static const float WHITE[3] = {1.f, 1.f, 1.f};
static const float GRAY[3] = {0.5f, 0.5f, 0.5f};
static const float RED[3] = {1.f, 0.125f, 0.125f};

//the game's ITEM_QUALITY0..6_COLOR: poor, common, uncommon, rare, epic, legendary, artifact
static const float QUALITY_COLORS[][3] = {
    {0.62f, 0.62f, 0.62f}, {1.f, 1.f, 1.f},       {0.12f, 1.f, 0.f},
    {0.f, 0.44f, 0.87f},   {0.64f, 0.21f, 0.93f}, {1.f, 0.5f, 0.f},
    {0.9f, 0.8f, 0.5f},
};

//ItemPrototype::InventoryType, as the tooltip words it; 0 is not worn
static const char *const SLOT_NAMES[INVENTORY_TYPES] = {
    NULL,        "Head",     "Neck",      "Shoulder", "Shirt",   "Chest",
    "Waist",     "Legs",     "Feet",      "Wrist",    "Hands",   "Finger",
    "Trinket",   "One-Hand", "Off Hand",  "Ranged",   "Back",    "Two-Hand",
    "Bag",       "Tabard",   "Chest",     "Main Hand", "Off Hand", "Held In Off-Hand",
    "Projectile", "Thrown",  "Ranged",
};

const float *tooltip_quality_color(unsigned quality) {
  return QUALITY_COLORS[quality < 7 ? quality : 1];
}

static void show_item(const PWowInventory *inv, u32 entry, u32 player_level) {
  const PWowItemTemplate *template = pe_wowinventory_template(inv, entry);
  if (!template || template->state != PE_WOWINV_TEMPLATE_KNOWN)
    return;

  hud_tooltip_line(template->name, tooltip_quality_color(template->quality));

  if (template->inventory_type > 0 && template->inventory_type < INVENTORY_TYPES &&
      SLOT_NAMES[template->inventory_type])
    hud_tooltip_line(SLOT_NAMES[template->inventory_type], WHITE);

  if (template->required_level > 1) {
    char line[48];
    snprintf(line, sizeof(line), "Requires Level %u", template->required_level);
    hud_tooltip_line(line, player_level < template->required_level ? RED : WHITE);
  }
}

static void show_spell(unsigned spell) {
  const char *name, *rank;
  if (!actionbar_spell_name(spell, &name, &rank))
    return;

  hud_tooltip_line(name, WHITE);
  if (rank[0])
    hud_tooltip_line(rank, GRAY);
}

void tooltip_update(const PWowObjectState *state) {
  hud_tooltip_clear();

  const char *name = hud_hovered_name();
  if (!name || !state->player_valid)
    return;

  unsigned quest_item = quest_window_item_under(name, state);
  if (quest_item) {
    show_item(&state->inventory, quest_item, state->player.level);
    return;
  }

  int slot;
  if (sscanf(name, "ActionButton%d", &slot) == 1) {
    unsigned spell = actionbar_spell(state, slot);
    if (spell)
      show_spell(spell);
    return;
  }

  if (strcmp(name, BACKPACK_BUTTON) == 0) {
    hud_tooltip_line("Backpack", WHITE);
    return;
  }

  bool is_bag_button;
  u32 entry = bags_item_under(name, &state->inventory, &is_bag_button);
  if (entry)
    show_item(&state->inventory, entry, state->player.level);
  else if (is_bag_button)
    hud_tooltip_line("Empty Bag Slot", WHITE);
}
