#include "questwindow.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bags.h"
#include "tooltip.h"

//QuestFrame.xml: the frame is 384 by 512 with its top 104 below the screen's, and
//the text scrolls in a 300 by 334 area 23 right and 81 down in it
#define WINDOW_LEFT 0.f
#define WINDOW_TOP 104.f
#define WINDOW_WIDTH 384.f
#define WINDOW_HEIGHT 512.f
#define HIT_INSET_RIGHT 30.f
#define HIT_INSET_BOTTOM 70.f
#define CONTENT_LEFT (WINDOW_LEFT + 23.f)
#define CONTENT_TOP (WINDOW_TOP + 81.f)
#define CONTENT_WIDTH 300.f
#define CONTENT_HEIGHT 334.f
#define TEXT_WIDTH 270.f
#define SCROLL_LINES_PER_NOTCH 3

#define BUTTON_HEIGHT 22.f
#define BUTTON_LEFT (WINDOW_LEFT + 23.f)
#define BUTTON_RIGHT_EDGE (WINDOW_LEFT + WINDOW_WIDTH - 39.f)
#define BUTTON_TOP (WINDOW_TOP + WINDOW_HEIGHT - 72.f - BUTTON_HEIGHT)
#define BUTTON_CAP 12.f
#define CLOSE_SIZE 32.f
#define NAME_CENTRE_X (WINDOW_LEFT + WINDOW_WIDTH * 0.5f)
#define NAME_TOP (WINDOW_TOP + 16.f)

#define TEXT_INDENT 12.f
#define TEXT_TOP_PADDING 10.f
#define PARAGRAPH_GAP 5.f
#define SECTION_GAP 15.f
#define ROW_PADDING 3.f
#define ROW_ICON_SIZE 16.f
#define ROW_TEXT_INDENT 22.f
#define ITEM_WIDTH 147.f
#define ITEM_HEIGHT 41.f
#define ITEM_ICON_SIZE 37.f
#define ITEM_NAME_FRAME_WIDTH 128.f
#define ITEM_NAME_FRAME_HEIGHT 64.f
#define ITEM_NAME_WIDTH 96.f
#define ITEM_COLUMNS 2
#define COIN_SIZE 13.f
#define COIN_GAP 4.f

//how far from the NPC the player may walk before the window shuts
#define WALK_AWAY_DISTANCE 12.f

#define LINES_MAX 128
#define LINE_MAX 160

#define GREETING_ART "interface/questframe/ui-questgreeting-"
#define BUTTON_ART "interface/buttons/ui-panel-button-"
#define CLOSE_ART "interface/buttons/ui-panel-minimizebutton-"
#define GOSSIP_ART "interface/gossipframe/"
#define QUESTION_MARK_ICON "interface/icons/inv_misc_questionmark.png"

static const float TITLE_COLOR[3] = {0.f, 0.f, 0.f};
static const float BODY_COLOR[3] = {0.1f, 0.05f, 0.0f};
static const float GOLD_COLOR[3] = {1.f, 0.82f, 0.f};
static const float GRAY_COLOR[3] = {0.5f, 0.5f, 0.5f};
static const float WHITE_COLOR[3] = {1.f, 1.f, 1.f};
static const float YELLOW_COLOR[3] = {1.f, 0.82f, 0.f};

static const char *const RACES[] = {"",    "Human", "Orc",    "Dwarf", "Night Elf",
                                    "Undead", "Tauren", "Gnome", "Troll"};
static const char *const CLASSES[] = {"",       "Warrior", "Paladin", "Hunter",
                                      "Rogue",  "Priest",  "",        "Shaman",
                                      "Mage",   "Warlock", "",        "Druid"};
#define COUNT_OF(array) ((int)(sizeof(array) / sizeof((array)[0])))

static struct {
  char player_name[32];
  unsigned race, character_class, gender;

  u32 serial;
  float scroll;
  float content_height;
  int choice; //the reward chosen, -1 for none
  u32 completed_serial;
} window = {.choice = -1};

void quest_window_set_player(const char *name, unsigned race, unsigned character_class,
                             unsigned gender) {
  snprintf(window.player_name, sizeof(window.player_name), "%s", name);
  window.race = race;
  window.character_class = character_class;
  window.gender = gender;
}

