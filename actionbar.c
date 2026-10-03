#include "actionbar.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <wowauth/wowdbc.h>

#include "hud.h"

#define SPELL_ICON_FIELD 117
#define ICON_PATH_FIELD 1
#define FORM_BONUS_BAR_FIELD 1
#define ACTION_TYPE_SPELL 0
#define BONUS_PAGES_FIRST 6
#define ICON_PATH_MAX 160
#define SPELL_ATTACK 6603
#define ATTACK_ICON "interface/icons/ability_steelmelee.png"

static PWowDBC spells;
static PWowDBC icons;
static PWowDBC forms;
static bool ready;

static u32 shown_serial;
static int shown_page = -1;

bool actionbar_init(const char *dbc_directory) {
  char path[256];

  snprintf(path, sizeof(path), "%s/Spell.dbc", dbc_directory);
  bool ok = pe_wowdbc_load(path, &spells);
  snprintf(path, sizeof(path), "%s/SpellIcon.dbc", dbc_directory);
  ok = ok && pe_wowdbc_load(path, &icons);
  snprintf(path, sizeof(path), "%s/SpellShapeshiftForm.dbc", dbc_directory);
  ok = ok && pe_wowdbc_load(path, &forms);

  ready = ok;
  return ok;
}

static int find_record(const PWowDBC *dbc, u32 id) {
  for (u32 i = 0; i < dbc->record_count; i++)
    if (pe_wowdbc_get_u32(dbc, i, 0) == id)
      return (int)i;
  return -1;
}

//Interface\Icons\Spell_Nature_Heal -> interface/icons/spell_nature_heal.png
//INFO the Spell.dbc row of Attack names the placeholder icon "Temp", a face; the
//client draws crossed swords for it
static bool spell_icon_path(u32 spell, char *out, size_t size) {
  if (spell == SPELL_ATTACK) {
    snprintf(out, size, "%s", ATTACK_ICON);
    return true;
  }

  int spell_record = find_record(&spells, spell);
  if (spell_record < 0)
    return false;

  int icon_record = find_record(&icons, pe_wowdbc_get_u32(&spells, spell_record, SPELL_ICON_FIELD));
  if (icon_record < 0)
    return false;

  const char *path = pe_wowdbc_get_string(&icons, icon_record, ICON_PATH_FIELD);
  size_t length = 0;
  for (; path[length] && length < size - 5; length++)
    out[length] = path[length] == '\\' ? '/' : (char)tolower((unsigned char)path[length]);
  snprintf(out + length, size - length, ".png");
  return length > 0;
}

static int page_of(const PWowObjectState *state) {
  int form = find_record(&forms, state->player.shapeshift_form);
  u32 bonus = form < 0 ? 0 : pe_wowdbc_get_u32(&forms, form, FORM_BONUS_BAR_FIELD);
  return bonus ? (int)(BONUS_PAGES_FIRST + bonus - 1) : 0;
}

unsigned actionbar_spell(const PWowObjectState *state, int slot) {
  u32 button = state->action_buttons[page_of(state) * ACTIONBAR_BUTTONS + slot - 1];
  return (button >> 24) == ACTION_TYPE_SPELL ? button & 0x00FFFFFF : 0;
}

static void show_slot(const PWowObjectState *state, int slot) {
  char node[64], icon[ICON_PATH_MAX];
  unsigned spell = actionbar_spell(state, slot);
  bool filled = spell && spell_icon_path(spell, icon, sizeof(icon));

  snprintf(node, sizeof(node), "ActionButton%dIcon", slot);
  hud_set_texture(node, filled ? icon : NULL);

  snprintf(node, sizeof(node), "ActionButton%dNormalTexture", slot);
  hud_set_texture(node, filled ? "interface/buttons/ui-quickslot2.png"
                               : "interface/buttons/ui-quickslot.png");
}

void actionbar_update(const PWowObjectState *state) {
  if (!ready || !state->player_valid)
    return;

  int page = page_of(state);
  if (page == shown_page && state->action_buttons_serial == shown_serial)
    return;

  shown_page = page;
  shown_serial = state->action_buttons_serial;
  for (int slot = 1; slot <= ACTIONBAR_BUTTONS; slot++)
    show_slot(state, slot);
}
