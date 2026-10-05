#include "TalentService.hpp"
#include <noggit/runtime/RuntimeManager.hpp>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
void require(bool condition, QString const& message) { if (!condition) throw std::runtime_error(message.toStdString()); }
QString place(Talent const& t) { return QString("row %1, column %2").arg(t.row + 1).arg(t.column + 1); }
QString hashOf(QByteArray const& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }
QString fileHash(QString const& path) {
  QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
  return hashOf(file.readAll());
}
QByteArray readFile(QString const& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Cannot read " + QDir::toNativeSeparators(path) + ".");
  return file.readAll();
}
void writeFile(QString const& path, QByteArray const& bytes) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
          "Cannot write " + QDir::toNativeSeparators(path) + ". Stop the local server and try again.");
}
QJsonObject readJson(QString const& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}
// Cells a prerequisite line may not cross: a talent sits there.
bool taken(TalentTree const& tree, int row, int column) { return tree.at(row, column) != nullptr; }
}
Talent const* TalentTree::find(Id id) const { for (auto const& t : talents) if (t.id == id) return &t; return nullptr; }
Talent* TalentTree::find(Id id) { for (auto& t : talents) if (t.id == id) return &t; return nullptr; }
Talent const* TalentTree::at(int row, int column) const { for (auto const& t : talents) if (t.row == row && t.column == column) return &t; return nullptr; }
int TalentTree::points(QHash<Id, int> const& build) const { int sum = 0; for (auto const& t : talents) sum += build.value(t.id); return sum; }
QVector<TalentClass> const& talentClasses() {
  static QVector<TalentClass> const list{{1, "Warrior"}, {2, "Paladin"}, {4, "Hunter"}, {8, "Rogue"}, {16, "Priest"},
                                         {64, "Shaman"}, {128, "Mage"}, {256, "Warlock"}, {1024, "Druid"}};
  return list;
}
QString className(quint32 mask) {
  for (auto const& c : talentClasses()) if (c.mask == mask) return c.name;
  return mask ? QString("Class mask %1").arg(mask) : QString("Template");
}
namespace TalentDbc {
QVector<quint32> record(Talent const& t, Id tab) {
  QVector<quint32> r(columns, 0);
  r[0] = t.id; r[1] = tab; r[2] = quint32(t.row); r[3] = quint32(t.column);
  for (int i = 0; i < std::min(t.maxRank(), 9); ++i) r[4 + i] = t.ranks[i];
  r[13] = t.prerequisite;
  r[16] = t.prerequisite ? quint32(std::max(1, t.prerequisiteRank) - 1) : 0;
  r[19] = t.flags; r[20] = t.requiredSpell;
  return r;
}
Talent fromRecord(Wdbc const& table, int row) {
  Talent t;
  t.id = table.cell(row, 0); t.row = int(table.cell(row, 2)); t.column = int(table.cell(row, 3));
  for (int i = 0; i < 9 && table.cell(row, 4 + i); ++i) t.ranks << table.cell(row, 4 + i);
  t.prerequisite = table.cell(row, 13);
  t.prerequisiteRank = t.prerequisite ? int(table.cell(row, 16)) + 1 : 1;
  t.flags = table.cell(row, 19); t.requiredSpell = table.cell(row, 20);
  return t;
}
}
namespace TalentLogic {
QVector<TalentTree> trees(Wdbc const& tabs, Wdbc const& talents) {
  require(talents.columns() == TalentDbc::columns, QString("Talent.dbc has %1 columns; Creator supports the 1.12 layout (%2).").arg(talents.columns()).arg(TalentDbc::columns));
  require(tabs.columns() >= 15, "TalentTab.dbc does not have the 1.12 layout.");
  QVector<TalentTree> out;
  for (int r = 0; r < tabs.rows(); ++r) {
    TalentTree tree;
    tree.tab = tabs.cell(r, 0); tree.name = tabs.text(r, 1); tree.icon = tabs.cell(r, 10);
    tree.classMask = tabs.cell(r, 12); tree.order = int(tabs.cell(r, 13)); tree.background = tabs.text(r, 14);
    for (int t = 0; t < talents.rows(); ++t) if (talents.cell(t, 1) == tree.tab) tree.talents << TalentDbc::fromRecord(talents, t);
    std::sort(tree.talents.begin(), tree.talents.end(), [](auto const& a, auto const& b) { return std::tie(a.row, a.column, a.id) < std::tie(b.row, b.column, b.id); });
    out << tree;
  }
  auto rank = [](quint32 mask) { auto const& list = talentClasses(); for (int i = 0; i < list.size(); ++i) if (list[i].mask == mask) return i; return int(list.size()); };
  std::sort(out.begin(), out.end(), [&](auto const& a, auto const& b) { return std::make_tuple(rank(a.classMask), a.classMask, a.order, a.tab) < std::make_tuple(rank(b.classMask), b.classMask, b.order, b.tab); });
  return out;
}
QByteArray talentTable(QByteArray const& base, QVector<TalentTree> const& replaced) {
  auto table = Wdbc::parse(base);
  require(table.columns() == TalentDbc::columns, QString("Talent.dbc has %1 columns; Creator supports the 1.12 layout (%2).").arg(table.columns()).arg(TalentDbc::columns));
  QSet<quint32> tabs; for (auto const& tree : replaced) tabs.insert(tree.tab);
  QVector<quint32> gone;
  for (int r = 0; r < table.rows(); ++r) if (tabs.contains(table.cell(r, 1))) gone << table.cell(r, 0);
  for (auto id : gone) table.remove(id);
  for (auto const& tree : replaced) {
    auto talents = tree.talents;
    std::sort(talents.begin(), talents.end(), [](auto const& a, auto const& b) { return std::tie(a.row, a.column) < std::tie(b.row, b.column); });
    for (auto const& t : talents) table.put(TalentDbc::record(t, tree.tab), {});
  }
  return table.bytes();
}
std::optional<TalentLink> link(TalentTree const& tree, Talent const& t) {
  if (!t.prerequisite) return std::nullopt;
  auto const* p = tree.find(t.prerequisite);
  if (!p || p->id == t.id) return std::nullopt;
  TalentLink l; l.from = p->id; l.to = t.id;
  int bt = t.row, bc = t.column, pt = p->row, pc = p->column; // the client's names: button tier/column, prerequisite tier/column
  auto blocker = [&](int row, int column) { auto const* in = tree.at(row, column); return in && in->id != t.id && in->id != p->id ? in : nullptr; };
  if (pt > bt) {
    l.path = {{pc, pt}, {bc, bt}};
    l.blocked = "The talent it requires is below it; the game only draws arrows down or sideways.";
    return l;
  }
  if (bc == pc) {
    l.path = {{pc, pt}, {bc, bt}};
    for (int r = pt + 1; r < bt; ++r) if (blocker(r, bc)) { l.blocked = QString("A talent at row %1 is in the way of the arrow.").arg(r + 1); break; }
    return l;
  }
  if (bt == pt) {
    l.path = {{pc, pt}, {bc, bt}};
    l.arrow = bc < pc ? TalentLink::Arrow::Left : TalentLink::Arrow::Right;
    for (int c = std::min(bc, pc) + 1; c < std::max(bc, pc); ++c) if (blocker(bt, c)) { l.blocked = QString("A talent at column %1 is in the way of the arrow.").arg(c + 1); break; }
    return l;
  }
  // Diagonal: across the prerequisite's row, then down; if that row is blocked, down its column, then across.
  int left = std::min(bc, pc), right = std::max(bc, pc);
  bool across = true;
  for (int c = left; c <= right; ++c) if (c != pc && taken(tree, pt, c)) across = false;
  if (across) { l.path = {{pc, pt}, {bc, pt}, {bc, bt}}; l.arrow = TalentLink::Arrow::Down; return l; }
  l.path = {{pc, pt}, {pc, bt}, {bc, bt}};
  l.arrow = bc < pc ? TalentLink::Arrow::Left : TalentLink::Arrow::Right;
  for (int c = left; c <= right; ++c) if (c != bc && blocker(bt, c)) { l.blocked = QString("Talents are in the way both across row %1 and across row %2: the game cannot draw this arrow.").arg(pt + 1).arg(bt + 1); break; }
  return l;
}
QVector<TalentLink> links(TalentTree const& tree) {
  QVector<TalentLink> out;
  for (auto const& t : tree.talents) if (auto l = link(tree, t)) out << *l;
  return out;
}
QString cannotRequire(TalentTree const& tree, Id talent, Id prerequisite) {
  if (!prerequisite) return {};
  auto const* t = tree.find(talent); auto const* p = tree.find(prerequisite);
  if (!t || !p) return "Prerequisites must be in the same tree.";
  if (talent == prerequisite) return "A talent cannot require itself.";
  if (p->row > t->row) return "The required talent must be above it or in the same row.";
  QSet<Id> seen;
  for (auto const* at = p; at && at->prerequisite; at = tree.find(at->prerequisite)) {
    if (at->prerequisite == talent) return "That would make the prerequisites go in a circle.";
    if (seen.contains(at->id)) break;
    seen.insert(at->id);
  }
  return {};
}
QVector<TalentProblem> check(TalentTree const& tree, QHash<Id, QString> const& spellOwner, QSet<Id> const& spells) {
  QVector<TalentProblem> out;
  if (tree.talents.size() > TalentRules::talentsPerTree)
    out.push_back({0, QString("This tree has %1 talents; the game's talent window has room for %2.").arg(tree.talents.size()).arg(TalentRules::talentsPerTree)});
  QHash<Id, Id> spellTalent;
  for (auto const& t : tree.talents) {
    if (t.row < 0 || t.row >= TalentRules::tiers || t.column < 0 || t.column >= TalentRules::columns)
      out.push_back({t.id, QString("It is outside the talent window (%1 rows of %2).").arg(TalentRules::tiers).arg(TalentRules::columns)});
    for (auto const& o : tree.talents)
      if (o.id < t.id && o.row == t.row && o.column == t.column) out.push_back({t.id, "Another talent is in the same place."});
    if (t.ranks.isEmpty()) out.push_back({t.id, "It has no ranks."});
    if (t.maxRank() > TalentRules::maxRanks) out.push_back({t.id, QString("It has %1 ranks; the server reads at most %2.").arg(t.maxRank()).arg(TalentRules::maxRanks)});
    for (int i = 0; i < t.maxRank(); ++i) {
      auto spell = t.ranks[i];
      if (!spell) { out.push_back({t.id, QString("Rank %1 has no spell.").arg(i + 1)}); continue; }
      if (!spells.isEmpty() && !spells.contains(spell)) out.push_back({t.id, QString("Rank %1's spell (%2) does not exist.").arg(i + 1).arg(spell)});
      if (auto owner = spellOwner.find(spell); owner != spellOwner.end())
        out.push_back({t.id, QString("Rank %1's spell is already a rank of %2. A spell can teach only one talent.").arg(i + 1).arg(*owner)});
      if (auto other = spellTalent.find(spell); other != spellTalent.end())
        out.push_back({t.id, *other == t.id ? QString("Rank %1 repeats an earlier rank's spell.").arg(i + 1)
                                            : QString("Rank %1's spell is also a rank of the talent at %2.").arg(i + 1).arg(place(*tree.find(*other)))});
      else spellTalent.insert(spell, t.id);
    }
    if (t.prerequisite) {
      auto const* p = tree.find(t.prerequisite);
      if (!p) out.push_back({t.id, "It requires a talent that is not in this tree."});
      else if (auto why = cannotRequire(tree, t.id, t.prerequisite); !why.isEmpty()) out.push_back({t.id, why});
      else {
        if (t.prerequisiteRank < 1 || t.prerequisiteRank > p->maxRank())
          out.push_back({t.id, QString("It requires %1 points in a talent with %2 ranks.").arg(t.prerequisiteRank).arg(p->maxRank())});
        if (auto l = link(tree, t); l && !l->blocked.isEmpty()) out.push_back({t.id, l->blocked + " In game this shows an error each time the talent window opens."});
      }
    }
    auto r = reach(tree, t.id);
    if (!r.reachable && !t.ranks.isEmpty()) out.push_back({t.id, "Players cannot learn it: " + r.why, false});
  }
  return out;
}
TalentReach reach(TalentTree const& tree, Id id) {
  TalentReach out;
  auto const* target = tree.find(id);
  if (!target) { out.why = "It is not in this tree."; return out; }
  out.treePoints = TalentRules::requiredPoints(target->row);
  if (target->ranks.isEmpty()) { out.why = "It has no ranks."; return out; }
  // The prerequisite chain, first prerequisite first, with the points each one needs.
  QVector<QPair<Talent const*, int>> chain{{target, 1}};
  QSet<Id> seen{id};
  for (auto const* at = target; at->prerequisite;) {
    auto const* p = tree.find(at->prerequisite);
    if (!p) { out.why = "It requires a talent that is not in this tree."; return out; }
    if (seen.contains(p->id)) { out.why = "Its prerequisites go in a circle."; return out; }
    if (p->row > at->row) { out.why = "A talent it requires is below the talent that needs it."; return out; }
    if (at->prerequisiteRank > p->maxRank() || at->prerequisiteRank < 1) { out.why = "It requires more points than a prerequisite has ranks."; return out; }
    seen.insert(p->id);
    chain.prepend({p, at->prerequisiteRank});
    at = p;
  }
  QHash<Id, int> planned; int spent = 0;
  // Spends single points on whatever is open, upper rows first, until `target` points are in the tree.
  auto fill = [&](int needed) {
    while (spent < needed) {
      Talent const* best = nullptr;
      for (auto const& f : tree.talents) {
        if (TalentRules::requiredPoints(f.row) > spent || planned.value(f.id) >= f.maxRank()) continue;
        if (f.prerequisite && planned.value(f.prerequisite) < f.prerequisiteRank) continue;
        if (!best || f.row < best->row) best = &f;
      }
      if (!best) return false;
      ++planned[best->id]; ++spent;
    }
    return true;
  };
  for (auto const& [t, need] : chain) {
    if (!fill(TalentRules::requiredPoints(t->row))) {
      out.why = QString("Row %1 opens at %2 points in this tree, but the talents above it hold only %3.").arg(t->row + 1).arg(TalentRules::requiredPoints(t->row)).arg(spent);
      return out;
    }
    if (planned.value(t->id) < need) { spent += need - planned.value(t->id); planned[t->id] = need; }
  }
  for (auto const& [t, need] : chain) if (t != target) out.path << t->id;
  out.points = spent;
  out.level = TalentRules::levelFor(spent);
  out.reachable = spent <= TalentRules::pointsAt(TalentRules::maxLevel);
  if (!out.reachable) out.why = QString("It needs %1 talent points (level %2); level %3 gives %4.").arg(spent).arg(out.level).arg(TalentRules::maxLevel).arg(TalentRules::pointsAt(TalentRules::maxLevel));
  return out;
}
QString cannotLearn(TalentTree const& tree, QHash<Id, int> const& build, Id id, int available) {
  auto const* t = tree.find(id);
  if (!t || t->ranks.isEmpty()) return "This talent has no ranks.";
  if (build.value(id) >= t->maxRank()) return "Already at its highest rank.";
  if (int needed = TalentRules::requiredPoints(t->row); tree.points(build) < needed) return QString("Requires %1 points in %2 Talents").arg(needed).arg(tree.name);
  if (t->prerequisite) {
    auto const* p = tree.find(t->prerequisite);
    if (!p) return "Requires a talent that is not in this tree";
    if (build.value(p->id) < t->prerequisiteRank) return QString("Requires %1 point%2 in the talent at %3").arg(t->prerequisiteRank).arg(t->prerequisiteRank == 1 ? "" : "s").arg(place(*p));
  }
  if (available <= 0) return "No talent points left at this level.";
  return {};
}
QString cannotUnlearn(TalentTree const& tree, QHash<Id, int> const& build, Id id) {
  auto const* t = tree.find(id);
  int rank = build.value(id);
  if (!t || rank <= 0) return "No points in it.";
  for (auto const& u : tree.talents)
    if (u.prerequisite == id && build.value(u.id) > 0 && u.prerequisiteRank > rank - 1)
      return QString("The talent at %1 depends on it.").arg(place(u));
  auto after = build; after[id] = rank - 1;
  for (auto const& u : tree.talents) {
    if (after.value(u.id) <= 0 || u.row <= t->row) continue;
    int above = 0;
    for (auto const& v : tree.talents) if (v.row < u.row) above += after.value(v.id);
    if (above < TalentRules::requiredPoints(u.row)) return QString("The talent at %1 needs %2 points above its row.").arg(place(u)).arg(TalentRules::requiredPoints(u.row));
  }
  return {};
}
QHash<Id, int> repair(TalentTree const& tree, QHash<Id, int> build) {
  for (auto it = build.begin(); it != build.end();) {
    auto const* t = tree.find(it.key());
    if (!t || it.value() <= 0) { it = build.erase(it); continue; }
    it.value() = std::min(it.value(), t->maxRank());
    ++it;
  }
  for (bool changed = true; changed;) {
    changed = false;
    for (auto const& t : tree.talents) {
      if (build.value(t.id) <= 0) continue;
      int above = 0;
      for (auto const& v : tree.talents) if (v.row < t.row) above += build.value(v.id);
      bool prerequisite = !t.prerequisite || build.value(t.prerequisite) >= t.prerequisiteRank;
      if (above < TalentRules::requiredPoints(t.row) || !prerequisite) { build.remove(t.id); changed = true; }
    }
  }
  return build;
}
QVector<TalentChange> diff(TalentTree const& original, TalentTree const& modified, std::function<QString(Talent const&)> const& name) {
  QVector<TalentChange> out;
  using K = TalentChange::Kind;
  for (auto const& m : modified.talents) {
    auto const* o = original.find(m.id);
    if (!o) { out.push_back({K::Added, m.id, "Added " + name(m) + " at " + place(m)}); continue; }
    if (o->row != m.row || o->column != m.column) out.push_back({K::Moved, m.id, "Moved " + name(m) + " from " + place(*o) + " to " + place(m)});
    if (o->maxRank() != m.maxRank()) out.push_back({K::Ranks, m.id, QString("%1: %2 ranks, was %3").arg(name(m)).arg(m.maxRank()).arg(o->maxRank())});
    else if (o->ranks != m.ranks) out.push_back({K::Spells, m.id, name(m) + ": different rank spells"});
    if (o->prerequisite != m.prerequisite || (m.prerequisite && o->prerequisiteRank != m.prerequisiteRank)) {
      auto const* p = modified.find(m.prerequisite);
      out.push_back({K::Prerequisite, m.id, name(m) + (p ? QString(": requires %1 point%2 in %3").arg(m.prerequisiteRank).arg(m.prerequisiteRank == 1 ? "" : "s").arg(name(*p))
                                                         : QString(": no longer requires another talent"))});
    }
  }
  for (auto const& o : original.talents)
    if (!modified.find(o.id)) out.push_back({K::Removed, o.id, "Removed " + name(o) + " from " + place(o)});
  return out;
}
bool move(TalentTree& tree, Id id, int row, int column) {
  if (row < 0 || row >= TalentRules::tiers || column < 0 || column >= TalentRules::columns) return false;
  auto* t = tree.find(id);
  if (!t) return false;
  if (auto const* there = tree.at(row, column); there && there->id != id) {
    auto* other = tree.find(there->id);
    other->row = t->row; other->column = t->column;
  }
  t->row = row; t->column = column;
  return true;
}
QPoint freeCell(TalentTree const& tree, int fromRow) {
  for (int r = std::max(0, fromRow); r < TalentRules::tiers; ++r)
    for (int c = 0; c < TalentRules::columns; ++c) if (!tree.at(r, c)) return {c, r};
  for (int r = 0; r < std::max(0, fromRow) && r < TalentRules::tiers; ++r)
    for (int c = 0; c < TalentRules::columns; ++c) if (!tree.at(r, c)) return {c, r};
  return {-1, -1};
}
QHash<Id, QString> spellOwners(QVector<TalentTree> const& trees, Id except) {
  QHash<Id, QString> out;
  for (auto const& tree : trees) {
    if (tree.tab == except || !tree.tab) continue;
    for (auto const& t : tree.talents)
      for (auto spell : t.ranks) if (spell) out.insert(spell, QString("the talent at row %1, column %2 of %3 %4").arg(t.row + 1).arg(t.column + 1).arg(className(tree.classMask), tree.name));
  }
  return out;
}
QJsonObject toJson(TalentTree const& tree) {
  QJsonArray talents;
  for (auto const& t : tree.talents) {
    QJsonArray ranks; for (auto spell : t.ranks) ranks << qint64(spell);
    talents << QJsonObject{{"id", qint64(t.id)}, {"row", t.row}, {"column", t.column}, {"ranks", ranks}, {"prerequisite", qint64(t.prerequisite)},
                           {"prerequisiteRank", t.prerequisiteRank}, {"flags", qint64(t.flags)}, {"requiredSpell", qint64(t.requiredSpell)}};
  }
  return QJsonObject{{"tab", qint64(tree.tab)}, {"key", tree.key}, {"name", tree.name}, {"background", tree.background}, {"icon", qint64(tree.icon)},
                     {"classMask", qint64(tree.classMask)}, {"order", tree.order}, {"talents", talents}};
}
TalentTree fromJson(QJsonObject const& json) {
  TalentTree tree;
  tree.tab = Id(json["tab"].toDouble()); tree.key = json["key"].toString(); tree.name = json["name"].toString();
  tree.background = json["background"].toString(); tree.icon = Id(json["icon"].toDouble());
  tree.classMask = quint32(json["classMask"].toDouble()); tree.order = json["order"].toInt();
  for (auto const& value : json["talents"].toArray()) {
    auto o = value.toObject(); Talent t;
    t.id = Id(o["id"].toDouble()); t.row = o["row"].toInt(); t.column = o["column"].toInt();
    for (auto const& r : o["ranks"].toArray()) t.ranks << Id(r.toDouble());
    t.prerequisite = Id(o["prerequisite"].toDouble()); t.prerequisiteRank = std::max(1, o["prerequisiteRank"].toInt(1));
    t.flags = quint32(o["flags"].toDouble()); t.requiredSpell = Id(o["requiredSpell"].toDouble());
    tree.talents << t;
  }
  return tree;
}
}
TalentStore* TalentStore::instance() {
  auto runtime = Runtime::RuntimeManager::instance();
  if (!runtime) return nullptr;
  if (auto store = runtime->findChild<TalentStore*>("creatorTalents", Qt::FindDirectChildrenOnly)) return store;
  auto store = new TalentStore(runtime->root(), runtime);
  store->setObjectName("creatorTalents");
  return store;
}
TalentStore::TalentStore(QString const& root, QObject* parent)
  : QObject(parent), _root(root), _file(root + "/Workspace/creator-talents.json"), _state(root + "/Workspace/talents/server.json"),
    _backups(root + "/Workspace/talents/original") {}
