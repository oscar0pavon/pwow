#ifndef PE_WOWOBJECT_H
#define PE_WOWOBJECT_H

#include <engine/numbers.h>
#include <stdbool.h>

//ongoing world state, the third phase after wowauth's login and wowworld's
//handshake/char-select: parses SMSG_UPDATE_OBJECT/SMSG_COMPRESSED_UPDATE_
//OBJECT, the packets that carry every creature's spawn position and display
//id, once the player is actually placed in the world, plus SMSG_MONSTER_MOVE,
//which is how the server actually moves a creature once spawned - an
//UPDATETYPE_MOVEMENT block in an UPDATE_OBJECT packet is rare in practice.
//pwow has no character-select-style need for anything but creatures yet, so
//players/items/gameobjects are parsed (to stay in sync) but not kept

#define PE_WOWOBJECT_CREATURES_MAX 512

//what a unit holds in its hands, from UNIT_VIRTUAL_ITEM_SLOT_DISPLAY and
//UNIT_VIRTUAL_ITEM_INFO (UpdateFields_1_12_1.h): slot 0 the main hand, 1 the
//off hand (a shield is an item here too) and 2 the ranged weapon. display is
//an ItemDisplayInfo id, 0 for an empty hand; item_class and item_subclass are
//the item's own (2 a weapon, 4 armor and subclass 6 of it a shield)
#define PE_WOWOBJECT_VIRTUAL_ITEM_SLOTS 3

typedef struct PWowVirtualItems {
  u32 display[PE_WOWOBJECT_VIRTUAL_ITEM_SLOTS];
  u8 item_class[PE_WOWOBJECT_VIRTUAL_ITEM_SLOTS];
  u8 item_subclass[PE_WOWOBJECT_VIRTUAL_ITEM_SLOTS];
} PWowVirtualItems;

//UNIT_FIELD_HEALTH, POWER1..5, MAXHEALTH, MAXPOWER1..5, LEVEL and the power
//type of BYTES_0 (UpdateFields_1_12_1.h). power 0 is mana, 1 rage, 2 focus and
//3 energy; a values block only carries what changed, so a field it left out
//keeps what it was
#define PE_WOWOBJECT_POWERS 5

typedef struct PWowUnitStats {
  u32 health, max_health;
  u32 power[PE_WOWOBJECT_POWERS];
  u32 max_power[PE_WOWOBJECT_POWERS];
  u32 level;
  u8 power_type;
  u8 shapeshift_form; //byte 3 of UNIT_FIELD_BYTES_2
  u64 target; //UNIT_FIELD_TARGET, who it has selected
} PWowUnitStats;

#define PE_WOWOBJECT_ACTION_BUTTONS 120

typedef struct PWowCreature {
  u64 guid;
  u32 entry;
  u32 display_id;
  PWowVirtualItems held;
  PWowUnitStats stats;
  float scale; //OBJECT_FIELD_SCALE_X, the size the server sets for this unit, 1 by default
  float x, y, z, o; //what creatures_sync() actually reads - the interpolated
                    //display position/facing, kept current every frame by
                    //pe_wowobject_state_tick() while moving below is set

  //SMSG_MONSTER_MOVE state: a spline from move_from to move_to, timed over
  //move_duration seconds, set by pe_wowobject_handle_monster_move() and
  //advanced by pe_wowobject_state_tick(), which is what actually writes
  //x/y/z/o above between the sparse spline packets the server sends
  bool moving;
  //the last SMSG_MONSTER_MOVE's own spline flags carry a real walk/run bit
  //for classic - PRE_WOTLK_RUNMODE (0x100), set for Run and clear for Walk
  //(WoWee's spline_packet.hpp; WotLK repurposed the same bit as DONE and
  //moved the distinction to separate opcodes instead). meaningless while
  //!moving - the "stop" spline packet leaves this at whatever it last was
  bool walking;
  float move_from_x, move_from_y, move_from_z;
  float move_to_x, move_to_y, move_to_z;
  float move_elapsed, move_duration;
  bool move_has_heading;
  float move_heading;
  bool move_has_final_facing;
  float move_final_facing;
} PWowCreature;

