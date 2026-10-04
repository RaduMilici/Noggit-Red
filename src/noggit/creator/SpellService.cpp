#include "SpellService.hpp"
#include "ClientPatch.hpp"
#include "Database.hpp"
#include <QRegularExpression>
#include <cmath>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
QString n(qint64 value) { return QString::number(value); }
void require(bool condition, QString const& message) { if (!condition) throw std::runtime_error(message.toStdString()); }
constexpr int learnEffect = 36;
constexpr Id teachTemplate = 483; // Fireball (Rank 2)'s teaching spell: how trainers teach a class spell
// Spell IDs have a tighter limit than other Creator content. Ignore old, incompatible
// IDs above the wire limit when finding the next ID; never overwrite an existing spell.
Id allocateSpell(Database& db) {
  QSet<Id> occupied;
  for (auto const& row : db.query("SELECT entry FROM spell_template WHERE entry BETWEEN 1 AND " + n(SpellService::idLimit)))
    occupied.insert(row["entry"].toUInt());
  return SpellService::nextFreeId(occupied);
}
QString owned(QString const& column) { return column + " IN (SELECT entry FROM creator_content WHERE kind='spell')"; }
Fields row(Database& db, Id entry) {
  auto rows = db.query("SELECT * FROM spell_template WHERE entry=" + n(entry));
  require(!rows.isEmpty(), QString("Spell %1 does not exist.").arg(entry));
  return rows[0];
}
// The Creator teaching spell trainers use for `entry` (0 if none).
Id companion(Database& db, Id entry) {
  auto rows = db.query("SELECT entry FROM spell_template WHERE effect1=" + n(learnEffect) + " AND effectTriggerSpell1=" + n(entry) + " AND " + owned("entry") + " LIMIT 1");
  return rows.isEmpty() ? 0 : rows[0]["entry"].toUInt();
}
void write(Database& db, Fields const& spell) {
  auto entry = spell.value("entry").toUInt();
  db.track(EntityType::Spell, entry);
  db.snapshot("spell_template", "entry", entry);
  db.exec("DELETE FROM spell_template WHERE entry=" + n(entry));
  db.insert("spell_template", spell);
  db.snapshotWhere("creator_content", "kind='spell' AND entry=" + n(entry));
  db.mark("spell", entry);
}
void unwrite(Database& db, Id entry) {
  db.track(EntityType::Spell, entry);
  db.snapshot("spell_template", "entry", entry);
  db.snapshotWhere("spell_chain", "spell_id=" + n(entry));
  db.exec("DELETE FROM spell_chain WHERE spell_id=" + n(entry));
  db.exec("DELETE FROM spell_template WHERE entry=" + n(entry));
  db.snapshotWhere("creator_content", "kind='spell' AND entry=" + n(entry));
  db.exec("DELETE FROM creator_content WHERE kind='spell' AND entry=" + n(entry));
}
struct Link { Id first = 0; int rank = 1; };
Link linkOf(Database& db, Id spell) {
  auto rows = db.query("SELECT first_spell,`rank` FROM spell_chain WHERE spell_id=" + n(spell));
  return rows.isEmpty() ? Link{spell, 1} : Link{rows[0]["first_spell"].toUInt(), std::max(1, rows[0]["rank"].toInt())};
}
// Sets `spell`'s chain row (the server checks: a first rank has rank 1 and no previous one), then its later ranks'.
void relink(Database& db, Id spell, Id previous, int depth = 0) {
  require(depth < 64, "The rank chain is too long or goes in a circle.");
  bool later = !db.query("SELECT spell_id FROM spell_chain WHERE prev_spell=" + n(spell) + " LIMIT 1").isEmpty();
  db.track(EntityType::Spell, spell);
  db.snapshotWhere("spell_chain", "spell_id=" + n(spell));
  db.exec("DELETE FROM spell_chain WHERE spell_id=" + n(spell));
  if (previous) {
    // A game spell starting the chain keeps its data: a chain needs no row for its first rank.
    auto link = linkOf(db, previous);
    if (db.owned("spell", previous) && db.query("SELECT spell_id FROM spell_chain WHERE spell_id=" + n(previous)).isEmpty()) {
      db.track(EntityType::Spell, previous);
      db.snapshotWhere("spell_chain", "spell_id=" + n(previous));
      db.insert("spell_chain", {{"spell_id", previous}, {"prev_spell", 0}, {"first_spell", previous}, {"rank", 1}, {"req_spell", 0}});
    }
    db.insert("spell_chain", {{"spell_id", spell}, {"prev_spell", previous}, {"first_spell", link.first}, {"rank", link.rank + 1}, {"req_spell", 0}});
  } else if (later) db.insert("spell_chain", {{"spell_id", spell}, {"prev_spell", 0}, {"first_spell", spell}, {"rank", 1}, {"req_spell", 0}});
  for (auto const& r : db.query("SELECT spell_id FROM spell_chain WHERE prev_spell=" + n(spell)))
    if (r["spell_id"].toUInt() != spell && db.owned("spell", r["spell_id"].toUInt())) relink(db, r["spell_id"].toUInt(), spell, depth + 1);
}
QSet<int> ids(std::optional<Wdbc> const& table) { QSet<int> out; if (table) for (int r = 0; r < table->rows(); ++r) out.insert(int(table->cell(r, 0))); return out; }
}
Id SpellService::nextFreeId(QSet<Id> const& occupied) {
  Id highest = 0;
  for (auto id : occupied) if (id <= idLimit) highest = std::max(highest, id);
  if (highest < idLimit) return highest + 1;
  // Reaching the wire limit does not mean lower, unused IDs are exhausted.
  for (Id id = idLimit; id > 0; --id) if (!occupied.contains(id)) return id;
  throw std::runtime_error("No spell IDs remain in the 1.12 client's supported range (1–65535).");
}
QVector<Choice> SpellService::search(QString const& text, bool own, int limit) {
  Database db; QVector<Choice> out;
  QString match = "name LIKE " + db.quote('%' + text + '%');
  bool number = false; auto id = text.trimmed().toUInt(&number);
  if (number) match = "(" + match + " OR entry=" + n(id) + ")";
  for (auto const& r : db.query("SELECT entry,name,nameSubtext,school," + owned("entry") + " AS own FROM spell_template WHERE " + match
                                + (own ? " AND " + owned("entry") : QString()) + " AND name<>'' ORDER BY own DESC,name,spellLevel LIMIT " + n(limit))) {
    auto rank = r["nameSubtext"].toString();
    out.push_back({r["entry"].toUInt(), r["name"].toString() + (rank.isEmpty() ? QString() : " (" + rank + ")"),
                   SpellCatalog::schoolName(r["school"].toInt()) + (r["own"].toInt() ? " · yours" : QString())});
  }
  return out;
}
QVector<SpellListEntry> SpellService::browse(QString const& text, bool own, int limit) {
  Database db; QVector<SpellListEntry> out;
  QString match = "name LIKE " + db.quote('%' + text + '%');
  bool number = false; auto id = text.trimmed().toUInt(&number);
  if (number) match = "(" + match + " OR entry=" + n(id) + ")";
  for (auto const& r : db.query("SELECT entry,name,nameSubtext,school,spellIconId,spellLevel," + owned("entry") + " AS own FROM spell_template WHERE " + match
                                + (own ? " AND " + owned("entry") : QString()) + " AND name<>'' ORDER BY own DESC,name,spellLevel,entry LIMIT " + n(limit)))
    out.push_back({r["entry"].toUInt(), r["spellIconId"].toUInt(), r["name"].toString(), r["nameSubtext"].toString(), r["school"].toInt(), r["spellLevel"].toInt(), r["own"].toInt() != 0});
  return out;
}
QVector<Choice> SpellService::quests(QString const& text) {
  Database db; QVector<Choice> out;
  for (auto const& r : db.query("SELECT entry,Title,QuestLevel FROM quest_template WHERE Title LIKE " + db.quote('%' + text + '%') + " ORDER BY Title LIMIT 250"))
    out.push_back({r["entry"].toUInt(), r["Title"].toString(), "Level " + r["QuestLevel"].toString()});
  return out;
}
SpellDesign SpellService::load(Id entry) {
  Database db;
  auto d = fromRow(row(db, entry));
  auto link = db.query("SELECT prev_spell FROM spell_chain WHERE spell_id=" + n(entry));
  d.previous = link.isEmpty() ? 0 : link[0]["prev_spell"].toUInt();
  d.editable = db.owned("spell", entry);
  auto teach = db.query("SELECT entry FROM spell_template WHERE effect1=" + n(learnEffect) + " AND effectTriggerSpell1=" + n(entry) + " ORDER BY " + owned("entry") + " DESC LIMIT 1");
  d.teach = teach.isEmpty() ? 0 : teach[0]["entry"].toUInt();
  d.teachable = d.teach != 0;
  if (!d.editable) d.notice = "This is a game spell: it is shown read-only. Clone it, or create its next rank, to make your own.";
  return d;
}
SpellDesign SpellService::blank() {
  Database db;
  Fields defaults;
  for (auto const& c : db.query("SHOW COLUMNS FROM spell_template")) defaults[c["Field"].toString()] = c["Default"].isNull() ? QString() : c["Default"].toString();
  // Locale flags and the global cooldown category as a plain player spell has them.
  auto sample = db.query("SELECT nameFlags,nameSubtextFlags,descriptionFlags,auraDescriptionFlags,startRecoveryCategory FROM spell_template WHERE entry=133");
  if (!sample.isEmpty()) for (auto it = sample[0].begin(); it != sample[0].end(); ++it) defaults[it.key()] = it.value();
  defaults["equippedItemClass"] = "-1";
  for (int j = 1; j <= 3; ++j) defaults["dmgMultiplier" + n(j)] = "1";
  auto d = fromRow(defaults);
  d.entry = 0; d.name = "New Spell"; d.castTime = 1; d.range = 1; d.icon = 1; d.procChance = 101; d.globalCooldown = 1500; d.level = 1;
  d.editable = true;
  return d;
}
SpellDesign SpellService::clone(Id source) {
  auto d = load(source);
  d.source = source; d.entry = 0; d.row["entry"] = "0"; d.previous = 0; d.teachable = false; d.teach = 0;
  d.editable = true; d.notice.clear();
  d.row["script_name"] = ""; // server scripts belong to the original's identity
  return d;
}
SpellDesign SpellService::nextRank(Id entry, int level, double scale, int cost, int cooldown) {
  auto base = load(entry);
  auto d = clone(entry);
  d.previous = entry; d.teachable = base.teachable;
  auto ranks = chain(entry);
  int at = 0; for (int i = 0; i < ranks.size(); ++i) if (ranks[i].entry == entry) at = ranks[i].rank;
  auto number = QRegularExpression("(\\d+)").match(base.rank);
  d.rank = QString("Rank %1").arg(number.hasMatch() ? number.captured(1).toInt() + 1 : std::max(at, 1) + 1);
  d.level = level; d.cost = cost; d.cooldown = cooldown;
  for (auto& e : d.effects)
    if (SpellCatalog::inputs(e).value && e.type != 28 && e.type != 41 && e.type != 24) {
      e.minValue = int(std::lround(e.minValue * scale)); e.maxValue = int(std::lround(e.maxValue * scale));
    }
  return d;
}
QVector<SpellRank> SpellService::chain(Id entry) {
  Database db; QVector<SpellRank> out;
  auto first = linkOf(db, entry).first;
  auto rows = db.query("SELECT c.spell_id AS entry,c.`rank`,s.spellLevel,s.name,s.nameSubtext," + owned("c.spell_id") + " AS own FROM spell_chain c JOIN spell_template s ON s.entry=c.spell_id WHERE c.first_spell="
                       + n(first) + " ORDER BY c.`rank`,c.spell_id");
  bool hasFirst = false;
  for (auto const& r : rows) {
    out.push_back({r["entry"].toUInt(), r["rank"].toInt(), r["spellLevel"].toInt(), r["name"].toString(), r["nameSubtext"].toString(), r["own"].toInt() != 0});
    hasFirst = hasFirst || out.back().entry == first;
  }
  if (!hasFirst) {
    auto s = db.query("SELECT entry,spellLevel,name,nameSubtext FROM spell_template WHERE entry=" + n(first));
    if (!s.isEmpty()) out.prepend({first, 1, s[0]["spellLevel"].toInt(), s[0]["name"].toString(), s[0]["nameSubtext"].toString(), db.owned("spell", first)});
  }
  return out;
}
SpellFacts SpellService::facts(SpellDesign const& d) {
  SpellFacts f;
  QSet<Id> spells, creatures, items;
  if (d.previous) spells.insert(d.previous);
  for (auto const& e : d.effects) {
    if (e.trigger) spells.insert(e.trigger);
    auto misc = SpellCatalog::inputs(e).misc;
    if (misc == SpellCatalog::Misc::Creature && e.misc > 0) creatures.insert(Id(e.misc));
    if (misc == SpellCatalog::Misc::Item && e.misc > 0) items.insert(Id(e.misc));
  }
  auto list = [](QSet<Id> const& set) { QStringList out; for (auto id : set) out << n(id); return out.join(','); };
  Database db;
  if (!spells.isEmpty()) for (auto const& r : db.query("SELECT entry FROM spell_template WHERE entry IN (" + list(spells) + ")")) f.spells.insert(r["entry"].toUInt());
  if (!creatures.isEmpty()) for (auto const& r : db.query("SELECT entry FROM creature_template WHERE entry IN (" + list(creatures) + ")")) f.creatures.insert(r["entry"].toUInt());
  if (!items.isEmpty()) for (auto const& r : db.query("SELECT entry FROM item_template WHERE entry IN (" + list(items) + ")")) f.items.insert(r["entry"].toUInt());
  for (Id at = d.previous, steps = 0; at && steps < 64; ++steps) {
    auto r = db.query("SELECT prev_spell FROM spell_chain WHERE spell_id=" + n(at));
    Id previous = r.isEmpty() ? 0 : r[0]["prev_spell"].toUInt();
    if (previous) f.previousOf[at] = previous;
    at = previous;
  }
  if (auto* client = ClientPatchService::instance()) {
    f.castTimes = ids(client->table("SpellCastTimes.dbc")); f.durations = ids(client->table("SpellDuration.dbc"));
    f.ranges = ids(client->table("SpellRange.dbc")); f.radii = ids(client->table("SpellRadius.dbc"));
  }
  return f;
}
QVector<SpellProblem> SpellService::validate(SpellDesign const& d) { return check(d, facts(d)); }
QStringList SpellService::uses(Id entry) {
  Database db; QStringList out;
  auto id = n(entry);
  auto teach = companion(db, entry);
  QString spells = id + (teach ? "," + n(teach) : QString());
  for (auto const& r : db.query("SELECT name FROM item_template WHERE " + id + " IN (spellid_1,spellid_2,spellid_3,spellid_4,spellid_5) LIMIT 5")) out << "Item: " + r["name"].toString();
  for (auto const& r : db.query("SELECT DISTINCT t.name FROM npc_trainer n JOIN creature_template t ON t.entry=n.entry WHERE n.spell IN (" + spells + ") LIMIT 5")) out << "Trainer: " + r["name"].toString();
  for (auto const& r : db.query("SELECT Title FROM quest_template WHERE RewSpell IN (" + spells + ") OR RewSpellCast=" + id + " LIMIT 5")) out << "Quest reward: " + r["Title"].toString();
  for (auto const& r : db.query("SELECT name FROM creature_template WHERE " + id + " IN (spell_id1,spell_id2,spell_id3,spell_id4) LIMIT 5")) out << "NPC: " + r["name"].toString();
  for (auto const& r : db.query("SELECT name,nameSubtext FROM spell_template WHERE entry<>" + n(teach) + " AND entry<>" + id + " AND " + id + " IN (effectTriggerSpell1,effectTriggerSpell2,effectTriggerSpell3) LIMIT 5"))
    out << "Spell: " + r["name"].toString() + " " + r["nameSubtext"].toString();
  for (auto const& r : db.query("SELECT s.name,s.nameSubtext FROM spell_chain c JOIN spell_template s ON s.entry=c.spell_id WHERE c.prev_spell=" + id + " LIMIT 5"))
    out << "Next rank: " + r["name"].toString() + " " + r["nameSubtext"].toString();
  return out;
}
Id SpellService::save(SpellDesign const& d) {
  for (auto const& p : validate(d)) require(!p.error, p.text);
  Database db;
  Id entry = d.entry;
  if (entry) require(db.owned("spell", entry), "Game spells are read-only here. Clone it, or create its next rank, to make your own.");
  else entry = allocateSpell(db);
  auto spell = toRow(d); spell["entry"] = n(entry);
  write(db, spell);
  relink(db, entry, d.previous);
  // The teaching spell trainers list: a copy of how the game's trainers teach, pointing at this spell.
  auto teach = companion(db, entry);
  if (d.teachable) {
    Fields lesson;
    if (teach) lesson = row(db, teach);
    else {
      auto templates = db.query("SELECT * FROM spell_template WHERE entry=" + n(teachTemplate) + " AND effect1=" + n(learnEffect));
      if (templates.isEmpty()) templates = db.query("SELECT * FROM spell_template WHERE effect1=" + n(learnEffect) + " AND effectImplicitTargetA1=1 AND attributes=256 LIMIT 1");
      require(!templates.isEmpty(), "No teaching spell to copy was found, so trainers cannot teach this spell.");
      lesson = templates[0];
      lesson["entry"] = n(allocateSpell(db));
      lesson["script_name"] = "";
    }
    lesson["name"] = spell["name"]; lesson["nameSubtext"] = spell["nameSubtext"];
    lesson["effectTriggerSpell1"] = n(entry);
    lesson["description"] = "Teaches " + d.name.trimmed() + (d.rank.trimmed().isEmpty() ? QString() : " (" + d.rank.trimmed() + ")") + ".";
    write(db, lesson);
  } else if (teach) {
    auto trainers = db.query("SELECT entry FROM npc_trainer WHERE spell=" + n(teach) + " UNION SELECT entry FROM npc_trainer_template WHERE spell=" + n(teach) + " LIMIT 1");
    require(trainers.isEmpty(), "A trainer still teaches this spell. Remove it from that trainer before turning off \"Trainers can teach it\".");
    unwrite(db, teach);
  }
  db.commit();
  return entry;
}
void SpellService::remove(Id entry) {
  auto used = uses(entry);
  require(used.isEmpty(), "This spell is still used:\n" + used.join('\n') + "\nRemove those uses first.");
  Database db;
  require(db.owned("spell", entry), "Only spells made in Noggit can be deleted.");
  if (auto teach = companion(db, entry)) unwrite(db, teach);
  unwrite(db, entry);
  db.commit();
}
}
