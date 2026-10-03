#include "wowworld.h"
#include "wow_wire.h"

#include <openssl/rand.h>

#include <ctype.h>
#include <string.h>
#include <sys/select.h>

//opcode numbers are vanilla 1.12.1's own (Opcodes_1_12_1.h), confirmed
//against vmangos's own source rather than assumed from a generic table
#define OP_SMSG_AUTH_CHALLENGE 0x1EC
#define OP_CMSG_AUTH_SESSION 0x1ED
#define OP_SMSG_AUTH_RESPONSE 0x1EE
#define OP_CMSG_CHAR_ENUM 55
#define OP_SMSG_CHAR_ENUM 59
#define OP_CMSG_PLAYER_LOGIN 61
#define OP_SMSG_LOGIN_VERIFY_WORLD 566
#define OP_SMSG_UPDATE_OBJECT 169
#define OP_SMSG_COMPRESSED_UPDATE_OBJECT 502
#define OP_SMSG_MONSTER_MOVE 221
#define OP_SMSG_ACTION_BUTTONS 0x129
#define OP_CMSG_CAST_SPELL 0x12E
#define OP_CMSG_USE_ITEM 0xAB
#define OP_CMSG_SWAP_ITEM 0x10C
#define OP_CMSG_AUTOEQUIP_ITEM 0x10A
#define OP_SMSG_DESTROY_OBJECT 0xAA
#define OP_CMSG_ITEM_QUERY_SINGLE 86
#define OP_SMSG_ITEM_QUERY_SINGLE_RESPONSE 88
#define OP_CMSG_CREATURE_QUERY 96
#define OP_SMSG_CREATURE_QUERY_RESPONSE 97
#define OP_CMSG_SET_SELECTION 0x13D
#define OP_CMSG_GOSSIP_HELLO 0x17B
#define OP_CMSG_GOSSIP_SELECT_OPTION 0x17C
#define OP_SMSG_GOSSIP_MESSAGE 0x17D
#define OP_SMSG_GOSSIP_COMPLETE 0x17E
#define OP_CMSG_NPC_TEXT_QUERY 0x17F
#define OP_SMSG_NPC_TEXT_UPDATE 0x180
#define OP_CMSG_QUESTGIVER_STATUS_QUERY 0x182
#define OP_SMSG_QUESTGIVER_STATUS 0x183
#define OP_SMSG_QUESTGIVER_QUEST_LIST 0x185
#define OP_CMSG_QUESTGIVER_QUERY_QUEST 0x186
#define OP_SMSG_QUESTGIVER_QUEST_DETAILS 0x188
#define OP_CMSG_QUESTGIVER_ACCEPT_QUEST 0x189
#define OP_CMSG_QUESTGIVER_COMPLETE_QUEST 0x18A
#define OP_SMSG_QUESTGIVER_REQUEST_ITEMS 0x18B
#define OP_SMSG_QUESTGIVER_OFFER_REWARD 0x18D
#define OP_CMSG_QUESTGIVER_CHOOSE_REWARD 0x18E
#define OP_SMSG_QUESTGIVER_QUEST_INVALID 0x18F
#define OP_CMSG_QUESTGIVER_CANCEL 0x190
#define OP_SMSG_QUESTGIVER_QUEST_COMPLETE 0x191
#define OP_SMSG_QUESTGIVER_QUEST_FAILED 0x192

//SharedDefines.h's ResponseCodes enum, position 12 (RESPONSE_SUCCESS..
//CSTATUS_AUTHENTICATING fill 0..11 first)
#define AUTH_OK 12

static void fail(char *error, int error_max, const char *msg) {
  if (error && error_max > 0)
    snprintf(error, error_max, "%s", msg);
}

//---------------------------------------------------------------------------
//the vanilla (build <= 5875) header cipher: a XOR against a repeating key,
//chained by adding the previous *output* byte in. real RC4 and the
//CMaNGOS-TBC HMAC-derived key variant are different builds' problem, not
//this one's
//---------------------------------------------------------------------------

