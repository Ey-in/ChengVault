#include "chengvault/crypto.hpp"
#include "chengvault/network.hpp"
#include "chengvault/repository.hpp"
#include "test_support.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <future>

namespace {
using cv::test::check;
using cv::test::throws;
template <class F> auto worker(F &&function) {
    auto future = std::async(std::launch::async, std::forward<F>(function));
    // The test owns QTcpServer on the main thread. Keep processing its accepts
    // while a real TCP client performs blocking requests on another thread.
    while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return future.get();
}
QStringList filesBelow(const QString &root) {
    QStringList files;
    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files.append(it.next());
    return files;
}
// A small protocol probe lets tests replay an actual previously accepted frame
// and tamper a ciphertext, actions the ordinary Client correctly never exposes.
struct ProtocolProbe {
    QTcpSocket socket;
    QByteArray transcript, tx, rx;
    quint64 nextSequence = 0;
    explicit ProtocolProbe(const cv::network::RemoteOptions &options) {
        socket.connectToHost(options.host, options.port);
        check(socket.waitForConnected(10000));
        QJsonObject hello{{"version", 1},
                          {"user", options.user},
                          {"device", options.deviceId},
                          {"nonce", QString::fromLatin1(cv::crypto::randomBytes(32).toHex())}};
        const auto helloBytes = QJsonDocument(hello).toJson(QJsonDocument::Compact);
        send(helloBytes);
        const auto response = QJsonDocument::fromJson(receive()).object();
        transcript = cv::crypto::sha256("ChengVault/session/v1" + helloBytes +
                                        QByteArray::fromHex(response["nonce"].toString().toLatin1()));
        check(cv::crypto::equal(cv::crypto::hmac(options.deviceKey, "server" + transcript),
                                QByteArray::fromHex(response["proof"].toString().toLatin1())));
        send(QJsonDocument(
                 QJsonObject{
                     {"proof", QString::fromLatin1(
                                   cv::crypto::hmac(options.deviceKey, "client" + transcript).toHex())}})
                 .toJson(QJsonDocument::Compact));
        tx = cv::crypto::hkdf(options.deviceKey, transcript, "client-to-server");
        rx = cv::crypto::hkdf(options.deviceKey, transcript, "server-to-client");
    }
    QByteArray exact(qsizetype size) {
        QByteArray out;
        while (out.size() < size) {
            if (!socket.bytesAvailable())
                check(socket.waitForReadyRead(10000), "probe read timeout");
            out += socket.read(size - out.size());
        }
        return out;
    }
    QByteArray receive() {
        const auto head = exact(4);
        return exact(qFromBigEndian<quint32>(head.constData()));
    }
    void send(const QByteArray &data) {
        QByteArray header(4, '\0');
        qToBigEndian(static_cast<quint32>(data.size()), header.data());
        socket.write(header + data);
        while (socket.bytesToWrite())
            check(socket.waitForBytesWritten(10000));
    }
    QByteArray packet(quint64 sequence, const QJsonObject &object) {
        QByteArray bytes(8, '\0');
        qToBigEndian(sequence, bytes.data());
        return cv::crypto::seal(transcript + bytes + QJsonDocument(object).toJson(QJsonDocument::Compact), tx,
                                "chacha20");
    }
    QJsonObject response() {
        return QJsonDocument::fromJson(cv::crypto::open(receive(), rx).mid(40)).object();
    }
    QJsonObject query(const QJsonObject &object) {
        send(packet(nextSequence++, object));
        return response();
    }
    void login(const cv::network::RemoteOptions &options) {
        check(query({{"op", "login"}, {"password", options.password}})["ok"].toBool());
    }
};
#include "network_repository_cases.hpp"
} // namespace

