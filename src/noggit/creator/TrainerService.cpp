#include "TrainerService.hpp"
#include "Database.hpp"
#include "ItemService.hpp"
#include <QSet>
#include <algorithm>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
QString n(qint64 value) { return QString::number(value); }
void require(bool condition, QString const& message) { if (!condition) throw std::runtime_error(message.toStdString()); }
QVector<TrainerSpell> spellsOf(QVector<Fields> const& rows) {
  QVector<TrainerSpell> out;
  for (auto const& r : rows) out.push_back({r["spell"].toUInt(), r["spellcost"].toLongLong(), r["reqlevel"].toInt(), r["reqskill"].toInt(), r["reqskillvalue"].toInt()});
  return out;
}
QString idList(QVector<Id> ids) {
  QStringList out; for (auto id : ids) if (id) out << n(id);
  out.removeDuplicates(); return out.join(',');
}
}
QVector<QPair<int, QString>> TrainerService::classes() {
  return {{1, "Warrior"}, {2, "Paladin"}, {3, "Hunter"}, {4, "Rogue"}, {5, "Priest"}, {7, "Shaman"}, {8, "Mage"}, {9, "Warlock"}, {11, "Druid"}};
}
QString TrainerService::className(int c) {
  for (auto const& [id, name] : classes()) if (id == c) return name;
  return "Class " + n(c);
}
QVector<Choice> TrainerService::owners(QString const& text) {
  Database db; QVector<Choice> out;
  for (auto const& r : db.query("SELECT entry,name,subname FROM creature_template WHERE (entry IN (SELECT entry FROM npc_trainer) OR trainer_id>0) AND name LIKE "
                                + db.quote('%' + text + '%') + " ORDER BY name LIMIT 250"))
    out.push_back({r["entry"].toUInt(), r["name"].toString(), r["subname"].toString()});
  return out;
}
Trainer TrainerService::load(Id entry) {
  Database db;
  auto rows = db.query("SELECT name,display_id1,npc_flags,trainer_id,trainer_type,trainer_class FROM creature_template WHERE entry=" + n(entry));
  require(!rows.isEmpty(), "This NPC no longer exists.");
  auto const& r = rows[0];
  Trainer t; t.entry = entry; t.name = r["name"].toString(); t.display = r["display_id1"].toUInt();
  t.teaches = r["npc_flags"].toUInt() & 16; t.type = r["trainer_type"].toInt(); t.playerClass = r["trainer_class"].toInt();
  t.sharedList = r["trainer_id"].toUInt(); t.editable = db.owned("npc", entry);
  if (t.type == 0 && !t.playerClass && !t.teaches) t.type = 2; // never a trainer yet: open to everyone by default
  t.spells = spellsOf(db.query("SELECT * FROM npc_trainer WHERE entry=" + n(entry) + " ORDER BY reqlevel,spell"));
  if (t.sharedList) t.shared = spellsOf(db.query("SELECT * FROM npc_trainer_template WHERE entry=" + n(t.sharedList) + " ORDER BY reqlevel,spell"));
  if (!t.editable) t.notice = "This is an original NPC: what it teaches is shown read-only. Copy its spells to one of your own NPCs to change them.";
  else if (t.sharedList) t.notice = "Part of this trainer's list is shared with original trainers. Make it editable to change those spells too.";
  return t;
}
QHash<Id, SpellInfo> TrainerService::spells(QVector<Id> const& teaching) {
  QHash<Id, SpellInfo> out;
  auto ids = idList(teaching);
  if (ids.isEmpty()) return out;
  Database db;
  QVector<Id> previous;
  for (auto const& r : db.query(
         "SELECT t.entry AS teach,s.entry AS learned,s.name,s.nameSubtext,s.description,s.effectBasePoints1,s.effectBasePoints2,s.effectBasePoints3,"
         "s.effectDieSides1,s.effectDieSides2,s.effectDieSides3,s.spellIconId,s.spellLevel,s.baseLevel,s.school,s.manaCost,s.powerType,c.prev_spell "
         "FROM spell_template t JOIN spell_template s ON s.entry=t.effectTriggerSpell1 LEFT JOIN spell_chain c ON c.spell_id=s.entry "
         "WHERE t.effect1=36 AND t.entry IN (" + ids + ")")) {
    SpellInfo s;
    s.teach = r["teach"].toUInt(); s.learned = r["learned"].toUInt(); s.name = r["name"].toString(); s.rank = r["nameSubtext"].toString();
    s.description = describeSpell(r); s.icon = r["spellIconId"].toInt(); s.school = r["school"].toInt();
    s.level = std::max(r["spellLevel"].toInt(), r["baseLevel"].toInt()); s.manaCost = r["manaCost"].toInt(); s.powerType = r["powerType"].toInt();
    s.previous = r["prev_spell"].toUInt();
    previous << s.previous;
    out[s.teach] = s;
  }
  QHash<Id, QString> names;
  if (auto list = idList(previous); !list.isEmpty())
    for (auto const& r : db.query("SELECT entry,name,nameSubtext FROM spell_template WHERE entry IN (" + list + ")"))
      names[r["entry"].toUInt()] = (r["name"].toString() + " " + r["nameSubtext"].toString()).trimmed();
  for (auto const& r : db.query("SELECT spell,MIN(spellcost) AS cost,MIN(reqlevel) AS level FROM (SELECT spell,spellcost,reqlevel FROM npc_trainer_template "
                                "UNION ALL SELECT spell,spellcost,reqlevel FROM npc_trainer) x WHERE spell IN (" + ids + ") GROUP BY spell"))
    if (out.contains(r["spell"].toUInt())) { auto& s = out[r["spell"].toUInt()]; s.suggestedCost = r["cost"].toLongLong(); s.suggestedLevel = r["level"].toInt(); }
  for (auto& s : out) {
    s.previousName = names.value(s.previous);
    if (!s.suggestedLevel) s.suggestedLevel = s.level;
  }
  return out;
}
QVector<SpellInfo> TrainerService::search(QString const& text, int limit) {
  QVector<Id> teaching;
  {
  // Closed before spells() opens its own connection: two at once wait on the authoring lock.
  Database db;
  bool numeric = false; auto number = text.trimmed().toUInt(&numeric);
  QString match = numeric ? "(s.entry=" + n(number) + " OR t.entry=" + n(number) + ")" : "s.name LIKE " + db.quote('%' + text.trimmed() + '%');
  // Several teaching spells can teach the same spell; prefer the one original trainers use.
  QHash<Id, QPair<Id, bool>> best;
  for (auto const& r : db.query("SELECT t.entry AS teach,t.effectTriggerSpell1 AS learned,(EXISTS(SELECT 1 FROM npc_trainer_template x WHERE x.spell=t.entry) "
                                "OR EXISTS(SELECT 1 FROM npc_trainer y WHERE y.spell=t.entry)) AS used FROM spell_template t JOIN spell_template s "
                                "ON s.entry=t.effectTriggerSpell1 WHERE t.effect1=36 AND " + match + " LIMIT 3000")) {
    auto learned = r["learned"].toUInt(); auto teach = r["teach"].toUInt(); bool used = r["used"].toInt();
    if (!best.contains(learned) || (used && !best[learned].second) || (used == best[learned].second && teach < best[learned].first)) best[learned] = {teach, used};
  }
  for (auto const& choice : best) teaching << choice.first;
  }
  auto infos = spells(teaching);
  QVector<SpellInfo> out(infos.begin(), infos.end());
  std::sort(out.begin(), out.end(), [](SpellInfo const& a, SpellInfo const& b) { return a.name != b.name ? a.name < b.name : a.level < b.level; });
  if (out.size() > limit) out.resize(limit);
  return out;
}
QVector<RowProblem> TrainerService::validate(Trainer const& t) {
  QVector<Id> ids; for (auto const& s : t.spells) ids << s.spell; for (auto const& s : t.shared) ids << s.spell;
  return check(t, spells(ids));
}
QVector<RowProblem> TrainerService::check(Trainer const& t, QHash<Id, SpellInfo> const& spells) {
  QVector<RowProblem> problems;
  auto add = [&](int row, QString const& text, bool error = true) { problems.push_back({row, text, error}); };
  if (t.type == 0 && !t.playerClass) add(-1, "Choose the class this trainer teaches, or let everyone train here.");
  if (!t.teaches && !t.spells.isEmpty()) add(-1, "Players cannot open this trainer until the NPC is a trainer. Turn on \"Trains players\".", false);
  QSet<Id> taught;
  for (auto const& s : t.spells) if (spells.contains(s.spell)) taught.insert(spells[s.spell].learned);
  for (auto const& s : t.shared) if (spells.contains(s.spell)) taught.insert(spells[s.spell].learned);
  QSet<Id> seen;
  for (int r = 0; r < t.spells.size(); ++r) {
    auto const& s = t.spells[r];
    if (!s.spell) { add(r, "Choose a spell."); continue; }
    if (!spells.contains(s.spell)) { add(r, "Trainers cannot teach this spell: it is not a learnable spell."); continue; }
    auto const& info = spells[s.spell];
    if (seen.contains(s.spell)) add(r, "This spell is already taught here.");
    seen.insert(s.spell);
    if (s.level < 0 || s.level > 60) add(r, "The required level must be 1 to 60 (0 for none).");
    else if (info.level > 1 && s.level && s.level < info.level) add(r, QString("Players could learn this before they can use it (usable from level %1).").arg(info.level), false);
    if (s.cost < 0 || s.cost > 2147483647) add(r, "The cost must be between 0 and 214,748g.");
    if (info.previous && !taught.contains(info.previous))
      add(r, "Players need " + (info.previousName.isEmpty() ? QString("the previous rank") : info.previousName) + " first, which is not taught here.", false);
  }
  return problems;
}
void TrainerService::save(Trainer const& t) {
  for (auto const& p : validate(t)) require(!p.error, p.text);
  Database db;
  require(db.owned("npc", t.entry), "Original NPCs keep what they teach. Copy their spells to one of your own NPCs to change them.");
  auto id = n(t.entry);
  db.track(EntityType::Trainer, t.entry);
  db.snapshotWhere("npc_trainer", "entry=" + id);
  db.exec("DELETE FROM npc_trainer WHERE entry=" + id);
  for (auto const& s : t.spells)
    db.insert("npc_trainer", {{"entry", t.entry}, {"spell", s.spell}, {"spellcost", s.cost}, {"reqskill", s.skill}, {"reqskillvalue", s.skillValue}, {"reqlevel", s.level}});
  db.snapshot("creature_template", "entry", t.entry);
  db.exec("UPDATE creature_template SET trainer_id=" + n(t.sharedList) + ",trainer_type=" + n(t.type) + ",trainer_class=" + n(t.type == 0 ? t.playerClass : 0)
          + ",npc_flags=(npc_flags&~16)|" + n(t.teaches ? 16 : 0) + " WHERE entry=" + id);
  db.commit();
}
QVector<LocalCharacter> TrainerService::characters() {
  Database db; QVector<LocalCharacter> out;
  for (auto const& r : db.query("SELECT guid,name,level,class,race FROM characters.characters WHERE name<>'' ORDER BY name"))
    out.push_back({r["guid"].toUInt(), r["name"].toString(), r["level"].toInt(), r["class"].toInt(), r["race"].toInt()});
  return out;
}
QSet<Id> TrainerService::knownSpells(Id character) {
  Database db; QSet<Id> out;
  for (auto const& r : db.query("SELECT spell FROM characters.character_spell WHERE disabled=0 AND guid=" + n(character))) out.insert(r["spell"].toUInt());
  return out;
}
}
