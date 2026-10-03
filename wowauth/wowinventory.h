#ifndef PE_WOWINVENTORY_H
#define PE_WOWINVENTORY_H

#include <engine/numbers.h>
#include <stdbool.h>

//what the local player carries: the item and container objects the server
//creates for it (SMSG_UPDATE_OBJECT blocks of type item and container), where
//the player's own fields say each one is (backpack and equipped bags), and
//what each kind of item is (SMSG_ITEM_QUERY_SINGLE_RESPONSE), asked for as it
//is first seen and kept

#define PE_WOWINV_ITEMS_MAX 256
#define PE_WOWINV_TEMPLATES_MAX 512
#define PE_WOWINV_CONTAINER_SLOTS_MAX 36
#define PE_WOWINV_EQUIP_SLOTS 23
#define PE_WOWINV_PACK_SLOTS 16
#define PE_WOWINV_FIRST_BAG_SLOT 19 //equipment slots 19 to 22 hold the four bags
#define PE_WOWINV_BAGS 4
#define PE_WOWINV_NAME_MAX 64

typedef struct PWowItem {
  bool used;
  u64 guid;
  u32 entry;
  u32 count; //ITEM_FIELD_STACK_COUNT
  u32 num_slots; //CONTAINER_FIELD_NUM_SLOTS, 0 for what is not a bag
  u64 slots[PE_WOWINV_CONTAINER_SLOTS_MAX]; //guids, 0 for an empty slot
} PWowItem;

typedef enum PWowTemplateState {
  PE_WOWINV_TEMPLATE_UNKNOWN,
  PE_WOWINV_TEMPLATE_ASKED,
  PE_WOWINV_TEMPLATE_KNOWN,
  PE_WOWINV_TEMPLATE_MISSING, //the server has no such entry
} PWowTemplateState;

typedef struct PWowItemTemplate {
  u32 entry;
  PWowTemplateState state;
  u32 display_info_id;
  u32 quality;
  u32 inventory_type;
  u32 stackable;
  u32 container_slots;
  u32 item_level;
  u32 required_level;
  char name[PE_WOWINV_NAME_MAX];
} PWowItemTemplate;

typedef struct PWowInventory {
  PWowItem items[PE_WOWINV_ITEMS_MAX];
  PWowItemTemplate templates[PE_WOWINV_TEMPLATES_MAX];
  int template_count;

  //PLAYER_FIELD_INV_SLOT_HEAD and PLAYER_FIELD_PACK_SLOT_1: item guids, 0 for
  //an empty slot
  u64 equipped[PE_WOWINV_EQUIP_SLOTS];
  u64 pack[PE_WOWINV_PACK_SLOTS];

  u32 money; //PLAYER_FIELD_COINAGE, in copper

  //counts every change, so a reader knows to look again
  u32 serial;
} PWowInventory;

//what a bag slot shows, as the game's GetContainerItemInfo says it
typedef struct PWowSlotView {
  bool filled;
  u32 entry;
  u32 count;
  u32 display_info_id; //0 until the template is known
  u32 quality;
} PWowSlotView;

//the bag ids of the game: 0 is the backpack and 1 to 4 the bags on the bar.
//size is 16 for the backpack, a bag's own slot count, 0 for none
int pe_wowinventory_bag_size(const PWowInventory *inv, int bag);

//the item at slot 1 to size of a bag
PWowSlotView pe_wowinventory_slot(const PWowInventory *inv, int bag, int slot);

//the item in the bag slot of the bar, 1 to 4: its entry and template, NULL
//entry 0 when there is none
u32 pe_wowinventory_bag_entry(const PWowInventory *inv, int bag);
const PWowItemTemplate *pe_wowinventory_template(const PWowInventory *inv, u32 entry);

//raw: an object's fields up to CONTAINER_END as its block sent them, present
//says which came. a field not sent keeps what it was
void pe_wowinventory_apply_item(PWowInventory *inv, u64 guid, const u32 *raw,
                                const bool *present, int field_count);
bool pe_wowinventory_has_item(const PWowInventory *inv, u64 guid);
void pe_wowinventory_remove_item(PWowInventory *inv, u64 guid);

//the player's own PLAYER_FIELD_INV_SLOT_HEAD.. and PACK_SLOT_1.. dwords,
//counted from the first of them
void pe_wowinventory_apply_player(PWowInventory *inv, const u32 *raw,
                                  const bool *present);

//SMSG_ITEM_QUERY_SINGLE_RESPONSE and SMSG_DESTROY_OBJECT
void pe_wowinventory_handle_item_query(PWowInventory *inv, const u8 *payload,
                                       int payload_len);
void pe_wowinventory_handle_destroy(PWowInventory *inv, const u8 *payload,
                                    int payload_len);

//the entry of an owned item whose template was never asked for, or 0. the
//caller sends the query and calls pe_wowinventory_mark_asked()
u32 pe_wowinventory_next_unasked(PWowInventory *inv);

//an item that is not owned, a quest's reward: its template is asked for with
//the rest
void pe_wowinventory_want_template(PWowInventory *inv, u32 entry);
void pe_wowinventory_mark_asked(PWowInventory *inv, u32 entry);

#endif