static void cipher_encrypt(PWowWorld *w, u8 *data, int len) {
  for (int i = 0; i < len; i++) {
    u8 x = (u8)((data[i] ^ w->cipher_key[w->send_index]) + w->send_prev);
    w->send_index = (u8)((w->send_index + 1) % w->cipher_key_len);
    data[i] = x;
    w->send_prev = x;
  }
}

static void cipher_decrypt(PWowWorld *w, u8 *data, int len) {
  for (int i = 0; i < len; i++) {
    u8 enc = data[i];
    u8 x = (u8)((enc - w->recv_prev) ^ w->cipher_key[w->recv_index]);
    w->recv_index = (u8)((w->recv_index + 1) % w->cipher_key_len);
    w->recv_prev = enc;
    data[i] = x;
  }
}

bool pe_wowworld_send_packet(PWowWorld *world, u16 opcode, const u8 *payload,
                             int payload_len) {
  //CMSG header: size (2 bytes, big-endian, counts the opcode's 4 bytes but
  //not itself) + opcode (4 bytes, little-endian; top 2 always zero, every
  //CMSG opcode fits in 16 bits)
  u16 size_field = (u16)(payload_len + 4);
  u8 header[6];
  header[0] = (u8)((size_field >> 8) & 0xFF);
  header[1] = (u8)(size_field & 0xFF);
  header[2] = (u8)(opcode & 0xFF);
  header[3] = (u8)((opcode >> 8) & 0xFF);
  header[4] = 0;
  header[5] = 0;

  if (world->cipher_key_len > 0)
    cipher_encrypt(world, header, 6);

  if (!send_all(world->fd, header, 6))
    return false;
  if (payload_len > 0 && !send_all(world->fd, payload, payload_len))
    return false;
  return true;
}

bool pe_wowworld_read_packet(PWowWorld *world, u16 *opcode, u8 *payload,
                             int payload_max, int *payload_len) {
  //SMSG header: size (2 bytes, big-endian, counts the opcode's 2 bytes but
  //not itself) + opcode (2 bytes, little-endian). Server-to-client opcodes
  //are half the width of client-to-server ones - a real protocol asymmetry,
  //not a typo
  u8 header[4];
  if (!read_exact(world->fd, header, 4))
    return false;

  if (world->cipher_key_len > 0)
    cipher_decrypt(world, header, 4);

  u16 size = (u16)((header[0] << 8) | header[1]);
  u16 op = (u16)(header[2] | (header[3] << 8));
  if (size < 2)
    return false;
  int len = size - 2;
  if (len > payload_max)
    return false;

  if (len > 0 && !read_exact(world->fd, payload, len))
    return false;

  *opcode = op;
  *payload_len = len;
  return true;
}

void pe_wowworld_close(PWowWorld *world) {
  if (world->connected)
    close(world->fd);
  world->connected = false;
}

