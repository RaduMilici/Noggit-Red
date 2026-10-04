#include "ClientPatch.hpp"
#include "Database.hpp"
#include <noggit/runtime/RuntimeManager.hpp>
#include <StormLib.h>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <cstdio>
#include <memory>
#include <stdexcept>
namespace Noggit::Creator {
namespace {
void require(bool condition, QString const& message) { if (!condition) throw std::runtime_error(message.toStdString()); }
QString const spellFile = "DBFilesClient\\Spell.dbc";
QString const patchName = "patch-Z.mpq"; // the highest patch the 1.x client loads (patch-2..9, then patch-A..Z)
QByteArray native(QString const& path) { return QFile::encodeName(QDir::toNativeSeparators(path)); }
QString sha(QString const& path) {
  QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256); hash.addData(&file); return hash.result().toHex();
}
struct Archive {
  HANDLE handle = nullptr;
  explicit Archive(QString const& path, bool write = false) {
    SFileOpenArchive(native(path).constData(), 0, write ? 0 : STREAM_FLAG_READ_ONLY, &handle);
  }
  ~Archive() { if (handle) SFileCloseArchive(handle); }
  std::optional<QByteArray> read(QString const& name) const {
    HANDLE file = nullptr;
    if (!handle || !SFileOpenFileEx(handle, name.toLatin1().constData(), SFILE_OPEN_FROM_MPQ, &file)) return {};
    DWORD high = 0; auto size = SFileGetFileSize(file, &high);
    QByteArray bytes(int(size), '\0'); DWORD done = 0;
    bool ok = high == 0 && SFileReadFile(file, bytes.data(), size, &done, nullptr) && done == size;
    SFileCloseFile(file);
    return ok ? std::optional<QByteArray>(bytes) : std::nullopt;
  }
};
// The file in `folder` whose name matches case-insensitively (the client is case-insensitive, Linux is not).
QString findFile(QString const& folder, QString const& name) {
  for (auto const& entry : QDir(folder).entryList(QDir::Files)) if (entry.compare(name, Qt::CaseInsensitive) == 0) return folder + "/" + entry;
  return {};
}
QString recordHash(Fields const& spell) {
  QHash<int, QString> strings; auto record = SpellDbc::record(spell, strings);
  QByteArray bytes; for (auto cell : record) bytes += QByteArray::number(cell) + ',';
  for (int column : {120, 129, 138, 147}) bytes += strings.value(column).toUtf8() + '\0';
  return QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex();
}
QString spellLabel(Fields const& spell) {
  auto rank = spell.value("nameSubtext").toString();
  return spell.value("name").toString() + (rank.isEmpty() ? QString() : " (" + rank + ")");
}
// Moves `from` over `to`; on Windows a file the game or Noggit holds open cannot be replaced.
void replace(QString const& from, QString const& to) {
  if (std::rename(native(from).constData(), native(to).constData()) == 0) return;
  QFile::remove(from);
  throw std::runtime_error(("Cannot replace " + QFileInfo(to).fileName() + " in the client's Data folder. Close WoW (and on Windows, Noggit's view of that client) and try again.").toStdString());
}
QVector<Fields> creatorSpells() {
  Database db;
  return db.query("SELECT * FROM spell_template WHERE entry IN (SELECT entry FROM creator_content WHERE kind='spell') ORDER BY entry");
}
}
QString ClientDataChange::summary() const {
  return QString(kind == Kind::Add ? "+" : kind == Kind::Remove ? "-" : "~") + " Spell.dbc: " + label;
}
ClientPatchService* ClientPatchService::instance() {
  auto runtime = Runtime::RuntimeManager::instance();
  if (!runtime) return nullptr;
  if (auto service = runtime->findChild<ClientPatchService*>("creatorClientData", Qt::FindDirectChildrenOnly)) return service;
  auto service = new ClientPatchService(runtime->root() + "/Workspace", runtime);
  service->setObjectName("creatorClientData");
  return service;
}
ClientPatchService::ClientPatchService(QString const& workspace, QObject* parent)
  : QObject(parent), _state(workspace + "/client-data/state.json"), _backups(workspace + "/client-data/original") {}