//---------------------------------------------------------------------------
//the text the quest writers wrote: $N the player's name, $R and $C race and class,
//$B a line break, $gmale:female; by the player's sex
//---------------------------------------------------------------------------

static void append(char *out, size_t size, size_t *length, const char *text, bool lower) {
  for (; *text && *length < size - 1; text++)
    out[(*length)++] = lower ? (char)tolower((unsigned char)*text) : *text;
  out[*length] = 0;
}

//after the $g, up to the semicolon: the first word for a man and the second for a woman
static const char *append_by_gender(char *out, size_t size, size_t *length, const char *text) {
  const char *colon = strchr(text, ':');
  const char *end = colon ? strchr(colon, ';') : NULL;
  if (!end)
    return text;

  const char *from = window.gender == 0 ? text : colon + 1;
  const char *to = window.gender == 0 ? colon : end;
  for (; from < to && *length < size - 1; from++)
    out[(*length)++] = *from;
  out[*length] = 0;
  return end + 1;
}

static void expand(const char *in, char *out, size_t size) {
  size_t length = 0;
  out[0] = 0;

  while (*in) {
    if (*in == '\r') {
      in++;
      continue;
    }
    if (*in != '$' || !in[1]) {
      char one[2] = {*in++, 0};
      append(out, size, &length, one, false);
      continue;
    }

    char token = in[1];
    in += 2;
    const char *race = window.race < COUNT_OF(RACES) ? RACES[window.race] : "";
    const char *class = window.character_class < COUNT_OF(CLASSES) ? CLASSES[window.character_class] : "";

    switch (token) {
    case 'N':
    case 'n':
      append(out, size, &length, window.player_name, false);
      break;
    case 'R':
      append(out, size, &length, race, false);
      break;
    case 'r':
      append(out, size, &length, race, true);
      break;
    case 'C':
      append(out, size, &length, class, false);
      break;
    case 'c':
      append(out, size, &length, class, true);
      break;
    case 'B':
    case 'b':
      append(out, size, &length, "\n", false);
      break;
    case 'G':
    case 'g':
      in = append_by_gender(out, size, &length, in);
      break;
    default:
      break;
    }
  }
}

typedef struct Lines {
  char text[LINES_MAX][LINE_MAX];
  int count;
} Lines;

static void add_line(Lines *lines, const char *text, size_t length) {
  if (lines->count == LINES_MAX)
    return;
  snprintf(lines->text[lines->count++], LINE_MAX, "%.*s", (int)length, text);
}

//breaks text into lines no wider than width, at spaces; a newline ends a line
static void wrap(const char *text, float width, Lines *lines) {
  lines->count = 0;
  char line[LINE_MAX] = "";

  while (*text) {
    if (*text == '\n') {
      add_line(lines, line, strlen(line));
      line[0] = 0;
      text++;
      continue;
    }

    const char *word_end = text;
    while (*word_end && *word_end != ' ' && *word_end != '\n')
      word_end++;

    char candidate[LINE_MAX];
    snprintf(candidate, sizeof(candidate), "%s%s%.*s", line, line[0] ? " " : "",
             (int)(word_end - text), text);
    if (line[0] && hud_text_width(candidate) > width) {
      add_line(lines, line, strlen(line));
      snprintf(line, sizeof(line), "%.*s", (int)(word_end - text), text);
    } else {
      snprintf(line, sizeof(line), "%s", candidate);
    }

    text = word_end;
    while (*text == ' ')
      text++;
  }
  if (line[0])
    add_line(lines, line, strlen(line));
}

//text cut with an ellipsis to fit
static void fit(const char *text, float width, char *out, size_t size) {
  snprintf(out, size, "%s", text);
  if (hud_text_width(out) <= width)
    return;

  for (size_t length = strlen(out); length > 0;) {
    length--;
    snprintf(out + length, size - length, "...");
    if (hud_text_width(out) <= width)
      return;
  }
}

//---------------------------------------------------------------------------
//the text area: things are laid down the page, and only what lies whole inside the
//part scrolled to is put on the canvas
//---------------------------------------------------------------------------

