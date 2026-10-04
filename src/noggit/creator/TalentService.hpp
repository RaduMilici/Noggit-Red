#pragma once
#include "ClientDbc.hpp"
#include <QJsonObject>
#include <QObject>
#include <QPoint>
#include <QSet>
#include <algorithm>
#include <functional>
#include <optional>
namespace Noggit::Creator {
// The talent window's rules, as the target client (Turtle's Blizzard_TalentUI) and the server apply them.
namespace TalentRules {
constexpr int tiers = 8;           // rows of the talent frame (Turtle shows 8; the 1.12 frame showed 7)
constexpr int columns = 4;
constexpr int talentsPerTree = 20; // talent buttons per tree; more makes the game show "Too many talents in talent frame!"
constexpr int maxRanks = 5;        // the server reads five rank spells per talent
constexpr int pointsPerTier = 5;   // fixed by client and server: row N opens at N*5 points spent in that tree
constexpr int firstLevel = 10, maxLevel = 60;
inline int pointsAt(int level) { return std::clamp(level - firstLevel + 1, 0, maxLevel - firstLevel + 1); }
inline int levelFor(int points) { return points <= 0 ? 1 : points + firstLevel - 1; }
inline int requiredPoints(int row) { return row * pointsPerTier; }
}
struct Talent {
  Id id = 0;
  int row = 0, column = 0;  // 0-based
  QVector<Id> ranks;        // the spell each rank teaches, first to last
  Id prerequisite = 0;      // a talent of the same tree, 0: none
  int prerequisiteRank = 1; // points it needs in that talent (Talent.dbc stores it 0-based)
  quint32 flags = 0;        // kept as loaded; the client's single-rank talents have 1
  Id requiredSpell = 0;     // kept as loaded
  int maxRank() const { return int(ranks.size()); }
  bool operator==(Talent const&) const = default;
};
struct TalentTree {
  Id tab = 0;               // TalentTab.dbc row; 0 for a template
  QString key;              // templates: their name in the workspace
  QString name, background; // background: "WarriorArms" (Interface\TalentFrame\WarriorArms-TopLeft)
  Id icon = 0;
  quint32 classMask = 0;
  int order = 0;
  QVector<Talent> talents;
  Talent const* find(Id id) const;
  Talent* find(Id id);
  Talent const* at(int row, int column) const;
  int points(QHash<Id, int> const& build) const; // points the build spent in this tree
  bool operator==(TalentTree const&) const = default;
};
struct TalentClass { quint32 mask = 0; QString name; };
QVector<TalentClass> const& talentClasses(); // the classes with talent trees, Warrior first
QString className(quint32 mask);
// A problem of one talent (talent 0: the whole tree). Errors stop the tree going into the test client.
struct TalentProblem { Id talent = 0; QString text; bool error = true; };
// A prerequisite arrow routed the way the game draws it: through these cells, ending in an arrow into the talent.
struct TalentLink {
  enum class Arrow { Down, Left, Right }; // the way the arrow points into the talent
  Id from = 0, to = 0;
  QVector<QPoint> path; // (column, row) cells, from the prerequisite to the talent
  Arrow arrow = Arrow::Down;
  QString blocked;      // the game cannot draw it (it shows an error) because of this
};
struct TalentReach {
  bool reachable = false; // by the last level
  int points = 0;         // fewest points the character spends to learn its first rank (that point included)
  int level = 0;          // the earliest level that gives those points
  int treePoints = 0;     // points this tree needs before its row opens
  QVector<Id> path;       // talents to learn first, earliest first
  QString why;            // why it cannot be reached
};
struct TalentChange {
  enum class Kind { Added, Removed, Moved, Ranks, Spells, Prerequisite };
  Kind kind = Kind::Added;
  Id talent = 0;
  QString text;
};
// Talent.dbc of the 1.12 client and server: 21 columns (ID, tab, row, column, 9 rank spells, 3 prerequisites,
// 3 prerequisite ranks, flags, required spell). The server reads five ranks and the first prerequisite.
namespace TalentDbc {
constexpr int columns = 21;
QVector<quint32> record(Talent const&, Id tab);
Talent fromRecord(Wdbc const&, int row);
}
// Database-free talent rules: tables, layout checks, reachability, the build preview and comparison.
namespace TalentLogic {
// The trees of TalentTab.dbc with their talents from Talent.dbc, by class then order.
QVector<TalentTree> trees(Wdbc const& tabs, Wdbc const& talents);
// `base` (a Talent.dbc) with every talent of the given trees replaced by theirs.
QByteArray talentTable(QByteArray const& base, QVector<TalentTree> const& replaced);
std::optional<TalentLink> link(TalentTree const&, Talent const&);
QVector<TalentLink> links(TalentTree const&);
// `spellOwner`: the rank spells of every other active tree (spell -> "Fury: Flurry"); `spells`: spells that
// exist (empty: not checked).
QVector<TalentProblem> check(TalentTree const&, QHash<Id, QString> const& spellOwner = {}, QSet<Id> const& spells = {});
// Whether `talent` could have `prerequisite` (same tree, earlier row or same row, no circle); "" if it can.
QString cannotRequire(TalentTree const&, Id talent, Id prerequisite);
// How early a character can learn `talent`, and through which prerequisites.
TalentReach reach(TalentTree const&, Id talent);
// The build preview: may the build learn the next rank of `talent` with `available` unspent points, or give
// one back? "" when it may; otherwise what stops it, in the game's words where it has some.
QString cannotLearn(TalentTree const&, QHash<Id, int> const& build, Id talent, int available);
QString cannotUnlearn(TalentTree const&, QHash<Id, int> const& build, Id talent);
// Drops the points an edited tree no longer allows (removed talents, fewer ranks, broken requirements).
QHash<Id, int> repair(TalentTree const&, QHash<Id, int> build);
// What changed from `original` to `modified`; `name(talent)` labels talents of either.
QVector<TalentChange> diff(TalentTree const& original, TalentTree const& modified, std::function<QString(Talent const&)> const& name);
// Moves `talent` to the cell, swapping with a talent already there. False when it cannot go there.
bool move(TalentTree&, Id talent, int row, int column);
// The first empty cell, row by row ({-1,-1} if the tree is full).
QPoint freeCell(TalentTree const&, int fromRow = 0);
// Every rank spell of `trees` except tree `except`'s, labelled with where it is ("the talent at row 2, column 3 of Warrior Fury").
QHash<Id, QString> spellOwners(QVector<TalentTree> const& trees, Id except);
QJsonObject toJson(TalentTree const&);
TalentTree fromJson(QJsonObject const&);
}
// The designer's talent trees: the server's own tables (kept as the originals) with the edited trees over them,
// and templates (cloned trees to experiment on). Stored in Workspace/creator-talents.json, apart from the
// database: trees are client and server tables, not world rows. A local test puts Talent.dbc into the local
// server's data (the original backed up) and into the test client's patch (see ClientPatchService).
class TalentStore final : public QObject {
  Q_OBJECT
public:
  static TalentStore* instance(); // null outside a managed Creator runtime
  TalentStore(QString const& root, QObject* parent = nullptr); // root: the Creator bundle
  QVector<TalentTree> originals() const; // throws when the server's tables cannot be read
  QVector<TalentTree> current() const;   // the originals with edited trees in their place
  bool modified(Id tab) const;
  QVector<Id> modifiedTabs() const;
  void save(TalentTree const&);          // a class tree (tab != 0) or a template (key)
  void reset(Id tab);                    // back to the original
  QVector<TalentTree> templates() const;
  void removeTemplate(QString const& key);
  Id nextTalentId() const;               // above every talent ID the originals, trees and templates use
  // Every rank spell of every active tree except `tab`'s, for the duplicate check.
  QHash<Id, QString> spellOwners(Id tab) const;
  // Problems of the edited trees that stop a test.
  QStringList blockingProblems() const;

  // Local test: the server's Talent.dbc.
  QString serverTable() const;      // the file the local server reads
  bool serverCurrent() const;       // the server has the edited trees (or no edits and its original)
  void installServer();             // before the local server restarts for a test
  void restoreServer();             // puts the server's own table back
  // The test client's Talent.dbc: the client's own copy with the edited trees over it; nullopt when nothing changed.
  std::optional<QByteArray> clientTable(QByteArray const& clientBase) const;
  QString changeHash() const;       // identifies the edited trees (for the client patch state)
  QString changeLabel() const;      // "Talent trees: Warrior Arms, Mage Fire"
  QByteArray exportTable() const;   // the server's Talent.dbc with the edited trees, for a production server
signals:
  void changed();
private:
  QString _root, _file, _state, _backups;
  QJsonObject load() const;
  void store(QJsonObject const&);
  QByteArray originalBytes(QString const& file) const;
};
}
