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
#include <QDateTime>
#include <QElapsedTimer>
#include <algorithm>
#include <vector>
#include <functional>
#include <memory>

namespace Noggit::Runtime {
// The manifest ships with Noggit. Payloads are streamed to disk, verified, then
// atomically committed. Completed files survive cancellation and offline restarts.
class GameDataDownload : public QObject {
public:
  explicit GameDataDownload(QString root, QObject* parent = nullptr, QNetworkAccessManager* network = nullptr)
    : QObject(parent), _root(std::move(root)), _network(network ? network : new QNetworkAccessManager(this)) {}
  ~GameDataDownload() override { cancel(); }
  std::function<void(qint64, qint64)> progress;
  std::function<void(QString)> finished;
  std::function<void(QString)> message;
  QString dataPath() const { return dataPath(_root); }
  // The server's DataDir: bundled data, or the download cache of a manifest release.
  static QString dataPath(QString const& root) {
    QFile file(root + "/Runtime/game-data.json");
    if (!file.open(QIODevice::ReadOnly)) return root + "/Runtime/mangosd/data";
    return root + "/Workspace/GameData/" + QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
  }
  void cancel() {
    _running = false; ++_generation;
    for (auto const& job : _jobs) {
      if (job->reply) {
        job->reply->disconnect(this);
        job->reply->abort(); job->reply->deleteLater(); job->reply = nullptr;
      }
      job->output.reset();
    }
    _jobs.clear();
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
          || size < 0 || size > 1024.0 * 1024 * 1024 || size != qint64(size)
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
    _running = true; _progressClock.start();
    reportProgress(true);
    pump();
  }
private:
  // Four bounded streams share Qt's event loop; no worker threads touch the UI.
  static constexpr int parallelDownloads = 4;
  struct Job {
    QJsonObject entry;
    QString relative, target;
    qint64 size = 0, received = 0;
    int attempts = 0;
    QNetworkReply* reply = nullptr;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    std::unique_ptr<QSaveFile> output;
  };
  void complete(QString error) {
    cancel();
    if (finished) finished(error);
  }
  void reportProgress(bool force = false) {
    if (!_running || (!force && _progressClock.elapsed() < 100)) return;
    _progressClock.restart();
    qint64 received = _done;
    for (auto const& job : _jobs) received += job->received;
    if (progress) progress(received, _total);
  }
  void pump() {
    if (!_running) return;
    while (_index < _files.size() && _jobs.size() < parallelDownloads) {
      auto entry = _files[_index++].toObject();
      auto relative = entry.value("path").toString();
      auto size = qint64(entry.value("size").toDouble());
      auto target = _destination + '/' + relative;
      // A receipt is written only after a verified atomic commit. Later local edits
      // (Creator installs its own Talent.dbc) are intentional and must survive restarts.
      if (_receipts.value(relative).toString() == entry.value("sha256").toString()
          && QFileInfo(target).isFile()) {
        _done += size; continue;
      }
      if (!QDir().mkpath(QFileInfo(target).absolutePath())) { complete("Cannot create game-data directory."); return; }
      auto job = std::make_shared<Job>();
      job->entry = entry; job->relative = relative; job->target = target; job->size = size;
      _jobs.push_back(job);
      attempt(job);
      if (!_running) return;
    }
    reportProgress(_jobs.empty());
    if (_index == _files.size() && _jobs.empty() && _running) complete({});
  }
  bool consume(std::shared_ptr<Job> const& job) {
    // Error pages are not game data, and must not be written or size-checked.
    if (job->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
      job->reply->readAll(); return true;
    }
    auto bytes = job->reply->readAll();
    job->received += bytes.size();
    if (job->received > job->size) {
      complete("Game-data download exceeded its expected size: " + job->relative); return false;
    }
    if (job->output->write(bytes) != bytes.size()) {
      complete("Cannot write game data: " + job->relative + ". Check free disk space."); return false;
    }
    job->hash.addData(bytes);
    reportProgress();
    return _running;
  }
  void attempt(std::shared_ptr<Job> const& job) {
    if (!_running) return;
    ++job->attempts; job->received = 0; job->hash.reset();
    job->output = std::make_unique<QSaveFile>(job->target);
    if (!job->output->open(QIODevice::WriteOnly)) {
      complete("Cannot write game data: " + job->output->errorString()); return;
    }
    QNetworkRequest request(_base.resolved(QUrl(job->relative)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("Accept-Encoding", "identity");
    job->reply = _network->get(request);
    job->reply->setReadBufferSize(1024 * 1024);
    auto generation = _generation;
    auto timeout = new QTimer(job->reply); timeout->setSingleShot(true); timeout->start(60000);
    connect(timeout, &QTimer::timeout, job->reply, &QNetworkReply::abort);
    connect(job->reply, &QIODevice::readyRead, this, [this, job, timeout, generation] {
      if (!_running || generation != _generation) return;
      timeout->start(60000); consume(job);
    });
    connect(job->reply, &QNetworkReply::finished, this, [this, job, generation] {
      if (!_running || generation != _generation) return;
      if (!consume(job) || generation != _generation) return;
      auto reply = job->reply; job->reply = nullptr; reply->deleteLater();
      auto http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      if (reply->error() != QNetworkReply::NoError || http != 200) {
        auto retryable = http == 0 || http == 200 || http == 408 || http == 429 || http >= 500;
        if (retryable) {
          int seconds = qMin(30, 1 << qMin(job->attempts - 1, 5));
          bool numeric = false;
          auto retryAfter = reply->rawHeader("Retry-After");
          auto requested = retryAfter.toLongLong(&numeric);
          if (!numeric) {
            auto date = QDateTime::fromString(QString::fromLatin1(retryAfter), Qt::RFC2822Date);
            requested = date.isValid() ? QDateTime::currentDateTimeUtc().secsTo(date) : 0;
          }
          seconds = int(qBound<qint64>(seconds, qMax<qint64>(seconds, requested), 300));
          job->output.reset(); job->received = 0;
          if (message) message(QString("Retrying %1 automatically in %2 seconds (attempt %3).")
              .arg(job->relative).arg(seconds).arg(job->attempts + 1));
          if (!_running || generation != _generation) return;
          QTimer::singleShot(seconds * 1000, this, [this, job, generation] {
            if (_running && generation == _generation) attempt(job);
          });
          return;
        }
        complete(QString("Game-data download failed: %1 (HTTP %2): %3. Completed files are kept.")
            .arg(job->relative).arg(http).arg(reply->errorString()));
        return;
      }
      if (job->received != job->size || job->hash.result().toHex() != job->entry.value("sha256").toString().toLatin1()) {
        complete("Game-data integrity check failed: " + job->relative + ". Completed files are kept."); return;
      }
      if (!job->output->commit()) { complete("Cannot save downloaded game data. Check free disk space."); return; }
      job->output.reset();
      _receipts.insert(job->relative, job->entry.value("sha256"));
      QFile receipt(_destination + "/verified.jsonl");
      auto bytes = '\n' + QJsonDocument(QJsonObject{{"path", job->relative}, {"sha256", job->entry.value("sha256")}})
          .toJson(QJsonDocument::Compact) + '\n';
      if (!receipt.open(QIODevice::WriteOnly | QIODevice::Append) || receipt.write(bytes) != bytes.size() || !receipt.flush()) {
        complete("Cannot save game-data download progress."); return;
      }
      _done += job->size;
      _jobs.erase(std::remove(_jobs.begin(), _jobs.end(), job), _jobs.end());
      // Queue the refill so progress callbacks and cancellation cannot reenter this job.
      QTimer::singleShot(0, this, [this, generation] {
        if (_running && generation == _generation) pump();
      });
    });
  }
  QString _root, _destination;
  QNetworkAccessManager* _network;
  std::vector<std::shared_ptr<Job>> _jobs;
  QElapsedTimer _progressClock;
  QUrl _base;
  QJsonArray _files;
  QJsonObject _receipts;
  int _index = 0;
  quint64 _generation = 0;
  qint64 _total = 0, _done = 0;
  bool _running = false;
};
}