bool pe_wowworld_connect(const char *host, int port, const char *account,
                         const u8 *session_key, u32 build, u32 realm_id,
                         PWowWorld *world, char *error, int error_max) {
  memset(world, 0, sizeof(*world));

  world->fd = tcp_connect(host, port);
  if (world->fd < 0) {
    fail(error, error_max, "could not connect to mangosd");
    return false;
  }
  world->connected = true;

  //SMSG_AUTH_CHALLENGE: a bare 4-byte server seed, sent before any
  //encryption is set up on either side
  u16 opcode;
  u8 payload[64];
  int payload_len;
  if (!pe_wowworld_read_packet(world, &opcode, payload, sizeof(payload),
                               &payload_len) ||
      opcode != OP_SMSG_AUTH_CHALLENGE || payload_len < 4) {
    fail(error, error_max, "malformed SMSG_AUTH_CHALLENGE");
    pe_wowworld_close(world);
    return false;
  }
  u32 server_seed = (u32)(payload[0] | (payload[1] << 8) | (payload[2] << 16) |
                          (payload[3] << 24));

  u32 client_seed;
  RAND_bytes((u8 *)&client_seed, sizeof(client_seed));

  char upper_account[64];
  snprintf(upper_account, sizeof(upper_account), "%s", account);
  for (char *p = upper_account; *p; p++)
    *p = (char)toupper((unsigned char)*p);

  //digest = SHA1(account | 0,0,0,0 | clientSeed | serverSeed | sessionKey),
  //matching vmangos's own WorldSocket::_HandleAuthSession byte for byte -
  //that is the account it checks against, not a generic client's guess at it
  u8 digest_input[64 + 4 + 4 + 4 + 40];
  int p = 0;
  int account_len = (int)strlen(upper_account);
  memcpy(digest_input + p, upper_account, account_len);
  p += account_len;
  memset(digest_input + p, 0, 4);
  p += 4;
  memcpy(digest_input + p, &client_seed, 4);
  p += 4;
  memcpy(digest_input + p, &server_seed, 4);
  p += 4;
  memcpy(digest_input + p, session_key, 40);
  p += 40;
  u8 digest[20];
  ww_sha1(digest_input, p, digest);

  //CMSG_AUTH_SESSION, vanilla layout: build, realm id, account, client seed,
  //digest. vmangos's own addon-info reader treats a missing or empty addon
  //block as "no addons" rather than a protocol error (AddonHandler::
  //BuildAddonPacket bails out to false without touching the auth result), so
  //it is left off entirely rather than sending a zlib stream nothing reads
  WBuf out;
  out.len = 0;
  wbuf_u32(&out, build);
  wbuf_u32(&out, realm_id);
  wbuf_cstring(&out, upper_account);
  wbuf_u32(&out, client_seed);
  wbuf_bytes(&out, digest, 20);

  if (!pe_wowworld_send_packet(world, OP_CMSG_AUTH_SESSION, out.data,
                               out.len)) {
    fail(error, error_max, "failed sending CMSG_AUTH_SESSION");
    pe_wowworld_close(world);
    return false;
  }

  //the client (and, symmetrically, the server) turns encryption on the
  //instant AUTH_SESSION is sent - SMSG_AUTH_RESPONSE itself already comes
  //back through the cipher
  memcpy(world->cipher_key, session_key, 40);
  world->cipher_key_len = 40;

  if (!pe_wowworld_read_packet(world, &opcode, payload, sizeof(payload),
                               &payload_len) ||
      opcode != OP_SMSG_AUTH_RESPONSE || payload_len < 1) {
    fail(error, error_max, "malformed SMSG_AUTH_RESPONSE");
    pe_wowworld_close(world);
    return false;
  }
  if (payload[0] != AUTH_OK) {
    fail(error, error_max, "world server rejected the session");
    pe_wowworld_close(world);
    return false;
  }

  return true;
}

