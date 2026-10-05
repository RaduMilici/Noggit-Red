#include "Database.hpp"
#include "SqlConnection.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <noggit/runtime/RuntimeManager.hpp>
#include <stdexcept>
namespace Noggit::Creator {
namespace { void fail(QString const& s) { throw std::runtime_error(s.toStdString()); }
QString identifier(QString const& s) { for (auto c : s) if (!c.isLetterOrNumber() && c != '_') fail("Invalid internal database identifier."); return '`' + s + '`'; }
}
struct Database::Impl { std::optional<SqlConnection> connection; };
Database::Database() : _impl(std::make_unique<Impl>()) {
  auto runtime = Runtime::RuntimeManager::instance();
  if (!runtime || !qApp->property("creatorDatabaseReady").toBool()) fail("Start the local database in LOCAL SERVER before editing content.");
  try { _impl->connection.emplace(Endpoint{"127.0.0.1", 13306, "creator", "creator-local", "mangos"}, "Local database"); }
  catch (std::exception const&) { fail("Local database is unavailable. Check LOCAL SERVER."); }
  auto locked = query("SELECT GET_LOCK('noggit_creator_authoring',3) AS locked");
  if (locked.isEmpty() || locked[0]["locked"].toInt() != 1) fail("Another local save is in progress. Please try again.");
  exec("CREATE TABLE IF NOT EXISTS creator_content (kind VARCHAR(20) NOT NULL, entry INT UNSIGNED NOT NULL, PRIMARY KEY(kind,entry)) ENGINE=InnoDB");
  _journal = runtime->root() + "/Workspace/creator-recovery.json";
  // Loading the tracker first lets it drop a change list staged by a save that is about to be rolled back.
  auto tracker = ChangeTracker::instance();
  QFile journal(_journal);
  if (journal.exists()) {
    if (!journal.open(QIODevice::ReadOnly)) fail("Cannot read the interrupted-save recovery file.");
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(journal.readAll(), &error); journal.close();
    if (error.error != QJsonParseError::NoError || !doc.isArray()) fail("The interrupted-save recovery file is invalid.");
    restore(doc.array());
    if (tracker) tracker->discard();
    if (!QFile::remove(_journal)) fail("Cannot finish recovery of the interrupted save.");
  }
}
Database::~Database() {
  if (!_committed && !_undo.isEmpty()) {
    try { restore(_undo); QFile::remove(_journal); } catch (...) { /* retained for recovery on next connection */ }
  }
}
QString Database::quote(QVariant const& value) const { return _impl->connection->quote(value); }
QVector<Fields> Database::query(QString const& sql) { return _impl->connection->query(sql); }
void Database::exec(QString const& sql) { query(sql); }
std::vector<std::vector<std::optional<std::string>>> Database::rows(std::string const& sql) { return _impl->connection->rows(sql); }
std::uint64_t Database::execute(std::string const& sql) { return _impl->connection->execute(sql); }
void Database::insert(QString const& table, Fields const& fields) {
  QStringList keys, values;
  for (auto it=fields.begin();it!=fields.end();++it) { keys << identifier(it.key()); values << quote(it.value()); }
  exec("INSERT INTO " + identifier(table) + " (" + keys.join(',') + ") VALUES (" + values.join(',') + ")");
}
Id Database::allocate(QString const& table, QString const& key, Id limit) {
  auto rows = query("SELECT COALESCE(MAX("+identifier(key)+"),0) AS maximum FROM "+identifier(table));
  auto next = std::max<qulonglong>(1000000, rows[0]["maximum"].toULongLong()+1);
  if (next>limit) fail("There is no free local identifier available for this content.");
  return static_cast<Id>(next);
}
bool Database::owned(QString const& kind, Id id) { return !query("SELECT entry FROM creator_content WHERE kind="+quote(kind)+" AND entry="+QString::number(id)).isEmpty(); }
void Database::mark(QString const& kind, Id id) { exec("INSERT IGNORE INTO creator_content(kind,entry) VALUES("+quote(kind)+","+QString::number(id)+")"); }
void Database::persist() {
  QSaveFile file(_journal); auto bytes = QJsonDocument(_undo).toJson();
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()) fail("Cannot protect this save with a recovery file.");
}
void Database::snapshot(QString const& table, QString const& key, Id id) {
  for (auto const& v : _undo) { auto obj=v.toObject(); if (obj["table"]==table && obj["key"]==key && obj["id"].toDouble()==id) return; }
  QJsonArray rows;
  for (auto const& row : query("SELECT * FROM "+identifier(table)+" WHERE "+identifier(key)+"="+QString::number(id))) rows.append(QJsonObject::fromVariantMap(row));
  _undo.append(QJsonObject{{"table",table},{"key",key},{"id",double(id)},{"rows",rows}}); persist();
}
void Database::snapshotWhere(QString const& table, QString const& where) {
  for (auto const& v : _undo) { auto obj=v.toObject(); if (obj["table"]==table && obj["where"]==where) return; }
  QJsonArray rows;
  for (auto const& row : query("SELECT * FROM "+identifier(table)+" WHERE "+where)) rows.append(QJsonObject::fromVariantMap(row));
  _undo.append(QJsonObject{{"table",table},{"where",where},{"rows",rows}}); persist();
}
// Newest first, so where before-images overlap the oldest one is what remains.
void Database::restore(QJsonArray const& undo) {
  for (int i=undo.size()-1;i>=0;--i) {
    auto object=undo[i].toObject(); QString table=object["table"].toString(), key=object["key"].toString();
    if (object.contains("where")) exec("DELETE FROM "+identifier(table)+" WHERE "+object["where"].toString());
    else exec("DELETE FROM "+identifier(table)+" WHERE "+identifier(key)+"="+QString::number(qulonglong(object["id"].toDouble())));
    for (auto const& row : object["rows"].toArray()) insert(table, row.toObject().toVariantMap());
  }
}
void Database::track(EntityType type, Id id) {
  for (auto const& t : _tracked) if (t.type==type && t.id==id) return;
  Tracked t{type,id,{},{}};
  t.before=ChangeTracker::capture([this](QString const& sql){return query(sql);},type,id,&t.label);
  _tracked.push_back(t);
}
// The recovery journal is the commit marker: the change list is staged before it is removed
// and published after, so Local Changes never lists a save that was rolled back.
void Database::commit() {
  auto tracker=ChangeTracker::instance();
  if (tracker && !_tracked.isEmpty()) {
    QVector<TrackedChange> changes;
    for (auto const& t : _tracked) {
      TrackedChange c; c.type=t.type; c.entity=t.id; c.before=t.before;
      c.after=ChangeTracker::capture([this](QString const& sql){return query(sql);},t.type,t.id,&c.label);
      if (c.label.isEmpty()) c.label=t.label;
      changes.push_back(c);
    }
    tracker->stage(changes);
  }
  if (!_undo.isEmpty() && !QFile::remove(_journal)) { if (tracker) tracker->discard(); fail("Cannot finish the local save."); }
  _committed=true;
  if (tracker) tracker->promote();
}
}
