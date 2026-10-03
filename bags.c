#include "bags.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <wowauth/wowdbc.h>

#include "ui_layout.h"

#define FRAMES 5
#define MAX_ITEMS PE_WOWINV_CONTAINER_SLOTS_MAX
#define COLUMNS 4
#define ROWS_IN_BG_TEXTURE 6
#define MAX_BG_TEXTURES 2
#define BG_TEXTURE_HEIGHT 512.f
#define CONTAINER_WIDTH 192.f
#define CONTAINER_SPACING 0.f
#define VISIBLE_CONTAINER_SPACING 3.f
#define CONTAINER_OFFSET_Y 70.f
#define SCREEN_HEIGHT 768.f
#define ROW_HEIGHT 41.f
#define BACKPACK_HEIGHT 240.f
#define ICON_FIELD 5
#define BACKPACK_ICON "interface/buttons/button-backpack-up.png"
#define EMPTY_BAG_ICON "interface/paperdoll/ui-paperdoll-slot-bag.png"
#define BACKPACK_BACKGROUND "interface/containerframe/ui-backpackbackground.png"
#define BAG_COMPONENTS "interface/containerframe/ui-bag-components.png"
#define BACKPACK_SLOT_BAG_INDEX 255
#define BACKPACK_FIRST_SLOT 23

typedef struct BagFrame {
  int frame, top, middle[MAX_BG_TEXTURES], bottom, name, portrait, money;
  int close_button;
  int coin[3];      //the gold, silver and copper buttons of the money frame
  int coin_text[3];
  int item[MAX_ITEMS], icon[MAX_ITEMS], count[MAX_ITEMS];

  bool open;
  int bag;
  int size;
  float height;
} BagFrame;

static BagFrame frames[FRAMES];
static int order[FRAMES];
static int open_count;
static int bar_icon[PE_WOWINV_BAGS + 1];
static PWowDBC item_display;
static bool ready;
static u32 shown_serial = (u32)-1;

//the item the pointer holds: where it lies, which stays put on the server
//until it is dropped
static struct {
  bool active;
  int bag, slot;
  u32 entry;
} cursor;

static int node_named(const char *format, int number, const char *suffix) {
  char name[96];
  snprintf(name, sizeof(name), format, number, suffix);
  return hud_node(name);
}

static int item_node(const char *format, int frame, int item, const char *suffix) {
  char name[96];
  snprintf(name, sizeof(name), format, frame, item, suffix);
  return hud_node(name);
}

bool bags_init(const char *dbc_directory) {
  char path[256];
  snprintf(path, sizeof(path), "%s/ItemDisplayInfo.dbc", dbc_directory);
  if (!pe_wowdbc_load(path, &item_display))
    return false;

  for (int k = 0; k < FRAMES; k++) {
    BagFrame *f = &frames[k];
    int n = k + 1;
    f->frame = node_named("ContainerFrame%d%s", n, "");
    f->top = node_named("ContainerFrame%d%s", n, "BackgroundTop");
    f->middle[0] = node_named("ContainerFrame%d%s", n, "BackgroundMiddle1");
    f->middle[1] = node_named("ContainerFrame%d%s", n, "BackgroundMiddle2");
    f->bottom = node_named("ContainerFrame%d%s", n, "BackgroundBottom");
    f->name = node_named("ContainerFrame%d%s", n, "Name");
    f->portrait = node_named("ContainerFrame%d%s", n, "Portrait");
    f->money = node_named("ContainerFrame%d%s", n, "MoneyFrame");
    f->close_button = node_named("ContainerFrame%d%s", n, "CloseButton");
    static const char *const coins[3] = {"Gold", "Silver", "Copper"};
    for (int c = 0; c < 3; c++) {
      char name[96];
      snprintf(name, sizeof(name), "ContainerFrame%dMoneyFrame%sButton", n, coins[c]);
      f->coin[c] = hud_node(name);
      snprintf(name, sizeof(name), "ContainerFrame%dMoneyFrame%sButtonText", n, coins[c]);
      f->coin_text[c] = hud_node(name);
    }

    for (int j = 0; j < MAX_ITEMS; j++) {
      f->item[j] = item_node("ContainerFrame%dItem%d%s", n, j + 1, "");
      f->icon[j] = item_node("ContainerFrame%dItem%d%s", n, j + 1, "IconTexture");
      f->count[j] = item_node("ContainerFrame%dItem%d%s", n, j + 1, "Count");
    }
    if (f->frame < 0 || f->item[MAX_ITEMS - 1] < 0)
      return false;
  }

  for (int bag = 0; bag <= PE_WOWINV_BAGS; bag++) {
    char name[64];
    if (bag == 0)
      snprintf(name, sizeof(name), "MainMenuBarBackpackButtonIconTexture");
    else
      snprintf(name, sizeof(name), "CharacterBag%dSlotIconTexture", bag - 1);
    bar_icon[bag] = hud_node(name);
  }

  hud_set_node_texture(bar_icon[0], BACKPACK_ICON);
  ready = true;
  return true;
}