//how many unrelated packets to read past before giving up on finding a
//specific one. generous: a crowded login can queue a lot of state
//(reputation, action bars, the player's own object update...) ahead of the
//packet actually being waited for
//what any wait loop and the poll keep from a packet that is not the one they
//wait for: true when it was state, folded in
static bool fold_world_packet(PWowObjectState *state, u16 opcode,
                              const u8 *payload, int payload_len) {
  switch (opcode) {
  case OP_SMSG_UPDATE_OBJECT:
    pe_wowobject_handle_packet(state, payload, payload_len, false);
    return true;
  case OP_SMSG_COMPRESSED_UPDATE_OBJECT:
    pe_wowobject_handle_packet(state, payload, payload_len, true);
    return true;
  case OP_SMSG_MONSTER_MOVE:
    pe_wowobject_handle_monster_move(state, payload, payload_len);
    return true;
  case OP_SMSG_ITEM_QUERY_SINGLE_RESPONSE:
    pe_wowinventory_handle_item_query(&state->inventory, payload, payload_len);
    return true;
  case OP_SMSG_DESTROY_OBJECT:
    pe_wowinventory_handle_destroy(&state->inventory, payload, payload_len);
    return true;
  case OP_SMSG_ACTION_BUTTONS:
    pe_wowobject_handle_action_buttons(state, payload, payload_len);
    return true;
  case OP_SMSG_CREATURE_QUERY_RESPONSE:
    pe_wowdialog_handle_creature_query(&state->names, payload, payload_len);
    return true;
  case OP_SMSG_GOSSIP_MESSAGE:
    pe_wowdialog_handle_gossip(&state->dialog, payload, payload_len);
    return true;
  case OP_SMSG_NPC_TEXT_UPDATE:
    pe_wowdialog_handle_npc_text(&state->dialog, payload, payload_len, false);
    return true;
  case OP_SMSG_QUESTGIVER_QUEST_LIST:
    pe_wowdialog_handle_quest_list(&state->dialog, payload, payload_len);
    return true;
  case OP_SMSG_QUESTGIVER_QUEST_DETAILS:
    pe_wowdialog_handle_quest_details(&state->dialog, payload, payload_len);
    return true;
  case OP_SMSG_QUESTGIVER_REQUEST_ITEMS:
    pe_wowdialog_handle_request_items(&state->dialog, payload, payload_len);
    return true;
  case OP_SMSG_QUESTGIVER_OFFER_REWARD:
    pe_wowdialog_handle_offer_reward(&state->dialog, payload, payload_len);
    return true;
  case OP_SMSG_QUESTGIVER_QUEST_COMPLETE:
    pe_wowdialog_handle_quest_complete(&state->dialog, payload, payload_len);
    return true;
  case OP_SMSG_GOSSIP_COMPLETE:
  case OP_SMSG_QUESTGIVER_QUEST_INVALID:
  case OP_SMSG_QUESTGIVER_QUEST_FAILED:
    pe_wowdialog_close(&state->dialog);
    return true;
  case OP_SMSG_QUESTGIVER_STATUS:
    pe_wowobject_handle_questgiver_status(state, payload, payload_len);
    return true;
  default:
    return false;
  }
}

#define WAIT_FOR_OPCODE_ATTEMPTS 256

bool pe_wowworld_char_enum(PWowWorld *world, PWowCharacter *out, int out_max,
                           int *count, char *error, int error_max) {
  if (!pe_wowworld_send_packet(world, OP_CMSG_CHAR_ENUM, NULL, 0)) {
    fail(error, error_max, "failed sending CMSG_CHAR_ENUM");
    return false;
  }

  u8 payload[PE_WOWWORLD_PACKET_MAX];
  u16 opcode;
  int payload_len = 0;
  bool found = false;
  for (int attempt = 0; attempt < WAIT_FOR_OPCODE_ATTEMPTS; attempt++) {
    if (!pe_wowworld_read_packet(world, &opcode, payload, sizeof(payload),
                                 &payload_len)) {
      fail(error, error_max, "disconnected while waiting for SMSG_CHAR_ENUM");
      return false;
    }
    if (opcode == OP_SMSG_CHAR_ENUM) {
      found = true;
      break;
    }
  }
  if (!found) {
    fail(error, error_max, "never saw SMSG_CHAR_ENUM");
    return false;
  }

  WBuf buf;
  memcpy(buf.data, payload, payload_len);
  buf.len = payload_len;
  buf.pos = 0;

  u8 num = wbuf_read_u8(&buf);
  *count = 0;
  for (int i = 0; i < num && *count < out_max; i++) {
    PWowCharacter *c = &out[*count];
    c->guid = wbuf_read_u64(&buf);
    wbuf_read_string(&buf, c->name, sizeof(c->name));

    c->race = wbuf_read_u8(&buf);
    c->character_class = wbuf_read_u8(&buf);
    c->gender = wbuf_read_u8(&buf);

    //the rest of Player::BuildEnumData's entry, none of which pwow needs
    //yet: skin, face, hair style, hair color, facial hair (5 x u8); level
    //(u8); zone, map (2 x u32); x, y, z (3 x float);
    //guild id, character flags (2 x u32); first-login flag (u8); pet
    //display id, level, family (3 x u32); 20 equipment slots, each a
    //display id (u32) plus an inventory type (u8)
    buf.pos += 5;
    buf.pos += 1;
    buf.pos += 4 + 4;
    buf.pos += 4 + 4 + 4;
    buf.pos += 4 + 4;
    buf.pos += 1;
    buf.pos += 4 + 4 + 4;
    buf.pos += 20 * (4 + 1);

    (*count)++;
  }

  return true;
}

