#include "chengvault/repository.hpp"
#include "chengvault/compression.hpp"
#include "chengvault/crypto.hpp"
#include "chengvault/filesystem.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSet>
#include <algorithm>

namespace cv {
namespace {
// Repository v1 separates two identities:
//   1. A random repository ID isolates server-side namespaces.
//   2. A random 256-bit master key isolates the cryptographic namespace.
// Neither identity is derived from the user password. A password replacement
// therefore cannot change content IDs or force all existing blocks to upload.
// The only password-dependent bytes live in repository.json's key envelope.
constexpr int KdfIterations = 120000;
// repository.json is public configuration, not a plaintext file index.
// version selects the parser and rejects unknown incompatible changes.
// id identifies this random repository namespace on the storage server.
// cipher selects the independently authenticated encryption of new objects.
// salt and iterations parameterize password-to-wrapping-key derivation.
// wrappedKey contains the encrypted random master key.
// proof binds the recovered master to the public ID and content cipher.
// createdUtc is informational and never used as an authorization decision.
void validId(const QString &objectId) {
    require(objectId.size() >= 8 && objectId.size() <= 80, "REPOSITORY_ID", "无效的对象标识");
    for (QChar c : objectId)
        require((c.isLetterOrNumber() && c.unicode() < 128) || c == '-', "REPOSITORY_ID",
                "对象标识含非法字符");
}
QString objectPath(const QString &root, const QString &objectId) {
    // Two hexadecimal prefix characters distribute large repositories across
    // 256 directories. An ID is validated before joining it to a root path.
    validId(objectId);
    return root + "/objects/" + objectId.left(2) + "/" + objectId + ".cvo";
}
QString snapshotPath(const QString &root, const QString &objectId) {
    validId(objectId);
    return root + "/snapshots/" + objectId + ".cvs";
}
QSet<QString> referenced(const QJsonObject &manifest) {
    // A hardlink references its canonical file and contributes no second data
    // stream. ADS and sparse ranges are already represented by stream chunks.
    // Looking only at ordinary-file data would delete live ADS during pruning.
    QSet<QString> ids;
    for (auto entry : manifest.value("entries").toArray())
        for (auto stream : entry.toObject().value("streams").toArray())
            for (auto chunk : stream.toObject().value("chunks").toArray()) {
                const auto objectId = chunk.toObject().value("id").toString();
                validId(objectId);
                ids.insert(objectId);
            }
    return ids;
}
void lockRepository(QLockFile &lock) {
    // A long task is never considered stale merely because it exceeds a timer.
    // QLockFile can still distinguish a process that has actually terminated.
    // Fail immediately so the UI can show "busy" rather than appear frozen.
    lock.setStaleLockTime(0);
    require(lock.tryLock(0), "REPOSITORY_BUSY", "仓库正由另一个任务使用");
}
QByteArray configProof(const QJsonObject &config, const QByteArray &key) {
    // The password envelope authenticates the master key itself. This second
    // proof binds that key to the public repository ID and selected cipher.
    // A modified plaintext config cannot redirect authenticated object access.
    return crypto::hmac(key, QByteArray("ChengVault/repository/v1\0", 25) +
                                 config.value("id").toString().toUtf8() + '\0' +
                                 config.value("cipher").toString().toUtf8());
}
void validateConfig(const QJsonObject &config) {
    // Bound the KDF work factor before invoking the deliberately expensive KDF.
    // Otherwise a hostile config could make a password prompt hang for hours.
    // An upper bound is a resource limit, not an algorithm downgrade fallback.
    require(config.value("version").toInt() == 1, "REPOSITORY_VERSION", "不支持的仓库版本");
    validId(config.value("id").toString());
    const auto cipher = config.value("cipher").toString();
    require(cipher == "aes256" || cipher == "aes256-ctr" || cipher == "chacha20", "REPOSITORY_CIPHER",
            "无效的仓库加密算法");
    require(config.value("iterations").toInt() >= 10000 && config.value("iterations").toInt() <= 2000000,
            "REPOSITORY_KDF", "无效的密码派生参数");
}
} // namespace

Repository::Repository(QString path, QString password)
    : path_(QDir(path).absolutePath()), password_(std::move(password)) {
    if (!QFileInfo::exists(path_ + "/repository.json"))
        return;
    require(!password_.isEmpty(), "REPOSITORY_PASSWORD_EMPTY", "仓库密码不能为空");
    config_ = readJson(path_ + "/repository.json");
    validateConfig(config_);
    // Authenticate before exposing any decrypted manifest or data to callers.
    // Once unlocked, the plaintext password is no longer needed by this object.
    const auto salt = QByteArray::fromHex(config_.value("salt").toString().toLatin1());
    require(salt.size() == 32, "REPOSITORY_KDF", "损坏的密码盐");
    const auto wrappingKey = crypto::pbkdf2(password_.toUtf8(), salt, config_.value("iterations").toInt());
    key_ =
        crypto::open(QByteArray::fromBase64(config_.value("wrappedKey").toString().toLatin1()), wrappingKey);
    require(key_.size() == 32 &&
                crypto::equal(configProof(config_, key_),
                              QByteArray::fromHex(config_.value("proof").toString().toLatin1())),
            "REPOSITORY_KEY", "仓库配置或主密钥无效");
    password_.clear();
}

Repository::Repository(QString path, QByteArray key, bool)
    : path_(QDir(path).absolutePath()), key_(std::move(key)) {
    // The scheduler obtains this key only after current-user DPAPI unwrapping.
    // The repository proof prevents accidentally using another task's cache.
    config_ = readJson(path_ + "/repository.json");
    validateConfig(config_);
    require(key_.size() == 32 &&
                crypto::equal(configProof(config_, key_),
                              QByteArray::fromHex(config_.value("proof").toString().toLatin1())),
            "REPOSITORY_KEY", "缓存密钥不属于此仓库");
}
Repository Repository::withMasterKey(const QString &path, const QByteArray &key) {
    return Repository(path, key, true);
}

void Repository::wrapKey(const QString &password) {
    require(!password.isEmpty(), "REPOSITORY_PASSWORD_EMPTY", "仓库密码不能为空");
    // A new salt invalidates precomputed guesses and changes the wrapping key.
    // seal additionally generates independent envelope salt/nonce material.
    // QSaveFile publication in writeJson preserves the old password envelope
    // if the disk fills or the replacement cannot be committed.
    const auto salt = crypto::randomBytes(32);
    const auto wrappingKey = crypto::pbkdf2(password.toUtf8(), salt, KdfIterations);
    config_["salt"] = QString::fromLatin1(salt.toHex());
    config_["iterations"] = KdfIterations;
    config_["wrappedKey"] = QString::fromLatin1(crypto::seal(key_, wrappingKey, "chacha20").toBase64());
    config_["proof"] = QString::fromLatin1(configProof(config_, key_).toHex());
    writeJson(path_ + "/repository.json", config_);
}

void Repository::init(const QString &cipher) {
    // Only a new empty directory can become a repository. This avoids silently
    // mixing foreign objects with a newly generated cryptographic namespace.
    // Single-file archives offer an unencrypted mode; repositories consistently
    // encrypt all content and metadata, including when a default plan says none.
    require(config_.isEmpty(), "REPOSITORY_EXISTS", "仓库已初始化");
    require(!password_.isEmpty(), "REPOSITORY_PASSWORD_EMPTY", "仓库密码不能为空");
    require(cipher == "chacha20" || cipher == "aes256" || cipher == "aes256-ctr", "REPOSITORY_CIPHER",
            "仓库必须选择 AES-256 或 ChaCha20");
    QDir dir(path_);
    require(!dir.exists() || dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
            "REPOSITORY_NOT_EMPTY", "新仓库目录必须为空");
    require(QDir().mkpath(path_ + "/objects") && QDir().mkpath(path_ + "/snapshots"), "IO_DIRECTORY",
            "无法创建仓库目录");
    QLockFile lock(path_ + "/repository.lock");
    lockRepository(lock);
    key_ = crypto::randomBytes(32);
    config_ = {{"version", 1},
               {"id", uniqueId()},
               {"cipher", cipher},
               {"createdUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    wrapKey(password_);
    password_.clear();
}

QByteArray serializeSnapshot(const QJsonObject &snapshot) {
    // The manifest is the snapshot commit record. Enforce the shared 64 MiB
    // format ceiling before any write so an oversized tree cannot create a
    // snapshot that readers later refuse to parse.
    const auto bytes = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
    require(bytes.size() <= ManifestLimit, "MANIFEST_SIZE", "快照清单超过 64 MiB 上限");
    return bytes;
}
TaskResult Repository::backup(const BackupPlan &plan, const Context &context) {
    if (config_.isEmpty())
        init(plan.cipher == "none" ? "chacha20" : plan.cipher);
    QLockFile lock(path_ + "/repository.lock");
    lockRepository(lock);
    quint64 newBytes = 0, newChunks = 0, reusedChunks = 0;
    // Snapshot completeness and incremental storage are independent concepts.
    // Every snapshot lists all selected paths, even when no new block is stored.
    // Reusing a chunk never means inheriting an older, incomplete file listing.
    // A newer snapshot remains independently restorable after older snapshots
    // are deleted, because pruning retains its entire transitive block set.
    QSet<QString> verifiedExisting;
    // Content identity is computed BEFORE compression and randomized encryption.
    // Repeated plaintext therefore yields a stable ID inside this repository,
    // while the HMAC prevents an untrusted server from testing known plaintext
    // using a public SHA-256 dictionary. IDs differ across repository keys.
    const auto sink = [&](const QByteArray &bytes) {
        context.check();
        require(bytes.size() <= ChunkSize, "CHUNK_SIZE", "数据块超过大小上限");
        const QString objectId =
            QString::fromLatin1(crypto::hmac(key_, QByteArray("chunk\0", 6) + bytes).toHex());
        const auto file = objectPath(path_, objectId);
        if (QFileInfo::exists(file)) {
            // A corrupted existing object must not silently infect a new snapshot.
            if (!verifiedExisting.contains(objectId)) {
                require(loadChunk(objectId) == bytes, "CHUNK_INTEGRITY", "已有数据块校验失败");
                verifiedExisting.insert(objectId);
            }
            ++reusedChunks;
        } else {
            // Each new object is independently compressed and authenticated.
            // This permits random restoration without decoding preceding files.
            // Successful object writes alone do not create a successful snapshot.
            const auto compressed = compression::encode(bytes, plan.compression);
            const auto envelope = crypto::seal(compressed, key_, config_.value("cipher").toString());
            atomicWrite(file, envelope);
            newBytes += static_cast<quint64>(envelope.size());
            ++newChunks;
        }
        return objectId;
    };
    auto effectivePlan = plan;
    effectivePlan.destination = path_;
    // fs::capture owns traversal, filtering, metadata and file-stability checks.
    // Passing the actual repository destination also lets it reject recursive
    // source/destination overlap before processing the directory tree.
    auto snapshot = fs::capture(effectivePlan, sink, context);
    const QString objectId = uniqueId();
    snapshot["snapshot"] =
        QJsonObject{{"id", objectId},
                    {"createdUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                    {"compression", plan.compression},
                    {"newChunks", static_cast<qint64>(newChunks)},
                    {"reusedChunks", static_cast<qint64>(reusedChunks)},
                    {"newBytes", static_cast<qint64>(newBytes)}};
    fs::validateManifest(snapshot);
    context.check();
    // The manifest is the commit record. Every referenced block already exists
    // before this atomic write. Cancellation or a crash can leave orphan blocks,
    // but can never advertise a snapshot that was only partly captured.
    const auto serialized = serializeSnapshot(snapshot);
    atomicWrite(snapshotPath(path_, objectId),
                crypto::seal(serialized, key_, config_.value("cipher").toString()));
    TaskResult result;
    result.files = snapshot.value("fileCount").toVariant().toULongLong();
    result.bytes = snapshot.value("totalBytes").toVariant().toULongLong();
    result.storedBytes = newBytes;
    result.output = path_;
    result.details = snapshot.value("snapshot").toObject();
    result.details["snapshotId"] = objectId;
    for (auto warning : snapshot.value("warnings").toArray())
        result.warnings.append(warning.toString());
    return result;
}

QByteArray Repository::loadChunk(const QString &objectId) const {
    // Check both envelope authentication and logical content identity. Valid
    // ciphertext copied under another object's filename must still be rejected.
    // The decompressor enforces the 1 MiB plaintext limit before allocation.
    const auto bytes =
        compression::decode(crypto::open(readFile(objectPath(path_, objectId), ChunkSize * 2 + 4096), key_));
    require(crypto::equal(crypto::hmac(key_, QByteArray("chunk\0", 6) + bytes),
                          QByteArray::fromHex(objectId.toLatin1())),
            "CHUNK_INTEGRITY", "数据块内容与标识不一致");
    return bytes;
}

QJsonObject Repository::manifest(const QString &objectId) const {
    // Parse only authenticated bytes. The filesystem validator then checks the
    // directory graph, stream lengths, chunk intervals and safe relative paths.
    // An authenticated yet malformed object is not implicitly trusted.
    require(key_.size() == 32, "REPOSITORY_CLOSED", "仓库未初始化");
    const auto bytes = crypto::open(readFile(snapshotPath(path_, objectId)), key_);
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && doc.isObject(), "MANIFEST_JSON", "快照清单损坏");
    const auto result = doc.object();
    require(result.value("snapshot").toObject().value("id").toString() == objectId, "SNAPSHOT_ID",
            "快照身份不匹配");
    fs::validateManifest(result);
    return result;
}

QJsonArray Repository::list() const {
    // A directory listing alone does not pin its files. Without this lock,
    // concurrent pruning can delete a manifest between enumeration and read.
    // Return the same explicit busy condition used by the other public tasks.
    QLockFile lock(path_ + "/repository.lock");
    lockRepository(lock);
    return listUnlocked();
}

QJsonArray Repository::listUnlocked() const {
    // Summaries are derived from authenticated manifests instead of a separate
    // mutable plaintext catalog. Listing cannot expose file names without a key.
    // UTC timestamps sort independently of the machine's local timezone.
    QVector<QJsonObject> summaries;
    for (const auto &name : QDir(path_ + "/snapshots").entryList({"*.cvs"}, QDir::Files)) {
        const auto item = manifest(name.chopped(4));
        auto summary = item.value("snapshot").toObject();
        summary["fileCount"] = item.value("fileCount");
        summary["totalBytes"] = item.value("totalBytes");
        summaries.push_back(summary);
    }
    std::sort(summaries.begin(), summaries.end(), [](const auto &a, const auto &b) {
        return a.value("createdUtc").toString() > b.value("createdUtc").toString();
    });
    QJsonArray result;
    for (const auto &item : summaries)
        result.append(item);
    return result;
}

TaskResult Repository::restore(const QString &objectId, const QString &destination,
                               const RestoreOptions &options, const Context &context) const {
    // Hold the same lock as prune throughout restoration; no live data block
    // can disappear between reading its manifest reference and its payload.
    QLockFile lock(path_ + "/repository.lock");
    lockRepository(lock);
    return fs::restore(
        manifest(objectId), [this](const auto &chunk) { return loadChunk(chunk); }, destination, options,
        context);
}

TaskResult Repository::verify(const Context &context) const {
    // Verify every distinct referenced object once, even when many snapshots
    // share it. Unreferenced crash leftovers are neither live data nor failures.
    QLockFile lock(path_ + "/repository.lock");
    lockRepository(lock);
    QSet<QString> chunks;
    auto snapshots = listUnlocked();
    for (auto item : snapshots)
        chunks.unite(referenced(manifest(item.toObject().value("id").toString())));
    TaskResult result;
    for (const auto &chunk : chunks) {
        const auto bytes = loadChunk(chunk);
        result.bytes += static_cast<quint64>(bytes.size());
        context.update(++result.files, static_cast<quint64>(chunks.size()), "校验数据块");
    }
    result.details = {{"snapshots", snapshots.size()}, {"chunks", chunks.size()}};
    return result;
}

TaskResult Repository::prune(int keepCount, int keepDays, const Context &context) {
    // Retention combines count and age limits, but never removes the newest
    // successful snapshot. Keeping zero versions would turn cleanup into data
    // destruction, so the public interface rejects that configuration outright.
    require(keepCount >= 1 && keepDays >= 0, "RETENTION_RULE", "至少保留一个成功版本，天数不能为负数");
    QLockFile lock(path_ + "/repository.lock");
    lockRepository(lock);
    const auto snapshots = listUnlocked();
    const auto cutoff = QDateTime::currentDateTimeUtc().addDays(-keepDays);
    QSet<QString> retainedChunks;
    QStringList obsolete;
    // Discover and validate every retained reference before deleting anything.
    for (qsizetype i = 0; i < snapshots.size(); ++i) {
        const auto item = snapshots[i].toObject();
        const auto objectId = item.value("id").toString();
        bool expire =
            i >= keepCount || (keepDays > 0 && QDateTime::fromString(item.value("createdUtc").toString(),
                                                                     Qt::ISODateWithMs) < cutoff);
        if (i == 0)
            expire = false;
        if (expire)
            obsolete.append(objectId);
        else
            retainedChunks.unite(referenced(manifest(objectId)));
    }
    for (const auto &objectId : retainedChunks) {
        context.check();
        loadChunk(objectId);
    }
    TaskResult result;
    // Delete expired manifests first, then sweep only objects that are absent
    // from every retained manifest. Interruption can leave extra storage behind,
    // but it never removes a block still needed by a visible retained snapshot.
    for (const auto &objectId : obsolete) {
        context.check();
        require(QFile::remove(snapshotPath(path_, objectId)), "IO_REMOVE", "无法淘汰快照");
        ++result.files;
    }
    quint64 removedChunks = 0;
    for (const auto &prefix : QDir(path_ + "/objects").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QDir dir(path_ + "/objects/" + prefix);
        for (const auto &name : dir.entryList({"*.cvo"}, QDir::Files)) {
            context.check();
            if (!retainedChunks.contains(name.chopped(4))) {
                const auto file = dir.filePath(name);
                const auto size = QFileInfo(file).size();
                require(QFile::remove(file), "IO_REMOVE", "无法清理无引用数据块");
                result.bytes += static_cast<quint64>(size);
                ++removedChunks;
            }
        }
    }
    result.details = {{"removedSnapshots", static_cast<qint64>(result.files)},
                      {"removedChunks", static_cast<qint64>(removedChunks)}};
    return result;
}

void Repository::changePassword(const QString &password) {
    // All data objects, manifests and their content IDs remain untouched.
    // DPAPI caches remain valid because the underlying random master is stable.
    require(key_.size() == 32, "REPOSITORY_CLOSED", "仓库未初始化");
    QLockFile lock(path_ + "/repository.lock");
    lockRepository(lock);
    wrapKey(password);
}
} // namespace cv
