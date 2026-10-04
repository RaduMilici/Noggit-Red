#pragma once
#include "ClientDbc.hpp"
#include <QObject>
#include <QStringList>
#include <optional>
namespace Noggit::Creator {
// A client-data difference between what Creator's spells need and what the local test client has.
struct ClientDataChange {
  enum class Kind { Add, Change, Remove };
  Kind kind = Kind::Add;
  Id spell = 0;
  QString label;
  QString summary() const; // "+ Spell.dbc: Holy Smite (Rank 2)"
};
struct ClientDataStatus {
  QString data;                       // the test client's Data folder; empty when no client is chosen
  QString patch;                      // the patch file Creator manages there (the highest one the client loads)
  bool installed = false;             // Creator's test patch is in the client now
  QString backup;                     // the client's own patch Creator replaced, kept in the workspace ("" if none)
  QVector<ClientDataChange> pending;  // spells the installed test patch does not match yet
  QStringList installedSpells;        // labels of the Creator spells in the installed patch
  QString problem;
};
// Client data for local testing. Items need none (the client asks the server for them), but spells must be
// in the client's Spell.dbc. Creator generates those rows from spell_template and puts them in the
// highest patch the client loads (patch-Z): the client's own patch-Z, if any, is backed up first and its
// files are kept inside Creator's version. Playing on production puts the original back. Tracked apart from
// Local Changes in Workspace/client-data/state.json; never sent to production by Sync.
class ClientPatchService final : public QObject {
  Q_OBJECT
public:
  static ClientPatchService* instance(); // null outside a managed Creator runtime
  explicit ClientPatchService(QString const& workspace, QObject* parent = nullptr);
  ClientDataStatus status(QString const& data, QVector<Fields> const& spells) const;
  ClientDataStatus status() const;                // for the configured test client and the local database
  // Builds the test patch from the local database's Creator spells and installs it (before a local test).
  void install();
  void install(QString const& data, QVector<Fields> const& spells);
  // Puts the client's own patch back (before playing on production, or on request).
  void restoreOriginal();
  void restoreOriginal(QString const& data);
  // The test patch as a standalone file, e.g. to hand to production players.
  void exportPatch(QString const& path);
  // A lookup table as the client has it without Creator's patch (cached), e.g. "SpellCastTimes.dbc".
  std::optional<Wdbc> table(QString const& file) const;
  std::optional<Wdbc> table(QString const& data, QString const& file) const;
  static QString dataFolder(); // the configured test client's Data folder ("" when none)

  // Database-free parts.
  // Spell.dbc with `spells` (spell_template rows) put over `base`.
  static QByteArray spellTable(QByteArray const& base, QVector<Fields> const& spells);
  // The client's archives in load order (later ones win), as file paths that exist.
  static QStringList archives(QString const& data);
signals:
  void changed();
private:
  QString _state, _backups;
  mutable QHash<QString, Wdbc> _tables;
  QByteArray build(QString const& data, QVector<Fields> const& spells, QString const& output) const;
};
}