void repositoryNetworkTests(cv::test::Suite &suite) {
    QTemporaryDir temp;
    check(temp.isValid(), "repository test temporary directory");
    const auto root = temp.path();
    cv::BackupPlan plan;
    plan.sources = {root + "/source"};
    plan.destination = root + "/repository";
    plan.cipher = "chacha20";
    plan.compression = "lz77";
    plan.strictMetadata = false;
    QDir().mkpath(plan.sources.first() + "/empty");
    cv::atomicWrite(plan.sources.first() + "/alpha.txt", QByteArray(1400000, 'A'));
    cv::atomicWrite(plan.sources.first() + "/beta.txt", "metadata and encrypted names");
    cv::Repository repository(plan.destination, "correct backup password");
    QString firstId, secondId, thirdId;
    QByteArray key;
    suite.run("repository.initialize.random-master", "集成", [&] {
        repository.init();
        key = repository.masterKey();
        check(key.size() == 32);
        check(cv::readFile(plan.destination + "/repository.json").indexOf("correct backup password") < 0);
    });
    suite.run("repository.empty-password-rejected", "安全", [] {
        QTemporaryDir dir;
        check(dir.isValid());
        const auto path = dir.path() + "/repo";
        try {
            cv::Repository repo(path, "");
            repo.init();
            check(false, "empty repository password accepted");
        } catch (const cv::Error &e) {
            check(e.code == "REPOSITORY_PASSWORD_EMPTY", "unexpected error " + e.code);
        }
        check(!QFileInfo::exists(path + "/repository.json"));
    });
    suite.run("repository.empty-password-preserves-existing", "安全", [] {
        QTemporaryDir dir;
        check(dir.isValid());
        const auto path = dir.path() + "/repo";
        {
            cv::Repository repo(path, "correct password");
            repo.init();
        }
        try {
            cv::Repository repo(path, "");
            check(false, "empty password opened an existing repository");
        } catch (const cv::Error &e) {
            check(e.code == "REPOSITORY_PASSWORD_EMPTY", "unexpected error " + e.code);
        }
        cv::Repository repo(path, "correct password");
        check(repo.list().isEmpty());
    });
    suite.run("repository.manifest-size-limit", "异常健壮性", [] {
        QJsonObject normal{{"version", 1}, {"entries", QJsonArray{}}};
        check(cv::serializeSnapshot(normal).size() < cv::ManifestLimit);
        QJsonObject oversized{{"version", 1},
                              {"entries", QJsonArray{}},
                              {"padding", QString(cv::ManifestLimit + 1024, 'x')}};
        try {
            (void)cv::serializeSnapshot(oversized);
            check(false, "oversized manifest accepted");
        } catch (const cv::Error &e) {
            check(e.code == "MANIFEST_SIZE", "unexpected error " + e.code);
        }
    });
    suite.run("repository.first-snapshot", "系统功能", [&] {
        auto result = repository.backup(plan);
        firstId = result.details["snapshotId"].toString();
        check(!firstId.isEmpty() && result.storedBytes > 0 && repository.list().size() == 1);
    });
    suite.run("repository.unchanged-blocks-reused", "性能", [&] {
        auto result = repository.backup(plan);
        secondId = result.details["snapshotId"].toString();
        check(result.storedBytes == 0 && result.details["newChunks"].toInt() == 0);
        check(result.details["reusedChunks"].toInt() > 0);
    });
    suite.run("repository.local-modification-one-chunk", "系统功能", [&] {
        QFile file(plan.sources.first() + "/alpha.txt");
        check(file.open(QIODevice::ReadWrite));
        check(file.seek(1200000) && file.write("changed", 7) == 7);
        file.close();
        auto result = repository.backup(plan);
        thirdId = result.details["snapshotId"].toString();
        check(result.details["newChunks"].toInt() == 1, "a partial file edit must add exactly one block");
    });
    suite.run("repository.snapshot-full-restore", "集成", [&] {
        cv::RestoreOptions options;
        options.strictMetadata = false;
        repository.restore(firstId, root + "/first-restore", options);
        check(cv::readFile(root + "/first-restore/source/alpha.txt", 2000000) == QByteArray(1400000, 'A'));
        check(QDir(root + "/first-restore/source/empty").exists());
    });
    suite.run("repository.manifest-encrypted", "安全", [&] {
        const auto stored = cv::readFile(plan.destination + "/snapshots/" + firstId + ".cvs");
        check(!stored.contains("alpha.txt") && !stored.contains("metadata and encrypted names"));
    });
    suite.run("repository.wrong-password", "安全",
              [&] { throws([&] { cv::Repository bad(plan.destination, "wrong password"); }); });
    suite.run("repository.dpapi-master-unlock", "集成", [&] {
        auto protectedKey = cv::crypto::protectLocalKey(key);
        auto reopened =
            cv::Repository::withMasterKey(plan.destination, cv::crypto::unprotectLocalKey(protectedKey));
        check(reopened.list().size() == 3);
        throws([&] { cv::Repository::withMasterKey(plan.destination, QByteArray(32, 'x')); });
    });
    suite.run("repository.password-change-rewrap-only", "安全", [&] {
        const auto oldObject = filesBelow(plan.destination + "/objects").first();
        const auto bytes = cv::readFile(oldObject);
        repository.changePassword("new backup password");
        cv::Repository reopened(plan.destination, "new backup password");
        check(reopened.masterKey() == key && cv::readFile(oldObject) == bytes);
        throws([&] { cv::Repository bad(plan.destination, "correct backup password"); });
    });
    suite.run("repository.verify-all-snapshots", "白盒单元", [&] {
        const auto result = repository.verify();
        check(result.details["snapshots"].toInt() == 3 && result.files > 0);
    });
    suite.run("repository.concurrent-task-lock", "并发稳定性", [&] {
        QLockFile lock(plan.destination + "/repository.lock");
        check(lock.tryLock());
        throws([&] { repository.backup(plan); });
    });
    suite.run("repository.list-lock-protects-snapshot-lifetime", "并发稳定性", [&] {
        const auto before = repository.list();
        QLockFile writer(plan.destination + "/repository.lock");
        check(writer.tryLock());
        rejects("REPOSITORY_BUSY", [&] { repository.list(); });
        writer.unlock();
        check(repository.list() == before,
              "listing must resume with all authenticated snapshots after writer unlock");
    });
    suite.run("repository.prune-shared-blocks", "异常健壮性", [&] {
        repository.prune(1);
        check(repository.list().size() == 1 && repository.list()[0].toObject()["id"] == thirdId);
        repository.verify();
        cv::RestoreOptions options;
        options.strictMetadata = false;
        repository.restore(thirdId, root + "/latest-restore", options);
        check(cv::readFile(root + "/latest-restore/source/alpha.txt", 2000000) ==
              cv::readFile(plan.sources.first() + "/alpha.txt", 2000000));
        throws([&] { repository.prune(0); });
    });
    suite.run("repository.path-traversal-rejected", "安全",
              [&] { throws([&] { repository.manifest("../../outside"); }); });
    suite.run("repository.cancel-no-new-snapshot", "异常健壮性", [&] {
        std::atomic_bool cancelled{true};
        cv::Context context;
        context.cancelled = &cancelled;
        const auto before = repository.list().size();
        throws([&] { repository.backup(plan, context); });
        check(repository.list().size() == before);
    });
    suite.run("repository.tampered-object-rejected", "安全", [&] {
        const auto path = filesBelow(plan.destination + "/objects").first();
        const auto saved = cv::readFile(path);
        auto changed = saved;
        changed[changed.size() - 1] ^= 1;
        cv::atomicWrite(path, changed);
        throws([&] { repository.verify(); });
        cv::atomicWrite(path, saved);
    });

    cv::network::Server server(root + "/server");
    cv::network::RemoteOptions alice, bob;
    alice.user = "alice";
    alice.password = "alice login";
    alice.deviceId = "desktop";
    alice.deviceKey = cv::crypto::randomBytes(32);
    bob.user = "bob";
    bob.password = "bob login";
    bob.deviceId = "laptop";
    bob.deviceKey = cv::crypto::randomBytes(32);
    suite.run("network.setup-two-users", "集成", [&] {
        server.configureUser("alice", alice.password, true);
        server.configureDevice("alice", alice.deviceId, alice.deviceKey);
        server.configureUser("bob", bob.password, false);
        server.configureDevice("bob", bob.deviceId, bob.deviceKey);
        check(server.listen(QHostAddress::LocalHost, 0));
        alice.port = server.port();
        bob.port = server.port();
        check(alice.port != 0);
    });
    cv::network::Client aliceClient(alice), bobClient(bob);
    suite.run("network.first-upload-real-tcp", "系统功能", [&] {
        auto result = worker([&] { return aliceClient.upload(plan.destination); });
        check(result.files > 0 && result.bytes > 0 && result.details["sentBytes"].toDouble() > 0);
    });
    suite.run("network.unchanged-zero-payload", "性能", [&] {
        auto result = worker([&] { return aliceClient.upload(plan.destination); });
        check(result.files == 0 && result.bytes == 0 && result.details["reusedObjects"].toInt() > 0);
        check(result.details["sentBytes"].toDouble() > 0, "control frames still consume real bytes");
    });
    suite.run("network.account-isolation-list", "安全", [&] {
        check(worker([&] { return aliceClient.list(); }).size() == 1);
        check(worker([&] { return bobClient.list(); }).isEmpty());
    });
    suite.run("network.account-isolation-download", "安全", [&] {
        throws([&] { worker([&] { return bobClient.download(repository.id(), root + "/bob-forbidden"); }); });
    });
    suite.run("network.full-download-restore", "集成", [&] {
        auto result = worker([&] { return aliceClient.download(repository.id(), root + "/downloaded"); });
        cv::Repository downloaded(result.output, "new backup password");
        downloaded.verify();
        cv::RestoreOptions options;
        options.strictMetadata = false;
        downloaded.restore(thirdId, root + "/remote-restore", options);
        check(cv::readFile(root + "/remote-restore/source/alpha.txt", 2000000) ==
              cv::readFile(plan.sources.first() + "/alpha.txt", 2000000));
    });
    suite.run("network.download-no-overwrite", "安全", [&] {
        throws([&] { worker([&] { return aliceClient.download(repository.id(), root + "/downloaded"); }); });
    });
    suite.run("network.changed-upload-incremental", "系统功能", [&] {
        cv::atomicWrite(plan.sources.first() + "/beta.txt", "just one changed file");
        repository.backup(plan);
        auto result = worker([&] { return aliceClient.upload(plan.destination); });
        check(result.files == 2, "one new object plus one new encrypted manifest should be sent");
        check(result.details["reusedObjects"].toInt() >= 3);
    });
    suite.run("network.wrong-login-password", "安全", [&] {
        auto wrong = alice;
        wrong.password = "not alice";
        throws([&] { worker([&] { return cv::network::Client(wrong).list(); }); });
    });
    suite.run("network.wrong-device-key", "安全", [&] {
        auto wrong = alice;
        wrong.deviceKey = cv::crypto::randomBytes(32);
        throws([&] { worker([&] { return cv::network::Client(wrong).list(); }); });
    });
    suite.run("network.device-bound-to-user", "安全", [&] {
        auto wrong = alice;
        wrong.user = bob.user;
        wrong.password = bob.password;
        throws([&] { worker([&] { return cv::network::Client(wrong).list(); }); });
    });
    suite.run("network.nonadmin-management-denied", "安全",
              [&] { throws([&] { worker([&] { return bobClient.admin("list-users", {}); }); }); });
    suite.run("network.admin-lists-no-secrets", "安全", [&] {
        const auto response = worker([&] { return aliceClient.admin("list-users", {}); });
        check(response["users"].toArray().size() == 2);
        const auto bytes = QJsonDocument(response).toJson();
        check(!bytes.contains("password") && !bytes.contains(alice.deviceKey.toHex()));
    });
    suite.run("network.disable-user", "系统功能", [&] {
        worker([&] { return aliceClient.admin("disable-user", {{"user", "bob"}, {"disabled", true}}); });
        throws([&] { worker([&] { return bobClient.list(); }); });
        worker([&] { return aliceClient.admin("disable-user", {{"user", "bob"}, {"disabled", false}}); });
    });
    suite.run("network.reset-login-password", "系统功能", [&] {
        worker([&] {
            return aliceClient.admin("reset-password", {{"user", "bob"}, {"password", "changed bob login"}});
        });
        throws([&] { worker([&] { return bobClient.list(); }); });
        bob.password = "changed bob login";
        bobClient = cv::network::Client(bob);
        check(worker([&] { return bobClient.list(); }).isEmpty());
    });
    suite.run("network.quota-enforced", "异常健壮性", [&] {
        worker([&] { return aliceClient.admin("set-quota", {{"user", "bob"}, {"quota", "1"}}); });
        throws([&] { worker([&] { return bobClient.upload(plan.destination); }); });
        check(worker([&] { return bobClient.list(); }).isEmpty());
    });
    suite.run("network.admin-create-register-device", "系统功能", [&] {
        worker([&] {
            return aliceClient.admin(
                "create-user", {{"user", "charlie"}, {"password", "third login"}, {"quota", "10485760"}});
        });
        auto charlie = alice;
        charlie.user = "charlie";
        charlie.password = "third login";
        charlie.deviceId = "third";
        charlie.deviceKey = cv::crypto::randomBytes(32);
        worker([&] {
            return aliceClient.admin("register-device",
                                     {{"user", "charlie"},
                                      {"device", charlie.deviceId},
                                      {"key", QString::fromLatin1(charlie.deviceKey.toHex())}});
        });
        check(worker([&] { return cv::network::Client(charlie).list(); }).isEmpty());
        worker([&] {
            return aliceClient.admin("revoke-device", {{"user", "charlie"}, {"device", charlie.deviceId}});
        });
        throws([&] { worker([&] { return cv::network::Client(charlie).list(); }); });
    });
    suite.run("network.interrupted-upload-resumes-offset", "异常健壮性", [&] {
        cv::atomicWrite(plan.sources.first() + "/random.bin", cv::crypto::randomBytes(2 * cv::ChunkSize));
        auto plainPlan = plan;
        plainPlan.compression = "none";
        repository.backup(plainPlan);
        std::atomic_bool cancelled{false};
        cv::Context context;
        context.cancelled = &cancelled;
        context.progress = [&](quint64, quint64, const QString &stage) {
            if (stage.startsWith("objects/"))
                cancelled = true;
        };
        throws([&] { worker([&] { return aliceClient.upload(plan.destination, context); }); });
        auto resumed = worker([&] { return aliceClient.upload(plan.destination); });
        check(resumed.details["resumedBytes"].toDouble() >= cv::ChunkSize);
    });
    suite.run("network.replayed-frame-rejected", "安全", [&] {
        worker([&] {
            ProtocolProbe peer(alice);
            const auto login = peer.packet(0, {{"op", "login"}, {"password", alice.password}});
            peer.send(login);
            check(peer.response()["ok"].toBool());
            peer.send(login);
            const auto rejected = peer.response();
            check(!rejected["ok"].toBool() && rejected["code"] == "NETWORK_REPLAY");
        });
    });
    suite.run("network.ciphertext-tamper-rejected", "安全", [&] {
        worker([&] {
            ProtocolProbe peer(alice);
            peer.send(peer.packet(0, {{"op", "login"}, {"password", alice.password}}));
            check(peer.response()["ok"].toBool());
            auto altered = peer.packet(1, {{"op", "list"}});
            altered[altered.size() - 1] ^= 1;
            peer.send(altered);
            check(!peer.response()["ok"].toBool());
        });
    });
    suite.run("network.oversized-frame-rejected", "安全", [&] {
        worker([&] {
            QTcpSocket socket;
            socket.connectToHost(alice.host, alice.port);
            check(socket.waitForConnected(10000));
            QByteArray header(4, '\0');
            qToBigEndian<quint32>(0x7FFFFFFF, header.data());
            socket.write(header);
            while (socket.bytesToWrite())
                check(socket.waitForBytesWritten(10000));
            if (socket.state() != QAbstractSocket::UnconnectedState)
                socket.waitForDisconnected(10000);
            check(socket.state() == QAbstractSocket::UnconnectedState);
        });
    });
    suite.run("network.server-stores-no-master-or-password", "安全", [&] {
        for (const auto &path : filesBelow(root + "/server")) {
            const auto bytes = cv::readFile(path);
            check(!bytes.contains("new backup password") && !bytes.contains("changed bob login"));
            check(!bytes.contains(key.toHex()) && !bytes.contains("alpha.txt"));
        }
    });
    suite.run("network.reject-user-path-traversal", "安全",
              [&] { throws([&] { server.configureUser("../escape", "password"); }); });
    repositoryAdditionalTests(suite, root);
    networkAdditionalTests(suite, server, alice, repository, root);
    server.stop();
}