QString ClientPatchService::dataFolder() {
  auto runtime = Runtime::RuntimeManager::instance();
  if (!runtime) return {};
  QSettings settings(runtime->root() + "/Workspace/runtime.ini", QSettings::IniFormat);
  auto exe = settings.value("clientExecutable").toString();
  if (exe.isEmpty()) return {};
  auto folder = QFileInfo(QDir::cleanPath(QDir(runtime->root()).absoluteFilePath(exe))).absolutePath();
  for (auto const& entry : QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) if (entry.compare("Data", Qt::CaseInsensitive) == 0) return folder + "/" + entry;
  return folder + "/Data";
}
QStringList ClientPatchService::archives(QString const& data) {
  QStringList names{"backup.MPQ", "base.MPQ", "dbc.MPQ", "fonts.MPQ", "interface.MPQ", "misc.MPQ", "model.MPQ", "sound.MPQ",
                    "speech.MPQ", "terrain.MPQ", "texture.MPQ", "wmo.MPQ", "patch.MPQ"};
  for (char c = '2'; c <= '9'; ++c) names << QString("patch-%1.MPQ").arg(c);
  for (char c = 'A'; c <= 'Z'; ++c) names << QString("patch-%1.MPQ").arg(c);
  QStringList out;
  for (auto const& name : names) if (auto path = findFile(data, name); !path.isEmpty()) out << path;
  return out;
}
QByteArray ClientPatchService::spellTable(QByteArray const& base, QVector<Fields> const& spells) {
  auto table = Wdbc::parse(base);
  require(table.columns() == SpellDbc::columns, QString("The client's Spell.dbc has %1 columns; Creator supports the 1.12 layout (%2).").arg(table.columns()).arg(SpellDbc::columns));
  for (auto const& spell : spells) { QHash<int, QString> strings; auto record = SpellDbc::record(spell, strings); table.put(record, strings); }
  return table.bytes();
}
namespace {
struct State {
  QJsonObject json;
  QString installed() const { return json["installed"].toString(); }
  QString backup() const { return json["backup"].toObject()["file"].toString(); }
};
State loadState(QString const& path) {
  QFile file(path); State s;
  if (file.open(QIODevice::ReadOnly)) s.json = QJsonDocument::fromJson(file.readAll()).object();
  return s;
}
void saveState(QString const& path, State const& s) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile file(path); auto bytes = QJsonDocument(s.json).toJson();
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(), "Cannot record the test client's state.");
}
// The client's own patch in Creator's slot: the file there if it is not Creator's, else its backup ("" if none).
QString originalPatch(QString const& data, State const& state) {
  auto patch = findFile(data, patchName);
  if (patch.isEmpty()) return {};
  if (state.installed().isEmpty() || sha(patch) != state.installed()) return patch;
  return QFileInfo::exists(state.backup()) ? state.backup() : QString();
}
// The client's archives as they are without Creator's patch.
QStringList originalArchives(QString const& data, State const& state) {
  auto patch = findFile(data, patchName), own = originalPatch(data, state);
  QStringList out;
  for (auto const& archive : ClientPatchService::archives(data)) {
    if (archive != patch) out << archive;
    else if (!own.isEmpty()) out << own;
  }
  return out;
}
std::optional<QByteArray> effective(QStringList const& archives, QString const& file) {
  for (int i = archives.size() - 1; i >= 0; --i) if (auto bytes = Archive(archives[i]).read(file)) return bytes;
  return {};
}
}
std::optional<Wdbc> ClientPatchService::table(QString const& data, QString const& file) const {
  auto key = data + "|" + file;
  if (auto it = _tables.find(key); it != _tables.end()) return *it;
  auto bytes = effective(originalArchives(data, loadState(_state)), "DBFilesClient\\" + file);
  if (!bytes) return {};
  try { auto table = Wdbc::parse(*bytes); _tables.insert(key, table); return table; } catch (std::exception const&) { return {}; }
}
std::optional<Wdbc> ClientPatchService::table(QString const& file) const {
  auto data = dataFolder();
  return data.isEmpty() ? std::nullopt : table(data, file);
}
ClientDataStatus ClientPatchService::status(QString const& data, QVector<Fields> const& spells) const {
  ClientDataStatus s; s.data = data;
  if (data.isEmpty() || !QFileInfo(data).isDir()) { s.problem = "Choose the WoW client in Client Profiles to test spells locally."; return s; }
  auto state = loadState(_state);
  auto patch = findFile(data, patchName);
  s.patch = patch.isEmpty() ? data + "/" + patchName : patch;
  s.installed = !patch.isEmpty() && !state.installed().isEmpty() && sha(patch) == state.installed();
  s.backup = QFileInfo::exists(state.backup()) ? state.backup() : QString();
  auto installed = s.installed ? state.json["spells"].toObject() : QJsonObject();
  for (auto it = installed.begin(); it != installed.end(); ++it) s.installedSpells << it.value().toObject()["label"].toString();
  QSet<QString> current;
  for (auto const& spell : spells) {
    auto id = spell.value("entry").toString(); current.insert(id);
    if (!installed.contains(id)) s.pending.push_back({ClientDataChange::Kind::Add, spell.value("entry").toUInt(), spellLabel(spell)});
    else if (installed[id].toObject()["hash"].toString() != recordHash(spell)) s.pending.push_back({ClientDataChange::Kind::Change, spell.value("entry").toUInt(), spellLabel(spell)});
  }
  for (auto it = installed.begin(); it != installed.end(); ++it)
    if (!current.contains(it.key())) s.pending.push_back({ClientDataChange::Kind::Remove, it.key().toUInt(), it.value().toObject()["label"].toString()});
  return s;
}
ClientDataStatus ClientPatchService::status() const {
  QVector<Fields> spells;
  try { spells = creatorSpells(); } catch (std::exception const& e) { auto s = status(dataFolder(), {}); s.problem = QString::fromUtf8(e.what()); return s; }
  return status(dataFolder(), spells);
}
QByteArray ClientPatchService::build(QString const& data, QVector<Fields> const& spells, QString const& output) const {
  auto state = loadState(_state);
  auto sources = originalArchives(data, state);
  auto base = effective(sources, spellFile);
  require(base.has_value(), "The client has no Spell.dbc.");
  auto table = spellTable(*base, spells);
  // The client's own patch-Z, whose files Creator's version keeps.
  auto own = originalPatch(data, state);
  std::unique_ptr<Archive> original = own.isEmpty() ? nullptr : std::make_unique<Archive>(own);
  QVector<QPair<QByteArray, QByteArray>> files; // name, bytes
  if (original) {
    require(original->handle, "Cannot read the client's own " + patchName + ".");
    SFILE_FIND_DATA found; HANDLE search = SFileFindFirstFile(original->handle, "*", &found, nullptr);
    if (search) {
      do {
        QByteArray name(found.cFileName);
        if (name.startsWith('(') || name.compare(spellFile.toLatin1(), Qt::CaseInsensitive) == 0) continue;
        auto bytes = original->read(QString::fromLatin1(name));
        require(bytes.has_value(), "Cannot read " + QString::fromLatin1(name) + " from the client's own " + patchName + ".");
        files.push_back({name, *bytes});
      } while (SFileFindNextFile(search, &found));
      SFileFindClose(search);
    }
    require(!files.isEmpty() || original->read(spellFile), "The client's own " + patchName + " has no file list, so Creator cannot keep its files. Rebuild it with a (listfile).");
  }
  files.push_back({spellFile.toLatin1(), table});
  QFile::remove(output);
  HANDLE archive = nullptr;
  require(SFileCreateArchive(native(output).constData(), MPQ_CREATE_LISTFILE, DWORD(files.size() + 16), &archive), "Cannot create the test client patch.");
  bool ok = true;
  for (auto const& [name, bytes] : files) {
    HANDLE file = nullptr;
    ok = ok && SFileCreateFile(archive, name.constData(), 0, DWORD(bytes.size()), 0, MPQ_FILE_COMPRESS | MPQ_FILE_REPLACEEXISTING, &file)
            && SFileWriteFile(file, bytes.constData(), DWORD(bytes.size()), MPQ_COMPRESSION_ZLIB) && SFileFinishFile(file);
  }
  ok = SFileCloseArchive(archive) && ok;
  if (!ok) { QFile::remove(output); throw std::runtime_error("Cannot write the test client patch."); }
  return table;
}
void ClientPatchService::install(QString const& data, QVector<Fields> const& spells) {
  require(!data.isEmpty() && QFileInfo(data).isDir(), "Choose the WoW client in Client Profiles first.");
  auto state = loadState(_state);
  auto patch = findFile(data, patchName);
  bool ours = !patch.isEmpty() && !state.installed().isEmpty() && sha(patch) == state.installed();
  if (!patch.isEmpty() && !ours) {
    // The client's own patch: back it up before Creator replaces it. An older, different backup is kept too.
    QDir().mkpath(_backups);
    auto backup = _backups + "/" + QFileInfo(patch).fileName();
    auto current = sha(patch);
    if (QFileInfo::exists(backup) && sha(backup) != current)
      QFile::rename(backup, backup + "." + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss"));
    if (!QFileInfo::exists(backup)) require(QFile::copy(patch, backup) && sha(backup) == current, "Cannot back up the client's " + QFileInfo(patch).fileName() + ".");
    state.json["backup"] = QJsonObject{{"file", backup}, {"sha256", current}, {"from", patch}, {"saved", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    state.json["installed"] = QString();
    saveState(_state, state);
  }
  if (spells.isEmpty()) { restoreOriginal(data); return; }
  if (patch.isEmpty()) patch = data + "/" + patchName;
  auto fresh = patch + ".creator-new";
  build(data, spells, fresh);
  replace(fresh, patch);
  QJsonObject installed;
  for (auto const& spell : spells) installed[spell.value("entry").toString()] = QJsonObject{{"hash", recordHash(spell)}, {"label", spellLabel(spell)}};
  state = loadState(_state);
  state.json["installed"] = sha(patch); state.json["spells"] = installed; state.json["data"] = data;
  state.json["updated"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  saveState(_state, state);
  emit changed();
}
void ClientPatchService::install() {
  auto data = dataFolder();
  install(data, creatorSpells());
}
void ClientPatchService::restoreOriginal(QString const& data) {
  if (data.isEmpty()) return;
  auto state = loadState(_state);
  auto patch = findFile(data, patchName);
  bool ours = !patch.isEmpty() && !state.installed().isEmpty() && sha(patch) == state.installed();
  if (ours) {
    if (!state.backup().isEmpty() && QFileInfo::exists(state.backup())) {
      auto fresh = patch + ".creator-new";
      QFile::remove(fresh);
      require(QFile::copy(state.backup(), fresh), "Cannot restore the client's own " + QFileInfo(patch).fileName() + ".");
      replace(fresh, patch);
    } else require(QFile::remove(patch), "Cannot remove Creator's test patch from the client.");
  }
  if (!state.installed().isEmpty() || state.json.contains("spells")) {
    state.json["installed"] = QString(); state.json.remove("spells");
    saveState(_state, state);
    emit changed();
  }
}
void ClientPatchService::restoreOriginal() { restoreOriginal(dataFolder()); }
void ClientPatchService::exportPatch(QString const& path) {
  auto data = dataFolder();
  require(!data.isEmpty(), "Choose the WoW client in Client Profiles first.");
  auto spells = creatorSpells();
  require(!spells.isEmpty(), "There are no Creator spells to export.");
  build(data, spells, path);
}
}