typedef struct Flow {
  float y;
} Flow;

static bool in_view(const Flow *flow, float height) {
  return flow->y >= window.scroll && flow->y + height <= window.scroll + CONTENT_HEIGHT;
}

static float page_x(float x) { return CONTENT_LEFT + x; }

static float page_y(const Flow *flow, float offset) {
  return CONTENT_TOP + flow->y + offset - window.scroll;
}

static void flow_paragraph(Flow *flow, const char *raw, const float color[3]) {
  char text[PE_WOWDIALOG_TEXT_MAX];
  expand(raw, text, sizeof(text));

  static Lines lines;
  wrap(text, TEXT_WIDTH, &lines);

  float height = hud_line_height();
  for (int i = 0; i < lines.count; i++) {
    if (in_view(flow, height))
      hud_canvas_text(lines.text[i], page_x(TEXT_INDENT), page_y(flow, 0.f), color, false);
    flow->y += height;
  }
}

static void flow_gap(Flow *flow, float height) { flow->y += height; }

static void flow_money(Flow *flow, u32 copper) {
  u32 amount[3] = {copper / 10000, copper / 100 % 100, copper % 100};
  static const float coin_coords[3][4] = {
      {0.f, 0.25f, 0.f, 1.f}, {0.25f, 0.5f, 0.f, 1.f}, {0.5f, 0.75f, 0.f, 1.f}};

  float height = hud_line_height() > COIN_SIZE ? hud_line_height() : COIN_SIZE;
  if (in_view(flow, height)) {
    float x = page_x(TEXT_INDENT);
    for (int coin = 0; coin < 3; coin++) {
      if (amount[coin] == 0)
        continue;

      char digits[16];
      snprintf(digits, sizeof(digits), "%u", amount[coin]);
      hud_canvas_text(digits, x, page_y(flow, (height - hud_line_height()) * 0.5f), WHITE_COLOR, true);
      x += hud_text_width(digits);
      hud_canvas_picture("interface/moneyframe/ui-moneyicons.png", x,
                         page_y(flow, (height - COIN_SIZE) * 0.5f), COIN_SIZE, COIN_SIZE,
                         coin_coords[coin], NULL, false);
      x += COIN_SIZE + COIN_GAP;
    }
  }
  flow->y += height;
}

static void flow_item(const Flow *flow, const PWowInventory *inventory, const PWowQuestItem *item,
                      int column, const char *region, bool selected) {
  float left = page_x(column * ITEM_WIDTH);
  float top = page_y(flow, 0.f);
  float icon_left = left + 2.f, icon_top = top + 2.f;

  hud_canvas_picture("interface/questframe/ui-questitemnameframe.png",
                     icon_left + ITEM_ICON_SIZE - 2.f, icon_top - 15.f, ITEM_NAME_FRAME_WIDTH,
                     ITEM_NAME_FRAME_HEIGHT, NULL, NULL, false);

  char icon[160];
  hud_canvas_picture(bags_item_icon(inventory, item->entry, icon, sizeof(icon)) ? icon
                                                                                 : QUESTION_MARK_ICON,
                     icon_left, icon_top, ITEM_ICON_SIZE, ITEM_ICON_SIZE, NULL, NULL, false);

  if (item->count > 1) {
    char count[16];
    snprintf(count, sizeof(count), "%u", item->count);
    hud_canvas_text(count, icon_left + ITEM_ICON_SIZE - hud_text_width(count) - 2.f,
                    icon_top + ITEM_ICON_SIZE - hud_line_height(), WHITE_COLOR, true);
  }

  const PWowItemTemplate *template = pe_wowinventory_template(inventory, item->entry);
  bool known = template && template->state == PE_WOWINV_TEMPLATE_KNOWN;
  char name[PE_WOWINV_NAME_MAX];
  fit(known ? template->name : "...", ITEM_NAME_WIDTH, name, sizeof(name));
  hud_canvas_text(name, icon_left + ITEM_ICON_SIZE + 8.f,
                  icon_top + (ITEM_ICON_SIZE - hud_line_height()) * 0.5f,
                  known ? tooltip_quality_color(template->quality) : GRAY_COLOR, true);

  if (selected)
    hud_canvas_picture("interface/questframe/ui-questitemhighlight.png", icon_left - 8.f,
                       icon_top - 8.f, 256.f, 64.f, NULL, NULL, true);
  hud_canvas_region(region, left, top, ITEM_WIDTH, ITEM_HEIGHT, false);
}