static int find_record(const PWowDBC *dbc, u32 id) {
  for (u32 i = 0; i < dbc->record_count; i++)
    if (pe_wowdbc_get_u32(dbc, i, 0) == id)
      return (int)i;
  return -1;
}

//ItemDisplayInfo's InventoryIcon, INV_Misc_Food_09 -> interface/icons/inv_misc_food_09.png
bool bags_item_icon(const PWowInventory *inv, u32 entry, char *out, size_t size) {
  const PWowItemTemplate *template = pe_wowinventory_template(inv, entry);
  if (!template || template->state != PE_WOWINV_TEMPLATE_KNOWN)
    return false;

  int record = find_record(&item_display, template->display_info_id);
  if (record < 0)
    return false;

  const char *name = pe_wowdbc_get_string(&item_display, record, ICON_FIELD);
  if (!name[0])
    return false;

  int length = snprintf(out, size, "interface/icons/");
  for (; *name && length < (int)size - 5; name++)
    out[length++] = (char)tolower((unsigned char)*name);
  snprintf(out + length, size - length, ".png");
  return true;
}

static void show_icon(const PWowInventory *inv, int node, u32 entry, const char *empty) {
  char icon[160];
  if (entry && bags_item_icon(inv, entry, icon, sizeof(icon)))
    hud_set_node_texture(node, icon);
  else
    hud_set_node_texture(node, empty);
}

#define COPPER_PER_SILVER 100
#define SILVER_PER_GOLD 100
#define MONEY_ICON_WIDTH_SMALL 13.f
#define MONEY_BUTTON_SPACING_SMALL -4.f

//MoneyFrame.lua's MoneyFrame_Update for the small frame of the backpack, which
//collapses its coins and always shows the copper: only the coins that are worth
//showing, each as wide as its digits and a coin, chained from the right
static void update_money(BagFrame *f, u32 money) {
  enum { GOLD, SILVER, COPPER };
  u32 gold = money / (COPPER_PER_SILVER * SILVER_PER_GOLD);
  u32 silver = money / COPPER_PER_SILVER % SILVER_PER_GOLD;
  u32 copper = money % COPPER_PER_SILVER;
  u32 amount[3] = {gold, silver, copper};
  float coin_width[3];

  for (int c = 0; c < 3; c++) {
    char text[16];
    snprintf(text, sizeof(text), "%u", amount[c]);
    hud_set_node_text(f->coin_text[c], text);
    coin_width[c] = hud_text_width(text) + MONEY_ICON_WIDTH_SMALL;
    hud_set_size(f->coin[c], coin_width[c], 13.f);
  }

  bool show[3] = {gold > 0, gold > 0 || silver > 0, true};
  float width = MONEY_ICON_WIDTH_SMALL;
  for (int c = 0; c < 3; c++) {
    hud_show_node(f->coin[c], show[c]);
    if (show[c])
      width += coin_width[c];
  }
  if (show[GOLD] && show[SILVER])
    width -= MONEY_BUTTON_SPACING_SMALL;
  if (show[SILVER])
    width -= MONEY_BUTTON_SPACING_SMALL;

  hud_set_anchor(f->coin[GOLD], UI_RIGHT, f->coin[SILVER],
                 show[SILVER] ? UI_LEFT : UI_RIGHT,
                 show[SILVER] ? MONEY_BUTTON_SPACING_SMALL : 0.f, 0.f);
  hud_set_anchor(f->coin[SILVER], UI_RIGHT, f->coin[COPPER], show[COPPER] ? UI_LEFT : UI_RIGHT,
                 show[COPPER] ? MONEY_BUTTON_SPACING_SMALL : 0.f, 0.f);
  hud_set_size(f->money, width, 13.f);
}

