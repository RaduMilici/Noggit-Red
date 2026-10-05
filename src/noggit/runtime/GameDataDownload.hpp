#pragma once
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <functional>
#include <memory>

namespace Noggit::Runtime {
// The manifest ships with Noggit. Payloads are streamed to disk, verified, then
// atomically committed. Completed files survive cancellation and offline restarts.
class GameDataDownload : public QObject {
public:
  explicit GameDataDownload(QString root, QObject* parent = nullptr, QNetworkAccessManager* network = nullptr)
    : QObject(parent), _root(std::move(root)), _network(network ? network : new QNetworkAccessManager(this)),
      _hash(QCryptographicHash::Sha256) {}
  ~GameDataDownload() override { cancel(); }
  std::function<void(qint64, qint64)> progress;
  std::function<void(QString)> finished;
  QString dataPath() const {
    QFile file(_root + "/Runtime/game-data.json");
    if (!file.open(QIODevice::ReadOnly)) return _root + "/Runtime/mangosd/data";
    return _root + "/Workspace/GameData/" + QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
  }
  void cancel() {
    if (_reply) { _reply->disconnect(this); _reply->abort(); _reply->deleteLater(); _reply = nullptr; }
    _output.reset(); _running = false;
  }
  void start() {
    cancel();
    QFile manifest(_root + "/Runtime/game-data.json");
    if (!manifest.exists()) { finished({}); return; } // Existing offline development bundles.
    if (!manifest.open(QIODevice::ReadOnly)) { finished("Cannot read game-data manifest."); return; }
    auto document = QJsonDocument::fromJson(manifest.readAll());
    auto object = document.object();
    _base = QUrl(object.value("baseUrl").toString());
    _files = object.value("files").toArray();
    _total = 0; _done = 0; _index = 0; _receipts = {};
    QSet<QString> paths;
    if (object.value("format").toInt() != 1 || _files.isEmpty() || _base.scheme() != "https"
        || _base.host().isEmpty() || !_base.path().endsWith('/') || _base.hasQuery() || _base.hasFragment()) {
      finished("Invalid game-data manifest or HTTPS download URL."); return;
    }
    for (auto value : _files) {
      auto entry = value.toObject(); auto path = entry.value("path").toString();
      auto parts = path.split('/'); auto sha = entry.value("sha256").toString();
      auto size = entry.value("size").toDouble();
      bool safe = parts.size() == 2 && QStringList{"dbc", "maps", "vmaps", "mmaps"}.contains(parts[0]);
      for (auto ch : path) safe &= (ch.isLetterOrNumber() && ch.unicode() < 128) || QString("/._-").contains(ch);
      if (!safe || parts.last() == "." || parts.last() == ".." || paths.contains(path.toLower())
          || size <= 0 || size > 1024.0 * 1024 * 1024 || size != qint64(size)
          || sha.size() != 64 || QByteArray::fromHex(sha.toLatin1()).size() != 32) {
        finished("Unsafe or invalid game-data manifest entry."); return;
      }
      paths.insert(path.toLower()); _total += qint64(size);
    }
    _destination = dataPath();
    if (!QDir().mkpath(_destination)) { finished("Cannot create game-data cache."); return; }
    QFile receipts(_destination + "/verified.jsonl");
    if (receipts.open(QIODevice::ReadOnly)) {
      while (!receipts.atEnd()) {
        auto record = QJsonDocument::fromJson(receipts.readLine()).object();
        auto path = record.value("path").toString();
        if (!path.isEmpty()) _receipts.insert(path, record.value("sha256"));
      }
    }
    _running = true;
    next();
  }
private:
  void complete(QString error) {
    cancel();
    if (finished) finished(error);
  }
  void next() {
    if (!_running) return;
    while (_index < _files.size()) {
      auto entry = _files[_index].toObject();
      auto relative = entry.value("path").toString();
      auto size = qint64(entry.value("size").toDouble());
      auto target = _destination + '/' + relative;
      if (_receipts.value(relative).toString() == entry.value("sha256").toString()
          && QFileInfo(target).isFile() && QFileInfo(target).size() == size) {
        _done += size; ++_index; continue;
      }
      if (!QDir().mkpath(QFileInfo(target).absolutePath())) { complete("Cannot create game-data directory."); return; }
      _output = std::make_unique<QSaveFile>(target);
      if (!_output->open(QIODevice::WriteOnly)) { complete("Cannot write game data: " + _output->errorString()); return; }
      _hash.reset(); _received = 0;
      QNetworkRequest request(_base.resolved(QUrl(relative)));
      request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
      request.setRawHeader("Accept-Encoding", "identity");
      _reply = _network->get(request);
      _reply->setReadBufferSize(1024 * 1024);
      auto timeout = new QTimer(_reply); timeout->setSingleShot(true); timeout->start(60000);
      connect(timeout, &QTimer::timeout, _reply, &QNetworkReply::abort);
      connect(_reply, &QIODevice::readyRead, this, [this, size, timeout] {
        timeout->start(60000);
        auto bytes = _reply->readAll();
        _received += bytes.size();
        if (_received > size || _output->write(bytes) != bytes.size()) {
          complete("Game-data download exceeded its expected size or disk write failed. Free space and retry."); return;
        }
        _hash.addData(bytes);
        if (progress) progress(_done + _received, _total);
      });
      connect(_reply, &QNetworkReply::finished, this, [this, relative, size, entry] {
        auto reply = _reply; _reply = nullptr; reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
          complete("Game-data download failed: " + reply->errorString() + ". Click Start to retry; completed files are kept."); return;
        }
        if (_received != size || _hash.result().toHex() != entry.value("sha256").toString().toLatin1()) {
          complete("Game-data integrity check failed. Click Start to retry."); return;
        }
        if (!_output->commit()) { complete("Cannot save downloaded game data. Check free disk space."); return; }
        _output.reset();
        _receipts.insert(relative, entry.value("sha256"));
        // Append a small receipt per file instead of rewriting the entire inventory.
        // A leading newline isolates any incomplete record left by an interrupted write.
        QFile receipt(_destination + "/verified.jsonl");
        auto bytes = '\n' + QJsonDocument(QJsonObject{{"path", relative}, {"sha256", entry.value("sha256")}})
            .toJson(QJsonDocument::Compact) + '\n';
        if (!receipt.open(QIODevice::WriteOnly | QIODevice::Append) || receipt.write(bytes) != bytes.size() || !receipt.flush()) {
          complete("Cannot save game-data download progress."); return;
        }
        _done += size; ++_index;
        next();
      });
      return;
    }
    if (progress) progress(_total, _total);
    complete({});
  }
  QString _root, _destination;
  QNetworkAccessManager* _network;
  QNetworkReply* _reply = nullptr;
  QCryptographicHash _hash;
  std::unique_ptr<QSaveFile> _output;
  QUrl _base;
  QJsonArray _files;
  QJsonObject _receipts;
  int _index = 0;
  qint64 _total = 0, _done = 0, _received = 0;
  bool _running = false;
};
}
