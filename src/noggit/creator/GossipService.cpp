#include "GossipService.hpp"
#include "ChangeTracker.hpp"
#include "Database.hpp"
#include <QJsonArray>
#include <QMultiHash>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
using Kind = DialogueCondition::Kind;
using Action = DialogueResponse::Action;
using Effect = DialogueEffect::Kind;
QString n(qint64 value) { return QString::number(value); }
void require(bool condition, QString const& message) { if (!condition) throw std::runtime_error(message.toStdString()); }
// Gossip option types (vmangos GossipOption) and the NPC role (npc_flags bit) each one needs.
constexpr int optionGossip = 1, optionQuestGiver = 2, optionVendor = 3, optionTrainer = 5, optionArmorer = 15;
constexpr int roleGossip = 1, roleQuestGiver = 2, roleVendor = 4, roleTrainer = 16;
constexpr int iconChat = 0, iconVendor = 1, iconTrainer = 3;
// The text quest-list markers carry in the world database; the game shows the quests' titles instead.
QString const questMarker = "GOSSIP_OPTION_QUESTGIVER";
// Script commands (vmangos ScriptMgr.h), laid out as the world database's own gossip scripts.
constexpr int commandTeleport = 6, commandQuestExplored = 7, commandCastSpell = 15;
constexpr int swapTargets = 1; // data_flags: the player becomes the source, which a teleport moves
// Condition types (vmangos ConditionType) and flags.
constexpr int conditionAnd = -1, conditionItem = 2, conditionQuestRewarded = 8, conditionQuestTaken = 9, conditionLevel = 15;
constexpr int reverseResult = 1;
constexpr int questEventFlag = 2; // quest_template.SpecialFlags: completed by an area trigger or a script
// action_menu_id is a signed mediumint.
constexpr Id menuLimit = 0x7fffff;
// Each table a dialogue writes: its key, and the creator_content kind that marks rows made in Noggit.
struct Table { char const *name, *key, *kind; };
Table const tables[] = {{"gossip_menu", "entry", "gossip_menu"}, {"npc_text", "ID", "npc_text"},
                        {"broadcast_text", "entry", "gossip_text"}, {"gossip_scripts", "id", "gossip_script"}};