//two to a row. the region of item i is prefix and i, a button only for what is chosen from
static void flow_items(Flow *flow, const PWowInventory *inventory, const PWowQuestItem *items,
                       int count, const char *prefix, bool chosen_from) {
  for (int i = 0; i < count; i++) {
    int column = i % ITEM_COLUMNS;
    if (column == 0 && i > 0)
      flow->y += ITEM_HEIGHT;

    if (in_view(flow, ITEM_HEIGHT)) {
      char region[32];
      snprintf(region, sizeof(region), "%s%d", prefix, i);
      flow_item(flow, inventory, &items[i], column, region, chosen_from && window.choice == i);
    }
  }
  if (count > 0)
    flow->y += ITEM_HEIGHT;
}

//---------------------------------------------------------------------------
//rows you click: an option of the gossip, a quest of a list
//---------------------------------------------------------------------------

static const char *gossip_icon(unsigned icon) {
  switch (icon) {
  case 1:
    return GOSSIP_ART "vendorgossipicon.png";
  case 2:
    return GOSSIP_ART "taxigossipicon.png";
  case 3:
    return GOSSIP_ART "trainergossipicon.png";
  case 6:
    return GOSSIP_ART "bankergossipicon.png";
  case 8:
    return GOSSIP_ART "tabardgossipicon.png";
  case 9:
    return GOSSIP_ART "battlemastergossipicon.png";
  default:
    return GOSSIP_ART "gossipgossipicon.png";
  }
}

static void flow_row(Flow *flow, const char *region, const char *icon, const char *text) {
  float height = hud_line_height() > ROW_ICON_SIZE ? hud_line_height() : ROW_ICON_SIZE;
  height += 2.f * ROW_PADDING;

  if (in_view(flow, height)) {
    float left = page_x(0.f), top = page_y(flow, 0.f);
    hud_canvas_region(region, left, top, CONTENT_WIDTH - 10.f, height, true);
    if (hud_canvas_hovered(region))
      hud_canvas_picture("interface/questframe/ui-questtitlehighlight.png", left, top,
                         CONTENT_WIDTH - 10.f, height, NULL, NULL, true);

    hud_canvas_picture(icon, left + 2.f, top + (height - ROW_ICON_SIZE) * 0.5f, ROW_ICON_SIZE,
                       ROW_ICON_SIZE, NULL, NULL, false);

    char fitted[PE_WOWDIALOG_LINE_MAX];
    fit(text, CONTENT_WIDTH - ROW_TEXT_INDENT - 12.f, fitted, sizeof(fitted));
    hud_canvas_text(fitted, left + ROW_TEXT_INDENT, top + (height - hud_line_height()) * 0.5f,
                    TITLE_COLOR, false);
  }
  flow->y += height;
}

static bool quest_is_available(const PWowQuestEntry *quest) { return quest->icon == 2; }

static void flow_quest_rows(Flow *flow, const PWowDialog *dialog, bool available) {
  for (int i = 0; i < dialog->quest_count; i++) {
    const PWowQuestEntry *quest = &dialog->quests[i];
    if (quest_is_available(quest) != available)
      continue;

    char region[32];
    snprintf(region, sizeof(region), "QuestRow%d", i);
    flow_row(flow, region,
             available ? GOSSIP_ART "availablequesticon.png" : GOSSIP_ART "activequesticon.png",
             quest->title);
  }
}

static bool has_quests(const PWowDialog *dialog, bool available) {
  for (int i = 0; i < dialog->quest_count; i++)
    if (quest_is_available(&dialog->quests[i]) == available)
      return true;
  return false;
}

//---------------------------------------------------------------------------
//the panels
//---------------------------------------------------------------------------

