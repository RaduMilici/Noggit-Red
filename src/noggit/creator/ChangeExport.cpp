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
#include <QSet>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
void require(bool b, QString const& message) { if (!b) throw std::runtime_error(message.toStdString()); }
void writeFile(QString const& path, QByteArray const& bytes) {
  QSaveFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(), "Cannot write the change package.");
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

  auto all = changes; QSet<Id> dependencies; QJsonObject source;
  {
    Database db;
    QSet<Id> npcs; QVector<Id> needed;
    for (auto const& c : changes) if (c.type == EntityType::Npc) npcs.insert(c.entity);
    auto need = [&](Id id) { if (id && !npcs.contains(id)) { npcs.insert(id); needed.push_back(id); } };
    for (auto const& c : changes) {
      if (c.type == EntityType::Spawn)
        for (auto const& row : c.after["creature"].toArray()) need(row.toObject()["id"].toString().toUInt());
      if (c.type == EntityType::Quest) {
        for (auto table : {"creature_questrelation", "creature_involvedrelation"})
          for (auto const& row : c.after[table].toArray()) need(row.toObject()["id"].toString().toUInt());
        auto quest = c.after["quest_template"].toArray().at(0).toObject();
        for (int i = 1; i <= 4; ++i) { auto target = quest["ReqCreatureOrGOId" + QString::number(i)].toString().toInt(); if (target > 0) need(Id(target)); }
      }
    }
    // Original NPCs exist on every compatible database; Creator NPCs must travel with the package.
    for (auto id : needed) {
      if (!db.owned("npc", id)) continue;
      TrackedChange d; d.type = EntityType::Npc; d.entity = id; d.action = ChangeAction::Create;
      d.after = ChangeTracker::capture([&db](QString const& sql) { return db.query(sql); }, EntityType::Npc, id, &d.label);
      if (d.after.isEmpty()) continue;
      all.push_back(d); dependencies.insert(id);
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
  return {target, changes.size(), dependencies.size()};
}
}
