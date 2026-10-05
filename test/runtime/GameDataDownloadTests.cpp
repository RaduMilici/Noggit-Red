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
  Reply(QNetworkRequest const& request, QByteArray payload, QObject* parent)
    : QNetworkReply(parent), bytes(std::move(payload)) {
    setRequest(request); setUrl(request.url()); open(QIODevice::ReadOnly);
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
    QTimer::singleShot(0, this, [this] { emit readyRead(); setFinished(true); emit finished(); });
  }
  void abort() override {}
  qint64 bytesAvailable() const override { return bytes.size() - offset + QNetworkReply::bytesAvailable(); }
protected:
  qint64 readData(char* target, qint64 maximum) override {
    auto count = qMin(maximum, qint64(bytes.size()) - offset);
    if (!count) return -1;
    memcpy(target, bytes.constData() + offset, size_t(count)); offset += count; return count;
  }
private:
  QByteArray bytes; qint64 offset = 0;
};
class Network : public QNetworkAccessManager {
public:
  QByteArray payload = "valid"; int requests = 0;
protected:
  QNetworkReply* createRequest(Operation, QNetworkRequest const& request, QIODevice*) override {
    ++requests; return new Reply(request, payload, this);
  }
};
QString run(GameDataDownload& download) {
  QEventLoop loop; bool finished = false; QString error;
  download.finished = [&](QString value) { error = value; finished = true; loop.quit(); };
  download.start();
  if (!finished) { QTimer::singleShot(2000, &loop, &QEventLoop::quit); loop.exec(); }
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
    std::cout << "Game-data download checks passed\n";
    return 0;
  } catch (std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