QJsonObject TalentStore::load() const { return readJson(_file); }
void TalentStore::store(QJsonObject const& json) {
  writeFile(_file, QJsonDocument(json).toJson());
  emit changed();
}
QString TalentStore::serverTable() const {
  auto folder = _root + "/Runtime/mangosd/data/dbc";
  for (auto const& entry : QDir(folder).entryList(QDir::Files)) if (entry.compare("Talent.dbc", Qt::CaseInsensitive) == 0) return folder + "/" + entry;
  return folder + "/Talent.dbc";
}
QByteArray TalentStore::originalBytes(QString const& file) const {
  if (file == "Talent.dbc") {
    auto state = readJson(_state);
    auto installed = state["installed"].toString(), server = serverTable();
    if (!installed.isEmpty() && fileHash(server) == installed) {
      auto backup = state["backup"].toObject();
      auto bytes = readFile(backup["file"].toString());
      require(hashOf(bytes) == backup["sha256"].toString(), "The backup of the server's own Talent.dbc is damaged: " + backup["file"].toString());
      return bytes;
    }
    return readFile(server);
  }
  auto path = QFileInfo(serverTable()).absolutePath() + "/" + file;
  for (auto const& entry : QDir(QFileInfo(path).absolutePath()).entryList(QDir::Files)) if (entry.compare(file, Qt::CaseInsensitive) == 0) path = QFileInfo(path).absolutePath() + "/" + entry;
  return readFile(path);
}
QVector<TalentTree> TalentStore::originals() const {
  return TalentLogic::trees(Wdbc::parse(originalBytes("TalentTab.dbc")), Wdbc::parse(originalBytes("Talent.dbc")));
}
QVector<TalentTree> TalentStore::current() const {
  auto out = originals();
  auto edited = load()["trees"].toObject();
  for (auto& tree : out)
    if (auto json = edited[QString::number(tree.tab)]; json.isObject()) tree.talents = TalentLogic::fromJson(json.toObject()).talents;
  return out;
}
bool TalentStore::modified(Id tab) const { return load()["trees"].toObject().contains(QString::number(tab)); }
QVector<Id> TalentStore::modifiedTabs() const {
  QVector<Id> out;
  for (auto const& key : load()["trees"].toObject().keys()) out << key.toUInt();
  return out;
}
void TalentStore::save(TalentTree const& tree) {
  auto json = load();
  if (tree.tab) {
    auto trees = json["trees"].toObject();
    TalentTree original;
    for (auto const& o : originals()) if (o.tab == tree.tab) original = o;
    require(original.tab == tree.tab, QString("Talent tree %1 is not in the server's TalentTab.dbc.").arg(tree.tab));
    auto mine = tree.talents, theirs = original.talents;
    auto order = [](auto const& a, auto const& b) { return a.id < b.id; };
    std::sort(mine.begin(), mine.end(), order); std::sort(theirs.begin(), theirs.end(), order);
    if (mine == theirs) trees.remove(QString::number(tree.tab));
    else trees[QString::number(tree.tab)] = TalentLogic::toJson(tree);
    json["trees"] = trees;
  } else {
    require(!tree.key.isEmpty(), "A template needs a name.");
    auto templates = json["templates"].toObject();
    templates[tree.key] = TalentLogic::toJson(tree);
    json["templates"] = templates;
  }
  json["version"] = 1;
  store(json);
}
void TalentStore::reset(Id tab) {
  auto json = load(); auto trees = json["trees"].toObject();
  if (!trees.contains(QString::number(tab))) return;
  trees.remove(QString::number(tab)); json["trees"] = trees;
  store(json);
}
QVector<TalentTree> TalentStore::templates() const {
  QVector<TalentTree> out;
  auto list = load()["templates"].toObject();
  for (auto it = list.begin(); it != list.end(); ++it) { auto tree = TalentLogic::fromJson(it.value().toObject()); tree.key = it.key(); tree.tab = 0; out << tree; }
  return out;
}
void TalentStore::removeTemplate(QString const& key) {
  auto json = load(); auto list = json["templates"].toObject();
  list.remove(key); json["templates"] = list;
  store(json);
}
Id TalentStore::nextTalentId() const {
  Id highest = 0;
  for (auto const& tree : originals()) for (auto const& t : tree.talents) highest = std::max(highest, t.id);
  for (auto const& tree : current()) for (auto const& t : tree.talents) highest = std::max(highest, t.id);
  for (auto const& tree : templates()) for (auto const& t : tree.talents) highest = std::max(highest, t.id);
  return highest + 1;
}
QHash<Id, QString> TalentStore::spellOwners(Id tab) const { return TalentLogic::spellOwners(current(), tab); }
QStringList TalentStore::blockingProblems() const {
  QStringList out;
  auto edited = modifiedTabs();
  for (auto const& tree : current()) {
    if (!edited.contains(tree.tab)) continue;
    for (auto const& p : TalentLogic::check(tree, spellOwners(tree.tab))) {
      if (!p.error) continue;
      auto const* t = tree.find(p.talent);
      out << className(tree.classMask) + " " + tree.name + (t ? QString(", row %1 column %2").arg(t->row + 1).arg(t->column + 1) : QString()) + ": " + p.text;
    }
  }
  return out;
}
QString TalentStore::changeHash() const {
  auto trees = load()["trees"].toObject();
  if (trees.isEmpty()) return {};
  return hashOf(QJsonDocument(trees).toJson(QJsonDocument::Compact));
}
QString TalentStore::changeLabel() const {
  QStringList names; auto edited = modifiedTabs();
  for (auto const& tree : originals()) if (edited.contains(tree.tab)) names << className(tree.classMask) + " " + tree.name;
  return "Talent trees: " + names.join(", ");
}
QByteArray TalentStore::exportTable() const {
  auto edited = modifiedTabs();
  require(!edited.isEmpty(), "No talent trees are edited.");
  auto problems = blockingProblems();
  require(problems.isEmpty(), "The edited talent trees have problems the game cannot handle. Fix them first:\n" + problems.mid(0, 8).join('\n'));
  QVector<TalentTree> trees;
  for (auto const& tree : current()) if (edited.contains(tree.tab)) trees << tree;
  return TalentLogic::talentTable(originalBytes("Talent.dbc"), trees);
}
std::optional<QByteArray> TalentStore::clientTable(QByteArray const& clientBase) const {
  auto edited = modifiedTabs();
  if (edited.isEmpty()) return std::nullopt;
  QVector<TalentTree> trees;
  for (auto const& tree : current()) if (edited.contains(tree.tab)) trees << tree;
  return TalentLogic::talentTable(clientBase, trees);
}
bool TalentStore::serverCurrent() const {
  auto state = readJson(_state);
  bool ours = !state["installed"].toString().isEmpty() && fileHash(serverTable()) == state["installed"].toString();
  if (modifiedTabs().isEmpty()) return !ours;
  return ours && state["hash"].toString() == changeHash();
}
void TalentStore::installServer() {
  if (modifiedTabs().isEmpty()) { restoreServer(); return; }
  auto problems = blockingProblems();
  require(problems.isEmpty(), "The edited talent trees have problems the game cannot handle. Fix them in the Talent Editor first:\n" + problems.mid(0, 8).join('\n'));
  auto server = serverTable();
  require(QFileInfo::exists(server), "The local server has no Talent.dbc at " + QDir::toNativeSeparators(server) + ".");
  auto state = readJson(_state);
  auto onDisk = fileHash(server);
  if (state["installed"].toString().isEmpty() || onDisk != state["installed"].toString()) {
    // The server's own table (or one replaced by hand since): keep it before Creator writes over it.
    QDir().mkpath(_backups);
    auto backup = _backups + "/" + onDisk + ".dbc";
    if (!QFileInfo::exists(backup)) require(QFile::copy(server, backup), "Cannot back up the server's Talent.dbc.");
    require(fileHash(backup) == onDisk, "The backup of the server's Talent.dbc is damaged: " + backup);
    state["backup"] = QJsonObject{{"file", backup}, {"sha256", onDisk}, {"saved", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    state["installed"] = QString();
    writeFile(_state, QJsonDocument(state).toJson());
  }
  auto backup = state["backup"].toObject();
  auto base = readFile(backup["file"].toString());
  require(hashOf(base) == backup["sha256"].toString(), "The backup of the server's own Talent.dbc is damaged: " + backup["file"].toString());
  QVector<TalentTree> trees; auto edited = modifiedTabs();
  for (auto const& tree : current()) if (edited.contains(tree.tab)) trees << tree;
  auto bytes = TalentLogic::talentTable(base, trees);
  writeFile(server, bytes);
  state["installed"] = hashOf(bytes); state["hash"] = changeHash();
  state["updated"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  writeFile(_state, QJsonDocument(state).toJson());
  emit changed();
}
void TalentStore::restoreServer() {
  auto state = readJson(_state);
  auto installed = state["installed"].toString(), server = serverTable();
  if (installed.isEmpty()) return;
  if (fileHash(server) == installed) {
    auto backup = state["backup"].toObject();
    auto bytes = readFile(backup["file"].toString());
    require(hashOf(bytes) == backup["sha256"].toString(), "The backup of the server's own Talent.dbc is damaged: " + backup["file"].toString());
    writeFile(server, bytes);
  }
  state["installed"] = QString(); state.remove("hash");
  writeFile(_state, QJsonDocument(state).toJson());
  emit changed();
}
}