bool pe_wowworld_player_login(PWowWorld *world, PWowObjectState *state,
                              u64 guid, PWowLoginResult *out, char *error,
                              int error_max) {
  WBuf req;
  req.len = 0;
  wbuf_u64(&req, guid);

  if (!pe_wowworld_send_packet(world, OP_CMSG_PLAYER_LOGIN, req.data,
                               req.len)) {
    fail(error, error_max, "failed sending CMSG_PLAYER_LOGIN");
    return false;
  }

  u8 payload[PE_WOWWORLD_PACKET_MAX];
  u16 opcode;
  int payload_len = 0;
  bool found = false;
  for (int attempt = 0; attempt < WAIT_FOR_OPCODE_ATTEMPTS; attempt++) {
    if (!pe_wowworld_read_packet(world, &opcode, payload, sizeof(payload),
                                 &payload_len)) {
      fail(error, error_max,
          "disconnected while waiting for SMSG_LOGIN_VERIFY_WORLD");
      return false;
    }
    if (fold_world_packet(state, opcode, payload, payload_len))
      continue;
    if (opcode == OP_SMSG_LOGIN_VERIFY_WORLD) {
      found = true;
      break;
    }
  }
  if (!found) {
    fail(error, error_max, "never saw SMSG_LOGIN_VERIFY_WORLD");
    return false;
  }

  WBuf buf;
  memcpy(buf.data, payload, payload_len);
  buf.len = payload_len;
  buf.pos = 0;
  out->map = wbuf_read_u32(&buf);
  out->x = wbuf_read_float(&buf);
  out->y = wbuf_read_float(&buf);
  out->z = wbuf_read_float(&buf);
  out->o = wbuf_read_float(&buf);

  return true;
}

bool pe_wowworld_query_item(PWowWorld *world, PWowObjectState *state,
                            u32 item_entry, PWowItemInfo *out, char *error,
                            int error_max) {
  WBuf req;
  req.len = 0;
  wbuf_u32(&req, item_entry);
  wbuf_u64(&req, 0); //QueryItem's guid field, only meaningful for an item
                     //already in a bag the client has open - always 0 for a
                     //plain lookup by entry, same as a real client sends
                     //for gear it only knows about from someone else's
                     //visible-item fields

  if (!pe_wowworld_send_packet(world, OP_CMSG_ITEM_QUERY_SINGLE, req.data,
                               req.len)) {
    fail(error, error_max, "failed sending CMSG_ITEM_QUERY_SINGLE");
    return false;
  }

  u8 payload[PE_WOWWORLD_PACKET_MAX];
  u16 opcode;
  int payload_len = 0;
  bool found = false;
  for (int attempt = 0; attempt < WAIT_FOR_OPCODE_ATTEMPTS; attempt++) {
    if (!pe_wowworld_read_packet(world, &opcode, payload, sizeof(payload),
                                 &payload_len)) {
      fail(error, error_max,
          "disconnected while waiting for SMSG_ITEM_QUERY_SINGLE_RESPONSE");
      return false;
    }
    u32 answered_entry = 0;
    if (payload_len >= 4)
      memcpy(&answered_entry, payload, sizeof(answered_entry));
    if (opcode == OP_SMSG_ITEM_QUERY_SINGLE_RESPONSE &&
        (answered_entry & 0x7FFFFFFF) == item_entry) {
      found = true;
      break;
    }
    fold_world_packet(state, opcode, payload, payload_len);
  }
  if (!found) {
    fail(error, error_max, "never saw SMSG_ITEM_QUERY_SINGLE_RESPONSE");
    return false;
  }

  //WorldSession::HandleItemQuerySingleOpcode (ItemHandler.cpp): an unknown
  //or undiscovered item entry gets a bare 4-byte reply of
  //itemEntry|0x80000000 instead of the full record below
  if (payload_len <= 4) {
    fail(error, error_max, "server has no such item entry");
    return false;
  }

  WBuf buf;
  memcpy(buf.data, payload, payload_len);
  buf.len = payload_len;
  buf.pos = 0;
  wbuf_read_u32(&buf); //ItemId, == item_entry
  wbuf_read_u32(&buf); //Class
  wbuf_read_u32(&buf); //SubClass
  char scratch[256];
  wbuf_read_string(&buf, scratch, sizeof(scratch)); //Name1
  wbuf_read_string(&buf, scratch, sizeof(scratch)); //Name2, always empty
  wbuf_read_string(&buf, scratch, sizeof(scratch)); //Name3, always empty
  wbuf_read_string(&buf, scratch, sizeof(scratch)); //Name4, always empty
  out->display_info_id = wbuf_read_u32(&buf);
  wbuf_read_u32(&buf); //Quality
  wbuf_read_u32(&buf); //Flags
  wbuf_read_u32(&buf); //BuyPrice
  wbuf_read_u32(&buf); //SellPrice
  out->inventory_type = wbuf_read_u32(&buf);

  return true;
}