struct Layout { int type, value2, flags; };
Layout layout(DialogueCondition const& c) {
  switch (c.kind) {
    case Kind::OnQuest: return {conditionQuestTaken, 0, 0};
    case Kind::NotOnQuest: return {conditionQuestTaken, 0, reverseResult};
    case Kind::QuestReady: return {conditionQuestTaken, 2, 0};
    case Kind::CompletedQuest: return {conditionQuestRewarded, 0, 0};
    case Kind::NotCompletedQuest: return {conditionQuestRewarded, 0, reverseResult};
    case Kind::HasItem: return {conditionItem, c.count, 0};
    case Kind::LacksItem: return {conditionItem, c.count, reverseResult};
    case Kind::MinLevel: return {conditionLevel, 1, 0};
    case Kind::MaxLevel: return {conditionLevel, 2, 0};
  }
  return {};
}
std::optional<DialogueCondition> simple(Fields const& row) {
  int type = row["type"].toInt(), v2 = row["value2"].toInt(), flags = row["flags"].toInt();
  Id v1 = row["value1"].toUInt();
  if (row["value3"].toInt() || row["value4"].toInt() || (flags & ~reverseResult) || !v1) return {};
  bool reverse = flags & reverseResult;
  DialogueCondition c; c.value = v1;
  if (type == conditionQuestTaken && v2 == 0) c.kind = reverse ? Kind::NotOnQuest : Kind::OnQuest;
  else if (type == conditionQuestTaken && v2 == 2 && !reverse) c.kind = Kind::QuestReady;
  else if (type == conditionQuestRewarded && v2 == 0) c.kind = reverse ? Kind::NotCompletedQuest : Kind::CompletedQuest;
  else if (type == conditionItem && v2 >= 1) { c.kind = reverse ? Kind::LacksItem : Kind::HasItem; c.count = v2; }
  else if (type == conditionLevel && (v2 == 1 || v2 == 2) && !reverse) c.kind = v2 == 1 ? Kind::MinLevel : Kind::MaxLevel;
  else return {};
  return c;
}
QString serviceName(int option) {
  switch (option) {
    case 4: return "flight master"; case 6: case 7: return "spirit healer"; case 8: return "innkeeper"; case 9: return "banker";
    case 10: case 11: return "guild"; case 12: return "battlemaster"; case 13: return "auctioneer"; case 14: return "stable master";
    case 16: case 17: return "unlearning"; default: return "option " + n(option);
  }
}
QString idList(QSet<Id> const& ids) { QStringList list; for (auto id : ids) list << n(id); list.sort(); return list.join(','); }
// Every row of the dialogue starting at `root`, original or not (to show or copy it).
GossipRows fetch(Database& db, Id root) {
  GossipRows rows;
  QSet<Id> menus, frontier{root};
  while (!frontier.isEmpty() && menus.size() < 4 * GossipService::maxNodes) {
    menus += frontier;
    QSet<Id> next;
    for (auto const& o : db.query("SELECT action_menu_id FROM gossip_menu_option WHERE option_id=" + n(optionGossip) + " AND action_menu_id>0 AND menu_id IN (" + idList(frontier) + ")"))
      if (auto target = o["action_menu_id"].toUInt(); !menus.contains(target)) next.insert(target);
    frontier = next;
  }
  auto all = [&](QString const& table, QString const& where) { rows[table] = db.query("SELECT * FROM " + table + " WHERE " + where); };
  all("gossip_menu", "entry IN (" + idList(menus) + ")");
  all("gossip_menu_option", "menu_id IN (" + idList(menus) + ") ORDER BY menu_id,id");
  QSet<Id> texts, lines, scripts, conditions;
  for (auto const& m : rows["gossip_menu"]) { texts.insert(m["text_id"].toUInt()); scripts.insert(m["script_id"].toUInt()); conditions.insert(m["condition_id"].toUInt()); }
  for (auto const& o : rows["gossip_menu_option"]) {
    lines.insert(o["option_broadcast_text"].toUInt()); scripts.insert(o["action_script_id"].toUInt()); conditions.insert(o["condition_id"].toUInt());
  }
  for (auto* set : {&texts, &lines, &scripts, &conditions}) set->remove(0);
  if (!texts.isEmpty()) {
    all("npc_text", "ID IN (" + idList(texts) + ")");
    for (auto const& t : rows["npc_text"]) for (int i = 0; i < 8; ++i) if (auto line = t["BroadcastTextID" + n(i)].toUInt()) lines.insert(line);
  }
  if (!lines.isEmpty()) all("broadcast_text", "entry IN (" + idList(lines) + ")");
  if (!scripts.isEmpty()) all("gossip_scripts", "id IN (" + idList(scripts) + ") ORDER BY id,delay,priority");
  // Combined conditions name the ones they combine.
  QSet<Id> seen;
  while (!conditions.isEmpty()) {
    seen += conditions;
    QSet<Id> parts;
    for (auto const& c : db.query("SELECT * FROM conditions WHERE condition_entry IN (" + idList(conditions) + ")")) {
      rows["conditions"].push_back(c);
      if (c["type"].toInt() < 0) for (int i = 1; i <= 4; ++i) if (auto part = c["value" + n(i)].toUInt(); part && !seen.contains(part)) parts.insert(part);
    }
    conditions = parts;
  }
  return rows;
}
}
Fields GossipService::conditionRow(DialogueCondition const& c) {
  auto l = layout(c);
  return {{"type", l.type}, {"value1", c.value}, {"value2", l.value2}, {"value3", 0}, {"value4", 0}, {"flags", l.flags}};
}
QString GossipService::describe(DialogueCondition const& c, std::function<QString(QString const&, Id)> const& name) {
  auto quest = [&] { return name("quest", c.value); };
  auto item = [&] { return (c.count > 1 ? n(c.count) + " × " : QString()) + name("item", c.value); };
  switch (c.kind) {
    case Kind::OnQuest: return "Player is on quest " + quest();
    case Kind::NotOnQuest: return "Player is not on quest " + quest();
    case Kind::QuestReady: return "Player finished the objectives of " + quest();
    case Kind::CompletedQuest: return "Player has completed quest " + quest();
    case Kind::NotCompletedQuest: return "Player has not completed quest " + quest();
    case Kind::HasItem: return "Player carries " + item();
    case Kind::LacksItem: return "Player does not carry " + item();
    case Kind::MinLevel: return "Player is level " + n(c.value) + " or higher";
    case Kind::MaxLevel: return "Player is level " + n(c.value) + " or lower";
  }
  return {};
}
QVector<int> GossipService::reachable(Dialogue const& d) {
  QVector<int> order; if (d.nodes.isEmpty()) return order;
  QVector<bool> seen(d.nodes.size());
  // Depth first, in response order: the order the tree shows.
  std::function<void(int)> visit = [&](int node) {
    seen[node] = true; order << node;
    for (auto const& r : d.nodes[node].responses)
      if (r.action == Action::Continue && r.target >= 0 && r.target < d.nodes.size() && !seen[r.target]) visit(r.target);
  };
  visit(0);
  return order;
}
Dialogue GossipService::copied(Dialogue const& from, Dialogue to) {
  to.nodes = from.nodes; to.listQuests = from.listQuests; to.dropped = from.dropped;
  for (auto& node : to.nodes) node.menu = 0;
  return to;
}
void GossipService::parse(Dialogue& d, Id root, GossipRows const& rows) {
  d.nodes.clear(); d.listQuests = false;
  if (!root) return;
  auto index = [&](QString const& table, QString const& key) {
    QMultiHash<Id, Fields> map; auto const list = rows.value(table);
    for (auto it = list.rbegin(); it != list.rend(); ++it) map.insert((*it)[key].toUInt(), *it); // values() then keeps table order
    return map;
  };
  auto menus = index("gossip_menu", "entry"), options = index("gossip_menu_option", "menu_id"), texts = index("npc_text", "ID");
  auto lines = index("broadcast_text", "entry"), scripts = index("gossip_scripts", "id"), conditions = index("conditions", "condition_entry");
  auto drop = [&](QString const& what) { if (!d.dropped.contains(what)) d.dropped << what; };
  auto line = [&](Id entry) { auto row = lines.value(entry); auto text = row["male_text"].toString(); return text.isEmpty() ? row["female_text"].toString() : text; };
  auto condition = [&](Id id) -> std::optional<QVector<DialogueCondition>> {
    if (!conditions.contains(id)) return {};
    auto row = conditions.value(id);
    if (row["type"].toInt() != conditionAnd) { if (auto c = simple(row)) return QVector<DialogueCondition>{*c}; return {}; }
    if (row["flags"].toInt()) return {};
    QVector<DialogueCondition> all;
    for (int i = 1; i <= 4; ++i) if (auto part = row["value" + n(i)].toUInt()) {
      auto c = conditions.contains(part) ? simple(conditions.value(part)) : std::nullopt;
      if (!c) return {};
      all << *c;
    }
    return all;
  };
  QVector<Id> order{root}; QHash<Id, int> node{{root, 0}};
  for (int i = 0; i < order.size(); ++i) {
    DialogueNode current; current.menu = order[i];
    auto heads = menus.values(order[i]);
    if (heads.isEmpty()) current.text = "Greetings, $N."; // the game's default greeting
    else {
      if (heads.size() > 1) drop("Greetings that change with conditions: only the default one is kept.");
      auto head = heads[0];
      for (auto const& h : heads) if (!h["condition_id"].toUInt()) { head = h; break; }
      if (head["script_id"].toUInt()) drop("Scripts that run when a node opens.");
      if (!texts.contains(head["text_id"].toUInt())) drop("A greeting text the editor cannot read.");
      else {
        auto text = texts.value(head["text_id"].toUInt()); int variants = 0;
        for (int k = 0; k < 8; ++k) if (auto entry = text["BroadcastTextID" + n(k)].toUInt()) { if (!variants++) current.text = line(entry); }
        if (variants > 1) drop("Random alternative greetings: only the first one is kept.");
      }
    }
    for (auto const& o : options.values(order[i])) {
      int type = o["option_id"].toInt();
      if (type == optionQuestGiver) { if (i == 0) d.listQuests = true; continue; }
      DialogueResponse r;
      r.text = o["option_text"].toString();
      if (r.text.isEmpty() && o["option_broadcast_text"].toUInt()) r.text = line(o["option_broadcast_text"].toUInt());
      if (type == optionGossip) {
        int target = o["action_menu_id"].toInt();
        r.action = Action::Close;
        if (target > 0) {
          if (!node.contains(target) && order.size() < maxNodes) { node[target] = order.size(); order << Id(target); }
          if (node.contains(target)) { r.action = Action::Continue; r.target = node[target]; }
          else drop(QString("More than %1 nodes: the rest end the conversation.").arg(maxNodes));
        }
      } else if (type == optionVendor || type == optionArmorer) r.action = Action::Vendor;
      else if (type == optionTrainer) r.action = Action::Trainer;
      else { drop("Responses for other services (" + serviceName(type) + ")."); continue; }
      if (o["action_poi_id"].toUInt()) drop("Map markers shown by responses.");
      if (o["box_coded"].toInt() || o["box_money"].toUInt() || !o["box_text"].toString().isEmpty() || o["box_broadcast_text"].toUInt())
        drop("Confirmation pop-ups and payments on responses.");
      if (auto id = o["condition_id"].toUInt()) {
        if (auto parsed = condition(id)) r.conditions = *parsed;
        else drop("A condition the editor cannot show, on \"" + r.text + "\": without it the response is shown to everyone.");
      }
      if (auto id = o["action_script_id"].toUInt(); id && (r.action == Action::Continue || r.action == Action::Close)) {
        for (auto const& s : scripts.values(id)) {
          int command = s["command"].toInt(), flags = s["data_flags"].toInt();
          DialogueEffect e; e.id = s["datalong"].toUInt();
          if (command == commandCastSpell && flags == 0) e.kind = Effect::CastSpell;
          else if (command == commandQuestExplored) e.kind = Effect::CompleteQuest;
          else if (command == commandTeleport && (flags & swapTargets)) {
            e.kind = Effect::Teleport; e.id = 0;
            e.position = {s["datalong"].toUInt(), s["x"].toFloat(), s["y"].toFloat(), s["z"].toFloat(), s["o"].toFloat()};
          } else { drop("Scripted steps the editor cannot show, on \"" + r.text + "\"."); continue; }
          r.effects << e;
        }
      }
      current.responses << r;
    }
    d.nodes << current;
  }
}
GossipRows GossipService::rows(Dialogue const& d, std::function<Id(QString const&, Id)> const& id, std::function<Id(Fields const&)> const& condition) {
  GossipRows out;
  if (d.nodes.isEmpty()) return out;
  // Nodes keep their menu where they can, so an edit changes rows in place.
  QVector<Id> menus(d.nodes.size());
  for (int i = 0; i < d.nodes.size(); ++i) if (d.nodes[i].menu) menus[i] = id("gossip_menu", d.nodes[i].menu);
  for (auto& menu : menus) if (!menu) menu = id("gossip_menu", 0);
  for (int i = 0; i < d.nodes.size(); ++i) {
    auto const& node = d.nodes[i];
    auto text = id("npc_text", 0), line = id("broadcast_text", 0);
    out["gossip_menu"] << Fields{{"entry", menus[i]}, {"text_id", text}, {"script_id", 0}, {"condition_id", 0}};
    Fields npcText{{"ID", text}};
    for (int k = 0; k < 8; ++k) { npcText["BroadcastTextID" + n(k)] = k ? 0 : line; npcText["Probability" + n(k)] = k ? 0 : 1; }
    out["npc_text"] << npcText;
    Fields spoken{{"entry", line}, {"male_text", node.text}, {"female_text", node.text}, {"chat_type", 0}, {"sound_id", 0}, {"language_id", 0}};
    for (int k = 1; k <= 3; ++k) { spoken["emote_id" + n(k)] = 0; spoken["emote_delay" + n(k)] = 0; }
    out["broadcast_text"] << spoken;
    int option = 0;
    auto add = [&](QString const& text, int type, int role, int icon, int action, Id script, Id rule) {
      out["gossip_menu_option"] << Fields{{"menu_id", menus[i]}, {"id", option++}, {"option_icon", icon}, {"option_text", text}, {"option_broadcast_text", 0},
                                          {"option_id", type}, {"npc_option_npcflag", role}, {"action_menu_id", action}, {"action_poi_id", 0},
                                          {"action_script_id", script}, {"box_coded", 0}, {"box_money", 0}, {"box_text", ""}, {"box_broadcast_text", 0},
                                          {"condition_id", rule}};
    };
    for (auto const& r : node.responses) {
      Id rule = 0;
      if (!r.conditions.isEmpty()) {
        // In the designer's order (reused conditions may have any ID); a new AND row is allocated after them all.
        QVector<Id> parts; for (auto const& c : r.conditions) if (auto part = condition(conditionRow(c)); !parts.contains(part)) parts << part;
        rule = parts.size() == 1 ? parts[0]
             : condition({{"type", conditionAnd}, {"value1", parts[0]}, {"value2", parts[1]}, {"value3", parts.value(2)}, {"value4", parts.value(3)}, {"flags", 0}});
      }
      Id script = 0;
      bool talking = r.action == Action::Continue || r.action == Action::Close;
      if (talking && !r.effects.isEmpty()) {
        script = id("gossip_scripts", 0);
        int step = 0;
        for (auto const& e : r.effects) {
          Fields s{{"id", script}, {"delay", 0}, {"priority", step++}, {"datalong", e.id}, {"datalong2", 0}, {"datalong3", 0}, {"datalong4", 0},
                   {"target_param1", 0}, {"target_param2", 0}, {"target_type", 0}, {"data_flags", 0}, {"dataint", 0}, {"dataint2", 0},
                   {"dataint3", 0}, {"dataint4", 0}, {"x", 0}, {"y", 0}, {"z", 0}, {"o", 0}, {"condition_id", 0}};
          if (e.kind == Effect::CastSpell) { s["command"] = commandCastSpell; s["comments"] = "Creator dialogue: cast on the player"; }
          else if (e.kind == Effect::CompleteQuest) { s["command"] = commandQuestExplored; s["comments"] = "Creator dialogue: complete the quest"; }
          else {
            s["command"] = commandTeleport; s["datalong"] = e.position.map; s["data_flags"] = swapTargets;
            s["x"] = e.position.x; s["y"] = e.position.y; s["z"] = e.position.z; s["o"] = e.position.orientation;
            s["comments"] = "Creator dialogue: teleport the player";
          }
          out["gossip_scripts"] << s;
        }
      }
      switch (r.action) {
        case Action::Continue: add(r.text, optionGossip, roleGossip, iconChat, int(menus.value(r.target)), script, rule); break;
        case Action::Close: add(r.text, optionGossip, roleGossip, iconChat, -1, script, rule); break;
        case Action::Vendor: add(r.text, optionVendor, roleVendor, iconVendor, 0, 0, rule); break;
        case Action::Trainer: add(r.text, optionTrainer, roleTrainer, iconTrainer, 0, 0, rule); break;
      }
    }
    // The quest list is a marker, not a response: the game lists the NPC's quests in the greeting only.
    if (i == 0 && d.listQuests) add(questMarker, optionQuestGiver, roleQuestGiver, iconChat, 0, 0, 0);
  }
  return out;
}
QVector<DialogueProblem> GossipService::check(Dialogue const& d, DialogueFacts const& facts) {
  QVector<DialogueProblem> problems;
  auto add = [&](int node, int response, QString const& text, bool error = true) { problems.push_back({node, response, text, error}); };
  if (d.nodes.isEmpty()) return problems;
  if (d.nodes.size() > maxNodes) add(-1, -1, QString("A dialogue holds at most %1 nodes; this one has %2.").arg(maxNodes).arg(d.nodes.size()));
  auto order = reachable(d);
  // A greeting without responses gets the game's usual ones (quests, browse goods, train me) from the NPC's roles.
  bool usual = d.nodes[0].responses.isEmpty() && !d.listQuests;
  if (!usual && !d.listQuests && !d.quests.isEmpty())
    add(0, -1, "This NPC gives quests, but the greeting does not list them, so players cannot get them. Turn on \"List this NPC's quests\".", false);
  if (d.listQuests && d.quests.isEmpty())
    add(0, -1, "The greeting lists this NPC's quests, but it gives none yet. Make it a giver on a quest's Givers page.", false);
  bool shop = false, training = false;
  for (int i = 0; i < d.nodes.size(); ++i) {
    auto const& node = d.nodes[i];
    bool seen = order.contains(i);
    if (!seen) add(i, -1, "No response leads here, so players never see this node. Link it from another node or delete it.");
    if (node.text.trimmed().isEmpty()) add(i, -1, "Write what the NPC says.");
    else if (node.text.size() > maxText) add(i, -1, QString("Keep what the NPC says under %1 characters.").arg(maxText));
    if (node.responses.size() > maxResponses) add(i, -1, QString("A node holds at most %1 responses.").arg(maxResponses));
    for (int j = 0; j < node.responses.size(); ++j) {
      auto const& r = node.responses[j];
      if (r.text.trimmed().isEmpty()) add(i, j, "Write what the player says.");
      else if (r.text.size() > maxResponseText) add(i, j, QString("Keep the response under %1 characters.").arg(maxResponseText));
      if (r.action == Action::Continue && (r.target < 0 || r.target >= d.nodes.size())) add(i, j, "Choose the node this response leads to.");
      if (r.action == Action::Vendor) {
        shop = shop || seen;
        if (!d.vendor) add(i, j, "This NPC is not a vendor, so players do not see this response. Turn on \"Sells items\" in Vendor.", false);
      }
      if (r.action == Action::Trainer) {
        training = training || seen;
        if (!d.trainer) add(i, j, "This NPC is not a trainer, so players do not see this response. Turn on \"Teaches\" in Trainer.", false);
      }
      if (r.conditions.size() > maxConditions) add(i, j, QString("A response can have at most %1 conditions.").arg(maxConditions));
      int minLevel = 0, maxLevel = 255;
      QSet<Id> onQuests;
      for (auto const& c : r.conditions) {
        if (c.aboutQuest()) {
          if (!c.value) add(i, j, "Choose the quest of the condition.");
          else if (!facts.quests.contains(c.value)) add(i, j, QString("The condition's quest %1 does not exist.").arg(c.value));
          if (c.kind == Kind::OnQuest || c.kind == Kind::QuestReady) onQuests.insert(c.value);
        } else if (c.aboutItem()) {
          if (!c.value) add(i, j, "Choose the item of the condition.");
          else if (!facts.items.contains(c.value)) add(i, j, QString("The condition's item %1 does not exist.").arg(c.value));
          if (c.count < 1 || c.count > 255) add(i, j, "The item count is 1 to 255.");
        } else {
          if (c.value < 1 || c.value > 60) add(i, j, "The level is 1 to 60.");
          if (c.kind == Kind::MinLevel) minLevel = std::max<int>(minLevel, c.value); else maxLevel = std::min<int>(maxLevel, c.value);
        }
      }
      if (minLevel > maxLevel) add(i, j, "No player meets both level conditions, so nobody sees this response.", false);
      if (!r.effects.isEmpty() && r.action != Action::Continue && r.action != Action::Close)
        add(i, j, "Effects only happen on responses that continue or end the conversation: shop and trainer responses open their window instead.");
      if (r.effects.size() > maxEffects) add(i, j, QString("A response can have at most %1 effects.").arg(maxEffects));
      for (auto const& e : r.effects) {
        if (e.kind == Effect::CastSpell) {
          if (!e.id) add(i, j, "Choose the spell to cast.");
          else if (!facts.spells.contains(e.id)) add(i, j, QString("Spell %1 does not exist.").arg(e.id));
        } else if (e.kind == Effect::Teleport) {
          auto const& p = e.position;
          if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || (p.x == 0 && p.y == 0 && p.z == 0))
            add(i, j, "Choose where the player is teleported to.");
        } else {
          if (!e.id) add(i, j, "Choose the quest to complete.");
          else if (!facts.quests.contains(e.id)) add(i, j, QString("Quest %1 does not exist.").arg(e.id));
          else if (!facts.completable.contains(e.id))
            add(i, j, "This game quest is not completed by an event, so talking cannot complete it. Use one of your own quests.");
          else if (!onQuests.contains(e.id))
            add(i, j, "Only players on the quest get credit. Add the condition \"Player is on quest\" for it, so only they see this response.", false);
        }
      }
    }
  }
  if (!usual && d.vendor && !shop) add(-1, -1, "This NPC sells items, but no response opens its shop: with a dialogue, players can only shop through one.", false);
  if (!usual && d.trainer && !training) add(-1, -1, "This NPC is a trainer, but no response opens training: with a dialogue, players can only train through one.", false);
  return problems;
}
QVector<Choice> GossipService::owners(QString const& text) {
  Database db; QVector<Choice> out;
  for (auto const& r : db.query("SELECT entry,name,subname FROM creature_template WHERE gossip_menu_id>0 AND name LIKE " + db.quote('%' + text + '%')
                                + " ORDER BY name LIMIT 250"))
    out.push_back({r["entry"].toUInt(), r["name"].toString(), r["subname"].toString()});
  return out;
}
Dialogue GossipService::load(Id entry) {
  Database db;
  auto heads = db.query("SELECT name,display_id1,npc_flags,gossip_menu_id FROM creature_template WHERE entry=" + n(entry));
  require(!heads.isEmpty(), "This NPC no longer exists.");
  Dialogue d; d.entry = entry; d.name = heads[0]["name"].toString(); d.display = heads[0]["display_id1"].toUInt();
  auto flags = heads[0]["npc_flags"].toUInt(); d.vendor = flags & roleVendor; d.trainer = flags & roleTrainer;
  d.editable = db.owned("npc", entry);
  for (auto const& q : db.query("SELECT q.entry,q.Title FROM creature_questrelation r JOIN quest_template q ON q.entry=r.quest WHERE r.id=" + n(entry) + " ORDER BY q.Title"))
    d.quests.push_back({q["entry"].toUInt(), q["Title"].toString(), {}});
  auto root = heads[0]["gossip_menu_id"].toUInt();
  if (root) {
    parse(d, root, fetch(db, root));
    d.shared = !db.owned("gossip_menu", root)
            || !db.query("SELECT entry FROM creature_template WHERE gossip_menu_id=" + n(root) + " AND entry<>" + n(entry) + " LIMIT 1").isEmpty();
    if (d.shared) for (auto& node : d.nodes) node.menu = 0; // saved as this NPC's own copy
  }
  if (!d.editable) d.notice = "This is a game NPC: its dialogue is shown read-only. Copy it to one of your own NPCs to change it.";
  else if (d.shared) d.notice = "This NPC shares its dialogue with another NPC. Saving gives it its own copy; the other one stays unchanged.";
  return d;
}
QVector<DialogueProblem> GossipService::validate(Dialogue const& d) { return check(d, facts(d)); }
DialogueFacts GossipService::facts(Dialogue const& d) {
  QSet<Id> quests, items, spells;
  for (auto const& node : d.nodes) for (auto const& r : node.responses) {
    for (auto const& c : r.conditions) if (c.value) { if (c.aboutQuest()) quests.insert(c.value); else if (c.aboutItem()) items.insert(c.value); }
    for (auto const& e : r.effects) if (e.id) { if (e.kind == Effect::CastSpell) spells.insert(e.id); else if (e.kind == Effect::CompleteQuest) quests.insert(e.id); }
  }
  DialogueFacts facts;
  if (!quests.isEmpty() || !items.isEmpty() || !spells.isEmpty()) {
    Database db;
    if (!quests.isEmpty())
      for (auto const& q : db.query("SELECT entry,SpecialFlags,entry IN (SELECT entry FROM creator_content WHERE kind='quest') AS own FROM quest_template WHERE entry IN (" + idList(quests) + ")")) {
        facts.quests.insert(q["entry"].toUInt());
        if ((q["SpecialFlags"].toUInt() & questEventFlag) || q["own"].toInt()) facts.completable.insert(q["entry"].toUInt());
      }
    if (!items.isEmpty()) for (auto const& r : db.query("SELECT entry FROM item_template WHERE entry IN (" + idList(items) + ")")) facts.items.insert(r["entry"].toUInt());
    if (!spells.isEmpty()) for (auto const& r : db.query("SELECT entry FROM spell_template WHERE entry IN (" + idList(spells) + ")")) facts.spells.insert(r["entry"].toUInt());
  }
  return facts;
}
namespace {
// Removes the Creator rows of an NPC's current dialogue (as Local Changes captures them), with their marks.
// Shared conditions stay: other content may use them, and they hold nothing but values.
QJsonObject removeOwned(Database& db, Id entry) {
  auto current = ChangeTracker::capture([&db](QString const& sql) { return db.query(sql); }, EntityType::Gossip, entry);
  for (auto const& t : tables) {
    QSet<Id> ids;
    for (auto const& row : current[t.name].toArray()) ids.insert(row.toObject()[t.key].toString().toUInt());
    if (ids.isEmpty()) continue;
    auto where = QString(t.key) + " IN (" + idList(ids) + ")";
    db.snapshotWhere(t.name, where); db.exec("DELETE FROM " + QString(t.name) + " WHERE " + where);
    if (QString(t.name) == "gossip_menu") {
      auto options = "menu_id IN (" + idList(ids) + ")";
      db.snapshotWhere("gossip_menu_option", options); db.exec("DELETE FROM gossip_menu_option WHERE " + options);
    }
    auto marks = QString("kind='") + t.kind + "' AND entry IN (" + idList(ids) + ")";
    db.snapshotWhere("creator_content", marks); db.exec("DELETE FROM creator_content WHERE " + marks);
  }
  return current;
}
// Another NPC uses this NPC's greeting: its rows are not this NPC's to change.
bool sharedGreeting(Database& db, Id entry) {
  return !db.query("SELECT o.entry FROM creature_template o JOIN creature_template t ON t.entry=" + n(entry)
                   + " WHERE t.gossip_menu_id>0 AND o.gossip_menu_id=t.gossip_menu_id AND o.entry<>t.entry LIMIT 1").isEmpty();
}
// Writes `d` as its NPC's dialogue, replacing the previous version (the caller tracks and commits).
void write(Database& db, Dialogue const& d) {
  // The previous version's IDs are reused, so an edit changes rows in place.
  auto previous = sharedGreeting(db, d.entry) ? QJsonObject{} : removeOwned(db, d.entry);
  QHash<QString, QVector<Id>> freed;
  QHash<QString, Id> next;
  for (auto const& t : tables) {
    auto& pool = freed[t.name];
    for (auto const& row : previous[t.name].toArray()) pool << row.toObject()[t.key].toString().toUInt();
    std::sort(pool.begin(), pool.end());
    pool.erase(std::unique(pool.begin(), pool.end()), pool.end());
    // The freed rows are gone, so the table's maximum may now be below them: new IDs start above both.
    if (!pool.isEmpty()) next[t.name] = pool.last() + 1;
  }
  auto id = [&](QString const& table, Id preferred) -> Id {
    auto& pool = freed[table];
    if (preferred && pool.removeOne(preferred)) return preferred;
    if (!pool.isEmpty()) return pool.takeFirst();
    QString key = table == "npc_text" ? "ID" : table == "gossip_scripts" ? "id" : "entry";
    auto free = db.allocate(table, key, table == "gossip_menu" ? menuLimit : 0xffffff);
    if (!next.contains(table) || next[table] < free) next[table] = free;
    require(next[table] <= (table == "gossip_menu" ? menuLimit : 0xffffffu), "There is no free local identifier available for this dialogue.");
    return next[table]++;
  };
  // Conditions are shared by value (the table allows each combination once).
  auto condition = [&](Fields const& row) -> Id {
    QStringList match; for (auto it = row.begin(); it != row.end(); ++it) match << it.key() + "=" + db.quote(it.value());
    auto found = db.query("SELECT condition_entry FROM conditions WHERE " + match.join(" AND ") + " LIMIT 1");
    if (!found.isEmpty()) return found[0]["condition_entry"].toUInt();
    // Allocated after its parts, so a combined condition comes after the ones it names, as the game requires.
    auto entry = db.allocate("conditions", "condition_entry", 0xffffff);
    db.snapshotWhere("conditions", "condition_entry=" + n(entry));
    auto full = row; full["condition_entry"] = entry; db.insert("conditions", full);
    db.snapshotWhere("creator_content", "kind='condition' AND entry=" + n(entry)); db.mark("condition", entry);
    return entry;
  };
  auto rows = GossipService::rows(d, id, condition);
  for (auto const& t : tables) {
    QSet<Id> ids; for (auto const& row : rows.value(t.name)) ids.insert(row[t.key].toUInt());
    if (ids.isEmpty()) continue;
    db.snapshotWhere(t.name, QString(t.key) + " IN (" + idList(ids) + ")");
    if (QString(t.name) == "gossip_menu") db.snapshotWhere("gossip_menu_option", "menu_id IN (" + idList(ids) + ")");
    db.snapshotWhere("creator_content", QString("kind='") + t.kind + "' AND entry IN (" + idList(ids) + ")");
    for (auto value : ids) db.mark(t.kind, value);
  }
  for (auto table : {"gossip_menu", "gossip_menu_option", "npc_text", "broadcast_text", "gossip_scripts"})
    for (auto const& row : rows.value(table)) db.insert(table, row);
  Id root = rows.value("gossip_menu").isEmpty() ? 0 : rows["gossip_menu"][0]["entry"].toUInt();
  db.snapshot("creature_template", "entry", d.entry);
  db.exec("UPDATE creature_template SET gossip_menu_id=" + n(root) + ",npc_flags=(npc_flags&~" + n(roleGossip) + ")|" + n(root ? roleGossip : 0) + " WHERE entry=" + n(d.entry));
  // Your quests a response completes become event quests, so the game lets talking complete them.
  QSet<Id> completed;
  for (auto const& node : d.nodes) for (auto const& r : node.responses) for (auto const& e : r.effects) if (e.kind == Effect::CompleteQuest && e.id) completed.insert(e.id);
  for (auto quest : completed) {
    if (!db.owned("quest", quest)) continue;
    auto flags = db.query("SELECT SpecialFlags FROM quest_template WHERE entry=" + n(quest));
    if (flags.isEmpty() || (flags[0]["SpecialFlags"].toUInt() & questEventFlag)) continue;
    db.track(EntityType::Quest, quest);
    db.snapshot("quest_template", "entry", quest);
    db.exec("UPDATE quest_template SET SpecialFlags=SpecialFlags|" + n(questEventFlag) + " WHERE entry=" + n(quest));
  }
}
}
void GossipService::removeWith(Database& db, Id entry) {
  db.track(EntityType::Gossip, entry);
  if (!sharedGreeting(db, entry)) removeOwned(db, entry);
}
void GossipService::copyWith(Database& db, Id from, Id to) {
  auto heads = db.query("SELECT gossip_menu_id FROM creature_template WHERE entry=" + n(from));
  if (heads.isEmpty() || !heads[0]["gossip_menu_id"].toUInt()) return;
  Dialogue d; d.entry = to;
  auto root = heads[0]["gossip_menu_id"].toUInt();
  parse(d, root, fetch(db, root));
  for (auto& node : d.nodes) node.menu = 0;
  write(db, d);
}
void GossipService::save(Dialogue const& d) {
  for (auto const& p : validate(d)) require(!p.error, p.text);
  Database db;
  require(db.owned("npc", d.entry), "Game NPCs keep their dialogue. Copy it to one of your own NPCs to change it.");
  require(!db.query("SELECT entry FROM creature_template WHERE entry=" + n(d.entry)).isEmpty(), "This NPC no longer exists.");
  db.track(EntityType::Gossip, d.entry);
  write(db, d);
  db.commit();
}
}
