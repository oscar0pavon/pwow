#ifndef PE_WOWWORLD_H
#define PE_WOWWORLD_H

#include <engine/numbers.h>
#include "wowobject.h"
#include <stdbool.h>

#define PE_WOWWORLD_ERROR_MAX 128
#define PE_WOWWORLD_PACKET_MAX 8192

//a live connection to a world server (mangosd), past the CMSG_AUTH_SESSION
//handshake: every packet from here on has its header run through the
//vanilla XOR+add chaining cipher (build <= 5875; TBC/WotLK use different
//keys or real RC4, which this does not implement)
typedef struct PWowWorld {
  int fd;
  bool connected;

  u8 cipher_key[40];
  int cipher_key_len;
  u8 send_index, send_prev;
  u8 recv_index, recv_prev;
} PWowWorld;

//connects to host:port and completes the CMSG_AUTH_SESSION handshake using
//the session key pe_wowauth_login already returned, leaving the header
//cipher initialized for every packet after. blocking: does not return until
//the handshake finishes or fails. on success, world->connected is true and
//the connection is ready for pe_wowworld_send/read_packet
bool pe_wowworld_connect(const char *host, int port, const char *account,
                         const u8 *session_key, u32 build, u32 realm_id,
                         PWowWorld *world, char *error, int error_max);

//sends one packet, encrypting its header first if the handshake has run
bool pe_wowworld_send_packet(PWowWorld *world, u16 opcode, const u8 *payload,
                             int payload_len);

//blocks until one full packet arrives, decrypting its header first. false on
//disconnect or a payload too big for payload_max
bool pe_wowworld_read_packet(PWowWorld *world, u16 *opcode, u8 *payload,
                             int payload_max, int *payload_len);

void pe_wowworld_close(PWowWorld *world);

#define PE_WOWWORLD_CHARACTERS_MAX 10

typedef struct PWowCharacter {
  u64 guid;
  char name[32];
} PWowCharacter;

//sends CMSG_CHAR_ENUM and blocks for SMSG_CHAR_ENUM, filling out with
//whichever characters the account has (only guid and name - the packet also
//carries appearance, equipment and pet info for the character-select menu,
//which pwow has no menu to draw). skips over any other packet that arrives
//first
bool pe_wowworld_char_enum(PWowWorld *world, PWowCharacter *out, int out_max,
                           int *count, char *error, int error_max);

typedef struct PWowLoginResult {
  u32 map;
  float x, y, z, o;
} PWowLoginResult;

//sends CMSG_PLAYER_LOGIN for guid and blocks for SMSG_LOGIN_VERIFY_WORLD -
//the server's confirmation that the character is now actually placed on a
//map, which is what starts the flow of SMSG_UPDATE_OBJECT packets for
//nearby creatures. object-update and monster-move packets seen while
//waiting are folded into state (same as pe_wowworld_poll would do with
//them) rather than discarded - the player's own first CREATE_OBJECT, the
//one PLAYER_VISIBLE_ITEM_1_0..19_0 actually arrives in, is one of them and
//is otherwise gone for good by the time this returns. call
//pe_wowobject_set_local_player_guid(state, guid) before this, not after -
//too late to matter once this has already read past that block. everything
//else on the wire (initial spells, action bars, reputation...) is still
//read and discarded
bool pe_wowworld_player_login(PWowWorld *world, PWowObjectState *state,
                              u64 guid, PWowLoginResult *out, char *error,
                              int error_max);

//call once per frame after a successful pe_wowworld_connect (and, normally,
//pe_wowworld_player_login): drains and dispatches whatever object-update
//packets have already arrived, without blocking if none have - safe to call
//every frame. capped at a generous number of packets per call so one
//crowded frame can't stall the caller waiting on the network. everything
//else on the wire (chat, spells, other players...) is read and silently
//discarded, same as pe_wowworld_player_login already does while it waits
//for its own confirmation - pwow only tracks creatures right now. sets
//world->connected false if the connection drops while draining
void pe_wowworld_poll(PWowWorld *world, PWowObjectState *state);

typedef struct PWowItemInfo {
  u32 display_info_id; //ItemDisplayInfo.dbc row id, 0 if the item has none
  u32 inventory_type;  //ItemPrototype::InventoryType - which equip slot(s)
                       //this item's own kind goes in, independent of which
                       //of the 19 PLAYER_VISIBLE_ITEM slots it was read from
} PWowItemInfo;

//sends CMSG_ITEM_QUERY_SINGLE for item_entry (an entry off
//PWowPlayerEquipment.item_entry, not a display id or a guid) and blocks for
//SMSG_ITEM_QUERY_SINGLE_RESPONSE - there is no local item-template data
//this could resolve the answer from instead (see TODO.md's equipment item,
//"needs ... an ItemDisplayInfo.dbc reader ... or a vmangos DB read"; this
//is the network side of that same question). object-update and monster-move
//packets that arrive while waiting are still folded into state, exactly
//like pe_wowworld_poll would, rather than silently discarded the way
//pe_wowworld_player_login discards everything while it waits - a slow
//reply here is not rare enough to risk desyncing the creature stream over
bool pe_wowworld_query_item(PWowWorld *world, PWowObjectState *state,
                            u32 item_entry, PWowItemInfo *out, char *error,
                            int error_max);

//CMSG_CAST_SPELL for spell, at target_guid, or at nobody (the caster) for 0.
//does not wait for an answer: whatever the server says back is not read
bool pe_wowworld_cast_spell(PWowWorld *world, u32 spell, u64 target_guid);

//an item by where the server keeps it: the bag index is 255 for the backpack
//and the backpack's slots are 23 to 38, a bag on the bar is 19 to 22 and its
//slots start at 0. neither waits for an answer
bool pe_wowworld_use_item(PWowWorld *world, u8 bag_index, u8 slot);
bool pe_wowworld_autoequip_item(PWowWorld *world, u8 bag_index, u8 slot);

//asks, without waiting, for the template of the first few owned items whose
//kind was not asked about yet; the answers arrive through pe_wowworld_poll()
void pe_wowworld_request_item_templates(PWowWorld *world, PWowObjectState *state);

#endif