static void fill_items(const PWowInventory *inv, BagFrame *f) {
  for (int j = 0; j < f->size; j++) {
    int slot = f->size - j;
    PWowSlotView view = pe_wowinventory_slot(inv, f->bag, slot);

    bool carried = cursor.active && cursor.bag == f->bag && cursor.slot == slot;
    show_icon(inv, f->icon[j], view.filled && !carried ? view.entry : 0, NULL);

    char count[16];
    snprintf(count, sizeof(count), "%u", view.count);
    hud_set_node_text(f->count[j], count);
    hud_show_node(f->count[j], view.filled && view.count > 1 && !carried);
  }
}

static void show_bag_portrait(const PWowInventory *inv, BagFrame *f) {
  if (f->bag == 0) {
    hud_set_node_text(f->name, "Backpack");
    hud_set_node_texture(f->portrait, BACKPACK_ICON);
    return;
  }

  u32 entry = pe_wowinventory_bag_entry(inv, f->bag);
  const PWowItemTemplate *template = pe_wowinventory_template(inv, entry);
  hud_set_node_text(f->name, template ? template->name : "");
  show_icon(inv, f->portrait, entry, NULL);
}

static void set_background(BagFrame *f, int size, int bag) {
  int top = f->top, bottom = f->bottom;
  int rows = (size + COLUMNS - 1) / COLUMNS;

  if (bag == 0) {
    hud_set_node_texture(top, BACKPACK_BACKGROUND);
    hud_set_size(top, 256.f, 256.f);
    hud_set_tex_coords(top, 0.f, 1.f, 0.f, 1.f);
    for (int i = 0; i < MAX_BG_TEXTURES; i++)
      hud_show_node(f->middle[i], false);
    hud_show_node(bottom, false);
    f->height = BACKPACK_HEIGHT;
    return;
  }

  hud_set_node_texture(top, BAG_COMPONENTS);
  for (int i = 0; i < MAX_BG_TEXTURES; i++) {
    hud_set_node_texture(f->middle[i], BAG_COMPONENTS);
    hud_show_node(f->middle[i], false);
  }
  hud_set_node_texture(bottom, BAG_COMPONENTS);
  hud_show_node(bottom, true);

  float top_height;
  if (size % COLUMNS == 2) {
    hud_set_tex_coords(top, 0.f, 1.f, 0.189453125f, 0.330078125f);
    top_height = 72.f;
  } else if (rows == 1) {
    hud_set_tex_coords(top, 0.f, 1.f, 0.00390625f, 0.16796875f);
    top_height = 86.f;
  } else {
    hud_set_tex_coords(top, 0.f, 1.f, 0.00390625f, 0.18359375f);
    top_height = 94.f;
  }
  hud_set_size(top, 256.f, top_height);

  float middle_height = 0.f;
  int last_middle = f->middle[0];
  if (rows == 1) {
    hud_set_anchor(bottom, UI_TOP, f->middle[0], UI_TOP, 0.f, 0.f);
  } else {
    int remaining = rows - 1;
    int texture_count = (remaining + ROWS_IN_BG_TEXTURE - 1) / ROWS_IN_BG_TEXTURE;
    const float first_row_texture_offset = 0.353515625f;

    for (int i = 0; i < texture_count && i < MAX_BG_TEXTURES; i++) {
      int middle = f->middle[i];
      float height;
      if (remaining > ROWS_IN_BG_TEXTURE) {
        height = ROWS_IN_BG_TEXTURE * ROW_HEIGHT + first_row_texture_offset;
        remaining -= ROWS_IN_BG_TEXTURE;
      } else {
        height = remaining * ROW_HEIGHT - 9.f;
      }
      hud_set_size(middle, 256.f, height);
      hud_set_tex_coords(middle, 0.f, 1.f, first_row_texture_offset,
                         height / BG_TEXTURE_HEIGHT + first_row_texture_offset);
      hud_show_node(middle, true);
      middle_height += height;
      last_middle = middle;
    }
    hud_set_anchor(bottom, UI_TOP, last_middle, UI_BOTTOM, 0.f, 0.f);
  }

  f->height = top_height + 10.f + middle_height;
}

