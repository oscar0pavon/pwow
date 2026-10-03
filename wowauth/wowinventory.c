#include "wowinventory.h"

#include <string.h>

#define FIELD_OBJECT_ENTRY 3
#define FIELD_ITEM_STACK_COUNT 14
#define FIELD_CONTAINER_NUM_SLOTS 48
#define FIELD_CONTAINER_SLOT_1 50
#define PLAYER_PACK_FIELD_OFFSET 46 //PACK_SLOT_1 counted from INV_SLOT_HEAD
#define BACKPACK 0

static u64 raw_guid(const u32 *raw, int field) {
  return raw[field] | ((u64)raw[field + 1] << 32);
}

static PWowItem *find_item(PWowInventory *inv, u64 guid) {
  for (int i = 0; i < PE_WOWINV_ITEMS_MAX; i++)
    if (inv->items[i].used && inv->items[i].guid == guid)
      return &inv->items[i];
  return NULL;
}

bool pe_wowinventory_has_item(const PWowInventory *inv, u64 guid) {
  return find_item((PWowInventory *)inv, guid) != NULL;
}

static PWowItem *find_or_add_item(PWowInventory *inv, u64 guid) {
  PWowItem *item = find_item(inv, guid);
  if (item)
    return item;

  for (int i = 0; i < PE_WOWINV_ITEMS_MAX; i++)
    if (!inv->items[i].used) {
      memset(&inv->items[i], 0, sizeof(PWowItem));
      inv->items[i].used = true;
      inv->items[i].guid = guid;
      return &inv->items[i];
    }
  return NULL;
}

void pe_wowinventory_apply_item(PWowInventory *inv, u64 guid, const u32 *raw,
                                const bool *present, int field_count) {
  PWowItem *item = find_or_add_item(inv, guid);
  if (!item)
    return;

  if (present[FIELD_OBJECT_ENTRY])
    item->entry = raw[FIELD_OBJECT_ENTRY];
  if (present[FIELD_ITEM_STACK_COUNT])
    item->count = raw[FIELD_ITEM_STACK_COUNT];
  if (present[FIELD_CONTAINER_NUM_SLOTS])
    item->num_slots = raw[FIELD_CONTAINER_NUM_SLOTS];

  for (int slot = 0; slot < PE_WOWINV_CONTAINER_SLOTS_MAX; slot++) {
    int field = FIELD_CONTAINER_SLOT_1 + 2 * slot;
    if (field + 1 < field_count && present[field] && present[field + 1])
      item->slots[slot] = raw_guid(raw, field);
  }
  inv->serial++;
}

void pe_wowinventory_remove_item(PWowInventory *inv, u64 guid) {
  PWowItem *item = find_item(inv, guid);
  if (item) {
    item->used = false;
    inv->serial++;
  }
}

void pe_wowinventory_apply_player(PWowInventory *inv, const u32 *raw,
                                  const bool *present) {
  for (int slot = 0; slot < PE_WOWINV_EQUIP_SLOTS; slot++)
    if (present[2 * slot] && present[2 * slot + 1])
      inv->equipped[slot] = raw_guid(raw, 2 * slot);

  for (int slot = 0; slot < PE_WOWINV_PACK_SLOTS; slot++) {
    int field = PLAYER_PACK_FIELD_OFFSET + 2 * slot;
    if (present[field] && present[field + 1])
      inv->pack[slot] = raw_guid(raw, field);
  }
  inv->serial++;
}

static const PWowItem *bag_item(const PWowInventory *inv, int bag) {
  if (bag < 1 || bag > PE_WOWINV_BAGS)
    return NULL;
  u64 guid = inv->equipped[PE_WOWINV_FIRST_BAG_SLOT + bag - 1];
  return guid ? find_item((PWowInventory *)inv, guid) : NULL;
}

int pe_wowinventory_bag_size(const PWowInventory *inv, int bag) {
  if (bag == BACKPACK)
    return PE_WOWINV_PACK_SLOTS;

  const PWowItem *item = bag_item(inv, bag);
  return item ? (int)item->num_slots : 0;
}

u32 pe_wowinventory_bag_entry(const PWowInventory *inv, int bag) {
  const PWowItem *item = bag_item(inv, bag);
  return item ? item->entry : 0;
}

const PWowItemTemplate *pe_wowinventory_template(const PWowInventory *inv, u32 entry) {
  for (int i = 0; i < inv->template_count; i++)
    if (inv->templates[i].entry == entry)
      return &inv->templates[i];
  return NULL;
}

PWowSlotView pe_wowinventory_slot(const PWowInventory *inv, int bag, int slot) {
  PWowSlotView view = {0};
  if (slot < 1 || slot > pe_wowinventory_bag_size(inv, bag))
    return view;

  u64 guid = 0;
  if (bag == BACKPACK) {
    guid = inv->pack[slot - 1];
  } else {
    const PWowItem *container = bag_item(inv, bag);
    if (slot <= PE_WOWINV_CONTAINER_SLOTS_MAX)
      guid = container->slots[slot - 1];
  }

  const PWowItem *item = guid ? find_item((PWowInventory *)inv, guid) : NULL;
  if (!item)
    return view;

  const PWowItemTemplate *template = pe_wowinventory_template(inv, item->entry);
  view.filled = true;
  view.entry = item->entry;
  view.count = item->count ? item->count : 1;
  if (template && template->state == PE_WOWINV_TEMPLATE_KNOWN) {
    view.display_info_id = template->display_info_id;
    view.quality = template->quality;
  }
  return view;
}

