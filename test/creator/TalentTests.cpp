#include <noggit/creator/TalentService.hpp>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtEndian>
#include <algorithm>
#include <stdexcept>
using namespace Noggit::Creator;
namespace {
void check(bool condition, char const* message) { if (!condition) throw std::runtime_error(message); }
bool has(QVector<TalentProblem> const& problems, Id talent, QString const& text, bool error = true) {
  for (auto const& p : problems) if (p.talent == talent && p.text.contains(text) && p.error == error) return true;
  return false;
}
// An empty WDBC table with `columns` columns.
Wdbc table(int columns) {
  QByteArray bytes("WDBC");
  auto append = [&](quint32 value) { char b[4]; qToLittleEndian(value, b); bytes.append(b, 4); };
  append(0); append(quint32(columns)); append(quint32(columns * 4)); append(1); bytes.append('\0');
  return Wdbc::parse(bytes);
}
Talent talent(Id id, int row, int column, QVector<Id> ranks, Id prerequisite = 0, int prerequisiteRank = 1) {
  Talent t; t.id = id; t.row = row; t.column = column; t.ranks = ranks; t.prerequisite = prerequisite; t.prerequisiteRank = prerequisiteRank; return t;
}
TalentTree sample() {
  TalentTree tree; tree.tab = 161; tree.name = "Arms"; tree.classMask = 1;
  tree.talents = {talent(1, 0, 0, {101, 102, 103, 104, 105}), talent(2, 0, 1, {201, 202, 203, 204, 205}),
                  talent(3, 1, 1, {301, 302, 303}), talent(4, 2, 1, {401}, 3, 3)};
  return tree;
}
void tables() {
  auto tabs = table(15), talents = table(TalentDbc::columns);
  QVector<quint32> tab(15, 0); tab[0] = 161; tab[10] = 1462; tab[12] = 1; tab[13] = 0;
  tabs.put(tab, {{1, "Arms"}, {14, "WarriorArms"}});
  auto tree = sample();
  for (auto const& t : tree.talents) talents.put(TalentDbc::record(t, 161), {});
  QVector<quint32> other(TalentDbc::columns, 0); other[0] = 900; other[1] = 164; other[4] = 999; talents.put(other, {});
  auto trees = TalentLogic::trees(tabs, talents);
  check(trees.size() == 1 && trees[0].name == "Arms" && trees[0].background == "WarriorArms" && trees[0].icon == 1462, "Tree metadata not read");
  check(trees[0].talents.size() == 4, "Tree talents not read");
  auto const* deep = trees[0].find(4);
  check(deep && deep->prerequisite == 3 && deep->prerequisiteRank == 3 && deep->ranks == QVector<Id>{401}, "Prerequisite rank is not 1-based after reading");
  check(TalentDbc::record(*deep, 161)[16] == 2, "Prerequisite rank is not stored 0-based");
  // Replacing a tree drops its old rows and keeps the other trees'.
  auto edited = sample(); edited.talents.removeAt(0); edited.talents << talent(50, 3, 2, {501, 502});
  auto out = Wdbc::parse(TalentLogic::talentTable(talents.bytes(), {edited}));
  check(out.find(1) < 0, "A deleted talent stayed in Talent.dbc");
  check(out.find(900) >= 0, "Another tree's talent was dropped");
  int row = out.find(50);
  check(row >= 0 && out.cell(row, 1) == 161 && out.cell(row, 2) == 3 && out.cell(row, 3) == 2 && out.cell(row, 4) == 501 && out.cell(row, 5) == 502, "A new talent was written wrongly");
  bool threw = false;
  try { TalentLogic::talentTable(table(20).bytes(), {edited}); } catch (std::exception const&) { threw = true; }
  check(threw, "A Talent.dbc of another layout was accepted");
}
void arrows() {
  TalentTree tree; tree.tab = 1;
  tree.talents = {talent(1, 0, 1, {1}), talent(2, 2, 1, {2}, 1)};
  auto l = TalentLogic::link(tree, *tree.find(2));
  check(l && l->blocked.isEmpty() && l->arrow == TalentLink::Arrow::Down && l->path.size() == 2, "A straight arrow down is wrong");
  tree.talents << talent(3, 1, 1, {3});
  check(!TalentLogic::link(tree, *tree.find(2))->blocked.isEmpty(), "A talent in the way of a vertical arrow was not noticed");
  tree.talents = {talent(1, 0, 0, {1}), talent(2, 0, 2, {2}, 1)};
  l = TalentLogic::link(tree, *tree.find(2));
  check(l && l->blocked.isEmpty() && l->arrow == TalentLink::Arrow::Right, "A sideways arrow is wrong");
  tree.talents << talent(3, 0, 1, {3});
  check(!TalentLogic::link(tree, *tree.find(2))->blocked.isEmpty(), "A talent in the way of a sideways arrow was not noticed");
  // Diagonal: across the prerequisite's row, then down.
  tree.talents = {talent(1, 0, 0, {1}), talent(2, 2, 2, {2}, 1)};
  l = TalentLogic::link(tree, *tree.find(2));
  check(l && l->blocked.isEmpty() && l->path.size() == 3 && l->path[1] == QPoint(2, 0) && l->arrow == TalentLink::Arrow::Down, "A diagonal arrow is wrong");
  // That row blocked: down the prerequisite's column, then across.
  tree.talents << talent(3, 0, 2, {3});
  l = TalentLogic::link(tree, *tree.find(2));
  check(l && l->blocked.isEmpty() && l->path[1] == QPoint(0, 2) && l->arrow == TalentLink::Arrow::Right, "The game's second diagonal route is wrong");
  tree.talents << talent(4, 2, 1, {4});
  check(!TalentLogic::link(tree, *tree.find(2))->blocked.isEmpty(), "An undrawable diagonal arrow was accepted");
  tree.talents = {talent(1, 3, 0, {1}), talent(2, 1, 0, {2}, 1)};
  check(!TalentLogic::link(tree, *tree.find(2))->blocked.isEmpty(), "A prerequisite below was accepted");
}
void problems() {
  auto tree = sample();
  check(TalentLogic::check(tree).isEmpty(), "A valid tree has problems");
  auto t = tree;
  t.talents << talent(5, 0, 0, {601});
  check(has(TalentLogic::check(t), 5, "same place"), "Two talents in one place accepted");
  t = tree; t.talents[0].ranks = {101, 102, 103, 104, 105, 106};
  check(has(TalentLogic::check(t), 1, "at most 5"), "Six ranks accepted");
  t = tree; t.talents[1].ranks[2] = 0;
  check(has(TalentLogic::check(t), 2, "Rank 3 has no spell"), "An empty rank accepted");
  t = tree; t.talents[1].ranks[2] = 101;
  check(has(TalentLogic::check(t), 2, "also a rank of"), "A spell in two talents accepted");
  check(has(TalentLogic::check(tree, {{301, "the talent at row 1, column 1 of Warrior Fury"}}), 3, "already a rank of"), "A spell of another tree accepted");
  check(has(TalentLogic::check(tree, {}, {101, 102, 103, 104, 105, 201, 202, 203, 204, 205, 301, 302, 303}), 4, "does not exist"), "A missing spell accepted");
  t = tree; t.talents[3].prerequisiteRank = 4;
  check(has(TalentLogic::check(t), 4, "with 3 ranks"), "Requiring more points than ranks accepted");
  t = tree; t.talents[2].prerequisite = 4;
  check(has(TalentLogic::check(t), 3, "above it"), "A prerequisite below accepted");
  t = tree; t.talents[0].prerequisite = 2; t.talents[1].prerequisite = 1;
  check(has(TalentLogic::check(t), 1, "circle") || has(TalentLogic::check(t), 2, "circle"), "Prerequisites in a circle accepted");
  t = tree; t.talents[3].row = 8;
  check(has(TalentLogic::check(t), 4, "outside the talent window"), "A talent outside the window accepted");
  t = tree; for (Id i = 10; i < 27; ++i) t.talents << talent(i, int(i - 10) / 4 + 3, int(i - 10) % 4, {i * 10});
  check(has(TalentLogic::check(t), 0, "room for 20"), "More than 20 talents accepted");
  check(TalentLogic::cannotRequire(tree, 3, 4).contains("above"), "Requiring a talent below was allowed");
  check(TalentLogic::cannotRequire(tree, 3, 3).contains("itself"), "Requiring itself was allowed");
  check(TalentLogic::cannotRequire(tree, 3, 4).size() && TalentLogic::cannotRequire(tree, 4, 1).isEmpty(), "A valid prerequisite was refused");
}
void reachability() {
  check(TalentRules::pointsAt(9) == 0 && TalentRules::pointsAt(10) == 1 && TalentRules::pointsAt(32) == 23 && TalentRules::pointsAt(60) == 51, "Points per level are wrong");
  check(TalentRules::levelFor(1) == 10 && TalentRules::levelFor(51) == 60, "Level for points is wrong");
  auto tree = sample();
  auto r = TalentLogic::reach(tree, 4); // row 3 needs 10 points; 3 points in its prerequisite
  check(r.reachable && r.points == 11 && r.level == 20 && r.treePoints == 10 && r.path == QVector<Id>{3}, "Earliest level of a third-row talent is wrong");
  r = TalentLogic::reach(tree, 1);
  check(r.reachable && r.points == 1 && r.level == 10, "A first-row talent is not learnable at level 10");
  // Not enough to spend above it: only one five-rank talent in the first row.
  auto thin = tree; thin.talents.removeAt(1);
  r = TalentLogic::reach(thin, 4);
  check(!r.reachable && r.why.contains("hold only"), "An unreachable row was not noticed");
  check(has(TalentLogic::check(thin), 4, "cannot learn", false), "An unreachable talent is not warned about");
  // Deep in the tree: row 8 needs 35 points before it.
  TalentTree deep; deep.tab = 1;
  for (int r2 = 0; r2 < 8; ++r2) for (int c = 0; c < 2; ++c) deep.talents << talent(Id(r2 * 4 + c + 1), r2, c, {Id(1000 + r2 * 10 + c), Id(2000 + r2 * 10 + c), Id(3000 + r2 * 10 + c)});
  r = TalentLogic::reach(deep, 29);
  check(r.reachable && r.points == 36 && r.level == 45, "The earliest level of a last-row talent is wrong");
}
void builds() {
  auto tree = sample();
  QHash<Id, int> build;
  check(TalentLogic::cannotLearn(tree, build, 3, 10).contains("Requires 5 points in Arms"), "A locked row could be learned");
  check(TalentLogic::cannotLearn(tree, build, 1, 0).contains("No talent points"), "A point was spent without one");
  build[1] = 5;
  check(TalentLogic::cannotLearn(tree, build, 1, 10).contains("highest rank"), "A maxed talent took another point");
  check(TalentLogic::cannotLearn(tree, build, 3, 10).isEmpty(), "An open row could not be learned");
  build[3] = 2; build[2] = 5;
  check(TalentLogic::cannotLearn(tree, build, 4, 10).contains("Requires 3 points"), "An unmet prerequisite was ignored");
  build[3] = 3;
  check(TalentLogic::cannotLearn(tree, build, 4, 10).isEmpty(), "A met prerequisite was refused");
  build[4] = 1;
  check(TalentLogic::cannotUnlearn(tree, build, 3).contains("depends on it"), "A prerequisite in use gave a point back");
  check(TalentLogic::cannotUnlearn(tree, build, 1).isEmpty(), "A spare point could not be given back");
  build[2] = 2; // exactly 10 points above the third row now
  check(TalentLogic::cannotUnlearn(tree, build, 1).contains("needs 10 points"), "A point holding a row open was given back");
  check(TalentLogic::cannotUnlearn(tree, build, 4).isEmpty(), "The last talent could not give its point back");
  auto edited = tree; edited.talents[2].ranks.resize(2);
  auto repaired = TalentLogic::repair(edited, build);
  check(repaired.value(3) == 2 && !repaired.contains(4), "An edited tree kept impossible points");
}
void comparison() {
  auto original = sample(), modified = sample();
  TalentLogic::move(modified, 1, 0, 1); // swaps with talent 2
  check(modified.find(1)->column == 1 && modified.find(2)->column == 0, "Moving onto a talent did not swap");
  check(!TalentLogic::move(modified, 1, 9, 0), "A move outside the window was accepted");
  modified.talents.removeAt(modified.talents.indexOf(*modified.find(4)));
  modified.talents << talent(9, 4, 0, {901});
  modified.find(3)->ranks << 304;
  modified.find(3)->prerequisite = 1;
  auto changes = TalentLogic::diff(original, modified, [](Talent const& t) { return QString("T%1").arg(t.id); });
  auto kinds = [&](TalentChange::Kind kind) { int n = 0; for (auto const& c : changes) n += c.kind == kind; return n; };
  check(kinds(TalentChange::Kind::Moved) == 2 && kinds(TalentChange::Kind::Added) == 1 && kinds(TalentChange::Kind::Removed) == 1
        && kinds(TalentChange::Kind::Ranks) == 1 && kinds(TalentChange::Kind::Prerequisite) == 1, "The comparison is wrong");
  auto json = TalentLogic::toJson(modified);
  auto back = TalentLogic::fromJson(QJsonDocument::fromJson(QJsonDocument(json).toJson()).object());
  check(back == modified, "A tree does not survive the workspace file");
  check(TalentLogic::freeCell(original) == QPoint(2, 0), "The first free slot is wrong");
}
// The store over a copy of the server's data: originals, edits, reset, the server table and its restore.
void store(QString const& dbc) {
  QTemporaryDir root; check(root.isValid(), "No temporary folder");
  QDir().mkpath(root.path() + "/Runtime/mangosd/data/dbc"); QDir().mkpath(root.path() + "/Workspace");
  for (auto file : {"Talent.dbc", "TalentTab.dbc"}) check(QFile::copy(dbc + "/" + file, root.path() + "/Runtime/mangosd/data/dbc/" + file), "Cannot copy the server's tables");
  TalentStore s(root.path());
  auto originals = s.originals();
  check(originals.size() == 27, "The bundled data should have 27 talent trees");
  // The game's own trees follow every rule the editor checks.
  for (auto const& tree : originals) {
    for (auto const& p : TalentLogic::check(tree, TalentLogic::spellOwners(originals, tree.tab)))
      if (p.error) throw std::runtime_error(("A game tree breaks a rule: " + tree.name + ": " + p.text).toStdString());
    for (auto const& t : tree.talents) check(TalentLogic::reach(tree, t.id).reachable, "A game talent is unreachable by level 60");
  }
  check(s.serverCurrent() && s.modifiedTabs().isEmpty(), "A fresh store has edits");
  auto arms = originals[0];
  check(className(arms.classMask) == "Warrior" && arms.name == "Arms", "Warrior Arms is not the first tree");
  // Move a talent nothing links to into the empty last row.
  auto edited = arms;
  Id loose = 0;
  for (auto const& t : edited.talents)
    if (!t.prerequisite && std::none_of(edited.talents.begin(), edited.talents.end(), [&](auto const& o) { return o.prerequisite == t.id; })) { loose = t.id; break; }
  QPoint cell(0, TalentRules::tiers - 1);
  check(loose && !edited.at(cell.y(), cell.x()) && TalentLogic::move(edited, loose, cell.y(), cell.x()), "No talent to move");
  for (auto const& p : TalentLogic::check(edited, TalentLogic::spellOwners(originals, edited.tab))) check(!p.error, "The moved tree has errors");
  s.save(edited);
  // The same move with a talent put in an arrow's way is refused for the test.
  auto blocked = edited;
  for (auto const& t : blocked.talents) if (auto l = TalentLogic::link(blocked, t); l && l->path.size() == 2 && std::abs(l->path[1].y() - l->path[0].y()) > 1) {
    auto in = QPoint(l->path[0].x(), l->path[0].y() + 1);
    if (!blocked.at(in.y(), in.x())) { blocked.talents << talent(9999, in.y(), in.x(), {1}); break; }
  }
  if (blocked.find(9999)) {
    s.save(blocked);
    bool refused = false; try { s.installServer(); } catch (std::exception const&) { refused = true; }
    check(refused, "A tree the game cannot draw was installed");
    s.save(edited);
  }
  check(s.modified(arms.tab) && !s.serverCurrent(), "An edit was not recorded");
  auto before = QFile(s.serverTable()); before.open(QIODevice::ReadOnly); auto original = before.readAll(); before.close();
  s.installServer();
  check(s.serverCurrent(), "The server table was not installed");
  auto installed = Wdbc::parse([&] { QFile f(s.serverTable()); f.open(QIODevice::ReadOnly); return f.readAll(); }());
  int row = installed.find(loose);
  check(row >= 0 && int(installed.cell(row, 2)) == cell.y() && int(installed.cell(row, 3)) == cell.x(), "The server table does not have the moved talent");
  check(s.originals()[0] == arms, "The originals changed after installing");
  check(s.clientTable(original).has_value(), "No client table for an edited tree");
  s.save(arms); // back to the game's own tree: no longer an edit
  check(!s.modified(arms.tab), "Saving the original tree kept it as an edit");
  s.installServer(); // nothing edited: the server's own table goes back
  QFile after(s.serverTable()); after.open(QIODevice::ReadOnly);
  check(after.readAll() == original, "The server's own Talent.dbc was not restored byte for byte");
  // Templates.
  auto copy = arms; copy.tab = 0; copy.key = "Arms experiment"; copy.name = copy.key;
  s.save(copy);
  check(s.templates().size() == 1 && s.templates()[0].talents == arms.talents && s.modifiedTabs().isEmpty(), "A template is not kept apart");
  check(s.nextTalentId() > 476, "Talent IDs are reused");
  s.removeTemplate(copy.key);
  check(s.templates().isEmpty(), "A template was not deleted");
}
}
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    tables(); arrows(); problems(); reachability(); builds(); comparison();
    // The server's data folder (Runtime/mangosd/data/dbc) checks the game's real trees too.
    if (argc > 1 && QFile::exists(QString::fromLocal8Bit(argv[1]) + "/Talent.dbc")) store(QString::fromLocal8Bit(argv[1]));
    else QTextStream(stdout) << "No server dbc folder: the game's own trees were not checked\n";
    QTextStream(stdout) << "Talent tests passed\n";
    return 0;
  } catch (std::exception const& e) { QTextStream(stderr) << e.what() << Qt::endl; return 1; }
}
