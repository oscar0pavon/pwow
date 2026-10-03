#include "wowdialog.h"

#include <stdio.h>
#include <string.h>

typedef struct Reader {
  const u8 *data;
  int length;
  int position;
  bool overrun;
} Reader;

static bool has(Reader *r, int count) {
  if (r->position + count > r->length) {
    r->overrun = true;
    return false;
  }
  return true;
}

static u32 read_u32(Reader *r) {
  if (!has(r, 4))
    return 0;
  const u8 *p = r->data + r->position;
  r->position += 4;
  return (u32)(p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24));
}

static u64 read_u64(Reader *r) {
  u64 low = read_u32(r);
  u64 high = read_u32(r);
  return low | (high << 32);
}

static u8 read_u8(Reader *r) {
  if (!has(r, 1))
    return 0;
  return r->data[r->position++];
}

static float read_float(Reader *r) {
  u32 bits = read_u32(r);
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

//a string longer than the room is cut and its end still read past
static void read_string(Reader *r, char *out, int size) {
  int length = 0;
  while (r->position < r->length && r->data[r->position]) {
    if (length < size - 1)
      out[length++] = (char)r->data[r->position];
    r->position++;
  }
  out[length] = 0;

  if (r->position >= r->length)
    r->overrun = true;
  else
    r->position++;
}

static Reader reader_of(const u8 *payload, int payload_len) {
  return (Reader){payload, payload_len, 0, false};
}

static void begin(PWowDialog *dialog, PWowDialogKind kind, u64 npc) {
  dialog->kind = kind;
  dialog->npc = npc;
  dialog->serial++;
  dialog->greeting[0] = 0;
  dialog->text_wanted = false;
  dialog->option_count = 0;
  dialog->quest_count = 0;
  dialog->quest_id = 0;
  dialog->title[0] = 0;
  dialog->text[0] = 0;
  dialog->objectives[0] = 0;
  dialog->completable = false;
  dialog->required_money = 0;
  dialog->required_count = 0;
  dialog->choice_count = 0;
  dialog->reward_count = 0;
  dialog->reward_money = 0;
}

static void read_quest_entries(Reader *r, PWowDialog *dialog, int count) {
  for (int i = 0; i < count && !r->overrun; i++) {
    PWowQuestEntry entry;
    entry.id = read_u32(r);
    entry.icon = read_u32(r);
    entry.level = (int)read_u32(r);
    read_string(r, entry.title, sizeof(entry.title));
    if (!r->overrun && dialog->quest_count < PE_WOWDIALOG_QUESTS_MAX)
      dialog->quests[dialog->quest_count++] = entry;
  }
}

//guid, text id, then the options (index, icon, coded, text) and the quests
//(id, icon, level, title). vanilla has no menu id and no box text or money
void pe_wowdialog_handle_gossip(PWowDialog *dialog, const u8 *payload, int payload_len) {
  Reader r = reader_of(payload, payload_len);
  u64 npc = read_u64(&r);
  u32 text_id = read_u32(&r);
  u32 option_count = read_u32(&r);
  if (r.overrun)
    return;

  begin(dialog, PE_WOWDIALOG_GOSSIP, npc);
  dialog->text_id = text_id;
  dialog->text_wanted = true;

  for (u32 i = 0; i < option_count && !r.overrun; i++) {
    PWowGossipOption option;
    option.index = read_u32(&r);
    option.icon = read_u8(&r);
    option.coded = read_u8(&r) != 0;
    read_string(&r, option.text, sizeof(option.text));
    if (!r.overrun && dialog->option_count < PE_WOWDIALOG_OPTIONS_MAX)
      dialog->options[dialog->option_count++] = option;
  }

  u32 quest_count = read_u32(&r);
  read_quest_entries(&r, dialog, (int)quest_count);
}

//the text id, then eight candidates of probability, male text, female text,
//language and three emotes with their delays. one is said, any with something in it
void pe_wowdialog_handle_npc_text(PWowDialog *dialog, const u8 *payload, int payload_len, bool female) {
  Reader r = reader_of(payload, payload_len);
  u32 text_id = read_u32(&r);
  if (dialog->kind != PE_WOWDIALOG_GOSSIP || text_id != dialog->text_id)
    return;

  char chosen[PE_WOWDIALOG_TEXT_MAX] = "";
  float chosen_probability = -1.f;

  for (int i = 0; i < 8 && !r.overrun; i++) {
    float probability = read_float(&r);
    char male[PE_WOWDIALOG_TEXT_MAX], other[PE_WOWDIALOG_TEXT_MAX];
    read_string(&r, male, sizeof(male));
    read_string(&r, other, sizeof(other));
    for (int skipped = 0; skipped < 7; skipped++)
      read_u32(&r);

    const char *text = female && other[0] ? other : male;
    if (r.overrun || !text[0] || probability <= chosen_probability)
      continue;

    chosen_probability = probability;
    snprintf(chosen, sizeof(chosen), "%s", text);
  }

  snprintf(dialog->greeting, sizeof(dialog->greeting), "%s", chosen);
  dialog->serial++;
}

//guid, the greeting, the emote and its delay, a count of one byte, then quests
void pe_wowdialog_handle_quest_list(PWowDialog *dialog, const u8 *payload, int payload_len) {
  Reader r = reader_of(payload, payload_len);
  u64 npc = read_u64(&r);
  char greeting[PE_WOWDIALOG_TEXT_MAX];
  read_string(&r, greeting, sizeof(greeting));
  read_u32(&r);
  read_u32(&r);
  u8 count = read_u8(&r);
  if (r.overrun)
    return;

  begin(dialog, PE_WOWDIALOG_QUEST_LIST, npc);
  snprintf(dialog->greeting, sizeof(dialog->greeting), "%s", greeting);
  read_quest_entries(&r, dialog, count);
}

static void read_items(Reader *r, PWowQuestItem *items, int *count) {
  u32 wire_count = read_u32(r);
  *count = 0;
  for (u32 i = 0; i < wire_count && !r->overrun; i++) {
    PWowQuestItem item;
    item.entry = read_u32(r);
    item.count = read_u32(r);
    item.display_info_id = read_u32(r);
    if (!r->overrun && item.entry && *count < PE_WOWDIALOG_ITEMS_MAX)
      items[(*count)++] = item;
  }
}

//guid, quest id, title, details, objectives, auto accept, the items to choose
//from, the items given and the money, the spell and the emotes
void pe_wowdialog_handle_quest_details(PWowDialog *dialog, const u8 *payload, int payload_len) {
  Reader r = reader_of(payload, payload_len);
  u64 npc = read_u64(&r);
  u32 quest_id = read_u32(&r);
  char title[PE_WOWDIALOG_LINE_MAX];
  char details[PE_WOWDIALOG_TEXT_MAX], objectives[PE_WOWDIALOG_TEXT_MAX];
  read_string(&r, title, sizeof(title));
  read_string(&r, details, sizeof(details));
  read_string(&r, objectives, sizeof(objectives));
  read_u32(&r);
  if (r.overrun)
    return;

  begin(dialog, PE_WOWDIALOG_QUEST_DETAILS, npc);
  dialog->quest_id = quest_id;
  snprintf(dialog->title, sizeof(dialog->title), "%s", title);
  snprintf(dialog->text, sizeof(dialog->text), "%s", details);
  snprintf(dialog->objectives, sizeof(dialog->objectives), "%s", objectives);

  read_items(&r, dialog->choices, &dialog->choice_count);
  read_items(&r, dialog->rewards, &dialog->reward_count);
  dialog->reward_money = read_u32(&r);
}

//guid, quest id, title, text, emote delay, emote, close on cancel, the money
//asked for, the items asked for, then four flags that say whether it may be turned in
void pe_wowdialog_handle_request_items(PWowDialog *dialog, const u8 *payload, int payload_len) {
  Reader r = reader_of(payload, payload_len);
  u64 npc = read_u64(&r);
  u32 quest_id = read_u32(&r);
  char title[PE_WOWDIALOG_LINE_MAX], text[PE_WOWDIALOG_TEXT_MAX];
  read_string(&r, title, sizeof(title));
  read_string(&r, text, sizeof(text));
  read_u32(&r);
  read_u32(&r);
  read_u32(&r);
  u32 money = read_u32(&r);
  if (r.overrun)
    return;

  begin(dialog, PE_WOWDIALOG_QUEST_PROGRESS, npc);
  dialog->quest_id = quest_id;
  dialog->required_money = money;
  snprintf(dialog->title, sizeof(dialog->title), "%s", title);
  snprintf(dialog->text, sizeof(dialog->text), "%s", text);

  read_items(&r, dialog->required, &dialog->required_count);
  read_u32(&r);
  dialog->completable = read_u32(&r) != 0;
}

//guid, quest id, title, text, auto finish, the emotes and their delays, the
//items to choose from, the items given, the money
void pe_wowdialog_handle_offer_reward(PWowDialog *dialog, const u8 *payload, int payload_len) {
  Reader r = reader_of(payload, payload_len);
  u64 npc = read_u64(&r);
  u32 quest_id = read_u32(&r);
  char title[PE_WOWDIALOG_LINE_MAX], text[PE_WOWDIALOG_TEXT_MAX];
  read_string(&r, title, sizeof(title));
  read_string(&r, text, sizeof(text));
  read_u32(&r);
  u32 emote_count = read_u32(&r);
  if (r.overrun || emote_count > 8)
    return;
  for (u32 i = 0; i < 2 * emote_count; i++)
    read_u32(&r);

  begin(dialog, PE_WOWDIALOG_QUEST_REWARD, npc);
  dialog->quest_id = quest_id;
  snprintf(dialog->title, sizeof(dialog->title), "%s", title);
  snprintf(dialog->text, sizeof(dialog->text), "%s", text);

  read_items(&r, dialog->choices, &dialog->choice_count);
  read_items(&r, dialog->rewards, &dialog->reward_count);
  dialog->reward_money = read_u32(&r);
}

void pe_wowdialog_handle_quest_complete(PWowDialog *dialog, const u8 *payload, int payload_len) {
  Reader r = reader_of(payload, payload_len);
  u32 quest = read_u32(&r);
  read_u32(&r);
  u32 xp = read_u32(&r);
  u32 money = read_u32(&r);
  if (r.overrun)
    return;

  dialog->completed_quest = quest;
  dialog->completed_xp = xp;
  dialog->completed_money = money;
  dialog->completed_serial++;
  pe_wowdialog_close(dialog);
}

void pe_wowdialog_close(PWowDialog *dialog) {
  if (dialog->kind == PE_WOWDIALOG_NONE)
    return;
  dialog->kind = PE_WOWDIALOG_NONE;
  dialog->serial++;
}

static PWowName *find_name(PWowNames *names, u32 entry) {
  for (int i = 0; i < names->count; i++)
    if (names->items[i].entry == entry)
      return &names->items[i];
  return NULL;
}

//the entry with its top bit set alone means the server has no such creature;
//otherwise the entry, the name and three empty ones, then the sub name
void pe_wowdialog_handle_creature_query(PWowNames *names, const u8 *payload, int payload_len) {
  Reader r = reader_of(payload, payload_len);
  u32 entry = read_u32(&r);
  if (r.overrun)
    return;

  PWowName *name = find_name(names, entry & 0x7FFFFFFFu);
  if (!name)
    return;

  if (entry & 0x80000000u) {
    name->state = PE_WOWNAME_MISSING;
    return;
  }

  char ignored[PE_WOWDIALOG_NAME_MAX];
  read_string(&r, name->name, sizeof(name->name));
  for (int i = 0; i < 3; i++)
    read_string(&r, ignored, sizeof(ignored));
  read_string(&r, name->subname, sizeof(name->subname));
  name->state = r.overrun ? PE_WOWNAME_MISSING : PE_WOWNAME_KNOWN;
}

const PWowName *pe_wowdialog_name(const PWowNames *names, u32 entry) {
  for (int i = 0; i < names->count; i++)
    if (names->items[i].entry == entry && names->items[i].state == PE_WOWNAME_KNOWN)
      return &names->items[i];
  return NULL;
}

bool pe_wowdialog_name_wanted(PWowNames *names, u32 entry) {
  if (find_name(names, entry) || names->count == PE_WOWDIALOG_NAMES_MAX)
    return false;

  PWowName *name = &names->items[names->count++];
  memset(name, 0, sizeof(*name));
  name->entry = entry;
  name->state = PE_WOWNAME_ASKED;
  return true;
}
