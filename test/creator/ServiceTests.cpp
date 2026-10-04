#include <noggit/creator/LootService.hpp>
#include <noggit/creator/VendorService.hpp>
#include <noggit/creator/TrainerService.hpp>
#include <noggit/creator/GossipService.hpp>
#include <noggit/creator/ClientDbc.hpp>
#include <noggit/creator/ClientPatch.hpp>
#include <noggit/creator/SpellService.hpp>
#include <noggit/creator/ItemDesignService.hpp>
#include <QtEndian>
#include <cstring>
#include <noggit/creator/ChangeTracker.hpp>
#include <QCoreApplication>
#include <QJsonArray>
#include <QTextStream>
#include <array>
#include <cmath>
#include <stdexcept>
using namespace Noggit::Creator;
namespace {
void check(bool condition, char const* message) { if (!condition) throw std::runtime_error(message); }
bool near(double a, double b, double tolerance) { return std::abs(a - b) <= tolerance; }
bool has(QVector<RowProblem> const& problems, int row, QString const& text, bool error = true) {
  for (auto const& p : problems) if (p.row == row && p.text.contains(text) && p.error == error) return true;
  return false;
}
LootRow row(Id item, double chance, int group = 0, int min = 1, int max = 1) { LootRow r; r.item = item; r.chance = chance; r.group = group; r.minCount = min; r.maxCount = max; return r; }
void lootValidation() {
  LootTable t; t.rows = {row(1, 25), row(2, 150), row(3, 0), row(4, 50, 0, 3, 2), row(99, 10), row(1, 30)};
  t.moneyMin = 500; t.moneyMax = 100;
  auto p = LootService::check(t, {1, 2, 3, 4}, {});
  check(has(p, 1, "between 0% and 100%"), "Chance over 100% accepted");
  check(has(p, 2, "never drops"), "0% item outside a group accepted");
  check(has(p, 3, "minimum quantity is greater"), "Min greater than max accepted");
  check(has(p, 4, "does not exist"), "Missing item accepted");
  check(has(p, 5, "already in the loot"), "Duplicate item accepted");
  check(has(p, -1, "minimum money"), "Money min over max accepted");
  check(!has(p, 0, ""), "A valid row was flagged");
  // Groups: explicit chances over 100%, and equal-chance items left with nothing.
  LootTable g; g.rows = {row(1, 70, 1), row(2, 50, 1), row(3, 60, 2), row(4, 40, 2), row(5, 0, 2)};
  auto q = LootService::check(g, {1, 2, 3, 4, 5}, {});
  check(has(q, 0, "add up to 120%") && has(q, 1, "add up to 120%"), "Broken group not reported on its rows");
  check(has(q, 4, "never drops"), "Starved equal-chance item not reported");
  // The same item in two groups is fine for NPCs, not for objects (one row per item there).
  LootTable n; n.rows = {row(7, 10, 1), row(7, 10, 2)};
  check(!has(LootService::check(n, {7}, {}), 1, "already"), "NPC loot rejected an item in two groups");
  n.owner.kind = LootOwner::Kind::Object;
  check(has(LootService::check(n, {7}, {}), 1, "already"), "Object loot accepted an item twice");
  // Quest drops are read-only here and may be negative.
  LootTable quest; quest.rows = {row(8, -35)};
  check(LootService::check(quest, {8}, {}).isEmpty(), "Quest drop flagged");
}
void lootChances() {
  QVector<LootRow> rows{row(1, 25), row(2, 100), row(3, 30, 1), row(4, 20, 1), row(5, 0, 1), row(6, 0, 1), row(7, 80, 2), row(8, 40, 2), row(9, -50)};
  check(near(LootService::expectedChance(rows, 0), 0.25, 1e-9) && near(LootService::expectedChance(rows, 1), 1.0, 1e-9), "Plain chances");
  check(near(LootService::expectedChance(rows, 2), 0.30, 1e-9) && near(LootService::expectedChance(rows, 3), 0.20, 1e-9), "Explicit group chances");
  check(near(LootService::expectedChance(rows, 4), 0.25, 1e-9) && near(LootService::expectedChance(rows, 5), 0.25, 1e-9), "Equal-chance items share the rest");
  check(near(LootService::expectedChance(rows, 7), 0.20, 1e-9), "A group's chances are cut off at 100%");
  check(near(LootService::expectedChance(rows, 8), 0.50, 1e-9), "Quest drop chance");
}
void lootSimulation() {
  LootTable t; t.moneyMin = 100; t.moneyMax = 300;
  t.rows = {row(1, 25, 0, 1, 2), row(2, 4), row(3, 50, 1), row(4, 0, 1), LootRow{}};
  t.rows[4].reference = 900; t.rows[4].chance = 100; t.rows[4].maxCount = 2;
  QHash<Id, QVector<LootRow>> refs{{900, {row(10, 50)}}};
  auto s = LootService::simulate(t, refs, 200000, 42);
  auto observed = [&](Id item) { for (auto const& i : s.items) if (i.item == item) return double(i.kills) / s.kills; return -1.0; };
  check(near(observed(1), 0.25, 0.01) && near(observed(2), 0.04, 0.005), "Observed plain drops are off");
  check(near(observed(3), 0.50, 0.01) && near(observed(4), 0.50, 0.01) && near(observed(3) + observed(4), 1.0, 1e-9), "A group must drop exactly one");
  check(near(observed(10), 0.75, 0.01), "A shared table rolled twice at 50% drops in 75% of kills");
  check(near(s.averageMoney, 200, 2), "Average money is off");
  double perKill = 0; for (auto const& i : s.items) if (i.item == 1 && i.kills) perKill = double(i.count) / i.kills;
  check(near(perKill, 1.5, 0.03), "Quantity range 1-2 should average 1.5");
  auto again = LootService::simulate(t, refs, 1000, 42);
  auto repeat = LootService::simulate(t, refs, 1000, 42);
  check(again.items.size() == repeat.items.size() && again.items[0].kills == repeat.items[0].kills, "Same seed, different result");
}
void vendorChecks() {
  Vendor v; v.sells = true;
  v.items = {{1}, {2, 3, 0}, {1}, {3, 0, 600}, {4}, {5, 300}};
  auto p = VendorService::check(v, {{1, 100}, {2, 50}, {3, 10}, {4, 0}});
  check(has(p, 1, "restock time"), "Limited stock without restock accepted");
  check(has(p, 2, "already in the shop"), "Duplicate vendor item accepted");
  check(has(p, 3, "only matters for limited stock", false), "Restock without stock not warned");
  check(has(p, 4, "free", false), "Free item not warned");
  check(has(p, 5, "does not exist") && has(p, 5, "1 to 255"), "Missing item / stock range not reported");
  check(!has(p, 0, ""), "Valid vendor row flagged");
  Vendor big; big.sells = false; for (Id i = 1; i <= 130; ++i) big.items.push_back({i});
  QHash<Id, qint64> prices; for (Id i = 1; i <= 130; ++i) prices[i] = 1;
  auto b = VendorService::check(big, prices);
  check(has(b, -1, "at most 128") && has(b, -1, "Sells items", false), "Shop size / vendor role not reported");
}
void trainerChecks() {
  QHash<Id, SpellInfo> spells;
  spells[100] = {100, 10, 0, "Fireball", "Rank 1", {}, {}, 0, 1};
  spells[101] = {101, 11, 10, "Fireball", "Rank 2", {}, "Fireball Rank 1", 0, 6};
  spells[102] = {102, 12, 99, "Frostbolt", "Rank 2", {}, "Frostbolt Rank 1", 0, 8};
  Trainer t; t.teaches = true; t.type = 0; t.playerClass = 8;
  t.spells = {{100, 10, 1}, {101, 100, 6}, {102, 200, 8}, {101, 100, 6}, {555, 0, 4}, {100, 5, 70}};
  auto p = TrainerService::check(t, spells);
  check(!has(p, 0, "") && !has(p, 1, ""), "Valid trainer rows flagged");
  check(has(p, 2, "not taught here", false), "Missing prerequisite not warned");
  check(has(p, 3, "already taught"), "Duplicate trainer spell accepted");
  check(has(p, 4, "cannot teach"), "Invalid trainer spell accepted");
  check(has(p, 5, "1 to 60"), "Invalid required level accepted");
  Trainer early; early.teaches = true; early.spells = {{101, 0, 2}, {100, 0, 1}};
  check(has(TrainerService::check(early, spells), 0, "before they can use it", false), "Too-low required level not warned");
  Trainer noClass; noClass.type = 0; noClass.playerClass = 0;
  check(has(TrainerService::check(noClass, spells), -1, "Choose the class"), "Class trainer without a class accepted");
}
QJsonObject rows(QString const& table, QJsonArray const& list) { return {{table, list}}; }
void serviceChanges() {
  // Local Changes summaries.
  TrainerSpell s;
  TrackedChange trainer; trainer.type = EntityType::Trainer; trainer.entity = 1000001; trainer.label = "Brother Alric"; trainer.action = ChangeAction::Update;
  QJsonObject head{{"entry", "1000001"}, {"trainer_id", "0"}, {"trainer_type", "2"}, {"trainer_class", "0"}, {"trainer", "1"}};
  QJsonObject fireball{{"entry", "1000001"}, {"spell", "100"}, {"spellcost", "10"}, {"reqskill", "0"}, {"reqskillvalue", "0"}, {"reqlevel", "1"}};
  QJsonObject frostbolt{{"entry", "1000001"}, {"spell", "102"}, {"spellcost", "50"}, {"reqskill", "0"}, {"reqskillvalue", "0"}, {"reqlevel", "8"}};
  trainer.before = {{"creature_trainer", QJsonArray{head}}, {"npc_trainer", QJsonArray{fireball}}};
  trainer.after = {{"creature_trainer", QJsonArray{head}}, {"npc_trainer", QJsonArray{fireball, frostbolt}},
                   {"spell_names", QJsonArray{QJsonObject{{"spell", "102"}, {"name", "Frostbolt"}, {"rank", "Rank 2"}}}}};
  check(trainer.summary() == "+ Trainer spell: Frostbolt Rank 2 (Brother Alric)", "Trainer spell summary");
  TrackedChange vendor; vendor.type = EntityType::Vendor; vendor.entity = 1000001; vendor.label = "Brother Alric"; vendor.action = ChangeAction::Update;
  QJsonObject vhead{{"entry", "1000001"}, {"vendor_id", "0"}, {"vendor", "1"}};
  QJsonObject bread{{"entry", "1000001"}, {"slot", "0"}, {"item", "4540"}, {"maxcount", "0"}, {"incrtime", "0"}, {"itemflags", "0"}, {"condition_id", "0"}};
  vendor.before = {{"creature_vendor", QJsonArray{vhead}}, {"npc_vendor", QJsonArray{bread}}};
  auto changed = bread; changed["maxcount"] = "5"; changed["incrtime"] = "900";
  vendor.after = {{"creature_vendor", QJsonArray{vhead}}, {"npc_vendor", QJsonArray{changed}}};
  check(vendor.summary() == "~ Vendor: Brother Alric", "Vendor summary");
  TrackedChange loot; loot.type = EntityType::Loot; loot.entity = 1000002; loot.label = "Restless Miller"; loot.action = ChangeAction::Update;
  QJsonObject lhead{{"entry", "1000002"}, {"loot_id", "1000002"}, {"gold_min", "10"}, {"gold_max", "50"}};
  QJsonObject bone{{"entry", "1000002"}, {"item", "5"}, {"ChanceOrQuestChance", "25"}, {"groupid", "0"}, {"mincountOrRef", "1"}, {"maxcount", "2"}, {"condition_id", "0"}};
  loot.before = {{"creature_loot", QJsonArray{lhead}}};
  loot.after = {{"creature_loot", QJsonArray{lhead}}, {"creature_loot_template", QJsonArray{bone}}};
  check(loot.summary() == "+ Loot: Restless Miller", "Loot summary");

  // Sync SQL: guarded by the owner NPC's Creator mark, only the role's flag, own loot table only.
  auto sql = ChangeTracker::sql({loot, vendor, trainer});
  check(sql.contains("SET @owned := (SELECT COUNT(*) FROM creator_content WHERE kind='npc' AND entry=1000002)"), "Loot not guarded by its NPC");
  check(sql.contains("UPDATE creature_template SET `loot_id`='1000002',`gold_min`='10',`gold_max`='50' WHERE entry=1000002 AND @owned>0;"), "Loot owner update");
  check(sql.contains("DELETE FROM creature_loot_template WHERE entry=1000002 AND @owned>0;"), "Loot table replace");
  check(sql.contains("REPLACE INTO `creature_loot_template` (") && sql.contains("FROM DUAL WHERE @owned>0;"), "Loot rows not guarded");
  check(sql.contains("`npc_flags`=(`npc_flags`&~4)|4") && sql.contains("`npc_flags`=(`npc_flags`&~16)|16"), "Role flags");
  check(sql.indexOf("-- Loot:") < sql.indexOf("-- Quest:") || !sql.contains("-- Quest:"), "Order");
  for (auto const& statement : ChangeTracker::statements(sql)) check(!statement.contains("spell_names") && !statement.contains("creature_vendor"), "Synthetic rows written");
  // A shared (not yet forked) loot table is never written.
  auto shared = loot; shared.after["creature_loot"] = QJsonArray{QJsonObject{{"entry", "1000002"}, {"loot_id", "40"}, {"gold_min", "0"}, {"gold_max", "0"}}};
  check(!ChangeTracker::sql({shared}).contains("creature_loot_template"), "A shared loot table would be overwritten");
  // Removed with the NPC: the services go first, while the NPC's mark still guards them.
  auto gone = vendor; gone.action = ChangeAction::Delete; gone.after = {};
  TrackedChange npc; npc.type = EntityType::Npc; npc.entity = 1000001; npc.action = ChangeAction::Delete; npc.label = "Brother Alric";
  npc.before = rows("creature_template", QJsonArray{QJsonObject{{"entry", "1000001"}, {"equipment_id", "0"}}});
  auto both = ChangeTracker::sql({npc, gone});
  check(both.indexOf("-- Remove vendor") >= 0 && both.indexOf("-- Remove vendor") < both.indexOf("-- Delete NPC"), "Vendor removal must precede the NPC");
}

// --- Dialogue ---
using Action = DialogueResponse::Action;
using CKind = DialogueCondition::Kind;
using EKind = DialogueEffect::Kind;
DialogueResponse response(QString const& text, Action action, int target = -1) { DialogueResponse r; r.text = text; r.action = action; r.target = target; return r; }
Dialogue malric() {
  Dialogue d; d.entry = 1000001; d.name = "Brother Malric"; d.editable = true; d.vendor = true; d.listQuests = true;
  d.quests = {{1000100, "The Missing Graves", {}}};
  DialogueNode greeting; greeting.text = "Something has disturbed the dead, $N.";
  greeting.responses = {response("What happened?", Action::Continue, 1), response("Show me your goods.", Action::Vendor), response("Farewell.", Action::Close)};
  DialogueNode graves; graves.text = "The graves were opened from below.$BBe careful.";
  auto found = response("I found the bones.", Action::Close);
  found.conditions = {{CKind::OnQuest, 1000100}, {CKind::MinLevel, 10}};
  DialogueEffect credit; credit.kind = EKind::CompleteQuest; credit.id = 1000100;
  DialogueEffect bless; bless.kind = EKind::CastSpell; bless.id = 1243;
  found.effects = {credit, bless};
  auto crypt = response("Take me to the crypt.", Action::Close);
  DialogueEffect teleport; teleport.kind = EKind::Teleport; teleport.position = {0, 1.5f, -2.5f, 30.25f, 0.5f};
  crypt.effects = {teleport};
  auto carry = response("I brought the relic.", Action::Continue, 0);
  carry.conditions = {{CKind::HasItem, 5000, 2}, {CKind::NotCompletedQuest, 1000100}};
  graves.responses = {found, crypt, carry, response("Something else...", Action::Continue, 0)};
  d.nodes = {greeting, graves};
  return d;
}
DialogueFacts malricFacts() { DialogueFacts f; f.quests = {1000100}; f.completable = {1000100}; f.items = {5000}; f.spells = {1243}; return f; }
bool hasProblem(QVector<DialogueProblem> const& problems, int node, int response, QString const& text, bool error = true) {
  for (auto const& p : problems) if (p.node == node && p.response == response && p.text.contains(text) && p.error == error) return true;
  return false;
}
// rows() then parse() gives the dialogue back: what is saved is what the editor shows next time.
void dialogueRoundTrip() {
  auto d = malric();
  QHash<QString, Id> next{{"gossip_menu", 5000000}, {"npc_text", 5100000}, {"broadcast_text", 5200000}, {"gossip_scripts", 5300000}};
  QVector<Fields> conditions;
  auto rows = GossipService::rows(d, [&](QString const& table, Id) { return next[table]++; }, [&](Fields const& row) -> Id {
    for (auto const& c : conditions) { auto same = c; same.remove("condition_entry"); if (same == row) return c["condition_entry"].toUInt(); }
    auto stored = row; stored["condition_entry"] = 7000000 + conditions.size(); conditions << stored; return stored["condition_entry"].toUInt();
  });
  check(rows["gossip_menu"].size() == 2 && rows["npc_text"].size() == 2 && rows["broadcast_text"].size() == 2, "One menu, text and line per node");
  check(rows["gossip_menu_option"].size() == 3 + 4 + 1, "Responses plus the quest-list marker");
  auto marker = rows["gossip_menu_option"][3];
  check(marker["option_id"].toInt() == 2 && marker["npc_option_npcflag"].toInt() == 2 && marker["menu_id"].toUInt() == 5000000, "Quest list marker on the greeting");
  auto vendor = rows["gossip_menu_option"][1];
  check(vendor["option_id"].toInt() == 3 && vendor["npc_option_npcflag"].toInt() == 4 && vendor["option_icon"].toInt() == 1, "Vendor option layout");
  auto close = rows["gossip_menu_option"][2];
  check(close["option_id"].toInt() == 1 && close["action_menu_id"].toInt() == -1, "Closing option layout");
  check(rows["gossip_menu_option"][0]["action_menu_id"].toUInt() == 5000001, "Continue leads to the second menu");
  // Two simple conditions and their AND per gated response; the AND comes after its parts.
  check(conditions.size() == 6, "Conditions: two pairs and their two combinations");
  for (auto const& c : conditions) if (c["type"].toInt() == -1) check(c["value1"].toUInt() < c["condition_entry"].toUInt() && c["value2"].toUInt() < c["condition_entry"].toUInt(), "AND must follow its parts");
  auto scripts = rows["gossip_scripts"];
  check(scripts.size() == 3, "Three script steps");
  check(scripts[0]["command"].toInt() == 7 && scripts[0]["datalong"].toUInt() == 1000100, "Quest credit step");
  check(scripts[1]["command"].toInt() == 15 && scripts[1]["data_flags"].toInt() == 0, "Spell cast on the player");
  check(scripts[2]["command"].toInt() == 6 && scripts[2]["data_flags"].toInt() == 1 && scripts[2]["datalong"].toUInt() == 0, "Teleport moves the player");
  rows["conditions"] = conditions;
  Dialogue back; back.entry = d.entry;
  GossipService::parse(back, 5000000, rows);
  check(back.listQuests && back.dropped.isEmpty(), "Quest list read back, nothing dropped");
  check(back.nodes.size() == 2, "Two nodes read back");
  for (auto& node : back.nodes) node.menu = 0;
  check(back.nodes == d.nodes, "Read back differently than written");
  // A game dialogue with things the editor cannot show says so.
  GossipRows original;
  original["gossip_menu"] = {Fields{{"entry", 10}, {"text_id", 20}, {"script_id", 0}, {"condition_id", 0}}};
  original["npc_text"] = {Fields{{"ID", 20}, {"BroadcastTextID0", 30}, {"BroadcastTextID1", 31}}};
  original["broadcast_text"] = {Fields{{"entry", 30}, {"male_text", "Hello"}}, Fields{{"entry", 31}, {"male_text", "Hi"}}};
  original["gossip_menu_option"] = {Fields{{"menu_id", 10}, {"id", 0}, {"option_id", 4}, {"option_text", "Fly"}},
                                    Fields{{"menu_id", 10}, {"id", 1}, {"option_id", 1}, {"option_text", "Odd"}, {"action_menu_id", -1}, {"condition_id", 99}}};
  original["conditions"] = {Fields{{"condition_entry", 99}, {"type", 6}, {"value1", 469}, {"value2", 0}, {"value3", 0}, {"value4", 0}, {"flags", 0}}};
  Dialogue game; GossipService::parse(game, 10, original);
  check(game.nodes.size() == 1 && game.nodes[0].text == "Hello" && game.nodes[0].responses.size() == 1, "Game dialogue read");
  check(game.dropped.size() == 3, "Random greetings, a flight option and an unknown condition are reported");
  auto copy = GossipService::copied(game, d);
  check(copy.entry == d.entry && copy.nodes[0].menu == 0 && copy.nodes[0].text == "Hello", "A copy belongs to its new NPC");
}
void dialogueChecks() {
  auto d = malric();
  check(GossipService::check(d, malricFacts()).isEmpty(), "The sample dialogue is valid");
  auto broken = d;
  broken.nodes.push_back({});                                 // nothing leads here, no text
  broken.nodes[0].responses[0].target = 9;                    // a missing node
  broken.nodes[0].responses[1].effects = {DialogueEffect{}};  // effects on a shop response
  broken.nodes[1].responses[0].conditions[1] = {CKind::MaxLevel, 5};
  broken.nodes[1].responses[0].conditions.push_back({CKind::MinLevel, 20});
  broken.vendor = false; broken.listQuests = false;
  auto p = GossipService::check(broken, malricFacts());
  check(hasProblem(p, 2, -1, "No response leads here") && hasProblem(p, 2, -1, "Write what the NPC says"), "Orphan, empty node accepted");
  check(hasProblem(p, 0, 0, "Choose the node"), "Missing target accepted");
  check(hasProblem(p, 0, 1, "Effects only happen") && hasProblem(p, 0, 1, "not a vendor", false), "Shop response effects / vendor role");
  check(hasProblem(p, 1, 0, "No player meets both", false), "Impossible level range not warned");
  check(hasProblem(p, 0, -1, "does not list them", false), "Unlisted quests not warned");
  auto facts = malricFacts(); facts.completable.clear(); facts.spells.clear();
  auto q = GossipService::check(d, facts);
  check(hasProblem(q, 1, 0, "not completed by an event") && hasProblem(q, 1, 0, "Spell 1243 does not exist"), "Uncompletable quest / missing spell");
  auto noRule = d; noRule.nodes[1].responses[0].conditions.clear();
  check(hasProblem(GossipService::check(noRule, malricFacts()), 1, 0, "Only players on the quest", false), "Credit without the quest condition not warned");
  auto noShop = d; noShop.nodes[0].responses.removeAt(1);
  check(hasProblem(GossipService::check(noShop, malricFacts()), -1, -1, "no response opens its shop", false), "Unreachable shop not warned");
  // A greeting without responses gets the game's usual options: nothing to warn about.
  auto usual = d; usual.listQuests = false; usual.nodes = {DialogueNode{"Hello."}};
  check(GossipService::check(usual, malricFacts()).isEmpty(), "Usual options flagged");
}
QJsonArray json(QVector<Fields> const& rows) {
  QJsonArray array;
  for (auto const& row : rows) { QJsonObject o; for (auto it = row.begin(); it != row.end(); ++it) o[it.key()] = it.value().toString(); array.append(o); }
  return array;
}
void dialogueChanges() {
  auto d = malric();
  QHash<QString, Id> next{{"gossip_menu", 5000000}, {"npc_text", 5100000}, {"broadcast_text", 5200000}, {"gossip_scripts", 5300000}};
  QVector<Fields> conditions;
  auto written = GossipService::rows(d, [&](QString const& table, Id) { return next[table]++; }, [&](Fields const& row) -> Id {
    auto stored = row; stored["condition_entry"] = 7000000 + conditions.size(); conditions << stored; return stored["condition_entry"].toUInt();
  });
  TrackedChange c; c.type = EntityType::Gossip; c.entity = 1000001; c.label = "Brother Malric"; c.action = ChangeAction::Update;
  QJsonObject head{{"entry", "1000001"}, {"gossip_menu_id", "5000000"}, {"gossip", "1"}};
  c.after = {{"creature_gossip", QJsonArray{head}}, {"conditions", json(conditions)}};
  for (auto table : {"gossip_menu", "gossip_menu_option", "npc_text", "broadcast_text", "gossip_scripts"}) c.after[table] = json(written[table]);
  QJsonArray marks;
  for (auto [table, key, kind] : {std::array<char const*, 3>{"gossip_menu", "entry", "gossip_menu"}, {"npc_text", "ID", "npc_text"}, {"broadcast_text", "entry", "gossip_text"}})
    for (auto const& row : written[table]) marks.append(QJsonObject{{"kind", kind}, {"entry", row[key].toString()}});
  c.after["creator_marks"] = marks;
  // The previous version had a menu this one no longer uses.
  QJsonObject oldHead{{"entry", "1000001"}, {"gossip_menu_id", "4999999"}, {"gossip", "1"}};
  c.before = {{"creature_gossip", QJsonArray{oldHead}}, {"gossip_menu", QJsonArray{QJsonObject{{"entry", "4999999"}, {"text_id", "4999998"}}}},
              {"creator_marks", QJsonArray{QJsonObject{{"kind", "gossip_menu"}, {"entry", "4999999"}}}}};
  check(c.summary() == "~ Dialogue: Brother Malric · 2 nodes", "Dialogue summary");
  auto created = c; created.before = {};
  check(created.summary() == "+ Dialogue: Brother Malric · 2 nodes", "New dialogue summary");
  auto sql = ChangeTracker::sql({c});
  check(sql.contains("SET @owned := (SELECT COUNT(*) FROM creator_content WHERE kind='npc' AND entry=1000001)"), "Dialogue not guarded by its NPC");
  check(sql.contains("DELETE FROM gossip_menu_option WHERE menu_id IN (4999999,5000000,5000001) AND menu_id IN (SELECT entry FROM creator_content WHERE kind='gossip_menu') AND @owned>0;"),
        "Old and new menus' options are only removed where they were made in Noggit");
  check(sql.contains("DELETE FROM creator_content WHERE kind='gossip_menu' AND entry=4999999 AND @owned>0;"), "Unused mark removed");
  check(!sql.contains("DELETE FROM conditions"), "Shared conditions must never be removed");
  check(sql.contains("INSERT IGNORE INTO `conditions` (") && !sql.contains("REPLACE INTO `conditions`"), "Conditions only added when missing");
  check(sql.contains("REPLACE INTO `gossip_menu` (") && sql.contains("REPLACE INTO `gossip_scripts` ("), "Dialogue rows written");
  check(sql.contains("INSERT IGNORE INTO creator_content(kind,entry) SELECT 'gossip_menu',5000000 FROM DUAL WHERE @owned>0;"), "Marks written");
  check(sql.contains("UPDATE creature_template SET `gossip_menu_id`=5000000,`npc_flags`=(`npc_flags`&~1)|1 WHERE entry=1000001 AND @owned>0;"), "Greeting and gossip flag");
  for (auto const& statement : ChangeTracker::statements(sql))
    check(!statement.contains("creature_gossip") && !statement.contains("creator_marks"), "Synthetic rows written");
  // Removed with the NPC: the dialogue goes first, while the NPC's mark still guards it.
  auto gone = c; gone.action = ChangeAction::Delete; gone.before = c.after; gone.after = {};
  TrackedChange npc; npc.type = EntityType::Npc; npc.entity = 1000001; npc.action = ChangeAction::Delete; npc.label = "Brother Malric";
  npc.before = rows("creature_template", QJsonArray{QJsonObject{{"entry", "1000001"}, {"equipment_id", "0"}}});
  auto both = ChangeTracker::sql({npc, gone});
  check(both.indexOf("-- Remove dialogue") >= 0 && both.indexOf("-- Remove dialogue") < both.indexOf("-- Delete NPC"), "Dialogue removal must precede the NPC");
  check(gone.summary().startsWith("- Dialogue"), "Removed dialogue summary");
  // The sync backup covers every row the script can change.
  auto scopes = ChangeTracker::footprint({c}, [](QString const&) { return QVector<Fields>{}; });
  auto covers = [&](QString const& table, QString const& part) {
    for (auto const& [t, where] : scopes) if (t == table && where.contains(part)) return true;
    return false;
  };
  check(covers("gossip_menu", "4999999") && covers("gossip_menu", "5000001") && covers("gossip_menu_option", "menu_id"), "Menus backed up");
  check(covers("npc_text", "5100000") && covers("broadcast_text", "5200001") && covers("gossip_scripts", "5300000"), "Texts and scripts backed up");
  check(covers("conditions", "7000000") && covers("creator_content", "kind='gossip_menu'") && covers("creature_template", "1000001"), "Conditions, marks, NPC backed up");
}

// --- Client data, spells and items ---
QByteArray wdbc(int columns, QVector<QVector<quint32>> const& rows, QByteArray strings = QByteArray(1, '\0')) {
  QByteArray out("WDBC"); char b[4];
  auto put = [&](quint32 v) { qToLittleEndian(v, b); out.append(b, 4); };
  put(rows.size()); put(columns); put(columns * 4); put(strings.size());
  for (auto const& r : rows) for (auto v : r) put(v);
  return out + strings;
}
float asFloat(quint32 v) { float f; std::memcpy(&f, &v, 4); return f; }
void clientTables() {
  auto table = Wdbc::parse(wdbc(3, {{1, 10, 1}, {2, 20, 0}}, QByteArray("\0Old\0", 5)));
  check(table.rows() == 2 && table.text(0, 2) == "Old" && table.text(1, 2).isEmpty(), "WDBC read");
  table.put({3, 30, 0}, {{2, "New"}});
  table.put({1, 11, 0}, {{2, "Changed"}});
  auto again = Wdbc::parse(table.bytes());
  check(again.rows() == 3 && again.cell(again.find(3), 1) == 30 && again.text(again.find(3), 2) == "New", "WDBC append");
  check(again.cell(again.find(1), 1) == 11 && again.text(again.find(1), 2) == "Changed" && again.cell(again.find(2), 1) == 20, "WDBC replace");
  bool rejected = false; try { Wdbc::parse("WDBC\1\0\0\0"); } catch (std::exception const&) { rejected = true; }
  check(rejected, "Damaged WDBC accepted");
  // Spell.dbc columns of the 1.12 client, as verified against the bundled spell_template.
  Fields spell{{"entry", "1000001"}, {"castingTimeIndex", "14"}, {"effect1", "2"}, {"effectBasePoints1", "49"}, {"effectImplicitTargetA1", "6"},
               {"spellIconId", "185"}, {"name", "Holy Smite"}, {"nameSubtext", "Rank 2"}, {"description", "Deals $s1 damage."}, {"auraDescription", ""},
               {"dmgMultiplier1", "1"}, {"equippedItemClass", "-1"}, {"spellFamilyFlags", "4294967298"}, {"effectRealPointsPerLevel1", "0.5"}};
  QHash<int, QString> strings;
  auto record = SpellDbc::record(spell, strings);
  check(record.size() == 173 && record[0] == 1000001 && record[18] == 14 && record[61] == 2 && record[76] == 49 && record[82] == 6 && record[117] == 185, "Spell.dbc integer columns");
  check(record[58] == 0xFFFFFFFFu && asFloat(record[167]) == 1.0f && asFloat(record[73]) == 0.5f && record[161] == 2 && record[162] == 1, "Spell.dbc signed, float and 64-bit columns");
  check(strings.value(120) == "Holy Smite" && strings.value(127) == "Holy Smite" && strings.value(129) == "Rank 2" && strings.value(138) == "Deals $s1 damage.", "Spell.dbc texts in every locale");
  QVector<quint32> existing(173, 0); existing[0] = 61500;
  auto patched = Wdbc::parse(ClientPatchService::spellTable(wdbc(173, {existing}), {spell}));
  check(patched.rows() == 2 && patched.find(61500) == 0 && patched.text(patched.find(1000001), 120) == "Holy Smite", "Patched Spell.dbc keeps the client's rows");
  bool layout = false; try { ClientPatchService::spellTable(wdbc(3, {{1, 2, 3}}), {spell}); } catch (std::exception const&) { layout = true; }
  check(layout, "A Spell.dbc of another layout was patched");
  check(ClientLists::seconds(2500) == "2.5 sec" && ClientLists::seconds(90000) == "1 min 30 sec" && ClientLists::seconds(0) == "Instant", "Readable times");
  auto casts = ClientLists::castTimes(Wdbc::parse(wdbc(4, {{1, 0, 0, 0}, {16, 1500, 0, 1500}})));
  check(casts.size() == 2 && casts[0].label == "Instant" && casts[1].label == "1.5 sec cast", "Cast time list");
}
SpellDesign spellOf(SpellCatalog::Template t) {
  SpellDesign d; d.name = "Test"; d.editable = true; d.castTime = 1; d.range = 1;
  for (auto column : {"entry", "effect1", "effect2", "effect3"}) d.row[column] = "0";
  SpellCatalog::apply(t, d, 0); return d;
}
void spellRules() {
  // The server's roll: base + baseDice .. base + dieSides; 0 or 1 sides is exactly base + baseDice.
  Fields row{{"effect1", "2"}, {"effectBasePoints1", "49"}, {"effectBaseDice1", "1"}, {"effectDieSides1", "11"},
             {"effect2", "6"}, {"effectApplyAuraName2", "22"}, {"effectBasePoints2", "-201"}, {"effectBaseDice2", "1"}, {"effectDieSides2", "1"}};
  auto d = SpellService::fromRow(row);
  check(d.effects[0].minValue == 50 && d.effects[0].maxValue == 60 && d.effects[1].minValue == -200 && d.effects[1].maxValue == -200, "Effect values read like the server rolls");
  d.effects[0].minValue = 120; d.effects[0].maxValue = 140;
  auto back = SpellService::fromRow(SpellService::toRow(d));
  check(back.effects[0].minValue == 120 && back.effects[0].maxValue == 140 && back.effects[1].minValue == -200, "Effect values written back");
  // Every template gives a valid spell once its referenced spell or creature exists.
  SpellFacts facts; facts.spells = {133}; facts.creatures = {299};
  for (auto const& t : SpellCatalog::templates()) {
    auto s = spellOf(t.id);
    if (SpellCatalog::inputs(s.effects[0]).trigger) s.effects[0].trigger = 133;
    if (SpellCatalog::inputs(s.effects[0]).misc == SpellCatalog::Misc::Creature) s.effects[0].misc = 299;
    for (auto const& p : SpellService::check(s, facts)) check(!p.error, qPrintable("Template " + t.label + ": " + p.text));
    check(!SpellService::describe(s, 0, [](QString const&, Id) { return QString("X"); }).isEmpty(), "Template without a readable description");
  }
  auto summon = spellOf(SpellCatalog::Template::Summon);
  auto summonProblems = SpellService::check(summon, facts);
  check(std::any_of(summonProblems.begin(), summonProblems.end(), [](SpellProblem const& p) { return p.text.contains("creature"); }), "Summon without a creature accepted");
  auto has = [](QVector<SpellProblem> const& list, QString const& text) { return std::any_of(list.begin(), list.end(), [&](SpellProblem const& p) { return p.error && p.text.contains(text); }); };
  auto incompatible = spellOf(SpellCatalog::Template::Heal);
  incompatible.entry = 65536;
  check(has(SpellService::check(incompatible, facts), "Clone the spell"), "Out-of-range spell ID accepted");
  incompatible.entry = 65535;
  check(!has(SpellService::check(incompatible, facts), "Clone the spell"), "Largest compatible spell ID rejected");
  incompatible.entry = 0; incompatible.previous = 1000000;
  facts.spells.insert(incompatible.previous);
  check(has(SpellService::check(incompatible, facts), "Clone that rank"), "Incompatible previous rank accepted");
  auto dot = spellOf(SpellCatalog::Template::DoT); dot.duration = 0; dot.effects[0].period = 0;
  check(has(SpellService::check(dot, facts), "how long") && has(SpellService::check(dot, facts), "how often"), "Aura without duration / period accepted");
  auto area = spellOf(SpellCatalog::Template::DirectDamage); area.effects[0].targetA = 22; area.effects[0].targetB = 15;
  check(has(SpellService::check(area, facts), "radius"), "Area effect without radius accepted");
  SpellDesign none; none.name = "Nothing";
  check(has(SpellService::check(none, facts), "at least one effect"), "Spell without effects accepted");
  auto ranked = spellOf(SpellCatalog::Template::Heal); ranked.entry = 10; ranked.previous = 11;
  SpellFacts chain; chain.spells = {11}; chain.previousOf = {{11, 12}, {12, 10}};
  check(has(SpellService::check(ranked, chain), "circle"), "Rank cycle accepted");
  check(SpellService::suggestedDescription(spellOf(SpellCatalog::Template::DoT)).contains("$o1") && SpellCatalog::needsScripting(3, 0), "Description tokens / scripting marks");
}
void itemRules() {
  ItemFacts facts; facts.spells = {2023};
  for (auto const& t : ItemDesignService::templates()) {
    ItemDesign d; d.editable = true; ItemDesignService::apply(t.id, d); d.name = t.label; d.display = 1;
    if (t.id == ItemDesignService::Template::Consumable || t.id == ItemDesignService::Template::Trinket) d.spells[0].spell = 2023;
    for (auto const& p : ItemDesignService::check(d, facts)) check(!p.error, qPrintable("Item template " + t.label + ": " + p.text));
  }
  ItemDesign sword; ItemDesignService::apply(ItemDesignService::Template::Weapon, sword); sword.name = "Sword"; sword.display = 1;
  auto has = [](QVector<ItemProblem> const& list, QString const& text) { return std::any_of(list.begin(), list.end(), [&](ItemProblem const& p) { return p.error && p.text.contains(text); }); };
  auto slow = sword; slow.delay = 0; check(has(ItemDesignService::check(slow, facts), "speed"), "Weapon without speed accepted");
  auto worn = sword; worn.inventoryType = 5; check(has(ItemDesignService::check(worn, facts), "cannot be worn"), "Sword on the chest accepted");
  auto stacked = sword; stacked.stackable = 5; check(has(ItemDesignService::check(stacked, facts), "do not stack"), "Stacking weapon accepted");
  auto missing = sword; missing.spells[0].spell = 999; check(has(ItemDesignService::check(missing, facts), "does not exist"), "Missing item spell accepted");
  auto nobody = sword; nobody.classes = 0; check(has(ItemDesignService::check(nobody, facts), "No class"), "Item nobody can use accepted");
  Fields row{{"entry", "5"}, {"name", "Old"}, {"stat_type1", "7"}, {"stat_value1", "5"}, {"spellid_1", "2023"}, {"spelltrigger_1", "1"}, {"script_name", "keep"}};
  auto d = ItemDesignService::fromRow(row); d.name = "New";
  auto out = ItemDesignService::toRow(d);
  check(out["name"] == "New" && out["stat_type1"] == "7" && out["stat_value1"] == "5" && out["spelltrigger_1"] == "1" && out["script_name"] == "keep", "Item row round trip keeps what the editor does not show");
  check(!ItemDesignService::slotsFor(2, 1).isEmpty() && ItemDesignService::slotsFor(2, 1)[0].first == 17, "Two-handed axes are two-handed");
}
void spellChanges() {
  TrackedChange c; c.type = EntityType::Spell; c.entity = 1000001; c.label = "Holy Smite (Rank 2)"; c.action = ChangeAction::Create;
  c.after = {{"spell_template", QJsonArray{QJsonObject{{"entry", "1000001"}, {"name", "Holy Smite"}, {"effect1", "2"}}}},
             {"spell_chain", QJsonArray{QJsonObject{{"spell_id", "1000001"}, {"prev_spell", "585"}, {"first_spell", "585"}, {"rank", "2"}, {"req_spell", "0"}}}}};
  check(c.summary() == "+ Spell: Holy Smite (Rank 2)", "Spell summary");
  auto sql = ChangeTracker::sql({c});
  check(sql.contains("REPLACE INTO `spell_template` (") && sql.contains("DELETE FROM spell_chain WHERE spell_id=1000001;") && sql.contains("REPLACE INTO `spell_chain` (")
        && sql.contains("INSERT IGNORE INTO creator_content(kind,entry) VALUES('spell',1000001);"), "Spell upsert");
  auto gone = c; gone.action = ChangeAction::Delete; gone.before = c.after; gone.after = {};
  auto removal = ChangeTracker::sql({gone});
  check(removal.contains("kind='spell' AND entry=1000001)") && removal.contains("DELETE FROM spell_template WHERE entry=1000001 AND @owned>0;"), "Spell removal guarded");
  auto scopes = ChangeTracker::footprint({c}, [](QString const&) { return QVector<Fields>{}; });
  bool template_ = false, chain = false;
  for (auto const& [t, where] : scopes) { template_ = template_ || (t == "spell_template" && where.contains("1000001")); chain = chain || (t == "spell_chain" && where.contains("1000001")); }
  check(template_ && chain, "Spell rows not in the sync backup");
}
}
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    lootValidation(); lootChances(); lootSimulation(); vendorChecks(); trainerChecks(); serviceChanges();
    dialogueRoundTrip(); dialogueChecks(); dialogueChanges();
    clientTables(); spellRules(); itemRules(); spellChanges();
    QTextStream(stdout) << "Loot, vendor, trainer, dialogue, client data, spell and item tests passed\n";
    return 0;
  } catch (std::exception const& e) { QTextStream(stderr) << e.what() << Qt::endl; return 1; }
}
