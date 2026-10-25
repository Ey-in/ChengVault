#pragma once
#include "common.hpp"

namespace cv {
// Serializes a manifest and rejects sizes above the shared 64 MiB format limit.
QByteArray serializeSnapshot(const QJsonObject &snapshot);
// A repository owns a stable random key. Changing a password only rewrites the
// key envelope; existing encrypted chunks and snapshots remain byte-identical.
class Repository {
  public:
    // Existing repositories are authenticated immediately. A nonexistent path
    // creates an unopened value which init() or the first backup() initializes.
    explicit Repository(QString path, QString password = {});
    // Used after DPAPI unwrapping by the background process; validates that the
    // cached key belongs to the selected repository before returning an object.
    static Repository withMasterKey(const QString &path, const QByteArray &key);
    // Initialization accepts only an empty directory and an encrypted cipher.
    // "none" belongs to standalone archives, not the encrypted chunk store.
    void init(const QString &cipher = "chacha20");
    // A successful result includes details.snapshotId, newChunks, reusedChunks.
    // The manifest is committed only after every referenced block is durable.
    TaskResult backup(const BackupPlan &plan, const Context &context = {});
    // Authenticated summaries are returned newest first. Listing requires a key
    // because even file counts and snapshot metadata come from encrypted files.
    QJsonArray list() const;
    // Returns a validated complete manifest, never an incremental difference.
    // It is therefore sufficient to restore this version without older versions.
    QJsonObject manifest(const QString &snapshotId) const;
    // RestoreOptions controls user-file conflicts; data blocks are authenticated
    // and decompressed before the shared filesystem layer receives any bytes.
    TaskResult restore(const QString &snapshotId, const QString &destination,
                       const RestoreOptions &options = {}, const Context &context = {}) const;
    // Count and age limits both apply, with a minimum of one successful version.
    // Shared references, including named data streams, are retained together.
    TaskResult prune(int keepCount, int keepDays = 0, const Context &context = {});
    // Reads all live manifests and unique blocks rather than trusting metadata.
    // A corrupted object makes the operation fail with a structured error code.
    TaskResult verify(const Context &context = {}) const;
    // Replaces only the password wrapping; snapshot IDs and ciphertext do not
    // change, preserving network deduplication and background cached keys.
    void changePassword(const QString &password);
    // This accessor is for DPAPI wrapping, never for display or log output.
    // Network uploads do not use or transmit this key.
    // Qt's implicit sharing makes these owning values cheap; returning a value
    // also avoids dangling references when callers open a temporary repository.
    QByteArray masterKey() const {
        return key_;
    }
    QString path() const {
        return path_;
    }
    QString id() const {
        return config_.value("id").toString();
    }

  private:
    Repository(QString path, QByteArray key, bool);
    QString path_;
    QString password_;
    QByteArray key_;
    QJsonObject config_;
    // Call only while holding repository.lock. Public readers must synchronize
    // with pruning; verify/prune already own the lock and cannot reacquire it.
    QJsonArray listUnlocked() const;
    QByteArray loadChunk(const QString &id) const;
    void wrapKey(const QString &password);
};
} // namespace cv