//zero-timeout select(): true only if a full read would not block right now
static bool data_ready(PWowWorld *world) {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(world->fd, &fds);
  struct timeval tv = {0, 0};
  return select(world->fd + 1, &fds, NULL, NULL, &tv) > 0;
}

#define POLL_PACKETS_MAX 64

void pe_wowworld_poll(PWowWorld *world, PWowObjectState *state) {
  u8 payload[PE_WOWWORLD_PACKET_MAX];
  for (int i = 0; i < POLL_PACKETS_MAX && world->connected; i++) {
    if (!data_ready(world))
      return;

    u16 opcode;
    int payload_len;
    if (!pe_wowworld_read_packet(world, &opcode, payload, sizeof(payload),
                                 &payload_len)) {
      world->connected = false;
      return;
    }
    fold_world_packet(state, opcode, payload, payload_len);
  }
}

#define TARGET_FLAG_SELF 0x0000
#define TARGET_FLAG_UNIT 0x0002

static void wbuf_packed_guid(WBuf *b, u64 guid) {
  u8 mask = 0;
  u8 bytes[8];
  int count = 0;

  for (int i = 0; i < 8; i++) {
    u8 byte = (u8)(guid >> (8 * i));
    if (byte) {
      mask |= (u8)(1 << i);
      bytes[count++] = byte;
    }
  }
  wbuf_u8(b, mask);
  wbuf_bytes(b, bytes, count);
}

bool pe_wowworld_cast_spell(PWowWorld *world, u32 spell, u64 target_guid) {
  WBuf buf = {0};
  wbuf_u32(&buf, spell);
  wbuf_u16(&buf, target_guid ? TARGET_FLAG_UNIT : TARGET_FLAG_SELF);
  if (target_guid)
    wbuf_packed_guid(&buf, target_guid);

  return pe_wowworld_send_packet(world, OP_CMSG_CAST_SPELL, buf.data, buf.len);
}

#define ITEM_QUERIES_PER_CALL 4

static bool send_item_query(PWowWorld *world, u32 entry) {
  WBuf buf = {0};
  wbuf_u32(&buf, entry);
  wbuf_u64(&buf, 0);
  return pe_wowworld_send_packet(world, OP_CMSG_ITEM_QUERY_SINGLE, buf.data, buf.len);
}

void pe_wowworld_request_item_templates(PWowWorld *world, PWowObjectState *state) {
  for (int i = 0; i < ITEM_QUERIES_PER_CALL; i++) {
    u32 entry = pe_wowinventory_next_unasked(&state->inventory);
    if (!entry || !send_item_query(world, entry))
      return;
    pe_wowinventory_mark_asked(&state->inventory, entry);
  }
}

