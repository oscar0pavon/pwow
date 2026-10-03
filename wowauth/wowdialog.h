#ifndef PE_WOWDIALOG_H
#define PE_WOWDIALOG_H

#include <engine/numbers.h>
#include <stdbool.h>

//what an NPC says when it is talked to: the gossip menu (SMSG_GOSSIP_MESSAGE
//with the text SMSG_NPC_TEXT_UPDATE answers to its text id), the list of quests
//it gives (SMSG_QUESTGIVER_QUEST_LIST) and one quest at a time as the server
//walks it through: the details to accept, the items it asks for, the reward to
//take. a packet replaces whatever dialog was open. the names of the creatures
//(SMSG_CREATURE_QUERY_RESPONSE) are kept beside them

#define PE_WOWDIALOG_TEXT_MAX 2048
#define PE_WOWDIALOG_LINE_MAX 128
#define PE_WOWDIALOG_OPTIONS_MAX 16
#define PE_WOWDIALOG_QUESTS_MAX 32
#define PE_WOWDIALOG_ITEMS_MAX 6
#define PE_WOWDIALOG_NAMES_MAX 256
#define PE_WOWDIALOG_NAME_MAX 64

typedef enum PWowDialogKind {
  PE_WOWDIALOG_NONE,
  PE_WOWDIALOG_GOSSIP,
  PE_WOWDIALOG_QUEST_LIST,
  PE_WOWDIALOG_QUEST_DETAILS,
  PE_WOWDIALOG_QUEST_PROGRESS,
  PE_WOWDIALOG_QUEST_REWARD,
} PWowDialogKind;

typedef struct PWowGossipOption {
  u32 index;
  u8 icon;
  bool coded;
  char text[PE_WOWDIALOG_LINE_MAX];
} PWowGossipOption;

//icon is the server's own: 2 for a quest that can be taken, 4 for one under way
//(3 and 4 and 5 and 6 are the game's, see the window that draws them)
typedef struct PWowQuestEntry {
  u32 id;
  u32 icon;
  int level;
  char title[PE_WOWDIALOG_LINE_MAX];
} PWowQuestEntry;

typedef struct PWowQuestItem {
  u32 entry;
  u32 count;
  u32 display_info_id;
} PWowQuestItem;

typedef struct PWowDialog {
  PWowDialogKind kind;
  u32 serial; //counts every change, so a reader knows to look again
  u64 npc;

  //gossip and the list of quests: what the NPC says. the gossip text is asked
  //for by its id, text_wanted says nobody has yet
  u32 text_id;
  bool text_wanted;
  char greeting[PE_WOWDIALOG_TEXT_MAX];
  PWowGossipOption options[PE_WOWDIALOG_OPTIONS_MAX];
  int option_count;
  PWowQuestEntry quests[PE_WOWDIALOG_QUESTS_MAX];
  int quest_count;

  //one quest
  u32 quest_id;
  char title[PE_WOWDIALOG_LINE_MAX];
  char text[PE_WOWDIALOG_TEXT_MAX]; //the story, what is asked for, or the thanks
  char objectives[PE_WOWDIALOG_TEXT_MAX];
  bool completable; //progress: everything asked for is in the bags
  u32 required_money;
  PWowQuestItem required[PE_WOWDIALOG_ITEMS_MAX];
  int required_count;
  PWowQuestItem choices[PE_WOWDIALOG_ITEMS_MAX];
  int choice_count;
  PWowQuestItem rewards[PE_WOWDIALOG_ITEMS_MAX];
  int reward_count;
  u32 reward_money;

  //SMSG_QUESTGIVER_QUEST_COMPLETE, the last one that came
  u32 completed_serial;
  u32 completed_quest, completed_xp, completed_money;
} PWowDialog;

typedef enum PWowNameState {
  PE_WOWNAME_UNKNOWN,
  PE_WOWNAME_ASKED,
  PE_WOWNAME_KNOWN,
  PE_WOWNAME_MISSING,
} PWowNameState;

typedef struct PWowName {
  u32 entry;
  PWowNameState state;
  char name[PE_WOWDIALOG_NAME_MAX];
  char subname[PE_WOWDIALOG_NAME_MAX]; //"Warrior Trainer", "" for most
} PWowName;

typedef struct PWowNames {
  PWowName items[PE_WOWDIALOG_NAMES_MAX];
  int count;
} PWowNames;

void pe_wowdialog_handle_gossip(PWowDialog *dialog, const u8 *payload, int payload_len);
void pe_wowdialog_handle_npc_text(PWowDialog *dialog, const u8 *payload, int payload_len, bool female);
void pe_wowdialog_handle_quest_list(PWowDialog *dialog, const u8 *payload, int payload_len);
void pe_wowdialog_handle_quest_details(PWowDialog *dialog, const u8 *payload, int payload_len);
void pe_wowdialog_handle_request_items(PWowDialog *dialog, const u8 *payload, int payload_len);
void pe_wowdialog_handle_offer_reward(PWowDialog *dialog, const u8 *payload, int payload_len);
void pe_wowdialog_handle_quest_complete(PWowDialog *dialog, const u8 *payload, int payload_len);

//the dialog is over: the player closed it, walked away, or the server did
void pe_wowdialog_close(PWowDialog *dialog);

void pe_wowdialog_handle_creature_query(PWowNames *names, const u8 *payload, int payload_len);

//the creature kind named entry, NULL until the server answered
const PWowName *pe_wowdialog_name(const PWowNames *names, u32 entry);

//true once for each entry never asked about; the caller then sends the query
bool pe_wowdialog_name_wanted(PWowNames *names, u32 entry);

#endif
