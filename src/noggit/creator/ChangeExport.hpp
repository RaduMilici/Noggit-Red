#pragma once
#include "ChangeTracker.hpp"
namespace Noggit::Creator {
struct ExportResult { QString folder; int changes = 0, dependencies = 0; };
class ExportService {
public:
  static QString folderName(QString const& packageName); // "Haunted Mill" -> "Haunted-Mill"
  // Writes <folder>/<folderName>/{manifest.json,changes.sql} from the tracked after-states.
  // Creator NPCs, items and quests the changes rely on are included so the package is
  // self-contained; finding them needs the local database.
  static ExportResult exportChanges(QString const& name, QString const& author, QString const& folder,
                                    QVector<TrackedChange> const& changes);
};
}