//CMSG_USE_ITEM: the item's bag and slot, the spell slot of the item (its
//first), then the targets of the spell, none
bool pe_wowworld_use_item(PWowWorld *world, u8 bag_index, u8 slot) {
  WBuf buf = {0};
  wbuf_u8(&buf, bag_index);
  wbuf_u8(&buf, slot);
  wbuf_u8(&buf, 0);
  wbuf_u16(&buf, TARGET_FLAG_SELF);
  return pe_wowworld_send_packet(world, OP_CMSG_USE_ITEM, buf.data, buf.len);
}

bool pe_wowworld_autoequip_item(PWowWorld *world, u8 bag_index, u8 slot) {
  WBuf buf = {0};
  wbuf_u8(&buf, bag_index);
  wbuf_u8(&buf, slot);
  return pe_wowworld_send_packet(world, OP_CMSG_AUTOEQUIP_ITEM, buf.data, buf.len);
}

//CMSG_SWAP_ITEM: destination bag and slot, then the source's; an empty
//destination takes the item, a full one trades places with it
bool pe_wowworld_swap_item(PWowWorld *world, u8 dst_bag, u8 dst_slot, u8 src_bag, u8 src_slot) {
  WBuf buf = {0};
  wbuf_u8(&buf, dst_bag);
  wbuf_u8(&buf, dst_slot);
  wbuf_u8(&buf, src_bag);
  wbuf_u8(&buf, src_slot);
  return pe_wowworld_send_packet(world, OP_CMSG_SWAP_ITEM, buf.data, buf.len);
}

bool pe_wowworld_set_selection(PWowWorld *world, u64 guid) {
  WBuf buf = {0};
  wbuf_u64(&buf, guid);
  return pe_wowworld_send_packet(world, OP_CMSG_SET_SELECTION, buf.data, buf.len);
}

static bool send_guid(PWowWorld *world, u16 opcode, u64 guid) {
  WBuf buf = {0};
  wbuf_u64(&buf, guid);
  return pe_wowworld_send_packet(world, opcode, buf.data, buf.len);
}

static bool send_guid_and_quest(PWowWorld *world, u16 opcode, u64 npc, u32 quest) {
  WBuf buf = {0};
  wbuf_u64(&buf, npc);
  wbuf_u32(&buf, quest);
  return pe_wowworld_send_packet(world, opcode, buf.data, buf.len);
}

bool pe_wowworld_gossip_hello(PWowWorld *world, u64 npc) {
  return send_guid(world, OP_CMSG_GOSSIP_HELLO, npc);
}

//the npc, the option, and the text typed for a coded option, left out of any other
bool pe_wowworld_gossip_select(PWowWorld *world, u64 npc, u32 option, const char *code) {
  WBuf buf = {0};
  wbuf_u64(&buf, npc);
  wbuf_u32(&buf, option);
  if (code && code[0])
    wbuf_cstring(&buf, code);
  return pe_wowworld_send_packet(world, OP_CMSG_GOSSIP_SELECT_OPTION, buf.data, buf.len);
}

bool pe_wowworld_quest_query(PWowWorld *world, u64 npc, u32 quest) {
  return send_guid_and_quest(world, OP_CMSG_QUESTGIVER_QUERY_QUEST, npc, quest);
}

bool pe_wowworld_quest_accept(PWowWorld *world, u64 npc, u32 quest) {
  return send_guid_and_quest(world, OP_CMSG_QUESTGIVER_ACCEPT_QUEST, npc, quest);
}

bool pe_wowworld_quest_complete(PWowWorld *world, u64 npc, u32 quest) {
  return send_guid_and_quest(world, OP_CMSG_QUESTGIVER_COMPLETE_QUEST, npc, quest);
}

bool pe_wowworld_quest_reward(PWowWorld *world, u64 npc, u32 quest, u32 choice) {
  WBuf buf = {0};
  wbuf_u64(&buf, npc);
  wbuf_u32(&buf, quest);
  wbuf_u32(&buf, choice);
  return pe_wowworld_send_packet(world, OP_CMSG_QUESTGIVER_CHOOSE_REWARD, buf.data, buf.len);
}

