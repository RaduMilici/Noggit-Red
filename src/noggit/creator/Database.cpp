#include "Database.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <noggit/runtime/RuntimeManager.hpp>
#include <stdexcept>
#ifdef USE_MYSQL_UID_STORAGE
#include <mysql.h>
#endif
namespace Noggit::Creator {
namespace { void fail(QString const& s) { throw std::runtime_error(s.toStdString()); }
QString identifier(QString const& s) { for (auto c : s) if (!c.isLetterOrNumber() && c != '_') fail("Invalid internal database identifier."); return '`' + s + '`'; }
}
struct Database::Impl {
#ifdef USE_MYSQL_UID_STORAGE
  MYSQL* connection = nullptr;
  ~Impl() { if (connection) mysql_close(connection); }
#endif
};
Database::Database() : _impl(std::make_unique<Impl>()) {
  auto runtime = Runtime::RuntimeManager::instance();
  if (!runtime || !qApp->property("creatorDatabaseReady").toBool()) fail("Start the local database in LOCAL SERVER before editing content.");
#ifdef USE_MYSQL_UID_STORAGE
  _impl->connection = mysql_init(nullptr);
  if (!_impl->connection) fail("Cannot initialize the local database connection.");
  unsigned timeout = 3;
  mysql_options(_impl->connection, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  mysql_options(_impl->connection, MYSQL_OPT_READ_TIMEOUT, &timeout);
  mysql_options(_impl->connection, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
  if (!mysql_real_connect(_impl->connection, "127.0.0.1", "creator", "creator-local", "mangos", 13306, nullptr, 0))
    fail("Local database is unavailable. Check LOCAL SERVER.");
  mysql_set_character_set(_impl->connection, "utf8mb4");
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
#else
  fail("This Noggit build does not include local database support.");
#endif
}
Database::~Database() {
  if (!_committed && !_undo.isEmpty()) {
    try { restore(_undo); QFile::remove(_journal); } catch (...) { /* retained for recovery on next connection */ }
  }
}
QString Database::quote(QVariant const& value) const {
  if (value.isNull()) return "NULL";
#ifdef USE_MYSQL_UID_STORAGE
  QByteArray bytes = value.toString().toUtf8(), escaped(bytes.size()*2+1, '\0');
  auto size = mysql_real_escape_string(_impl->connection, escaped.data(), bytes.constData(), bytes.size());
  escaped.resize(size); return "'" + QString::fromUtf8(escaped) + "'";
#else
  return {};
#endif
}
QVector<Fields> Database::query(QString const& sql) {
  QVector<Fields> rows;
#ifdef USE_MYSQL_UID_STORAGE
  auto bytes = sql.toUtf8();
  if (mysql_real_query(_impl->connection, bytes.constData(), bytes.size()))
    fail("Local content save failed: " + QString::fromUtf8(mysql_error(_impl->connection)));
  MYSQL_RES* result = mysql_store_result(_impl->connection);
  if (!result) { if (mysql_field_count(_impl->connection)) fail("Cannot read local content."); return rows; }
  auto fields = mysql_fetch_fields(result); unsigned count = mysql_num_fields(result);
  while (auto row = mysql_fetch_row(result)) {
    Fields values; auto lengths = mysql_fetch_lengths(result);
    for (unsigned i=0;i<count;++i) values[QString::fromUtf8(fields[i].name)] = row[i] ? QVariant(QString::fromUtf8(row[i], lengths[i])) : QVariant();
    rows.push_back(values);
  }
  mysql_free_result(result);
#endif
  return rows;
}
void Database::exec(QString const& sql) { query(sql); }
std::vector<std::vector<std::optional<std::string>>> Database::rows(std::string const& sql) {
  std::vector<std::vector<std::optional<std::string>>> rows;
#ifdef USE_MYSQL_UID_STORAGE
  if (mysql_real_query(_impl->connection, sql.data(), sql.size()))
    fail("Reading local content failed: " + QString::fromUtf8(mysql_error(_impl->connection)));
  MYSQL_RES* result = mysql_store_result(_impl->connection);
  if (!result) { if (mysql_field_count(_impl->connection)) fail("Cannot read local content."); return rows; }
  unsigned count = mysql_num_fields(result);
  while (auto row = mysql_fetch_row(result)) {
    auto lengths = mysql_fetch_lengths(result); std::vector<std::optional<std::string>> values;
    for (unsigned i=0;i<count;++i) values.push_back(row[i] ? std::optional<std::string>(std::string(row[i], lengths[i])) : std::nullopt);
    rows.push_back(std::move(values));
  }
  mysql_free_result(result);
#endif
  return rows;
}
std::uint64_t Database::execute(std::string const& sql) {
#ifdef USE_MYSQL_UID_STORAGE
  if (mysql_real_query(_impl->connection, sql.data(), sql.size()))
    fail("Local content save failed: " + QString::fromUtf8(mysql_error(_impl->connection)));
  if (auto result = mysql_store_result(_impl->connection)) mysql_free_result(result);
  return mysql_affected_rows(_impl->connection);
#else
  return 0;
#endif
}
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
