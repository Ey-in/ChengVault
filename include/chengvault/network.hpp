#pragma once
#include "common.hpp"
#include <QHostAddress>
#include <memory>

namespace cv::network {
struct RemoteOptions {
    // A loopback default allows an isolated demonstration without exposing a
    // service to the LAN. Both sides must explicitly choose another address.
    QString host = "127.0.0.1";
    quint16 port = 48484;
    QString user;
    QString password;
    QString deviceId;
    QByteArray deviceKey; // 32 bytes, provisioned outside the TCP connection.
    // Bounds apply per blocking socket operation. GUI callers must run the
    // client on a worker thread and use Context for cancellation/progress.
    int timeoutMs = 15000;
};

// Each method creates an authenticated encrypted session. Upload compares
// stable object names and transfers only missing encrypted objects.
class Client {
  public:
    explicit Client(RemoteOptions options);
    // The local repository is locked until the remote commit succeeds.
    // details includes real payload and framed transfer byte counters.
    TaskResult upload(const QString &repositoryPath, const Context &context = {}) const;
    // Require an empty destination; incomplete downloads stay in an adjacent
    // internal directory so retrying the same destination can resume safely.
    TaskResult download(const QString &repositoryId, const QString &localPath,
                        const Context &context = {}) const;
    // Lists only the authenticated user's public repository summaries. Detailed
    // file metadata remains inside manifests encrypted with the repository key.
    QJsonArray list() const;
    // Role authorization happens on the server, including for direct CLI calls.
    // See docs/network.md for action names and the deliberately small JSON API.
    QJsonObject admin(const QString &action, const QJsonObject &arguments) const;

  private:
    RemoteOptions options_;
};

// Qt event loop accepts connections; each bounded session runs on a worker.
// The server only stores client-encrypted objects and cannot unlock snapshots.
class Server {
  public:
    explicit Server(QString root);
    // stop() closes the acceptor and joins all bounded worker sessions; callers
    // must keep the instance alive while the owning Qt event loop is running.
    ~Server();
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;
    void configureUser(const QString &name, const QString &password, bool administrator = false,
                       quint64 quotaBytes = 10737418240ULL);
    // Local administration provisions the initial PSK; the server persists it
    // with current-user DPAPI. It is not obtainable through a list API.
    void configureDevice(const QString &user, const QString &id, const QByteArray &key);
    // Port zero asks the OS for an unused port, convenient for genuine network
    // integration tests without assuming port 48484 is available.
    bool listen(const QHostAddress &address = QHostAddress::LocalHost, quint16 listenPort = 48484);
    quint16 port() const;
    QString errorString() const;
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace cv::network
