#pragma once

// The quest and item editors' reads and writes on the local Creator database. The SQL itself is built by
// the pure modules (noggit/quest, noggit/item); this layer reads what they need and runs their plans with
// Creator's guarantees:
//  - only content made in Noggit (listed in creator_content) is ever changed or deleted;
//  - every row a save touches is journaled first, so an interrupted save is rolled back;
//  - each quest, item and NPC a save changes is recorded in Local Changes.
// Every call opens its own connection (it holds the authoring lock only while it runs).

#include <noggit/item/ItemTemplateSql.hpp>
#include <noggit/quest/QuestSavePlan.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Noggit::Creator
{
  class Database;

  // Table layouts (column lists in table order), read once per editing session.
  class Layouts
  {
  public:
    std::vector<std::string> const& columnsOf(Database& db, std::string const& table);

  private:
    std::map<std::string, std::vector<std::string>> _columns;
  };

  struct NamedEntry
  {
    std::uint32_t entry = 0;
    std::string name;
    std::uint32_t kind = 0;    // items: quality; objects: gameobject type; creatures: npc flags
    std::uint32_t display = 0; // items: display id (for the icon)
  };

  struct AreaTrigger
  {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t map = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f, radius = 0.0f;
    std::uint32_t quest = 0; // the quest it completes now
    bool other_use = false;  // teleporter, inn, scripted event, battleground entrance
  };

  struct QuestLink
  {
    Noggit::Quest::Giver giver;
    std::uint32_t quest = 0;
    bool starts = false; // gives the quest (true) or takes it back
  };

  struct QuestList
  {
    std::vector<Noggit::Quest::ChainQuest> quests; // chain columns included; editable = made in Noggit
    std::vector<QuestLink> links;
  };

  // What the pickers offer, and which quests and items were made in Noggit.
  struct ContentLists
  {
    std::vector<NamedEntry> creatures, objects, items, spells;
    std::vector<AreaTrigger> area_triggers;
    QuestList quests;
    std::set<std::uint32_t> own_quests, own_items;
  };

  ContentLists loadContentLists(Layouts& layouts);
  QuestList loadQuestList(Layouts& layouts);

  // --- quests ---

  struct QuestEditorData
  {
    Noggit::Quest::QuestDatabase db;
    Noggit::Quest::QuestContent content; // as stored; empty for a new quest
    Noggit::Quest::QuestStored stored;
    std::uint32_t next_entry = 0;        // the ID a new quest or copy gets
    std::map<std::uint32_t, std::vector<std::uint32_t>> xp_by_level; // XP of the game's quests, per level
  };

  // entry 0: a new quest (schemas and IDs only).
  QuestEditorData loadQuest(Layouts& layouts, std::uint32_t entry, QuestList const& list);

  struct QuestSaveRequest
  {
    Noggit::Quest::SaveMode mode = Noggit::Quest::SaveMode::Create;
    std::uint32_t entry = 0;  // the quest written (new ID for create / copy)
    std::uint32_t source = 0; // the quest copied
    Noggit::Quest::QuestContent content;
    Noggit::Quest::NameLookup names;
  };

  // The plan for a save, for validation and the confirmation.
  Noggit::Quest::SavePlan planQuestSave(Layouts& layouts, QuestEditorData const& data, QuestList const& list,
                                        QuestSaveRequest const& request);
  // Plans again on a fresh connection and writes it; returns what was written. Throws when refused.
  Noggit::Quest::SavePlan saveQuest(Layouts& layouts, QuestEditorData const& data, QuestList const& list,
                                    QuestSaveRequest const& request);

  Noggit::Quest::SavePlan planQuestDeletion(Layouts& layouts, QuestEditorData const& data, QuestList const& list,
                                            std::uint32_t quest, Noggit::Quest::NameLookup const& names);
  Noggit::Quest::SavePlan deleteQuest(Layouts& layouts, QuestEditorData const& data, QuestList const& list,
                                      std::uint32_t quest, Noggit::Quest::NameLookup const& names);

  // Writes the chain links the chain editor changed (quests made in Noggit only).
  void saveChain(Layouts& layouts, std::map<std::uint32_t, Noggit::Quest::ChainQuest> const& changes);

  // --- items ---

  struct ItemEditorData
  {
    Noggit::Item::ItemSchema schema;
    Noggit::Item::ItemFields values; // entry 0: empty
    std::uint32_t next_entry = 0;
    Noggit::Item::ItemUses uses;
  };

  ItemEditorData loadItem(Layouts& layouts, std::uint32_t entry);
  // Returns the new item's ID (it may differ from next_entry if another item was made meanwhile).
  std::uint32_t createItem(Layouts& layouts, ItemEditorData const& data, Noggit::Item::ItemFields const& fields);
  void updateItem(Layouts& layouts, ItemEditorData const& data, std::uint32_t entry, Noggit::Item::ItemFields const& fields);
  void deleteItem(Layouts& layouts, ItemEditorData const& data, std::uint32_t entry);
}
