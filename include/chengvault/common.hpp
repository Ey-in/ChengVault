#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <atomic>
#include <functional>
#include <stdexcept>
#include <utility>

namespace cv {
// All public operations use structured errors. Never report a partially written
// snapshot as successful; callers may safely expose the code in the UI/log.
class Error : public std::runtime_error {
  public:
    QString code;
    explicit Error(QString c, const QString &message)
        : std::runtime_error(message.toUtf8().constData()), code(std::move(c)) {}
};

// A Context does not own its cancellation flag or callback. The launching
// thread must keep both alive until its worker has completed. Callbacks run
// on that worker; GUI consumers marshal notifications to the UI thread.
struct Context {
    std::atomic_bool *cancelled = nullptr;
    std::function<void(quint64, quint64, const QString &)> progress;
    void check() const {
        if (cancelled && cancelled->load())
            throw Error("CANCELLED", "操作已取消");
    }
    void update(quint64 done, quint64 total, const QString &text) const {
        check();
        if (progress)
            progress(done, total, text);
    }
};

// Rules are persisted as JSON so CLI, GUI, and the background process share
// exactly the same matching semantics. See docs/configuration.md for keys.
struct BackupPlan {
    QStringList sources;
    QString destination;
    QString format = "indexed";
    QString compression = "lz77";
    QString cipher = "none";
    QString password;
    QJsonObject filters;
    bool strictMetadata = true;
    // The default serialization is safe to persist as an ordinary task file.
    // Including the password is an explicit opt-in for a protected transport.
    QJsonObject toJson(bool includePassword = false) const;
    static BackupPlan fromJson(const QJsonObject &json);
};

// Restore is conservative by default. Path selection is relative to the
// manifest; filesystem validation still rejects traversal and link escapes.
struct RestoreOptions {
    bool overwrite = false;
    bool strictMetadata = true;
    QStringList selectedPaths;
};

// Counts describe committed work, while warnings retain nonfatal metadata
// differences. A caller must inspect success rather than infer it from bytes.
struct TaskResult {
    bool success = true;
    quint64 files = 0;
    quint64 bytes = 0;
    quint64 storedBytes = 0;
    QString output;
    QStringList warnings;
    QJsonObject details;
    QJsonObject toJson() const;
};

constexpr qsizetype ChunkSize = 1024 * 1024;
// Maximum serialized manifest size accepted by the archive and repository
// formats; readers reject larger metadata before allocating or parsing it.
constexpr qint64 ManifestLimit = 64LL * 1024 * 1024;
// Filesystem capture and restore exchange bounded chunks through these two
// callbacks, keeping archive and repository storage independent of NTFS I/O.
using ChunkSink = std::function<QString(const QByteArray &)>;
using ChunkSource = std::function<QByteArray(const QString &)>;

// This helper is for bounded metadata/blocks. Large content uses streaming
// I/O; a size change while reading is a failure, not a partial successful read.
QByteArray readFile(const QString &path, qint64 maxSize = 64 * 1024 * 1024);
// Commit uses a sibling temporary file. Parent directory creation may remain
// after failure, but a preexisting destination is never partially rewritten.
void atomicWrite(const QString &path, const QByteArray &data);
QJsonObject readJson(const QString &path);
void writeJson(const QString &path, const QJsonObject &json);
QString uniqueId();
// CHENGVAULT_DATA_HOME permits isolated portable/test profiles; otherwise
// use the current Windows user's application-local configuration directory.
QString appDataDirectory();
void require(bool condition, const QString &code, const QString &message);
} // namespace cv
