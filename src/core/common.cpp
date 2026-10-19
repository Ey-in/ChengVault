#include "chengvault/common.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <limits>

namespace cv {
// The serialized plan is a shared contract for three frontends. Defaults live
// here so a missing optional field has identical meaning in each process.
QJsonObject BackupPlan::toJson(bool includePassword) const {
    QJsonObject object{{"sources", QJsonArray::fromStringList(sources)},
                       {"destination", destination},
                       {"format", format},
                       {"compression", compression},
                       {"cipher", cipher},
                       {"filters", filters},
                       {"strictMetadata", strictMetadata}};
    // Persisting a task must never leak its password into plain JSON.
    if (includePassword)
        object.insert("password", password);
    return object;
}
BackupPlan BackupPlan::fromJson(const QJsonObject &json) {
    // Parsing does not touch paths or create output. Execution validates the
    // selected algorithm, source tree and destination against current state.
    BackupPlan plan;
    for (const auto &item : json.value("sources").toArray())
        plan.sources << item.toString();
    plan.destination = json.value("destination").toString();
    plan.format = json.value("format").toString("indexed");
    plan.compression = json.value("compression").toString("lz77");
    plan.cipher = json.value("cipher").toString("none");
    plan.password = json.value("password").toString();
    plan.filters = json.value("filters").toObject();
    plan.strictMetadata = json.value("strictMetadata").toBool(true);
    return plan;
}
QJsonObject TaskResult::toJson() const {
    // Keep warnings as an array rather than joining text: CLI automation and
    // the report generator can distinguish individual metadata limitations.
    return {{"success", success},     {"files", qint64(files)},
            {"bytes", qint64(bytes)}, {"storedBytes", qint64(storedBytes)},
            {"output", output},       {"warnings", QJsonArray::fromStringList(warnings)},
            {"details", details}};
}
void require(bool condition, const QString &code, const QString &message) {
    // Domain codes survive background-worker and network error translation;
    // localized prose is intended for display rather than program decisions.
    if (!condition)
        throw Error(code, message);
}
QByteArray readFile(const QString &path, qint64 maxSize) {
    require(maxSize >= 0 && maxSize < std::numeric_limits<qint64>::max(), "SIZE_LIMIT",
            "读取上限超出有效范围");
    // Check the size before the allocation and verify it after the read. This
    // detects disappearance/truncation rather than accepting a shorter object.
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "READ_FAILED", path + ": " + file.errorString());
    const qint64 expected = file.size();
    require(expected >= 0 && expected <= maxSize, "SIZE_LIMIT", "文件超过读取上限: " + path);
    // One look-ahead byte detects concurrent growth without allowing readAll
    // to consume unbounded data beyond the size checked above.
    const auto data = file.read(expected + 1);
    require(data.size() == expected && file.size() == expected, "SHORT_READ", "文件读取不完整: " + path);
    return data;
}
void atomicWrite(const QString &path, const QByteArray &data) {
    // QSaveFile keeps temporary content in the destination filesystem so its
    // final rename can replace the target without a partial-copy interval.
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "MKDIR_FAILED", "无法建立输出目录");
    QSaveFile file(path);
    // Disallow QSaveFile's optional direct-write fallback: existing data must
    // survive disk-full and access-denied failures.
    file.setDirectWriteFallback(false);
    require(file.open(QIODevice::WriteOnly), "WRITE_FAILED", path + ": " + file.errorString());
    require(file.write(data) == data.size(), "SHORT_WRITE", file.errorString());
    require(file.commit(), "COMMIT_FAILED", file.errorString());
}
QJsonObject readJson(const QString &path) {
    // Accept objects only: arrays and scalar JSON are valid syntax but cannot
    // satisfy any persisted plan, account or snapshot schema in this project.
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(readFile(path), &error);
    require(error.error == QJsonParseError::NoError && doc.isObject(), "BAD_JSON",
            path + ": " + error.errorString());
    return doc.object();
}
void writeJson(const QString &path, const QJsonObject &json) {
    // Human-readable JSON remains useful for task inspection. Atomic commit
    // protects readers from observing half of an administrative update.
    atomicWrite(path, QJsonDocument(json).toJson(QJsonDocument::Indented));
}
QString uniqueId() {
    // UUIDs identify records and temporary names. They are not authentication
    // secrets; cryptographic keys always come from crypto::randomBytes.
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
QString appDataDirectory() {
    // Integration tests and portable launches can isolate their job state
    // without touching another user's installed configuration.
    const auto overridePath = qEnvironmentVariable("CHENGVAULT_DATA_HOME");
    const auto path = overridePath.isEmpty()
                          ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                          : overridePath;
    require(QDir().mkpath(path), "MKDIR_FAILED", path);
    return path;
}
} // namespace cv