//PLAYER_VISIBLE_ITEM_1_0..19_0 (UpdateFields_1_12_1.h, confirmed against
//vmangos - the exact server this talks to): one dword entry per equip slot,
//in Player::EquipmentSlots order (head, neck, shoulders, ... tabard), same
//19 slots wowworld.c's char_enum comment already skips over. entry, not a
//display id or guid - resolving it to an ItemDisplayInfo id needs a
//CMSG_ITEM_QUERY_SINGLE round trip (pe_wowworld_query_item)
#define PE_WOWOBJECT_PLAYER_EQUIP_SLOTS 19

typedef struct PWowPlayerEquipment {
  //false until at least one CREATE_OBJECT/VALUES block for the local
  //player has actually been parsed - item_entry is all zero either way, so
  //this is the only way to tell "confirmed empty-handed" from "not seen yet"
  bool valid;
  u32 item_entry[PE_WOWOBJECT_PLAYER_EQUIP_SLOTS]; //0 = empty slot
} PWowPlayerEquipment;

typedef struct PWowObjectState {
  PWowCreature creatures[PE_WOWOBJECT_CREATURES_MAX];
  int count;

  //sender's own guid, so pe_wowobject_handle_packet can tell "the local
  //player's own object" apart from every other player passing through -
  //set once via pe_wowobject_set_local_player_guid(), right after
  //pe_wowworld_player_login() returns it (main.c already has it there for
  //characters[0].guid, the one it logged in as)
  u64 local_player_guid;
  PWowPlayerEquipment player_equipment;

  //the local player's own unit fields, and PLAYER_XP and PLAYER_NEXT_LEVEL_XP
  //that only its owner is sent. false until its first block is parsed
  bool player_valid;
  PWowUnitStats player;
  u32 player_xp, player_next_level_xp;

  //SMSG_ACTION_BUTTONS: 120 slots of action | type << 24, 0 for an empty one.
  //type 0 is a spell (action its id), 0x40 a macro and 0x80 an item. serial
  //counts how many times they came, so a reader knows to look again
  u32 action_buttons[PE_WOWOBJECT_ACTION_BUTTONS];
  u32 action_buttons_serial;
} PWowObjectState;

//parses one SMSG_ACTION_BUTTONS payload into state
void pe_wowobject_handle_action_buttons(PWowObjectState *state,
                                        const u8 *payload, int payload_len);

//call once, right after login, before the first pe_wowworld_poll(): without
//this, the local player's own CREATE_OBJECT/VALUES blocks are indistinguishable
//from any other player's and player_equipment is never filled in
void pe_wowobject_set_local_player_guid(PWowObjectState *state, u64 guid);

//parses one SMSG_UPDATE_OBJECT (or, with compressed true, SMSG_COMPRESSED_
//UPDATE_OBJECT - decompressed first) payload and folds any creature blocks
//it finds into state: CREATE_OBJECT/CREATE_OBJECT2 add or update a creature
//by guid, MOVEMENT updates the position of one already known,
//OUT_OF_RANGE_OBJECTS removes it. an unrecognised block type (NEAR_OBJECTS,
//or anything future) aborts the rest of *this* packet only; state from
//blocks already applied earlier in it is kept
void pe_wowobject_handle_packet(PWowObjectState *state, const u8 *payload,
                                int payload_len, bool compressed);

//parses one SMSG_MONSTER_MOVE payload and starts (or, for a "stop" packet,
//cancels) the named creature's interpolated move. a guid this has not seen
//a CREATE_OBJECT for yet is ignored, same as everywhere else in this file
void pe_wowobject_handle_monster_move(PWowObjectState *state,
                                      const u8 *payload, int payload_len);

//call once a frame, before creatures_sync() (or anything else) reads
//state->creatures[].x/y/z/o: advances every creature currently mid-spline by
//delta_seconds and writes its interpolated position/facing back into
//x/y/z/o
void pe_wowobject_state_tick(PWowObjectState *state, double delta_seconds);

#endif