static void place_items(BagFrame *f, int size, int bag) {
  for (int j = 0; j < MAX_ITEMS; j++) {
    hud_show_node(f->item[j], j < size);
    if (j >= size)
      continue;

    if (j == 0)
      hud_set_anchor(f->item[j], UI_BOTTOMRIGHT, f->frame, UI_BOTTOMRIGHT, -12.f,
                     bag == 0 ? 30.f : 9.f);
    else if (j % COLUMNS == 0)
      hud_set_anchor(f->item[j], UI_BOTTOMRIGHT, f->item[j - COLUMNS], UI_TOPRIGHT, 0.f, 4.f);
    else
      hud_set_anchor(f->item[j], UI_BOTTOMRIGHT, f->item[j - 1], UI_BOTTOMLEFT, -5.f, 0.f);
  }
}

//ContainerFrame.lua's updateContainerFrameAnchors, without the bank's shrinking:
//the first bag at the bottom right, the next on top of it, a new column to the
//left when the screen is full
static void anchor_open_frames() {
  float free_height = SCREEN_HEIGHT - CONTAINER_OFFSET_Y;
  int column = 0;

  for (int i = 0; i < open_count; i++) {
    BagFrame *f = &frames[order[i]];

    if (i == 0) {
      hud_set_anchor(f->frame, UI_BOTTOMRIGHT, UI_SCREEN, UI_BOTTOMRIGHT, 0.f, CONTAINER_OFFSET_Y);
    } else if (free_height < f->height) {
      column++;
      free_height = SCREEN_HEIGHT - CONTAINER_OFFSET_Y;
      hud_set_anchor(f->frame, UI_BOTTOMRIGHT, UI_SCREEN, UI_BOTTOMRIGHT,
                     -(column * CONTAINER_WIDTH), CONTAINER_OFFSET_Y);
    } else {
      hud_set_anchor(f->frame, UI_BOTTOMRIGHT, frames[order[i - 1]].frame, UI_TOPRIGHT, 0.f,
                     CONTAINER_SPACING);
    }
    free_height -= f->height + VISIBLE_CONTAINER_SPACING;
  }
}

static void generate_frame(const PWowInventory *inv, BagFrame *f, int size, int bag) {
  f->size = size;
  f->bag = bag;
  set_background(f, size, bag);
  place_items(f, size, bag);
  hud_set_size(f->frame, CONTAINER_WIDTH, f->height);
  hud_show_node(f->money, bag == 0);
  if (bag == 0)
    update_money(f, inv->money);
  show_bag_portrait(inv, f);
  fill_items(inv, f);
}

static void put_down(const PWowInventory *inv);