static void layout_gossip(Flow *flow, const PWowDialog *dialog) {
  flow_paragraph(flow, dialog->greeting[0] ? dialog->greeting : "Greetings, $N.", BODY_COLOR);
  flow_gap(flow, SECTION_GAP);

  for (int i = 0; i < dialog->option_count; i++) {
    char region[32];
    snprintf(region, sizeof(region), "QuestOption%d", i);
    flow_row(flow, region, gossip_icon(dialog->options[i].icon), dialog->options[i].text);
  }

  if (dialog->quest_count > 0)
    flow_gap(flow, PARAGRAPH_GAP);
  flow_quest_rows(flow, dialog, false);
  flow_quest_rows(flow, dialog, true);
}

static void layout_quest_list(Flow *flow, const PWowDialog *dialog) {
  flow_paragraph(flow, dialog->greeting, BODY_COLOR);
  flow_gap(flow, SECTION_GAP);

  if (has_quests(dialog, false)) {
    flow_paragraph(flow, "Current Quests", TITLE_COLOR);
    flow_quest_rows(flow, dialog, false);
    flow_gap(flow, SECTION_GAP);
  }
  if (has_quests(dialog, true)) {
    flow_paragraph(flow, "Available Quests", TITLE_COLOR);
    flow_quest_rows(flow, dialog, true);
  }
}

static void flow_rewards(Flow *flow, const PWowObjectState *state, const PWowDialog *dialog) {
  bool any_reward = dialog->choice_count > 0 || dialog->reward_count > 0 || dialog->reward_money > 0;
  if (!any_reward)
    return;

  flow_gap(flow, SECTION_GAP);
  flow_paragraph(flow, "Rewards", TITLE_COLOR);
  flow_gap(flow, PARAGRAPH_GAP);

  if (dialog->choice_count > 0) {
    flow_paragraph(flow, "You will be able to choose one of these rewards:", BODY_COLOR);
    flow_gap(flow, PARAGRAPH_GAP);
    flow_items(flow, &state->inventory, dialog->choices, dialog->choice_count, "QuestChoice", true);
    flow_gap(flow, PARAGRAPH_GAP);
  }

  if (dialog->reward_count > 0 || dialog->reward_money > 0) {
    flow_paragraph(flow, dialog->choice_count > 0 ? "You will also receive:" : "You will receive:",
                   BODY_COLOR);
    flow_gap(flow, PARAGRAPH_GAP);
    flow_items(flow, &state->inventory, dialog->rewards, dialog->reward_count, "QuestReward", false);
    if (dialog->reward_money > 0)
      flow_money(flow, dialog->reward_money);
  }
}

static void layout_quest_details(Flow *flow, const PWowObjectState *state) {
  const PWowDialog *dialog = &state->dialog;

  flow_paragraph(flow, dialog->title, TITLE_COLOR);
  flow_gap(flow, PARAGRAPH_GAP);
  flow_paragraph(flow, dialog->text, BODY_COLOR);

  if (dialog->objectives[0]) {
    flow_gap(flow, SECTION_GAP);
    flow_paragraph(flow, "Quest Objectives", TITLE_COLOR);
    flow_gap(flow, PARAGRAPH_GAP);
    flow_paragraph(flow, dialog->objectives, BODY_COLOR);
  }
  flow_rewards(flow, state, dialog);
}

static void layout_quest_progress(Flow *flow, const PWowObjectState *state) {
  const PWowDialog *dialog = &state->dialog;

  flow_paragraph(flow, dialog->title, TITLE_COLOR);
  flow_gap(flow, PARAGRAPH_GAP);
  flow_paragraph(flow, dialog->text, BODY_COLOR);

  if (dialog->required_count > 0) {
    flow_gap(flow, SECTION_GAP);
    flow_paragraph(flow, "Required Items:", TITLE_COLOR);
    flow_gap(flow, PARAGRAPH_GAP);
    flow_items(flow, &state->inventory, dialog->required, dialog->required_count, "QuestRequired", false);
  }
  if (dialog->required_money > 0) {
    flow_gap(flow, SECTION_GAP);
    flow_paragraph(flow, "Required Money:", TITLE_COLOR);
    flow_money(flow, dialog->required_money);
  }
}

