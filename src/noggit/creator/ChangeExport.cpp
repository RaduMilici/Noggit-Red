#include "ChangeExport.hpp"
#include "Database.hpp"
#include <noggit/runtime/RuntimeManager.hpp>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QPair>
#include <QSet>
#include <cstdlib>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
void require(bool b, QString const& message) { if (!b) throw std::runtime_error(message.toStdString()); }
void writeFile(QString const& path, QByteArray const& bytes) {
  QSaveFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(), "Cannot write the change package.");
}
Id number(QJsonObject const& row, QString const& column) { return Id(std::abs(row[column].toString().toInt())); }
// The NPCs, items and quests a change's after-state refers to.
QVector<QPair<EntityType, Id>> references(TrackedChange const& c) {
  QVector<QPair<EntityType, Id>> out;
  auto rows = [&](QString const& table) { return c.after[table].toArray(); };
  auto first = rows(c.type == EntityType::Quest ? "quest_template" : c.type == EntityType::Npc ? "creature_template" : "creature").at(0).toObject();
  if (c.type == EntityType::Spawn) out.push_back({EntityType::Npc, number(first, "id")});
  if (c.type == EntityType::Npc)
    for (auto const& row : rows("creature_equip_template"))
      for (int i = 1; i <= 3; ++i) out.push_back({EntityType::Item, number(row.toObject(), "equipentry" + QString::number(i))});
  if (c.type == EntityType::Quest) {
    for (auto table : {"creature_questrelation", "creature_involvedrelation"})
      for (auto const& row : rows(table)) out.push_back({EntityType::Npc, number(row.toObject(), "id")});
    for (int i = 1; i <= 4; ++i)
      if (first["ReqCreatureOrGOId" + QString::number(i)].toString().toInt() > 0) out.push_back({EntityType::Npc, number(first, "ReqCreatureOrGOId" + QString::number(i))});
    for (auto table : {"quest_start_scripts", "quest_end_scripts"})
      for (auto const& row : rows(table))
        if (number(row.toObject(), "command") == 10) out.push_back({EntityType::Npc, number(row.toObject(), "datalong")}); // summon
    QStringList items{"SrcItemId"};
    for (int i = 1; i <= 4; ++i) items << "ReqItemId" + QString::number(i) << "RewItemId" + QString::number(i);
    for (int i = 1; i <= 6; ++i) items << "RewChoiceItemId" + QString::number(i);
    for (auto const& column : items) out.push_back({EntityType::Item, number(first, column)});
    for (auto const& row : rows("item_start_link")) out.push_back({EntityType::Item, number(row.toObject(), "entry")});
    for (auto column : {"PrevQuestId", "NextQuestId", "NextQuestInChain"}) out.push_back({EntityType::Quest, number(first, column)});
  }
  return out;
}
QJsonObject sourceVersion(Database& db) {
  QJsonObject source;
  QFile info(Runtime::RuntimeManager::instance()->root() + "/Runtime/creator-runtime.json");
  if (info.open(QIODevice::ReadOnly)) {
    auto runtime = QJsonDocument::fromJson(info.readAll()).object();
    for (auto key : {"server", "database", "platform"}) if (runtime.contains(key)) source[key] = runtime[key];
  }
  // Tortoise records applied world updates; the newest one identifies the content revision.
  try {
    auto rows = db.query("SELECT Id,Name,AppliedAt FROM migrations ORDER BY Id DESC LIMIT 1");
    if (!rows.isEmpty())
      source["contentVersion"] = QJsonObject{{"migration", rows[0]["Name"].toString()}, {"migrationId", rows[0]["Id"].toString()}, {"appliedAt", rows[0]["AppliedAt"].toString()}};
  } catch (std::exception const&) { /* not available on this database */ }
  return source;
}
}
QString ExportService::folderName(QString const& packageName) {
  return packageName.trimmed().replace(QRegularExpression("[^\\p{L}\\p{N}_]+"), "-").remove(QRegularExpression("^-+|-+$"));
}
ExportResult ExportService::exportChanges(QString const& name, QString const& author, QString const& folder, QVector<TrackedChange> const& changes) {
  require(!changes.isEmpty(), "There are no local changes to export.");
  auto title = name.trimmed();
  require(!title.isEmpty() && title.size() <= 80, "Enter a package name of up to 80 characters.");
  auto directory = folderName(title);
  require(!directory.isEmpty(), "Use at least one letter or number in the package name.");
  require(QFileInfo(folder).isDir(), "Choose an existing folder for the package.");
  auto target = QDir(folder).filePath(directory);
  require(!QFileInfo::exists(target), "A package named \"" + directory + "\" already exists in that folder. Choose another name or folder.");

  auto all = changes; QJsonObject source;
  {
    Database db;
    // Creator content the changes refer to travels with the package (and what that refers to, in turn);
    // the game's own content exists on every compatible database.
    QSet<QPair<int, Id>> known;
    for (auto const& c : changes) known.insert({int(c.type), c.entity});
    for (int i = 0; i < all.size(); ++i) {
      for (auto const& [type, id] : references(all[i])) {
        if (!id || known.contains({int(type), id})) continue;
        known.insert({int(type), id});
        if (!db.owned(toString(type), id)) continue;
        TrackedChange d; d.type = type; d.entity = id; d.action = ChangeAction::Create;
        d.after = ChangeTracker::capture([&db](QString const& sql) { return db.query(sql); }, type, id, &d.label);
        if (!d.after.isEmpty()) all.push_back(d);
      }
    }
    source = sourceVersion(db);
  }

  auto sql = ChangeTracker::sql(all).toUtf8();
  QJsonArray entities;
  for (int i = 0; i < all.size(); ++i) {
    auto const& c = all[i]; bool dependency = i >= changes.size();
    entities.append(QJsonObject{{"type", toString(c.type)}, {"id", double(c.entity)}, {"action", toString(c.action)},
                                {"name", c.label}, {"includedAsDependency", dependency}});
  }
  QJsonObject manifest{
    {"format", 1}, {"generator", "Noggit Creator"}, {"name", title}, {"author", author.trimmed()},
    {"created", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {"source", source}, {"entities", entities},
    {"files", QJsonObject{{"changes.sql", QJsonObject{{"sha256", QString(QCryptographicHash::hash(sql, QCryptographicHash::Sha256).toHex())}}}}}};

  // Build beside the target and rename, so a failed export never leaves a half-written package.
  auto partial = QDir(folder).filePath("." + directory + ".partial");
  QDir(partial).removeRecursively();
  try {
    require(QDir().mkpath(partial), "Cannot create the package folder.");
    writeFile(partial + "/manifest.json", QJsonDocument(manifest).toJson());
    writeFile(partial + "/changes.sql", sql);
    require(QDir().rename(partial, target), "Cannot create the package folder.");
  } catch (...) { QDir(partial).removeRecursively(); throw; }
  return {target, changes.size(), all.size() - changes.size()};
}
}