static BagFrame *open_frame_of(int bag) {
  for (int k = 0; k < FRAMES; k++)
    if (frames[k].open && frames[k].bag == bag)
      return &frames[k];
  return NULL;
}

static void close_frame(BagFrame *f) {
  int index = (int)(f - frames);
  f->open = false;
  hud_show_node(f->frame, false);

  int kept = 0;
  for (int i = 0; i < open_count; i++)
    if (order[i] != index)
      order[kept++] = order[i];
  open_count = kept;
  anchor_open_frames();
}

static void open_bag(const PWowInventory *inv, int bag) {
  int size = pe_wowinventory_bag_size(inv, bag);
  if (size <= 0 || open_frame_of(bag))
    return;

  for (int k = 0; k < FRAMES; k++) {
    if (frames[k].open)
      continue;

    frames[k].open = true;
    order[open_count++] = k;
    generate_frame(inv, &frames[k], size, bag);
    hud_show_node(frames[k].frame, true);
    anchor_open_frames();
    return;
  }
}

static void toggle_bag(const PWowInventory *inv, int bag) {
  BagFrame *open = open_frame_of(bag);
  if (open)
    close_frame(open);
  else
    open_bag(inv, bag);
}

//ToggleBackpack: the backpack open shuts every bag
static void toggle_backpack(const PWowInventory *inv) {
  if (!open_frame_of(0)) {
    open_bag(inv, 0);
    return;
  }
  while (open_count > 0)
    close_frame(&frames[order[0]]);
}

void bags_toggle_all(const PWowInventory *inv) {
  if (open_count > 0) {
    while (open_count > 0)
      close_frame(&frames[order[0]]);
    return;
  }

  open_bag(inv, 0);
  for (int bag = 1; bag <= PE_WOWINV_BAGS; bag++)
    open_bag(inv, bag);
}

void bags_update(const PWowInventory *inv) {
  if (!ready || inv->serial == shown_serial)
    return;
  shown_serial = inv->serial;

  if (cursor.active && pe_wowinventory_slot(inv, cursor.bag, cursor.slot).entry != cursor.entry)
    put_down(inv);

  for (int bag = 1; bag <= PE_WOWINV_BAGS; bag++)
    show_icon(inv, bar_icon[bag], pe_wowinventory_bag_entry(inv, bag), EMPTY_BAG_ICON);

  for (int k = 0; k < FRAMES; k++) {
    BagFrame *f = &frames[k];
    if (!f->open)
      continue;

    int size = pe_wowinventory_bag_size(inv, f->bag);
    if (size <= 0) {
      close_frame(f);
    } else if (size != f->size) {
      generate_frame(inv, f, size, f->bag);
      anchor_open_frames();
    } else {
      show_bag_portrait(inv, f);
      fill_items(inv, f);
      if (f->bag == 0)
        update_money(f, inv->money);
    }
  }
}

//where the server keeps the item at slot 1 to size of a bag
static void server_place(int bag, int slot, u8 *bag_index, u8 *slot_index) {
  if (bag == 0) {
    *bag_index = BACKPACK_SLOT_BAG_INDEX;
    *slot_index = (u8)(BACKPACK_FIRST_SLOT + slot - 1);
  } else {
    *bag_index = (u8)(PE_WOWINV_FIRST_BAG_SLOT + bag - 1);
    *slot_index = (u8)(slot - 1);
  }
}

static void refresh_open_frames(const PWowInventory *inv) {
  for (int k = 0; k < FRAMES; k++)
    if (frames[k].open)
      fill_items(inv, &frames[k]);
}

static void put_down(const PWowInventory *inv) {
  cursor.active = false;
  hud_set_cursor(NULL);
  refresh_open_frames(inv);
}

void bags_cancel_cursor(const PWowInventory *inv) {
  if (cursor.active)
    put_down(inv);
}

