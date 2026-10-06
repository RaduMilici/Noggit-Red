#include "../../src/noggit/runtime/GameDataDownload.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
using Noggit::Runtime::GameDataDownload;
void check(bool value, char const* message) { if (!value) throw std::runtime_error(message); }
class Reply : public QNetworkReply {
public:
  Reply(QNetworkRequest const& request, QByteArray payload, int status, int delay, QObject* parent)
    : QNetworkReply(parent), bytes(std::move(payload)) {
    setRequest(request); setUrl(request.url()); open(QIODevice::ReadOnly);
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
    if (status != 200) setError(QNetworkReply::TemporaryNetworkFailureError, "Temporary test failure");
    QTimer::singleShot(delay, this, [this] {
      if (aborted) return;
      emit readyRead();
      if (aborted) return;
      setFinished(true); emit finished();
    });
  }
  void abort() override { aborted = true; }
  qint64 bytesAvailable() const override { return bytes.size() - offset + QNetworkReply::bytesAvailable(); }
protected:
  qint64 readData(char* target, qint64 maximum) override {
    auto count = qMin(maximum, qint64(bytes.size()) - offset);
    if (!count) return -1;
    memcpy(target, bytes.constData() + offset, size_t(count)); offset += count; return count;
  }
private:
  QByteArray bytes; qint64 offset = 0; bool aborted = false;
};
class Network : public QNetworkAccessManager {
public:
  QByteArray payload = "valid";
  int requests = 0, active = 0, maximumActive = 0, failures = 0, failureStatus = 503, delay = 0;
  QHash<QString, int> requestsByPath;
protected:
  QNetworkReply* createRequest(Operation, QNetworkRequest const& request, QIODevice*) override {
    ++requests; ++requestsByPath[request.url().path()];
    ++active; maximumActive = qMax(maximumActive, active);
    auto reply = new Reply(request, payload, failures-- > 0 ? failureStatus : 200, delay, this);
    connect(reply, &QNetworkReply::finished, this, [this] { --active; });
    return reply;
  }
};
QString run(GameDataDownload& download) {
  QEventLoop loop; bool finished = false; QString error;
  download.finished = [&](QString value) { error = value; finished = true; loop.quit(); };
  download.start();
  if (!finished) { QTimer::singleShot(10000, &loop, &QEventLoop::quit); loop.exec(); }
  check(finished, "Download timed out"); return error;
}
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir temporary; auto root = temporary.path(); QDir().mkpath(root + "/Runtime");
    QJsonObject entry{{"path", "dbc/Test.dbc"}, {"size", 5},
      {"sha256", QString(QCryptographicHash::hash("valid", QCryptographicHash::Sha256).toHex())}};
    auto writeManifest = [&](QString path) {
      entry["path"] = path;
      QFile file(root + "/Runtime/game-data.json"); check(file.open(QIODevice::WriteOnly), "Manifest open");
      file.write(QJsonDocument(QJsonObject{{"format", 1}, {"baseUrl", "https://assets.example/"},
        {"files", QJsonArray{entry}}}).toJson());
    };
    writeManifest("dbc/Test.dbc");
    Network network; GameDataDownload download(root, nullptr, &network);
    network.payload = "wrong";
    check(run(download).contains("integrity"), "Bad hash must fail");
    check(!QFileInfo::exists(download.dataPath() + "/dbc/Test.dbc"), "Failed data must not be published");
    network.payload = "valid";
    check(run(download).isEmpty(), "Retry must succeed");
    check(QFileInfo(download.dataPath() + "/dbc/Test.dbc").size() == 5, "Verified file must exist");
    auto requests = network.requests;
    check(run(download).isEmpty() && network.requests == requests, "Offline cache must skip network");
    QFile::remove(download.dataPath() + "/dbc/Test.dbc");
    check(run(download).isEmpty() && network.requests == requests + 1, "Deleted cached file must redownload");
    writeManifest("dbc/../escape");
    requests = network.requests;
    check(!run(download).isEmpty() && requests == network.requests, "Unsafe manifest must fail before network");
    writeManifest("dbc/Test.dbc");
    QFile::remove(download.dataPath() + "/dbc/Test.dbc");
    download.finished = [](QString) { throw std::runtime_error("Canceled download completed"); };
    download.start(); download.cancel();
    QCoreApplication::processEvents();
    check(!QFileInfo::exists(download.dataPath() + "/dbc/Test.dbc"), "Canceled file must not be published");
    check(run(download).isEmpty(), "Retry after cancel must succeed");

    // More than one reply must be in flight, bounded to four. Out-of-order
    // completion must still publish every file and preserve offline receipts.
    QTemporaryDir parallelRoot; QDir().mkpath(parallelRoot.path() + "/Runtime");
    QJsonArray entries;
    for (int i = 0; i < 9; ++i) {
      auto item = entry; item["path"] = QString("dbc/Test%1.dbc").arg(i); entries.append(item);
    }
    QFile parallelManifest(parallelRoot.path() + "/Runtime/game-data.json");
    check(parallelManifest.open(QIODevice::WriteOnly), "Parallel manifest open");
    parallelManifest.write(QJsonDocument(QJsonObject{{"format", 1}, {"baseUrl", "https://assets.example/"},
      {"files", entries}}).toJson()); parallelManifest.close();
    Network parallelNetwork; parallelNetwork.delay = 20;
    GameDataDownload parallel(parallelRoot.path(), nullptr, &parallelNetwork);
    qint64 finalReceived = -1, finalTotal = -1;
    parallel.progress = [&](qint64 received, qint64 total) {
      check(received <= total, "Parallel progress exceeds total");
      finalReceived = received; finalTotal = total;
    };
    check(run(parallel).isEmpty(), "Parallel download must succeed");
    check(parallelNetwork.maximumActive == 4, "Must keep exactly four requests in flight");
    for (int i = 0; i < 9; ++i) {
      QFile saved(parallel.dataPath() + QString("/dbc/Test%1.dbc").arg(i));
      check(saved.open(QIODevice::ReadOnly) && saved.readAll() == "valid", "Every parallel file must match");
    }
    check(finalReceived == 45 && finalTotal == 45, "Parallel progress must aggregate files");
    requests = parallelNetwork.requests;
    check(run(parallel).isEmpty() && parallelNetwork.requests == requests, "Parallel receipts must skip network");

    for (int i = 0; i < 9; ++i) QFile::remove(parallel.dataPath() + QString("/dbc/Test%1.dbc").arg(i));
    parallelNetwork.failures = 2;
    requests = parallelNetwork.requests;
    check(run(parallel).isEmpty(), "Failures during concurrent downloads must recover");
    check(parallelNetwork.requests == requests + 11, "Concurrent failure must retry only two failed files");

    // A retry stays internal: one 503/429 must not finish or discard other
    // completed files. Error response bodies may exceed a manifest file's size.
    for (int status : {503, 429}) {
      auto path = parallel.dataPath() + "/dbc/Test0.dbc"; QFile::remove(path);
      parallelNetwork.failures = 1; parallelNetwork.failureStatus = status;
      requests = parallelNetwork.requests;
      int notices = 0; parallel.message = [&](QString) { ++notices; };
      check(run(parallel).isEmpty(), "Transient HTTP failure must recover automatically");
      check(parallelNetwork.requests == requests + 2 && notices == 1, "Only failed file should retry");
      check(QFileInfo(path).size() == 5, "Retried file must be verified and committed");
    }
    QFile::remove(parallel.dataPath() + "/dbc/Test0.dbc");
    parallelNetwork.failures = 1; parallelNetwork.failureStatus = 404;
    check(run(parallel).contains("HTTP 404"), "Permanent HTTP error must remain actionable");

    // Cancel while a backoff timer is pending. It must neither launch another
    // request nor invoke finished after cancellation (also across a restart).
    parallelNetwork.failures = 1; parallelNetwork.failureStatus = 503;
    QEventLoop canceledLoop; bool notified = false;
    parallel.finished = [&](QString) { notified = true; };
    parallel.message = [&](QString) { parallel.cancel(); canceledLoop.quit(); };
    parallel.start();
    QTimer::singleShot(2000, &canceledLoop, &QEventLoop::quit); canceledLoop.exec();
    check(!notified, "Cancel during retry must not finish");
    parallel.message = {};
    check(run(parallel).isEmpty(), "Restart after backoff cancel must work");
    requests = parallelNetwork.requests;
    QEventLoop wait; QTimer::singleShot(1200, &wait, &QEventLoop::quit); wait.exec();
    check(parallelNetwork.requests == requests, "Canceled retry timer must not leak into restart");
    std::cout << "Game-data download checks passed\n";
    return 0;
  } catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
