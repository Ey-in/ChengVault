#include "chengvault/network.hpp"
#include "chengvault/crypto.hpp"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QtEndian>
#include <atomic>
#include <limits>
#include <thread>
#include <vector>

namespace cv::network {
namespace {
// The protocol has two phases with deliberately different trust levels.
// Pre-authentication: bounded length-prefixed JSON, device identity and nonces.
// Post-authentication: only authenticated encrypted Session records are parsed.
// The server never accepts a repository password or a repository master key.
// Transport keys come from device provisioning, not from the login password.
// All numbers below are resource ceilings, checked before reading allocations.
constexpr quint32 MaxFrame = 8 * 1024 * 1024;
constexpr qint64 MaxObject = 64 * 1024 * 1024;
constexpr int PasswordRounds = 120000;
// Wire layout (all multi-byte integers are big-endian):
//   uint32 frameLength | frameLength bytes
// Before authentication the payload is a single UTF-8 JSON object.
// After authentication the payload is a versioned crypto::seal envelope.
// The decrypted payload starts with a 32-byte transcript digest.
// The next eight bytes are the sender's unsigned message sequence number.
// The remaining bytes are one UTF-8 JSON request or response object.
// Sequence zero is used for login in each direction, not for ClientHello.
// Client-to-server and server-to-client sequence spaces advance independently.
// The sequence value must match exactly; there is no reordering window.
// The envelope supplies nonce freshness, encryption and ciphertext authentication.
// The transcript and sequence supply channel binding and replay protection.
// A MAC-valid packet does not skip namespace or role authorization checks.
// Limits are applied at framing, JSON inventory and individual-object levels.
// These checks are complementary: a legal-sized frame can still be malformed.
void validName(const QString &name) {
    // Namespace components are strict ASCII identifiers, not filesystem paths.
    // In particular, dot segments, drive prefixes and Windows ADS are excluded.
    require(QRegularExpression("^[A-Za-z0-9_-]{1,64}$").match(name).hasMatch(), "NETWORK_NAME",
            "账户、设备或仓库标识必须是1至64位字母、数字、下划线或短横线");
}
void validHash(const QString &hash) {
    require(QRegularExpression("^[0-9a-f]{64}$").match(hash).hasMatch(), "NETWORK_HASH", "无效对象摘要");
}
void validPath(const QString &path) {
    // Accept an explicit grammar instead of trying to normalize hostile input.
    // The only synchronized files are the config, immutable chunks and snapshots.
    // Locks, logs, credentials and arbitrary files never cross this interface.
    require(path == "repository.json" ||
                QRegularExpression("^objects/[0-9a-f]{2}/[0-9a-f]{64}\\.cvo$").match(path).hasMatch() ||
                QRegularExpression("^snapshots/[A-Za-z0-9_-]{8,80}\\.cvs$").match(path).hasMatch(),
            "NETWORK_PATH", "非法仓库相对路径");
    if (path.startsWith("objects/"))
        require(path.mid(8, 2) == path.mid(11, 2), "NETWORK_PATH", "块目录前缀错误");
}
void safeAncestors(const QString &path) {
    // QSaveFile protects replacement atomicity, but cannot by itself prevent
    // following a malicious junction in one of the parent directories.
    auto current = QFileInfo(path).absoluteFilePath();
    while (!current.isEmpty()) {
        const QFileInfo info(current);
        require(!info.isSymLink() && !info.isJunction(), "NETWORK_PATH",
                "仓库与下载路径不能包含链接或目录联接");
        const auto parent = info.absolutePath();
        if (parent == current)
            break;
        current = parent;
    }
}
QJsonObject parseObject(const QByteArray &bytes) {
    // Arrays, scalar JSON and malformed Unicode are not operation envelopes.
    // Every application request must have one top-level object.
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), "NETWORK_JSON", "无效协议JSON");
    return document.object();
}
QByteArray jsonBytes(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

struct Wire {
    // TCP preserves byte ordering, but does not preserve writes as messages.
    // readExact handles both fragmented headers and coalesced adjacent frames.
    // The frame ceiling is validated before reserving its payload buffer.
    QTcpSocket &socket;
    int timeout;
    quint64 sent = 0, received = 0;
    Wire(QTcpSocket &transport, int timeoutMs) : socket(transport), timeout(timeoutMs) {}
    // Wire owns no socket and never changes thread affinity. Its lifetime is
    // nested inside the stack-owned socket in client or worker code.
    // Bytes read into Qt's internal queue are consumed only by this object.
    // sent/received count complete packets including their length header.
    // Base64 expansion is thus included in the measured network wire overhead.
    QByteArray readExact(qsizetype count) {
        // A closed peer or timeout is an error even when an earlier partial read
        // succeeded; truncated frames never become valid shorter JSON records.
        QByteArray result;
        result.reserve(count);
        while (result.size() < count) {
            if (!socket.bytesAvailable())
                require(socket.waitForReadyRead(timeout), "NETWORK_READ", "网络连接中断或读取超时");
            const auto bytes = socket.read(count - result.size());
            require(!bytes.isEmpty(), "NETWORK_READ", "网络连接关闭");
            result.append(bytes);
        }
        received += static_cast<quint64>(count);
        return result;
    }
    void sendRaw(const QByteArray &frame) {
        // Drain the Qt output queue while this blocking session is active.
        // Count actual framed bytes, not only application payload sizes.
        require(!frame.isEmpty() && frame.size() <= MaxFrame, "NETWORK_FRAME", "报文超出上限");
        QByteArray packet(4, '\0');
        qToBigEndian(static_cast<quint32>(frame.size()), packet.data());
        packet += frame;
        require(socket.write(packet) == packet.size(), "NETWORK_WRITE", "无法写入网络数据");
        while (socket.bytesToWrite())
            require(socket.waitForBytesWritten(timeout), "NETWORK_WRITE", "网络写入超时");
        sent += static_cast<quint64>(packet.size());
    }
    QByteArray receiveRaw() {
        // Zero-length frames are invalid, including during the handshake.
        // Rejecting absurd lengths happens before attempting a payload read.
        const auto header = readExact(4);
        const auto length = qFromBigEndian<quint32>(header.constData());
        require(length > 0 && length <= MaxFrame, "NETWORK_FRAME", "报文长度超出上限");
        return readExact(length);
    }
    void sendJson(const QJsonObject &value) {
        sendRaw(jsonBytes(value));
    }
    QJsonObject receiveJson() {
        return parseObject(receiveRaw());
    }
};

struct Session {
    // Both the session binding and monotonically increasing sequence number are
    // INSIDE the authenticated plaintext. No request is parsed before the
    // envelope MAC and those two fields have passed verification.
    // Separate send/receive keys prevent reflecting a valid packet backwards.
    Wire &wire;
    QByteArray txKey, rxKey, sessionId;
    quint64 txSequence = 0, rxSequence = 0;
    Session(Wire &transport, QByteArray outgoingKey, QByteArray incomingKey, QByteArray binding)
        : wire(transport), txKey(std::move(outgoingKey)), rxKey(std::move(incomingKey)),
          sessionId(std::move(binding)) {}
    void send(const QJsonObject &object) {
        require(txSequence != std::numeric_limits<quint64>::max(), "NETWORK_SEQUENCE", "会话序号耗尽");
        QByteArray plaintext = sessionId;
        QByteArray sequence(8, '\0');
        qToBigEndian(txSequence++, sequence.data());
        plaintext += sequence;
        plaintext += jsonBytes(object);
        wire.sendRaw(crypto::seal(plaintext, txKey, "chacha20"));
    }
    QJsonObject receive() {
        // MAC verification is inside crypto::open and precedes all JSON parsing.
        // A valid tag from an earlier session still fails the transcript binding.
        const auto plain = crypto::open(wire.receiveRaw(), rxKey);
        require(plain.size() >= 40 && crypto::equal(plain.first(32), sessionId), "NETWORK_SESSION",
                "会话身份不匹配");
        const auto sequence = qFromBigEndian<quint64>(plain.constData() + 32);
        require(sequence == rxSequence && rxSequence != std::numeric_limits<quint64>::max(), "NETWORK_REPLAY",
                "拒绝重复、乱序或过期报文");
        ++rxSequence;
        return parseObject(plain.mid(40));
    }
    QJsonObject request(const QJsonObject &object) {
        // A transport-successful write is not an operation success. Every public
        // request requires an authenticated server acknowledgement with ok=true.
        send(object);
        const auto response = receive();
        require(response.value("ok").toBool(), response.value("code").toString("NETWORK_ERROR"),
                response.value("error").toString("服务器拒绝请求"));
        return response;
    }
};

Session clientHandshake(Wire &wire, const RemoteOptions &options) {
    // Preserve the exact serialized ClientHello in the transcript. The server
    // hashes the bytes it received rather than reconstructing different JSON.
    // Both fresh random nonces bind every connection to distinct session keys.
    validName(options.user);
    validName(options.deviceId);
    require(options.deviceKey.size() == 32, "DEVICE_KEY", "设备配对密钥必须为32字节");
    const auto clientNonce = crypto::randomBytes(32);
    QJsonObject hello{{"version", 1},
                      {"user", options.user},
                      {"device", options.deviceId},
                      {"nonce", QString::fromLatin1(clientNonce.toHex())}};
    const auto helloBytes = jsonBytes(hello);
    wire.sendRaw(helloBytes);
    const auto response = wire.receiveJson();
    // A peer without the provisioned PSK cannot complete the server proof even
    // if it knows the account password or impersonates the server's IP address.
    const auto serverNonce = QByteArray::fromHex(response.value("nonce").toString().toLatin1());
    require(serverNonce.size() == 32, "NETWORK_AUTH", "服务器握手无效");
    const auto transcript = crypto::sha256(QByteArray("ChengVault/session/v1") + helloBytes + serverNonce);
    // Distinct proof labels prevent the client proof from doubling as a server
    // proof. The login password is sent only after this mutual device check.
    require(crypto::equal(crypto::hmac(options.deviceKey, "server" + transcript),
                          QByteArray::fromHex(response.value("proof").toString().toLatin1())),
            "NETWORK_AUTH", "服务器身份认证失败");
    wire.sendJson(
        {{"proof", QString::fromLatin1(crypto::hmac(options.deviceKey, "client" + transcript).toHex())}});
    Session session{wire, crypto::hkdf(options.deviceKey, transcript, "client-to-server"),
                    crypto::hkdf(options.deviceKey, transcript, "server-to-client"), transcript};
    // Successful device pairing alone grants no repository operations. A second
    // credential, the current account password, must pass inside this channel.
    session.request({{"op", "login"}, {"password", options.password}});
    return session;
}

QJsonArray inventory(const QString &root, const Context &context) {
    // Inventory hashes are hashes of ciphertext, useful to a storage service
    // that cannot see the plaintext HMAC domain or the protected file names.
    // Repository locking at the caller keeps the scan and upload consistent.
    QJsonArray result;
    require(QFileInfo::exists(root + "/repository.json"), "REPOSITORY_MISSING", "本地仓库不存在");
    QStringList files{"repository.json"};
    for (const auto &dir : {"objects", "snapshots"}) {
        QDirIterator it(root + "/" + dir, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
            files.append(QDir(root).relativeFilePath(it.next()));
    }
    for (const auto &path : files) {
        // Checking ancestor junctions matters as much as checking the leaf file.
        // Otherwise a repository-looking name could expose another directory.
        context.check();
        validPath(path);
        const auto full = root + "/" + path;
        safeAncestors(full);
        const auto size = QFileInfo(full).size();
        require(size > 0 && size <= MaxObject, "NETWORK_OBJECT", "仓库对象大小超过64MiB限制");
        result.append(QJsonObject{{"path", path},
                                  {"size", size},
                                  {"hash", QString::fromLatin1(crypto::hashFile(full, context).toHex())}});
    }
    return result;
}

void validateInventory(const QJsonArray &entries, quint64 quota) {
    // Validate the complete proposed state before accepting any object bytes.
    // Duplicate paths would otherwise make a supposedly atomic index ambiguous.
    // The bounded item count also bounds summation below uint64 overflow.
    require(!entries.isEmpty() && entries.size() <= 50000, "NETWORK_INVENTORY", "无效的仓库清单");
    QSet<QString> paths;
    QHash<QString, qint64> sizes;
    quint64 sum = 0;
    for (auto value : entries) {
        const auto item = value.toObject();
        const auto path = item.value("path").toString();
        validPath(path);
        validHash(item.value("hash").toString());
        require(!paths.contains(path), "NETWORK_INVENTORY", "仓库清单包含重复路径");
        paths.insert(path);
        const auto size = item.value("size").toVariant().toLongLong();
        require(size > 0 && size <= MaxObject, "NETWORK_OBJECT", "仓库对象大小无效");
        const auto hash = item.value("hash").toString();
        require(!sizes.contains(hash) || sizes.value(hash) == size, "NETWORK_INVENTORY",
                "相同对象摘要声明了不同长度");
        sizes[hash] = size;
        sum += static_cast<quint64>(size);
        require(sum <= quota, "QUOTA_EXCEEDED", "远程仓库超出账户配额");
    }
    require(paths.contains("repository.json"), "NETWORK_INVENTORY", "仓库清单缺少配置");
}
QJsonObject userRecord(const QString &password, bool admin, quint64 quota) {
    // Login credentials use a separate random salt for each account/reset.
    // Only a slow password verifier is stored, never a reversible password.
    // These parameters do not participate in repository encryption.
    require(!password.isEmpty(), "USER_PASSWORD", "网络账户密码不能为空");
    const auto salt = crypto::randomBytes(32);
    // JSON stores quota as a decimal string to avoid any cross-language loss of
    // integer precision. The C++ implementation converts it back to uint64.
    return {
        {"salt", QString::fromLatin1(salt.toHex())},
        {"password", QString::fromLatin1(crypto::pbkdf2(password.toUtf8(), salt, PasswordRounds).toHex())},
        {"iterations", PasswordRounds},
        {"administrator", admin},
        {"disabled", false},
        {"quota", QString::number(quota)},
        {"devices", QJsonObject{}}};
}

class AcceptServer : public QTcpServer {
  public:
    std::function<void(qintptr)> accept;

  protected:
    void incomingConnection(qintptr descriptor) override {
        accept(descriptor);
    }
};
} // namespace

struct Server::Impl {
    // The acceptor belongs to the caller's Qt event-loop thread. Each accepted
    // descriptor is adopted by exactly one blocking worker QTcpSocket. A shared
    // mutex protects account metadata and short atomic index transitions.
    // No QTcpSocket instance is accessed from two threads.
    QString root;
    AcceptServer server;
    mutable QMutex mutex;
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic_bool> finished;
    };
    std::vector<Worker> workers;
    std::atomic_bool stopping{false};
    std::atomic_int connections{0};
    // Persistent account schema:
    //   name -> {salt,password,iterations,administrator,disabled,quota,devices}
    // password is a PBKDF2 verifier, not a transport encryption key.
    // devices maps a device ID to a current-user DPAPI-protected random PSK.
    // The original PSK is necessary for HMAC authentication and HKDF sessions.
    // quota is independent of the client's own plaintext content compression.
    // Neither login verification nor device enrollment can derive a backup key.
    // Therefore password reset does not grant the administrator plaintext access.

    explicit Impl(QString directory) : root(QDir(directory).absolutePath()) {
        require(QDir().mkpath(root + "/users"), "IO_DIRECTORY", "无法创建服务器目录");
        server.accept = [this](qintptr descriptor) {
            // Reap completed sessions continuously instead of retaining one OS
            // thread handle for every historical connection to a long-lived server.
            for (auto it = workers.begin(); it != workers.end();) {
                if (it->finished->load()) {
                    it->thread.join();
                    it = workers.erase(it);
                } else
                    ++it;
            }
            if (connections.load() >= 32) {
                QTcpSocket rejected;
                rejected.setSocketDescriptor(descriptor);
                rejected.abort();
                return;
            }
            ++connections;
            auto finished = std::make_shared<std::atomic_bool>(false);
            workers.push_back({std::thread([this, descriptor, finished] {
                                   serve(descriptor);
                                   --connections;
                                   finished->store(true);
                               }),
                               finished});
        };
    }
    QJsonObject database() const {
        // Atomic JSON replacement is sufficient for the small course-scale
        // account database. The caller holds mutex during read/modify/write.
        return QFileInfo::exists(root + "/accounts.json") ? readJson(root + "/accounts.json") : QJsonObject{};
    }
    QJsonObject account(const QString &name) const {
        // Unknown and disabled identities never receive repository data.
        // Account lookup happens again for every authenticated operation.
        QMutexLocker lock(&mutex);
        const auto db = database();
        require(db.contains(name), "NETWORK_AUTH", "账户或设备认证失败");
        const auto record = db.value(name).toObject();
        require(!record.value("disabled").toBool(), "NETWORK_AUTH", "账户已禁用");
        return record;
    }
    void configureUser(const QString &name, const QString &password, bool admin, quint64 quota) {
        validName(name);
        require(quota > 0, "QUOTA", "配额必须大于零");
        const auto record = userRecord(password, admin, quota);
        QMutexLocker lock(&mutex);
        auto db = database();
        require(!db.contains(name), "USER_EXISTS", "账户已存在");
        // Existing accounts are never replaced by create-user. Password resets,
        // role-preserving changes and device management have separate operations.
        db[name] = record;
        writeJson(root + "/accounts.json", db);
    }
    void configureDevice(const QString &user, const QString &id, const QByteArray &key) {
        // DPAPI protects the locally provisioned PSK at rest. This is credential
        // storage only; transfer and backup encryption still use our algorithms.
        // A device is always nested under one account, never globally shared.
        validName(user);
        validName(id);
        require(key.size() == 32, "DEVICE_KEY", "设备密钥必须为32字节");
        QMutexLocker lock(&mutex);
        auto db = database();
        require(db.contains(user), "USER_MISSING", "账户不存在");
        auto record = db[user].toObject();
        auto devices = record["devices"].toObject();
        devices[id] = QString::fromLatin1(crypto::protectLocalKey(key).toBase64());
        record["devices"] = devices;
        db[user] = record;
        writeJson(root + "/accounts.json", db);
    }
    QString repositoryRoot(const QString &user, const QString &id) const {
        // User comes from authenticated session state, never from an arbitrary
        // request field. A client cannot select another account's storage root.
        validName(user);
        validName(id);
        return root + "/users/" + user + "/repositories/" + id;
    }
    static QJsonObject readIndex(const QString &directory) {
        return QFileInfo::exists(directory + "/index.json") ? readJson(directory + "/index.json")
                                                            : QJsonObject{};
    }
    quint64 otherUsage(const QString &user, const QString &exclude) const {
        // Calculate the final-state quota using committed inventories. Excluding
        // the repository being replaced avoids double charging its old version.
        quint64 result = 0;
        const QDir dir(root + "/users/" + user + "/repositories");
        for (const auto &id : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (id == exclude)
                continue;
            const auto index = readIndex(dir.filePath(id));
            for (auto item : index.value("inventory").toArray())
                result += item.toObject().value("size").toVariant().toULongLong();
        }
        return result;
    }
    QJsonObject administrative(const QString &user, const QString &action, const QJsonObject &args) {
        // Role checks are server-side and repeated even if a GUI hid controls.
        // Administrator list responses deliberately omit verifier and PSK data.
        require(account(user).value("administrator").toBool(), "ADMIN_REQUIRED", "需要管理员权限");
        const auto name = args.value("user").toString();
        if (action == "create-user") {
            configureUser(name, args.value("password").toString(), args.value("administrator").toBool(),
                          args.contains("quota") ? args.value("quota").toVariant().toULongLong()
                                                 : 10737418240ULL);
            return {{"ok", true}};
        }
        if (action == "register-device") {
            configureDevice(name, args.value("device").toString(),
                            QByteArray::fromHex(args.value("key").toString().toLatin1()));
            return {{"ok", true}};
        }
        QMutexLocker lock(&mutex);
        auto db = database();
        if (action == "list-users") {
            QJsonArray users;
            for (auto it = db.begin(); it != db.end(); ++it) {
                const auto record = it.value().toObject();
                QJsonArray devices;
                for (const auto &device : record["devices"].toObject().keys())
                    devices.append(device);
                users.append(QJsonObject{{"user", it.key()},
                                         {"administrator", record["administrator"]},
                                         {"disabled", record["disabled"]},
                                         {"quota", record["quota"]},
                                         {"devices", devices}});
            }
            return {{"ok", true}, {"users", users}};
        }
        validName(name);
        require(db.contains(name), "USER_MISSING", "账户不存在");
        auto record = db[name].toObject();
        if (action == "disable-user") {
            // Prevent this operation from locking out the administrator session
            // that is currently issuing it. Other accounts remain manageable.
            require(name != user, "ADMIN_SELF", "不能禁用当前管理员账户");
            record["disabled"] = args.value("disabled").toBool(true);
        } else if (action == "reset-password") {
            const auto fresh = userRecord(args.value("password").toString(), false, 0);
            record["salt"] = fresh["salt"];
            record["password"] = fresh["password"];
            record["iterations"] = fresh["iterations"];
        } else if (action == "set-quota") {
            const auto quota = args.value("quota").toVariant().toULongLong();
            require(quota > 0, "QUOTA", "配额必须大于零");
            record["quota"] = QString::number(quota);
        } else if (action == "revoke-device") {
            // The main request loop compares the enrolled record on each request,
            // so removing it also revokes sessions that were already connected.
            auto devices = record["devices"].toObject();
            devices.remove(args.value("device").toString());
            record["devices"] = devices;
        } else
            throw Error("ADMIN_ACTION", "未知账户管理操作");
        db[name] = record;
        writeJson(root + "/accounts.json", db);
        return {{"ok", true}};
    }
    void serve(qintptr descriptor) {
        // Unauthenticated errors simply close the connection. Once Session is
        // established, errors themselves are encrypted and authenticated.
        // Exception unwinding releases any in-progress repository upload lock.
        QTcpSocket socket;
        if (!socket.setSocketDescriptor(descriptor))
            return;
        Wire wire{socket, 5000};
        // Request state machine:
        //   hello -> proof -> encrypted login -> operations -> bye/disconnect
        // Upload additionally requires begin before any put or commit.
        // A pending upload capability belongs only to this authenticated session.
        // Completed blobs may survive a disconnected session and be reused later.
        // The upload lock ends on disconnect even when the client sends no bye.
        // No catch handler attempts to publish the pending inventory on failure.
        // The last committed index is the only authoritative downloadable state.
        try {
            const auto helloBytes = wire.receiveRaw();
            require(helloBytes.size() <= 4096, "NETWORK_AUTH", "握手报文过大");
            const auto hello = parseObject(helloBytes);
            require(hello.value("version").toInt() == 1, "NETWORK_VERSION", "协议版本不支持");
            const auto user = hello.value("user").toString();
            validName(user);
            const auto device = hello.value("device").toString();
            validName(device);
            const auto credentials = account(user);
            const auto devices = credentials.value("devices").toObject();
            require(devices.contains(device), "NETWORK_AUTH", "账户或设备认证失败");
            const auto key =
                crypto::unprotectLocalKey(QByteArray::fromBase64(devices[device].toString().toLatin1()));
            const auto clientNonce = QByteArray::fromHex(hello.value("nonce").toString().toLatin1());
            require(clientNonce.size() == 32 && key.size() == 32, "NETWORK_AUTH", "握手随机数无效");
            const auto serverNonce = crypto::randomBytes(32);
            const auto transcript =
                crypto::sha256(QByteArray("ChengVault/session/v1") + helloBytes + serverNonce);
            wire.sendJson({{"nonce", QString::fromLatin1(serverNonce.toHex())},
                           {"proof", QString::fromLatin1(crypto::hmac(key, "server" + transcript).toHex())}});
            const auto proof = wire.receiveJson();
            require(crypto::equal(crypto::hmac(key, "client" + transcript),
                                  QByteArray::fromHex(proof.value("proof").toString().toLatin1())),
                    "NETWORK_AUTH", "设备身份认证失败");
            Session session{wire, crypto::hkdf(key, transcript, "server-to-client"),
                            crypto::hkdf(key, transcript, "client-to-server"), transcript};
            try {
                const auto login = session.receive();
                const auto passwordHash =
                    crypto::pbkdf2(login.value("password").toString().toUtf8(),
                                   QByteArray::fromHex(credentials["salt"].toString().toLatin1()),
                                   credentials["iterations"].toInt());
                require(login.value("op") == "login" &&
                            crypto::equal(passwordHash,
                                          QByteArray::fromHex(credentials["password"].toString().toLatin1())),
                        "NETWORK_LOGIN", "账户密码错误");
                session.send({{"ok", true}});
                QJsonArray pendingInventory;
                QString pendingRepo;
                std::unique_ptr<QLockFile> uploadLock;
                // One session can stage at most one upload. All put operations
                // are restricted to the inventory accepted by that begin call.
                // Disconnecting keeps partial bytes but discards the capability.
                while (!stopping.load()) {
                    const auto request = session.receive();
                    const auto op = request.value("op").toString();
                    if (op == "bye") {
                        session.send({{"ok", true}});
                        break;
                    }
                    const auto current = account(user);
                    require(current["devices"].toObject().value(device) == devices.value(device),
                            "DEVICE_REVOKED", "设备授权已撤销或密钥已更换");
                    require(current["password"] == credentials["password"] &&
                                current["salt"] == credentials["salt"],
                            "LOGIN_EXPIRED", "账户密码已更换，请重新登录");
                    if (op == "admin") {
                        session.send(administrative(user, request.value("action").toString(),
                                                    request.value("args").toObject()));
                        continue;
                    }
                    if (op == "list") {
                        // Do not enumerate arbitrary user directories from JSON.
                        // The server derives the account root from this session.
                        QMutexLocker lock(&mutex);
                        QJsonArray list;
                        const QDir repositories(root + "/users/" + user + "/repositories");
                        for (const auto &id : repositories.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                            auto index = readIndex(repositories.filePath(id));
                            if (index.isEmpty())
                                continue;
                            index.remove("inventory");
                            list.append(index);
                        }
                        session.send({{"ok", true}, {"repositories", list}});
                        continue;
                    }
                    const auto repo = request.value("repository").toString();
                    const auto directory = repositoryRoot(user, repo);
                    if (op == "begin") {
                        // Verify existing blobs instead of trusting a filename
                        // or size; the client may be repairing corrupted storage.
                        // A partial blob can resume only below its expected size.
                        require(!uploadLock, "UPLOAD_ACTIVE", "此会话已有上传任务");
                        require(QDir().mkpath(directory + "/blobs"), "IO_DIRECTORY", "无法建立远程仓库");
                        uploadLock = std::make_unique<QLockFile>(directory + "/upload.lock");
                        uploadLock->setStaleLockTime(0);
                        require(uploadLock->tryLock(0), "UPLOAD_BUSY", "此仓库已有上传任务");
                        const auto quota = current["quota"].toString().toULongLong();
                        QMutexLocker lock(&mutex);
                        const auto used = otherUsage(user, repo);
                        require(used <= quota, "QUOTA_EXCEEDED", "账户配额已用尽");
                        pendingInventory = request.value("inventory").toArray();
                        // The accepted inventory is retained server-side. Later
                        // puts cannot replace it with an unchecked alternative.
                        validateInventory(pendingInventory, quota - used);
                        pendingRepo = repo;
                        QJsonArray missing;
                        QSet<QString> added;
                        // Inventory entries carry {path,size,hash}. path restores
                        // the local repository layout; hash addresses ciphertext.
                        // The server stores blobs by hash, not by client path.
                        // Repeated hashes are requested at most once per session.
                        // Every returned offset comes from an existing .part file.
                        // A complete but invalid part deliberately restarts at zero.
                        for (auto entry : pendingInventory) {
                            const auto item = entry.toObject();
                            const auto hash = item["hash"].toString();
                            if (added.contains(hash))
                                continue;
                            added.insert(hash);
                            const auto blob = directory + "/blobs/" + hash;
                            bool valid =
                                QFileInfo(blob).size() == item["size"].toVariant().toLongLong() &&
                                QFileInfo::exists(blob) &&
                                crypto::equal(crypto::hashFile(blob), QByteArray::fromHex(hash.toLatin1()));
                            if (!valid) {
                                const auto part = blob + ".part";
                                qint64 offset = QFileInfo::exists(part) ? QFileInfo(part).size() : 0;
                                if (offset < 0 || offset >= item["size"].toVariant().toLongLong())
                                    offset = 0;
                                missing.append(QJsonObject{{"hash", hash}, {"offset", offset}});
                            }
                        }
                        session.send({{"ok", true}, {"missing", missing}});
                        continue;
                    }
                    if (op == "put") {
                        // Strict contiguous offsets prevent overlapping writes,
                        // sparse-file inflation and appending to unrelated blobs.
                        // Only a complete digest-verified part becomes immutable.
                        require(uploadLock && pendingRepo == repo, "UPLOAD_STATE", "未开始此仓库上传");
                        const auto hash = request["hash"].toString();
                        validHash(hash);
                        qint64 expected = -1;
                        for (auto entry : pendingInventory)
                            if (entry.toObject()["hash"] == hash)
                                expected = entry.toObject()["size"].toVariant().toLongLong();
                        require(expected > 0, "UPLOAD_OBJECT", "对象不属于上传清单");
                        const auto offset = request["offset"].toVariant().toLongLong();
                        const auto bytes = QByteArray::fromBase64(request["data"].toString().toLatin1());
                        require(offset >= 0 && !bytes.isEmpty() && bytes.size() <= ChunkSize &&
                                    offset <= expected - bytes.size(),
                                "UPLOAD_RANGE", "上传分段越界");
                        const auto blob = directory + "/blobs/" + hash;
                        QFile file(blob + ".part");
                        require(file.open(QIODevice::ReadWrite), "IO_OPEN", "无法写入上传暂存文件");
                        if (offset == 0)
                            require(file.resize(0), "IO_WRITE", "无法重置上传暂存文件");
                        require(file.size() == offset && file.seek(offset) &&
                                    file.write(bytes) == bytes.size() && file.flush(),
                                "UPLOAD_OFFSET", "上传偏移不一致或磁盘写入失败");
                        const bool complete = file.size() == expected;
                        file.close();
                        if (complete) {
                            // Keep an incomplete object in .part form until its
                            // full ciphertext hash matches the inventory digest.
                            require(crypto::equal(crypto::hashFile(blob + ".part"),
                                                  QByteArray::fromHex(hash.toLatin1())),
                                    "UPLOAD_HASH", "上传对象摘要不一致，请重新上传");
                            atomicWrite(blob, readFile(blob + ".part", MaxObject));
                            QFile::remove(blob + ".part");
                        }
                        session.send(
                            {{"ok", true}, {"offset", offset + bytes.size()}, {"complete", complete}});
                        continue;
                    }
                    if (op == "commit") {
                        // Recheck quota under the shared mutex: two different
                        // repositories may have begun using the same free quota.
                        // The new index becomes visible in one atomic replacement.
                        require(uploadLock && pendingRepo == repo, "UPLOAD_STATE", "未开始此仓库上传");
                        QMutexLocker lock(&mutex);
                        const auto quota = current["quota"].toString().toULongLong();
                        const auto used = otherUsage(user, repo);
                        require(used <= quota, "QUOTA_EXCEEDED", "账户配额已用尽");
                        validateInventory(pendingInventory, quota - used);
                        qint64 size = 0;
                        int snapshots = 0;
                        QSet<QString> keep;
                        // index.json is the public repository catalog. It records
                        // repository ID, last update time and encrypted byte size.
                        // Snapshots and object counts are derived from inventory.
                        // Plain filenames, ACLs and owner SIDs remain encrypted.
                        // index references are checked before QSaveFile commits.
                        // Only after commit may garbage collection remove old blobs.
                        for (auto entry : pendingInventory) {
                            const auto item = entry.toObject();
                            const auto hash = item["hash"].toString();
                            require(QFileInfo(directory + "/blobs/" + hash).size() ==
                                        item["size"].toVariant().toLongLong(),
                                    "UPLOAD_INCOMPLETE", "上传对象尚未全部提交");
                            keep.insert(hash);
                            size += item["size"].toVariant().toLongLong();
                            if (item["path"].toString().startsWith("snapshots/"))
                                ++snapshots;
                        }
                        const QJsonObject index{
                            {"id", repo},
                            {"updatedUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                            {"size", size},
                            {"snapshots", snapshots},
                            {"objects", pendingInventory.size()},
                            {"inventory", pendingInventory}};
                        writeJson(directory + "/index.json", index);
                        // Publish the new index before collecting old blobs. Download sessions
                        // hold the account mutex while reading each block, and old clients get
                        // an explicit stale-version error rather than a mixed snapshot.
                        for (const auto &name : QDir(directory + "/blobs").entryList(QDir::Files))
                            if (!name.endsWith(".part") && !keep.contains(name))
                                QFile::remove(directory + "/blobs/" + name);
                        uploadLock.reset();
                        pendingInventory = {};
                        pendingRepo.clear();
                        session.send({{"ok", true}, {"size", size}, {"snapshots", snapshots}});
                        continue;
                    }
                    if (op == "index") {
                        // Staged uploads have no committed index and are not yet
                        // downloadable, even if several blobs already arrived.
                        QMutexLocker lock(&mutex);
                        const auto index = readIndex(directory);
                        require(!index.isEmpty(), "REMOTE_MISSING", "当前账户中没有此仓库");
                        session.send({{"ok", true}, {"index", index}});
                        continue;
                    }
                    if (op == "get") {
                        // A hash is not a bearer capability. Check membership in
                        // THIS user's current committed repository before reading.
                        // Server-side path construction never uses client paths.
                        QMutexLocker lock(&mutex);
                        const auto hash = request["hash"].toString();
                        validHash(hash);
                        qint64 expected = -1;
                        for (auto entry : readIndex(directory)["inventory"].toArray())
                            if (entry.toObject()["hash"] == hash)
                                expected = entry.toObject()["size"].toVariant().toLongLong();
                        require(expected > 0, "REMOTE_OBJECT", "对象不属于当前仓库版本");
                        const auto offset = request["offset"].toVariant().toLongLong();
                        require(offset >= 0 && offset < expected, "DOWNLOAD_RANGE", "下载偏移越界");
                        QFile file(directory + "/blobs/" + hash);
                        require(file.open(QIODevice::ReadOnly) && file.seek(offset), "IO_READ",
                                "无法读取远程对象");
                        const auto bytes = file.read(qMin<qint64>(ChunkSize, expected - offset));
                        require(!bytes.isEmpty(), "IO_READ", "远程对象不完整");
                        session.send({{"ok", true},
                                      {"data", QString::fromLatin1(bytes.toBase64())},
                                      {"size", expected}});
                        continue;
                    }
                    throw Error("NETWORK_OPERATION", "未知网络操作");
                }
            } catch (const Error &error) {
                try {
                    session.send(
                        {{"ok", false}, {"code", error.code}, {"error", QString::fromUtf8(error.what())}});
                } catch (...) {
                }
            }
        } catch (...) { /* Failed unauthenticated sessions receive no useful oracle. */
        }
        socket.disconnectFromHost();
    }
};

Server::Server(QString root) : impl_(std::make_unique<Impl>(std::move(root))) {}
Server::~Server() {
    stop();
}
void Server::configureUser(const QString &name, const QString &password, bool admin, quint64 quota) {
    impl_->configureUser(name, password, admin, quota);
}
void Server::configureDevice(const QString &user, const QString &id, const QByteArray &key) {
    impl_->configureDevice(user, id, key);
}
bool Server::listen(const QHostAddress &address, quint16 listenPort) {
    impl_->stopping = false;
    return impl_->server.listen(address, listenPort);
}
quint16 Server::port() const {
    return impl_->server.serverPort();
}
QString Server::errorString() const {
    return impl_->server.errorString();
}
void Server::stop() {
    // Joining workers ensures no account or index write can outlive the server
    // object. Blocking sockets have bounded waits, including during shutdown.
    impl_->server.close();
    impl_->stopping = true;
    for (auto &worker : impl_->workers)
        if (worker.thread.joinable())
            worker.thread.join();
    impl_->workers.clear();
}

Client::Client(RemoteOptions options) : options_(std::move(options)) {}
namespace {
void connect(QTcpSocket &socket, const RemoteOptions &options) {
    require(options.timeoutMs >= 100 && options.timeoutMs <= 120000, "NETWORK_TIMEOUT", "网络超时参数无效");
    socket.connectToHost(options.host, options.port);
    require(socket.waitForConnected(options.timeoutMs), "NETWORK_CONNECT",
            "无法连接备份服务器：" + socket.errorString());
}
} // namespace
QJsonArray Client::list() const {
    // Public interfaces share the same handshake and message implementation;
    // read-only list calls do not bypass login or device authentication.
    QTcpSocket socket;
    connect(socket, options_);
    Wire wire{socket, options_.timeoutMs};
    auto session = clientHandshake(wire, options_);
    const auto result = session.request({{"op", "list"}}).value("repositories").toArray();
    session.request({{"op", "bye"}});
    return result;
}
QJsonObject Client::admin(const QString &action, const QJsonObject &arguments) const {
    QTcpSocket socket;
    connect(socket, options_);
    Wire wire{socket, options_.timeoutMs};
    auto session = clientHandshake(wire, options_);
    const auto result = session.request({{"op", "admin"}, {"action", action}, {"args", arguments}});
    session.request({{"op", "bye"}});
    return result;
}
TaskResult Client::upload(const QString &repositoryPath, const Context &context) const {
    // Hold the repository lock through commit so concurrent pruning, password
    // replacement or backup cannot alter this inventory during transmission.
    // Cancellation closes the session and leaves the previous index visible.
    const auto root = QDir(repositoryPath).absolutePath();
    QLockFile lock(root + "/repository.lock");
    lock.setStaleLockTime(0);
    require(lock.tryLock(0), "REPOSITORY_BUSY", "本地仓库正由另一个任务使用");
    const auto entries = inventory(root, context);
    const auto repo = readJson(root + "/repository.json")["id"].toString();
    validName(repo);
    QTcpSocket socket;
    connect(socket, options_);
    Wire wire{socket, options_.timeoutMs};
    auto session = clientHandshake(wire, options_);
    const auto response = session.request({{"op", "begin"}, {"repository", repo}, {"inventory", entries}});
    const auto missing = response.value("missing").toArray();
    TaskResult result;
    quint64 resumedBytes = 0;
    // Three different size counters are deliberately retained:
    // bytes counts object payload transmitted during this attempt.
    // resumedBytes counts prefixes already present from an earlier attempt.
    // wire.sent includes framing, authentication and metadata-control traffic.
    // None of these is the logical original-file size advertised by a snapshot.
    // This distinction makes zero-payload incremental demonstrations auditable.
    for (auto value : missing) {
        context.check();
        const auto item = value.toObject();
        const auto hash = item["hash"].toString();
        QString path;
        qint64 size = 0;
        for (auto entry : entries)
            if (entry.toObject()["hash"] == hash) {
                path = entry.toObject()["path"].toString();
                size = entry.toObject()["size"].toVariant().toLongLong();
                break;
            }
        // Even an authenticated peer must not request arbitrary local files.
        // Resolve its hash only against the inventory captured for this upload.
        require(!path.isEmpty(), "NETWORK_INVENTORY", "服务器请求了未知对象");
        qint64 offset = item["offset"].toVariant().toLongLong();
        require(offset >= 0 && offset < size, "NETWORK_OFFSET", "服务器返回无效续传偏移");
        resumedBytes += static_cast<quint64>(offset);
        // The server's continuation offset is checked against the exact local
        // object size before seeking. A peer cannot seek beyond this source.
        QFile file(root + "/" + path);
        require(file.open(QIODevice::ReadOnly) && file.seek(offset), "IO_OPEN", "无法读取上传对象");
        while (offset < size) {
            context.check();
            const auto bytes = file.read(qMin<qint64>(ChunkSize, size - offset));
            require(!bytes.isEmpty(), "IO_READ", "上传对象发生截断");
            const auto reply = session.request({{"op", "put"},
                                                {"repository", repo},
                                                {"hash", hash},
                                                {"offset", offset},
                                                {"data", QString::fromLatin1(bytes.toBase64())}});
            offset += bytes.size();
            result.bytes += static_cast<quint64>(bytes.size());
            require(reply["offset"].toVariant().toLongLong() == offset, "NETWORK_OFFSET",
                    "服务器确认偏移错误");
            context.update(result.files, static_cast<quint64>(missing.size()), path);
        }
        ++result.files;
    }
    context.check();
    session.request({{"op", "commit"}, {"repository", repo}});
    session.request({{"op", "bye"}});
    result.output = repo;
    result.storedBytes = result.bytes;
    result.details = {{"repositoryId", repo},
                      {"uploadedObjects", static_cast<qint64>(result.files)},
                      {"reusedObjects", entries.size() - missing.size()},
                      {"payloadBytes", static_cast<qint64>(result.bytes)},
                      {"sentBytes", static_cast<qint64>(wire.sent)},
                      {"receivedBytes", static_cast<qint64>(wire.received)},
                      {"resumedBytes", static_cast<qint64>(resumedBytes)}};
    return result;
}

TaskResult Client::download(const QString &repositoryId, const QString &localPath,
                            const Context &context) const {
    // Download is a repository materialization, not a file restoration policy.
    // Require an empty final directory and publish only after all object hashes
    // and the repository identity are valid. Restore handles user-file conflicts.
    validName(repositoryId);
    const auto root = QDir(localPath).absolutePath();
    safeAncestors(root);
    require(!QDir(root).exists() || QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
            "DOWNLOAD_DESTINATION", "下载目标必须是空目录；已有仓库请使用新的下载目录");
    const auto stage = root + ".cv-download-" + repositoryId;
    safeAncestors(stage);
    require(QDir().mkpath(stage), "IO_DIRECTORY", "无法创建下载暂存目录");
    QLockFile lock(stage + "/download.lock");
    lock.setStaleLockTime(0);
    require(lock.tryLock(0), "DOWNLOAD_BUSY", "此下载任务已在运行");
    QTcpSocket socket;
    connect(socket, options_);
    Wire wire{socket, options_.timeoutMs};
    auto session = clientHandshake(wire, options_);
    const auto index = session.request({{"op", "index"}, {"repository", repositoryId}})["index"].toObject();
    const auto entries = index["inventory"].toArray();
    validateInventory(entries, std::numeric_limits<quint64>::max());
    // A network response is validated just like a locally parsed archive. The
    // destination path never derives directly from unchecked remote JSON.
    TaskResult result;
    // Downloads authenticate transport first and verify ciphertext hashes second.
    // Repository opening then authenticates the password-wrapped master key.
    // Actual restoration adds envelope, decompressor and raw-content validation.
    // A successful download alone does not claim that a supplied password works.
    // Publication into an empty directory keeps those responsibilities separate.
    for (auto value : entries) {
        context.check();
        const auto item = value.toObject();
        const auto path = item["path"].toString();
        const auto hash = item["hash"].toString();
        const auto size = item["size"].toVariant().toLongLong();
        const auto filePath = stage + "/" + path;
        safeAncestors(filePath);
        if (QFileInfo::exists(filePath) && QFileInfo(filePath).size() == size &&
            crypto::equal(crypto::hashFile(filePath, context), QByteArray::fromHex(hash.toLatin1()))) {
            ++result.files;
            continue;
        }
        require(QDir().mkpath(QFileInfo(filePath).absolutePath()), "IO_DIRECTORY", "无法创建下载目录");
        // A hash-specific partial name lets interrupted downloads resume without
        // accidentally appending bytes from an older remote version.
        const auto partPath = filePath + "." + hash + ".part";
        safeAncestors(partPath);
        QFile file(partPath);
        require(file.open(QIODevice::ReadWrite), "IO_OPEN", "无法写入下载对象");
        qint64 offset = file.size();
        if (offset < 0 || offset >= size) {
            file.resize(0);
            offset = 0;
        }
        require(file.seek(offset), "IO_SEEK", "无法定位下载偏移");
        while (offset < size) {
            context.check();
            const auto response = session.request(
                {{"op", "get"}, {"repository", repositoryId}, {"hash", hash}, {"offset", offset}});
            const auto bytes = QByteArray::fromBase64(response["data"].toString().toLatin1());
            require(!bytes.isEmpty() && bytes.size() <= ChunkSize && bytes.size() <= size - offset,
                    "DOWNLOAD_RANGE", "服务器返回越界数据");
            require(file.write(bytes) == bytes.size(), "IO_WRITE", "下载写入失败");
            offset += bytes.size();
            result.bytes += static_cast<quint64>(bytes.size());
            context.update(result.files, static_cast<quint64>(entries.size()), path);
        }
        require(file.flush(), "IO_WRITE", "无法刷新下载对象");
        file.close();
        require(crypto::equal(crypto::hashFile(partPath, context), QByteArray::fromHex(hash.toLatin1())),
                "DOWNLOAD_HASH", "下载对象摘要不匹配");
        atomicWrite(filePath, readFile(partPath, MaxObject));
        QFile::remove(partPath);
        ++result.files;
    }
    session.request({{"op", "bye"}});
    context.check();
    require(readJson(stage + "/repository.json")["id"].toString() == repositoryId, "REPOSITORY_ID",
            "下载仓库身份不匹配");
    QSet<QString> expectedPaths;
    for (auto value : entries)
        expectedPaths.insert(value.toObject()["path"].toString());
    QDirIterator leftovers(stage, QDir::Files, QDirIterator::Subdirectories);
    while (leftovers.hasNext()) {
        const auto full = leftovers.next();
        const auto relative = QDir(stage).relativeFilePath(full);
        if (relative == "download.lock" || expectedPaths.contains(relative))
            continue;
        safeAncestors(full);
        auto original = relative;
        if (original.endsWith(".part")) {
            require(original.size() > 70 && original[original.size() - 70] == '.', "DOWNLOAD_STAGE",
                    "暂存目录中存在未知文件");
            validHash(original.mid(original.size() - 69, 64));
            original.chop(70);
        }
        validPath(original);
        // A resumed download may now target a newer server index. Remove only
        // recognized internal artifacts that are absent from that exact index,
        // otherwise a stale completed snapshot could become visible locally.
        require(QFile::remove(full), "IO_REMOVE", "无法清理旧下载版本的暂存对象");
    }
    lock.unlock();
    // A final same-volume directory rename publishes the verified repository.
    // On failure the complete stage remains available and no old files are lost.
    if (QDir(root).exists())
        require(QDir().rmdir(root), "DOWNLOAD_DESTINATION", "下载目标目录已发生变化");
    require(QDir().rename(stage, root), "IO_RENAME", "无法发布下载结果，完整数据保留在暂存目录");
    result.output = root;
    result.details = {{"repositoryId", repositoryId},
                      {"receivedBytes", static_cast<qint64>(wire.received)},
                      {"sentBytes", static_cast<qint64>(wire.sent)}};
    return result;
}
} // namespace cv::network
