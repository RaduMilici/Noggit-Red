#pragma once
#include "Services.hpp"
#include <QHash>
#include <functional>
namespace Noggit::Creator {
class Database;
// A rule a player must meet to see a response: a row of the world's `conditions` table.
struct DialogueCondition {
  enum class Kind { OnQuest, NotOnQuest, QuestReady, CompletedQuest, NotCompletedQuest, HasItem, LacksItem, MinLevel, MaxLevel };
  Kind kind = Kind::OnQuest;
  Id value = 0;  // the quest, item or level
  int count = 1; // items: how many
  bool aboutQuest() const { return kind <= Kind::NotCompletedQuest; }
  bool aboutItem() const { return kind == Kind::HasItem || kind == Kind::LacksItem; }
  bool operator==(DialogueCondition const&) const = default;
};
// What happens when a response is chosen: a gossip script step, run with the NPC as caster and the
// player as target. The game runs them for responses that continue or end the conversation only.
struct DialogueEffect {
  enum class Kind { CastSpell, Teleport, CompleteQuest };
  Kind kind = Kind::CastSpell;
  Id id = 0;          // the spell, or the quest
  Position position;  // Teleport: where the player goes
  bool operator==(DialogueEffect const&) const = default;
};
struct DialogueResponse {
  enum class Action { Continue, Close, Vendor, Trainer };
  QString text;
  Action action = Action::Close;
  int target = -1; // Continue: the node it leads to
  QVector<DialogueCondition> conditions; // all must hold
  QVector<DialogueEffect> effects;
  bool operator==(DialogueResponse const&) const = default;
};
struct DialogueNode {
  QString text; // what the NPC says: $N name, $C class, $R race, $B line break
  QVector<DialogueResponse> responses;
  Id menu = 0;  // its gossip menu when loaded; 0 for a new node
  bool operator==(DialogueNode const&) const = default;
};
struct Dialogue {
  Id entry = 0, display = 0;
  QString name, notice;
  bool editable = false;
  bool shared = false;         // uses a game NPC's dialogue; saving gives this NPC its own copy
  bool listQuests = false;     // the greeting lists the quests this NPC gives (the game lists them nowhere else)
  QVector<DialogueNode> nodes; // nodes[0] is the greeting; none: the NPC has no dialogue
  QStringList dropped;         // what the loaded dialogue does that the editor cannot show (left out of copies)
  // What the checks need to know about the NPC.
  bool vendor = false, trainer = false;
  QVector<Choice> quests;      // the quests it gives
};
struct DialogueProblem { int node = -1, response = -1; QString text; bool error = true; };
// What the database-free checks need to know about the rest of the world.
struct DialogueFacts {
  QSet<Id> quests, items, spells;
  QSet<Id> completable; // quests a script may complete: event quests, and your own (saving gives them the flag)
};
// The rows a dialogue is stored in, by table ("gossip_menu", "gossip_menu_option", "npc_text",
// "broadcast_text", "gossip_scripts", "conditions").
using GossipRows = QMap<QString, QVector<Fields>>;
// Talking to an NPC: its gossip menus, as a tree of nodes and responses. Only Creator NPCs can be changed;
// a game NPC's dialogue can be copied to your own. Uses only what the Tortoise world database stores:
// no custom scripting.
class GossipService {
public:
  static constexpr int maxNodes = 64, maxResponses = 15, maxConditions = 4, maxEffects = 4;
  static constexpr int maxText = 2000, maxResponseText = 255;
  static QVector<Choice> owners(QString const& text); // NPCs with a dialogue, to copy from
  static Dialogue load(Id entry);
  static void save(Dialogue const&);
  static QVector<DialogueProblem> validate(Dialogue const&); // check() with facts()
  static DialogueFacts facts(Dialogue const&);               // about the quests, items and spells it names
  // Inside the caller's save: removes a Creator NPC's dialogue (deleting the NPC), or gives a new Creator NPC
  // its own copy of `from`'s dialogue (cloning; two NPCs never share a dialogue made in Noggit).
  static void removeWith(Database&, Id entry);
  static void copyWith(Database&, Id from, Id to);

  // Database-free parts.
  static QVector<DialogueProblem> check(Dialogue const&, DialogueFacts const&);
  // `from`'s conversation, ready to save as `to`'s own.
  static Dialogue copied(Dialogue const& from, Dialogue to);
  // The nodes reachable from the greeting, in tree order.
  static QVector<int> reachable(Dialogue const&);
  // Reads the dialogue starting at gossip menu `root` from its rows.
  static void parse(Dialogue&, Id root, GossipRows const&);
  // The rows for `d`. `id(table, preferred)` gives a free ID for a new row (keeping `preferred` when it can);
  // `condition(row)` the condition_entry of a condition (an existing one with the same values, or a new one).
  static GossipRows rows(Dialogue const&, std::function<Id(QString const&, Id)> const& id,
                         std::function<Id(Fields const&)> const& condition);
  static Fields conditionRow(DialogueCondition const&);
  // "Player is on quest The Missing Graves"; names come from `name(kind, id)` (kind: "quest", "item").
  static QString describe(DialogueCondition const&, std::function<QString(QString const&, Id)> const& name);
};
}
