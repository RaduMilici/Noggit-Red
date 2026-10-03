#pragma once
#include "ChangeTracker.hpp"
#include <QJsonObject>
namespace Noggit::Creator {
struct ExportResult { QString folder; int changes = 0, dependencies = 0; };
// Tracked changes plus the Creator NPCs, items and quests they rely on, as one self-contained SQL script.
struct ChangePackage {
  QVector<TrackedChange> changes; // the tracked changes first, then dependencies
  int tracked = 0;
  QVector<bool> owned;            // per change: Creator content in the local world (deleted content is not)
  QString sql;
  QJsonObject source;             // local runtime and content version
  QJsonObject manifest(QString const& name, QString const& author) const;
};
class ExportService {
public:
  static QString folderName(QString const& packageName); // "Haunted Mill" -> "Haunted-Mill"
  // Finding dependencies and ownership needs the local database.
  static ChangePackage build(QVector<TrackedChange> const& changes);
  // The local runtime and world content version, as recorded in manifests.
  static QJsonObject localSource();
  // Writes `target`/{manifest.json,changes.sql} plus `extra` files; `target` must not exist yet.
  static void write(ChangePackage const& package, QString const& name, QString const& author, QString const& target,
                    QJsonObject const& manifestExtra = {}, QMap<QString, QByteArray> const& extra = {});
  // Writes <folder>/<folderName>/{manifest.json,changes.sql} from the tracked after-states.
  static ExportResult exportChanges(QString const& name, QString const& author, QString const& folder,
                                    QVector<TrackedChange> const& changes);
};
}