static void layout_quest_reward(Flow *flow, const PWowObjectState *state) {
  const PWowDialog *dialog = &state->dialog;

  flow_paragraph(flow, dialog->title, TITLE_COLOR);
  flow_gap(flow, PARAGRAPH_GAP);
  flow_paragraph(flow, dialog->text, BODY_COLOR);
  flow_rewards(flow, state, dialog);
}

//---------------------------------------------------------------------------
//the frame around: its art, the NPC's name, the buttons
//---------------------------------------------------------------------------

static void draw_frame() {
  const char *art = GREETING_ART;
  char path[96];

  struct {
    const char *piece;
    float left, top, width;
  } pieces[] = {{"topleft", 0.f, 0.f, 256.f},
                {"topright", 256.f, 0.f, 128.f},
                {"botleft", 0.f, 256.f, 256.f},
                {"botright", 256.f, 256.f, 128.f}};

  for (int i = 0; i < 4; i++) {
    snprintf(path, sizeof(path), "%s%s.png", art, pieces[i].piece);
    hud_canvas_picture(path, WINDOW_LEFT + pieces[i].left, WINDOW_TOP + pieces[i].top,
                       pieces[i].width, 256.f, NULL, NULL, false);
  }

  hud_canvas_region("QuestWindow", WINDOW_LEFT, WINDOW_TOP, WINDOW_WIDTH - HIT_INSET_RIGHT,
                    WINDOW_HEIGHT - HIT_INSET_BOTTOM, false);
}

static void draw_close_button() {
  float left = WINDOW_LEFT + WINDOW_WIDTH - 42.f - CLOSE_SIZE * 0.5f;
  float top = WINDOW_TOP + 31.f - CLOSE_SIZE * 0.5f;
  hud_canvas_region("QuestClose", left, top, CLOSE_SIZE, CLOSE_SIZE, true);
  hud_canvas_picture(hud_canvas_held("QuestClose") ? CLOSE_ART "down.png" : CLOSE_ART "up.png", left,
                     top, CLOSE_SIZE, CLOSE_SIZE, NULL, NULL, false);
  if (hud_canvas_hovered("QuestClose"))
    hud_canvas_picture(CLOSE_ART "highlight.png", left, top, CLOSE_SIZE, CLOSE_SIZE, NULL, NULL, true);
}

//UIPanelButtonTemplate: a texture in three pieces, the ends 12 wide and the middle stretched
static void draw_button(const char *name, const char *label, float left, float width, bool enabled) {
  static const float ends_height = 0.6875f;
  static const float left_cap[4] = {0.f, 0.09375f, 0.f, ends_height};
  static const float middle[4] = {0.09375f, 0.53125f, 0.f, ends_height};
  static const float right_cap[4] = {0.53125f, 0.625f, 0.f, ends_height};
  static const float whole[4] = {0.f, 0.625f, 0.f, ends_height};

  const char *texture = hud_canvas_held(name) ? BUTTON_ART "down.png" : BUTTON_ART "up.png";
  float top = BUTTON_TOP;

  hud_canvas_picture(texture, left, top, BUTTON_CAP, BUTTON_HEIGHT, left_cap, NULL, false);
  hud_canvas_picture(texture, left + BUTTON_CAP, top, width - 2.f * BUTTON_CAP, BUTTON_HEIGHT, middle,
                     NULL, false);
  hud_canvas_picture(texture, left + width - BUTTON_CAP, top, BUTTON_CAP, BUTTON_HEIGHT, right_cap,
                     NULL, false);
  if (enabled && hud_canvas_hovered(name))
    hud_canvas_picture(BUTTON_ART "highlight.png", left, top, width, BUTTON_HEIGHT, whole, NULL, true);

  hud_canvas_text(label, left + (width - hud_text_width(label)) * 0.5f,
                  top + (BUTTON_HEIGHT - hud_line_height()) * 0.5f, enabled ? GOLD_COLOR : GRAY_COLOR,
                  true);
  hud_canvas_region(name, left, top, width, BUTTON_HEIGHT, enabled);
}