static PWowItemTemplate *find_or_add_template(PWowInventory *inv, u32 entry) {
  for (int i = 0; i < inv->template_count; i++)
    if (inv->templates[i].entry == entry)
      return &inv->templates[i];

  if (inv->template_count == PE_WOWINV_TEMPLATES_MAX)
    return NULL;

  PWowItemTemplate *template = &inv->templates[inv->template_count++];
  memset(template, 0, sizeof(*template));
  template->entry = entry;
  return template;
}

typedef struct Reader {
  const u8 *data;
  int length;
  int position;
  bool overrun;
} Reader;

static u32 read_u32(Reader *r) {
  if (r->position + 4 > r->length) {
    r->overrun = true;
    return 0;
  }
  u32 value;
  memcpy(&value, r->data + r->position, 4);
  r->position += 4;
  return value;
}

static void read_string(Reader *r, char *out, int size) {
  int length = 0;
  while (r->position < r->length && r->data[r->position]) {
    if (length < size - 1)
      out[length++] = (char)r->data[r->position];
    r->position++;
  }
  r->position++;
  out[length] = 0;
}

void pe_wowinventory_handle_item_query(PWowInventory *inv, const u8 *payload,
                                       int payload_len) {
  Reader r = {payload, payload_len, 0, false};
  u32 entry = read_u32(&r);

  //an entry the server does not know is answered with just entry | 0x80000000
  bool missing = (entry & 0x80000000u) != 0;
  PWowItemTemplate *template = find_or_add_template(inv, entry & 0x7FFFFFFFu);
  if (!template)
    return;

  if (missing || payload_len <= 4) {
    template->state = PE_WOWINV_TEMPLATE_MISSING;
    inv->serial++;
    return;
  }

  char ignored[PE_WOWINV_NAME_MAX];
  read_u32(&r); //class
  read_u32(&r); //subclass
  read_string(&r, template->name, PE_WOWINV_NAME_MAX);
  for (int i = 0; i < 3; i++)
    read_string(&r, ignored, sizeof(ignored));
  template->display_info_id = read_u32(&r);
  template->quality = read_u32(&r);
  read_u32(&r); //flags
  read_u32(&r); //buy price
  read_u32(&r); //sell price
  template->inventory_type = read_u32(&r);

  //allowable class and race, item level, required level, skill, skill rank,
  //spell, honor rank, city rank, reputation faction and rank, max count
  read_u32(&r); //allowable class
  read_u32(&r); //allowable race
  template->item_level = read_u32(&r);
  template->required_level = read_u32(&r);
  for (int i = 0; i < 8; i++)
    read_u32(&r);
  template->stackable = read_u32(&r);
  template->container_slots = read_u32(&r);

  template->state = r.overrun ? PE_WOWINV_TEMPLATE_MISSING : PE_WOWINV_TEMPLATE_KNOWN;
  inv->serial++;
}

void pe_wowinventory_handle_destroy(PWowInventory *inv, const u8 *payload,
                                    int payload_len) {
  if (payload_len < 8)
    return;

  u64 guid;
  memcpy(&guid, payload, 8);
  pe_wowinventory_remove_item(inv, guid);
}

static bool owned(const PWowInventory *inv, u64 guid) {
  for (int i = 0; i < PE_WOWINV_EQUIP_SLOTS; i++)
    if (inv->equipped[i] == guid)
      return true;
  for (int i = 0; i < PE_WOWINV_PACK_SLOTS; i++)
    if (inv->pack[i] == guid)
      return true;
  return false;
}

u32 pe_wowinventory_next_unasked(PWowInventory *inv) {
  for (int i = 0; i < PE_WOWINV_ITEMS_MAX; i++) {
    PWowItem *item = &inv->items[i];
    if (!item->used || !item->entry)
      continue;

    bool in_a_container = false;
    for (int b = 1; b <= PE_WOWINV_BAGS && !in_a_container; b++) {
      const PWowItem *bag = bag_item(inv, b);
      for (int s = 0; bag && s < PE_WOWINV_CONTAINER_SLOTS_MAX; s++)
        in_a_container = in_a_container || bag->slots[s] == item->guid;
    }
    if (!owned(inv, item->guid) && !in_a_container)
      continue;

    PWowItemTemplate *template = find_or_add_template(inv, item->entry);
    if (template && template->state == PE_WOWINV_TEMPLATE_UNKNOWN)
      return item->entry;
  }
  return 0;
}

void pe_wowinventory_mark_asked(PWowInventory *inv, u32 entry) {
  PWowItemTemplate *template = find_or_add_template(inv, entry);
  if (template)
    template->state = PE_WOWINV_TEMPLATE_ASKED;
}