bool pe_wowworld_quest_status_query(PWowWorld *world, u64 npc) {
  return send_guid(world, OP_CMSG_QUESTGIVER_STATUS_QUERY, npc);
}

static bool send_creature_query(PWowWorld *world, u32 entry, u64 guid) {
  WBuf buf = {0};
  wbuf_u32(&buf, entry);
  wbuf_u64(&buf, guid);
  return pe_wowworld_send_packet(world, OP_CMSG_CREATURE_QUERY, buf.data, buf.len);
}

static void request_dialog_items(PWowObjectState *state) {
  const PWowDialog *dialog = &state->dialog;
  if (dialog->kind == PE_WOWDIALOG_NONE)
    return;

  for (int i = 0; i < dialog->choice_count; i++)
    pe_wowinventory_want_template(&state->inventory, dialog->choices[i].entry);
  for (int i = 0; i < dialog->reward_count; i++)
    pe_wowinventory_want_template(&state->inventory, dialog->rewards[i].entry);
  for (int i = 0; i < dialog->required_count; i++)
    pe_wowinventory_want_template(&state->inventory, dialog->required[i].entry);
}

#define DETAIL_QUERIES_PER_CALL 8

void pe_wowworld_request_details(PWowWorld *world, PWowObjectState *state) {
  PWowDialog *dialog = &state->dialog;
  if (dialog->text_wanted) {
    WBuf buf = {0};
    wbuf_u32(&buf, dialog->text_id);
    wbuf_u64(&buf, dialog->npc);
    pe_wowworld_send_packet(world, OP_CMSG_NPC_TEXT_QUERY, buf.data, buf.len);
    dialog->text_wanted = false;
  }
  request_dialog_items(state);

  int sent = 0;
  for (int i = 0; i < state->count && sent < DETAIL_QUERIES_PER_CALL; i++) {
    PWowCreature *creature = &state->creatures[i];

    if (pe_wowdialog_name_wanted(&state->names, creature->entry)) {
      send_creature_query(world, creature->entry, creature->guid);
      sent++;
    }
    if ((creature->npc_flags & PE_WOWOBJECT_NPC_QUESTGIVER) && !creature->quest_status_asked) {
      pe_wowworld_quest_status_query(world, creature->guid);
      creature->quest_status_asked = true;
      sent++;
    }
  }
}

//the game's MSG_MOVE_* opcodes (Opcodes_1_12_1.h)
static u16 move_opcode(PWowMove move) {
  switch (move) {
  case PE_WOWMOVE_START_FORWARD:
    return 181;
  case PE_WOWMOVE_START_BACKWARD:
    return 182;
  case PE_WOWMOVE_STOP:
    return 183;
  case PE_WOWMOVE_START_STRAFE_LEFT:
    return 184;
  case PE_WOWMOVE_START_STRAFE_RIGHT:
    return 185;
  case PE_WOWMOVE_STOP_STRAFE:
    return 186;
  case PE_WOWMOVE_SET_FACING:
    return 218;
  case PE_WOWMOVE_HEARTBEAT:
  default:
    return 238;
  }
}

static void wbuf_float(WBuf *b, float value) {
  u32 bits;
  memcpy(&bits, &value, sizeof(bits));
  wbuf_u32(b, bits);
}

//MovementInfo of a player on foot: flags, the time, the position and facing, and
//how long it has been falling
bool pe_wowworld_send_move(PWowWorld *world, PWowMove move, u32 flags, u32 time_ms,
                           float x, float y, float z, float orientation) {
  WBuf buf = {0};
  wbuf_u32(&buf, flags);
  wbuf_u32(&buf, time_ms);
  wbuf_float(&buf, x);
  wbuf_float(&buf, y);
  wbuf_float(&buf, z);
  wbuf_float(&buf, orientation);
  wbuf_u32(&buf, 0);
  return pe_wowworld_send_packet(world, move_opcode(move), buf.data, buf.len);
}