static void draw_buttons(const PWowDialog *dialog) {
  const float left_width = 120.f, right_width = 78.f;
  float right_left = BUTTON_RIGHT_EDGE - right_width;

  switch (dialog->kind) {
  case PE_WOWDIALOG_QUEST_DETAILS:
    draw_button("QuestAccept", "Accept", BUTTON_LEFT, 77.f, true);
    draw_button("QuestDecline", "Decline", right_left, right_width, true);
    break;
  case PE_WOWDIALOG_QUEST_PROGRESS:
    draw_button("QuestContinue", "Continue", BUTTON_LEFT, left_width, dialog->completable);
    draw_button("QuestCancel", "Cancel", right_left, right_width, true);
    break;
  case PE_WOWDIALOG_QUEST_REWARD:
    draw_button("QuestComplete", "Complete Quest", BUTTON_LEFT, left_width,
                dialog->choice_count == 0 || window.choice >= 0);
    draw_button("QuestCancel", "Cancel", right_left, right_width, true);
    break;
  default:
    draw_button("QuestCancel", "Goodbye", right_left, right_width, true);
    break;
  }
}

static void draw_npc_name(const PWowObjectState *state, const PWowCreature *npc) {
  const PWowName *name = npc ? pe_wowdialog_name(&state->names, npc->entry) : NULL;
  if (!name)
    return;

  hud_canvas_text(name->name, NAME_CENTRE_X - hud_text_width(name->name) * 0.5f, NAME_TOP,
                  WHITE_COLOR, true);
}

//---------------------------------------------------------------------------