static void pick_up(const PWowInventory *inv, int bag, int slot) {
  PWowSlotView view = pe_wowinventory_slot(inv, bag, slot);
  char icon[160];
  if (!view.filled || !bags_item_icon(inv, view.entry, icon, sizeof(icon)))
    return;

  cursor.active = true;
  cursor.bag = bag;
  cursor.slot = slot;
  cursor.entry = view.entry;
  hud_set_cursor(icon);
  refresh_open_frames(inv);
}

static void drop_on(const PWowInventory *inv, int bag, int slot, PWowWorld *world) {
  bool same_place = cursor.bag == bag && cursor.slot == slot;
  if (!same_place) {
    u8 dst_bag, dst_slot, src_bag, src_slot;
    server_place(bag, slot, &dst_bag, &dst_slot);
    server_place(cursor.bag, cursor.slot, &src_bag, &src_slot);
    pe_wowworld_swap_item(world, dst_bag, dst_slot, src_bag, src_slot);
  }
  put_down(inv);
}

static void use_item(const PWowInventory *inv, int bag, int slot, PWowWorld *world) {
  PWowSlotView view = pe_wowinventory_slot(inv, bag, slot);
  if (!view.filled)
    return;

  const PWowItemTemplate *template = pe_wowinventory_template(inv, view.entry);
  bool equippable = template && template->inventory_type != 0;

  u8 bag_index, slot_index;
  server_place(bag, slot, &bag_index, &slot_index);
  if (equippable)
    pe_wowworld_autoequip_item(world, bag_index, slot_index);
  else
    pe_wowworld_use_item(world, bag_index, slot_index);
}

bool bags_click(HudClick click, const PWowInventory *inv, PWowWorld *world) {
  int frame, item, bag;
  if (!click.name || !ready)
    return false;

  if (sscanf(click.name, "ContainerFrame%dItem%d", &frame, &item) == 2 && frame >= 1 &&
      frame <= FRAMES && item >= 1 && item <= MAX_ITEMS) {
    BagFrame *f = &frames[frame - 1];
    int slot = f->size - item + 1;
    if (!f->open)
      return true;

    if (click.button == 2 && cursor.active)
      put_down(inv);
    else if (click.button == 2)
      use_item(inv, f->bag, slot, world);
    else if (cursor.active)
      drop_on(inv, f->bag, slot, world);
    else
      pick_up(inv, f->bag, slot);
    return true;
  }

  if (sscanf(click.name, "ContainerFrame%dCloseButton", &frame) == 1 && frame >= 1 &&
      frame <= FRAMES) {
    if (frames[frame - 1].open)
      close_frame(&frames[frame - 1]);
    return true;
  }

  if (strcmp(click.name, "MainMenuBarBackpackButton") == 0) {
    toggle_backpack(inv);
    return true;
  }

  if (sscanf(click.name, "CharacterBag%dSlot", &bag) == 1 && bag >= 0 && bag < PE_WOWINV_BAGS) {
    toggle_bag(inv, bag + 1);
    return true;
  }
  return false;
}

u32 bags_item_under(const char *name, const PWowInventory *inv, bool *is_bag_button) {
  int frame, item, bag;
  *is_bag_button = false;
  if (!ready || !name)
    return 0;

  if (sscanf(name, "ContainerFrame%dItem%d", &frame, &item) == 2 && frame >= 1 &&
      frame <= FRAMES && item >= 1 && item <= MAX_ITEMS) {
    const BagFrame *f = &frames[frame - 1];
    if (!f->open || item > f->size || (cursor.active && cursor.bag == f->bag &&
                                       cursor.slot == f->size - item + 1))
      return 0;
    return pe_wowinventory_slot(inv, f->bag, f->size - item + 1).entry;
  }

  if (sscanf(name, "CharacterBag%dSlot", &bag) == 1 && bag >= 0 && bag < PE_WOWINV_BAGS) {
    *is_bag_button = true;
    return pe_wowinventory_bag_entry(inv, bag + 1);
  }
  return 0;
}