static float distance_to(const PWowCreature *npc, const vec3 position) {
  float dx = npc->x - position[0], dy = npc->y - position[1], dz = npc->z - position[2];
  return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void announce_completed_quest(PWowObjectState *state) {
  PWowDialog *dialog = &state->dialog;
  if (dialog->completed_serial == window.completed_serial)
    return;
  window.completed_serial = dialog->completed_serial;

  char text[160];
  snprintf(text, sizeof(text), "Quest complete: %s (%u experience)", dialog->title, dialog->completed_xp);
  hud_notice(text, YELLOW_COLOR);

  PWowCreature *npc = pe_wowobject_find_creature(state, dialog->npc);
  if (npc)
    npc->quest_status_asked = false;
}

static void follow_dialog(PWowObjectState *state) {
  if (window.serial == state->dialog.serial)
    return;

  window.serial = state->dialog.serial;
  window.scroll = 0.f;
  window.choice = -1;
}

static void clamp_scroll() {
  float most = window.content_height - CONTENT_HEIGHT;
  if (window.scroll > most)
    window.scroll = most > 0.f ? most : 0.f;
  if (window.scroll < 0.f)
    window.scroll = 0.f;
}

void quest_window_update(PWowObjectState *state, const vec3 player_position) {
  announce_completed_quest(state);
  follow_dialog(state);

  PWowDialog *dialog = &state->dialog;
  if (dialog->kind == PE_WOWDIALOG_NONE)
    return;

  PWowCreature *npc = pe_wowobject_find_creature(state, dialog->npc);
  if (!npc || distance_to(npc, player_position) > WALK_AWAY_DISTANCE) {
    pe_wowdialog_close(dialog);
    return;
  }

  clamp_scroll();

  draw_frame();

  Flow flow = {TEXT_TOP_PADDING};
  switch (dialog->kind) {
  case PE_WOWDIALOG_GOSSIP:
    layout_gossip(&flow, dialog);
    break;
  case PE_WOWDIALOG_QUEST_LIST:
    layout_quest_list(&flow, dialog);
    break;
  case PE_WOWDIALOG_QUEST_DETAILS:
    layout_quest_details(&flow, state);
    break;
  case PE_WOWDIALOG_QUEST_PROGRESS:
    layout_quest_progress(&flow, state);
    break;
  case PE_WOWDIALOG_QUEST_REWARD:
    layout_quest_reward(&flow, state);
    break;
  default:
    break;
  }
  window.content_height = flow.y;

  draw_npc_name(state, npc);
  draw_close_button();
  draw_buttons(dialog);
}

//---------------------------------------------------------------------------
//what is done
//---------------------------------------------------------------------------

bool quest_window_close(PWowObjectState *state) {
  if (state->dialog.kind == PE_WOWDIALOG_NONE)
    return false;
  pe_wowdialog_close(&state->dialog);
  return true;
}

static void ask_marker_again(PWowObjectState *state, u64 guid) {
  PWowCreature *npc = pe_wowobject_find_creature(state, guid);
  if (npc)
    npc->quest_status_asked = false;
}

static void accept_quest(PWowObjectState *state, PWowWorld *world) {
  PWowDialog *dialog = &state->dialog;
  pe_wowworld_quest_accept(world, dialog->npc, dialog->quest_id);

  char text[160];
  snprintf(text, sizeof(text), "Quest accepted: %s", dialog->title);
  hud_notice(text, YELLOW_COLOR);

  ask_marker_again(state, dialog->npc);
  pe_wowdialog_close(dialog);
}

//a quest of a list: one that can be taken is asked for, one under way is handed in
static void pick_quest(PWowObjectState *state, PWowWorld *world, int index) {
  const PWowDialog *dialog = &state->dialog;
  if (index < 0 || index >= dialog->quest_count)
    return;

  const PWowQuestEntry *quest = &dialog->quests[index];
  if (quest_is_available(quest))
    pe_wowworld_quest_query(world, dialog->npc, quest->id);
  else
    pe_wowworld_quest_complete(world, dialog->npc, quest->id);
}

static void pick_option(PWowObjectState *state, PWowWorld *world, int index) {
  const PWowDialog *dialog = &state->dialog;
  if (index < 0 || index >= dialog->option_count)
    return;

  if (dialog->options[index].coded) {
    hud_notice("This option needs a code, which can't be typed yet", GRAY_COLOR);
    return;
  }
  pe_wowworld_gossip_select(world, dialog->npc, dialog->options[index].index, "");
}

bool quest_window_click(HudClick click, PWowObjectState *state, PWowWorld *world) {
  if (!click.name || strncmp(click.name, "Quest", 5) != 0 || click.button != 1)
    return false;

  PWowDialog *dialog = &state->dialog;
  const char *name = click.name;
  int index;

  if (strcmp(name, "QuestAccept") == 0)
    accept_quest(state, world);
  else if (strcmp(name, "QuestContinue") == 0)
    pe_wowworld_quest_complete(world, dialog->npc, dialog->quest_id);
  else if (strcmp(name, "QuestComplete") == 0)
    pe_wowworld_quest_reward(world, dialog->npc, dialog->quest_id, window.choice < 0 ? 0 : (u32)window.choice);
  else if (strcmp(name, "QuestDecline") == 0 || strcmp(name, "QuestCancel") == 0 ||
           strcmp(name, "QuestClose") == 0)
    pe_wowdialog_close(dialog);
  else if (sscanf(name, "QuestRow%d", &index) == 1)
    pick_quest(state, world, index);
  else if (sscanf(name, "QuestOption%d", &index) == 1)
    pick_option(state, world, index);
  else if (sscanf(name, "QuestChoice%d", &index) == 1)
    window.choice = index;
  return true;
}

bool quest_window_scroll(int notches) {
  const char *hovered = hud_hovered_name();
  if (!hovered || strncmp(hovered, "Quest", 5) != 0)
    return false;

  window.scroll -= notches * SCROLL_LINES_PER_NOTCH * hud_line_height();
  clamp_scroll();
  return true;
}

unsigned quest_window_item_under(const char *name, const PWowObjectState *state) {
  const PWowDialog *dialog = &state->dialog;
  int index;

  if (sscanf(name, "QuestChoice%d", &index) == 1 && index >= 0 && index < dialog->choice_count)
    return dialog->choices[index].entry;
  if (sscanf(name, "QuestReward%d", &index) == 1 && index >= 0 && index < dialog->reward_count)
    return dialog->rewards[index].entry;
  if (sscanf(name, "QuestRequired%d", &index) == 1 && index >= 0 && index < dialog->required_count)
    return dialog->required[index].entry;
  return 0;
}
