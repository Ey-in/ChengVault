#include "chengvault/scheduler.hpp"
#include "chengvault/crypto.hpp"
#include "chengvault/repository.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTimeZone>
#include <QtConcurrent>
#include <thread>
#include <vector>
#include <windows.h>

namespace cv::schedule {
namespace {
QString jobPath(const QString &id) {
    // An ID is a filename component supplied by persisted configuration or UI.
    // Reject separators and dot traversal before joining the per-user job root.
    // The same validation protects read, update and removal operations.
    require(!id.isEmpty() && id.size() <= 128 && !id.contains('/') && !id.contains('\\') &&
                !id.contains(':') && !id.contains(".."),
            "BAD_JOB_ID", "无效任务编号");
    return jobsDirectory() + '/' + id + ".json";
}
void appendLog(const QJsonObject &line) {
    // Each event is one compact JSON line so a reader can recover earlier
    // complete records after interruption. This diagnostic append is best
    // effort and must not replace the task's committed state transition.
    try {
        QFile file(appDataDirectory() + "/agent-events.jsonl");
        if (file.open(QIODevice::WriteOnly | QIODevice::Append))
            file.write(QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n');
    } catch (const std::exception &) {
        // An inaccessible profile may prevent even constructing the log path.
        // Diagnostic failure must never escape a timer or destructor callback.
    }
}
} // namespace
QDateTime nextRun(const QJsonObject &job, const QDateTime &after) {
    // The supplied timestamp makes boundary cases reproducible in tests.
    // Calendar jobs retain its timezone and use date arithmetic, while interval
    // jobs add elapsed seconds. A due time must be strictly after this input.
    // Invalid modes/times fail while saving instead of scheduling silently.
    const QString mode = job.value("mode").toString("interval");
    require(after.isValid(), "BAD_SCHEDULE", "计算计划需要有效的参考时间");
    if (mode == "interval")
        return after.addSecs(qMax(1, job.value("intervalSeconds").toInt(3600)));
    if (mode == "realtime")
        return after.addSecs(1);
    const QTime clock = QTime::fromString(job.value("time").toString("02:00"), "HH:mm");
    require(clock.isValid(), "BAD_SCHEDULE", "计划时间格式必须为 HH:mm");
    QDateTime candidate(after.date(), clock, after.timeZone());
    if (candidate <= after)
        candidate = candidate.addDays(1);
    if (mode == "weekly") {
        const int day = job.value("weekday").toInt(1);
        require(day >= 1 && day <= 7, "BAD_SCHEDULE", "星期必须为1至7");
        while (candidate.date().dayOfWeek() != day)
            candidate = candidate.addDays(1);
    } else
        require(mode == "daily", "BAD_SCHEDULE", "未知调度模式");
    return candidate;
}
QString jobsDirectory() {
    // All frontends share the same per-user app-data directory. A portable
    // profile can override it for isolated tests without editing real tasks.
    const QString path = appDataDirectory() + "/jobs";
    require(QDir().mkpath(path), "MKDIR_FAILED", path);
    return path;
}
QJsonArray listJobs() {
    // A corrupt job remains visible as invalid. Do not abort enumeration and
    // hide the remaining valid jobs, and never execute the malformed record.
    QJsonArray result;
    for (const QString &file : QDir(jobsDirectory()).entryList({"*.json"}, QDir::Files)) {
        try {
            const auto job = readJson(jobsDirectory() + '/' + file);
            // A persisted ID must identify this exact file. Otherwise a damaged
            // record could launch one plan but overwrite another task on finish.
            // Invalid records are visible in the UI but never admitted as work.
            require(jobPath(job["id"].toString()) == jobsDirectory() + '/' + file, "BAD_JOB_ID",
                    "任务编号与配置文件名不一致");
            result.append(job);
        } catch (const std::exception &e) {
            // Use the actual filename stem so the UI can remove this invalid
            // record. Returning "name.json" as an ID would append .json twice.
            result.append(QJsonObject{{"id", QFileInfo(file).completeBaseName()},
                                      {"state", "invalid"},
                                      {"error", QString::fromUtf8(e.what())}});
        }
    }
    return result;
}
QJsonObject saveJob(QJsonObject job, const BackupPlan &plan, const QByteArray &masterKey) {
    // Store a key protected for the current Windows account, not a password or
    // clear master key. A copied job cannot be decrypted by an unrelated user.
    // DPAPI enables unattended access only; archive encryption remains local
    // AES/ChaCha and is not delegated to this credential-protection operation.
    require(!plan.sources.isEmpty() && !plan.destination.isEmpty(), "BAD_JOB", "请先配置备份来源与仓库");
    if (job.value("id").toString().isEmpty())
        job["id"] = uniqueId();
    job["plan"] = plan.toJson(false);
    job["key"] = QString::fromLatin1(crypto::protectLocalKey(masterKey).toBase64());
    job["enabled"] = true;
    job["state"] = "waiting";
    // A newly created task runs once immediately; subsequent runs follow the
    // persisted schedule. A crash leaves an overdue timestamp and is retried.
    job["nextRun"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    nextRun(job, QDateTime::currentDateTime());
    writeJson(jobPath(job["id"].toString()), job);
    return job;
}
void removeJob(const QString &id) {
    require(QFile::remove(jobPath(id)), "REMOVE_FAILED", "任务删除失败");
}
void setEnabled(const QString &id, bool enabled) {
    // Pausing changes future scheduling. It does not kill an in-flight worker
    // or remove successful snapshots; its current commit may still complete.
    auto job = readJson(jobPath(id));
    job["enabled"] = enabled;
    writeJson(jobPath(id), job);
}
void setAutoStart(bool enabled, const QString &executable) {
    // Use HKCU instead of a machine-wide service or administrator registry key.
    // Quote the executable because a deployment path may contain spaces.
    // Registry sync/status makes a permission failure visible to the caller.
    QSettings registry("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                       QSettings::NativeFormat);
    if (enabled) {
        require(QFileInfo::exists(executable), "AGENT_MISSING", "后台程序不存在");
        registry.setValue("ChengVaultAgent", '"' + QDir::toNativeSeparators(executable) + "\" --background");
    } else
        registry.remove("ChengVaultAgent");
    registry.sync();
    require(registry.status() == QSettings::NoError, "AUTOSTART_FAILED", "无法更新当前用户自启动设置");
}
bool autoStartEnabled() {
    QSettings registry("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                       QSettings::NativeFormat);
    return registry.contains("ChengVaultAgent");
}

struct DirectoryWatch::Impl {
    // Watcher threads own their native I/O handles. They only set dirty state;
    // repository mutation stays on the engine's single backup worker.
    // A shared manual-reset event wakes every blocked watcher during shutdown.
    std::atomic_bool stop{false};
    std::atomic_bool changed{false};
    std::vector<std::thread> threads;
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Impl() {
        // Signal before joining: a directory with no activity could otherwise
        // leave ReadDirectoryChangesW blocked for the lifetime of the process.
        // All threads finish before the shared event handle is closed.
        stop = true;
        SetEvent(stopEvent);
        for (auto &thread : threads)
            if (thread.joinable())
                thread.join();
        CloseHandle(stopEvent);
    }
};
DirectoryWatch::DirectoryWatch(const QStringList &paths) : impl_(std::make_unique<Impl>()) {
    // Windows watches directories. A selected file watches its parent and lets
    // the next full filtered scan decide whether that file actually changed.
    // Events are hints, not an authoritative manifest or an incremental journal.
    for (auto path : paths) {
        if (!QFileInfo(path).isDir())
            path = QFileInfo(path).absolutePath();
        const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
        Impl *state = impl_.get();
        state->threads.emplace_back([state, native] {
            HANDLE directory = CreateFileW(
                native.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (directory == INVALID_HANDLE_VALUE) {
                // Mark dirty so inaccessible/deleted sources cause a visible
                // backup failure. Silently ignoring the source would produce
                // a false successful real-time task with no change detection.
                state->changed = true;
                return;
            }
            HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            QByteArray buffer(60 * 1024, '\0');
            // Keep the notification buffer below the documented network limit.
            // Rather than replay individual rename events, coalesce all events
            // into a rescan; this also handles rename pairs and queue overflow.
            while (!state->stop) {
                OVERLAPPED operation{};
                operation.hEvent = event;
                ResetEvent(event);
                const DWORD filters = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                      FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                      FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SECURITY;
                if (!ReadDirectoryChangesW(directory, buffer.data(), DWORD(buffer.size()), TRUE, filters,
                                           nullptr, &operation, nullptr)) {
                    state->changed = true;
                    break;
                }
                HANDLE waits[] = {state->stopEvent, event};
                const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wait == WAIT_OBJECT_0 || state->stop) {
                    // The OVERLAPPED storage and buffer must remain alive until
                    // cancellation completes. Waiting prevents native I/O from
                    // writing into memory released when this thread returns.
                    CancelIoEx(directory, &operation);
                    WaitForSingleObject(event, INFINITE);
                    break;
                }
                DWORD bytes = 0;
                GetOverlappedResult(directory, &operation, &bytes, FALSE);
                // Zero bytes means overflow, not "nothing changed".
                state->changed = true;
            }
            CloseHandle(event);
            CloseHandle(directory);
        });
    }
}
DirectoryWatch::~DirectoryWatch() = default;
bool DirectoryWatch::takeChanged() {
    // Atomic exchange consumes only the observed batch; an event arriving
    // afterwards sets the flag for the next tick instead of being lost.
    return impl_->changed.exchange(false);
}

Engine::Engine(QObject *parent) : QObject(parent) {
    // Engine slots run on their QObject thread. The timer only evaluates due
    // work; expensive scanning/compression runs in QtConcurrent. runningId_
    // provides process-local task mutual exclusion, one backup at a time.
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    connect(&running_, &QFutureWatcher<TaskResult>::finished, this, [this] {
        // A user may remove a task while its current backup is finishing. Do
        // not recreate the deleted configuration or crash the background loop.
        QJsonObject job;
        try {
            const auto path = jobPath(runningId_);
            if (!QFileInfo::exists(path)) {
                runningId_.clear();
                return;
            }
            job = readJson(path);
        } catch (const std::exception &e) {
            appendLog({{"id", runningId_}, {"error", e.what()}});
            runningId_.clear();
            return;
        }
        QJsonObject event{{"id", runningId_},
                          {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
        try {
            const auto result = running_.result();
            // Reset retries only after backup and retention both succeed.
            // A successful file count alone is insufficient to mark the job
            // complete because garbage collection may still have failed.
            require(result.success, result.details["code"].toString("JOB_FAILED"),
                    result.details["error"].toString("后台任务失败"));
            job["state"] = "waiting";
            job["lastResult"] = result.toJson();
            job["failures"] = 0;
            job.remove("error");
            job["nextRun"] = nextRun(job, QDateTime::currentDateTime()).toString(Qt::ISODateWithMs);
            event["result"] = result.toJson();
        } catch (const std::exception &e) {
            // Reopen the directory notification handle after a failed run.
            // A deleted/renamed source can invalidate the previous handle.
            // Retry delay grows linearly and caps at five minutes. Persisting
            // the next time prevents a restart from creating a busy retry loop.
            watches_.remove(runningId_);
            // Treat persisted counters as untrusted input. Saturation prevents
            // signed overflow and maintains the five-minute maximum retry delay.
            const int failures = qBound(0, job.value("failures").toInt(), 59) + 1;
            job["failures"] = failures;
            job["state"] = "retrying";
            job["error"] = QString::fromUtf8(e.what());
            job["nextRun"] =
                QDateTime::currentDateTime().addSecs(qMin(300, 5 * failures)).toString(Qt::ISODateWithMs);
            event["error"] = QString::fromUtf8(e.what());
        }
        job["lastRun"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        try {
            writeJson(jobPath(runningId_), job);
        } catch (const std::exception &e) {
            event["error"] = QString::fromUtf8(e.what());
        }
        appendLog(event);
        runningId_.clear();
    });
}
Engine::~Engine() {
    stop();
}
void Engine::start() {
    // Evaluate overdue tasks immediately after startup. A periodic timer alone
    // would unnecessarily delay crash recovery until the first timer interval.
    timer_.start();
    tick();
}
void Engine::stop() {
    // Stop admission before cancellation and join. Watchers are released only
    // after the worker can no longer depend on engine-owned cancellation state.
    timer_.stop();
    cancelled_ = true;
    // QObject destruction must not let a worker exception cross a noexcept
    // destructor. Normal job failures are returned as data by launch().
    try {
        running_.waitForFinished();
    } catch (const std::exception &e) {
        appendLog({{"id", runningId_}, {"error", QString::fromUtf8(e.what())}});
    }
    watches_.clear();
}
void Engine::tick(const QDateTime &now) {
    // A running task blocks new launches, including repeated timer events.
    // Configuration is reread each tick so GUI edits need no in-memory IPC.
    // Files are atomically replaced by writers, avoiding partial JSON reads.
    if (!runningId_.isEmpty())
        return;
    QJsonArray jobs;
    try {
        jobs = listJobs();
    } catch (const std::exception &e) {
        // The profile may become unavailable after startup (permissions, moved
        // drive, or a full filesystem). Retry enumeration on the next tick.
        appendLog({{"error", QString::fromUtf8(e.what())}, {"time", now.toString(Qt::ISODateWithMs)}});
        return;
    }
    QSet<QString> activeWatches;
    for (const auto &value : jobs) {
        const auto job = value.toObject();
        if (job["enabled"].toBool() && job["mode"] == "realtime")
            activeWatches.insert(job["id"].toString());
    }
    for (auto it = watches_.begin(); it != watches_.end();) {
        // Paused/removed jobs release native handles and stale debounce times.
        // A later re-enable constructs a new watcher for the current sources.
        if (!activeWatches.contains(it.key())) {
            dirty_.remove(it.key());
            it = watches_.erase(it);
        } else
            ++it;
    }
    for (const auto &value : jobs) {
        auto job = value.toObject();
        if (!job.value("enabled").toBool())
            continue;
        try {
            // A malformed enabled job must not throw out of a Qt timer callback or
            // starve the remaining jobs. Validate schedule fields before admission.
            nextRun(job, now);
            const QString id = job.value("id").toString();
            const bool realtime = job.value("mode").toString() == "realtime";
            if (realtime) {
                if (!watches_.contains(id)) {
                    const auto plan = BackupPlan::fromJson(job.value("plan").toObject());
                    watches_[id] = std::make_shared<DirectoryWatch>(plan.sources);
                }
                if (watches_[id]->takeChanged())
                    dirty_[id] = now;
                // Require two quiet seconds after the most recent event batch.
                // First run and retries bypass this debounce so an idle tree still
                // gets its initial snapshot and errors can recover without edits.
                const bool first = job.value("lastRun").toString().isEmpty();
                const bool retry = job.value("state").toString() == "retrying";
                if (!first && !retry && (!dirty_.contains(id) || dirty_[id].secsTo(now) < 2))
                    continue;
            }
            const auto due = QDateTime::fromString(job.value("nextRun").toString(), Qt::ISODateWithMs);
            if (!due.isValid() || due <= now) {
                // Invalid or overdue timestamps receive one catch-up run, not one
                // run per missed interval. The completion computes the next time.
                dirty_.remove(id);
                launch(job, now);
                return;
            }
        } catch (const std::exception &e) {
            appendLog({{"id", job.value("id")},
                       {"error", QString::fromUtf8(e.what())},
                       {"time", now.toString(Qt::ISODateWithMs)}});
        }
    }
}
void Engine::launch(QJsonObject job, const QDateTime &now) {
    // Persist running state before submitting the worker. If the process dies,
    // the old due timestamp remains and startup can perform a catch-up run.
    // Capture job by value so later GUI edits cannot mutate this invocation.
    const QString id = job["id"].toString();
    job["state"] = "running";
    job["started"] = now.toString(Qt::ISODateWithMs);
    writeJson(jobPath(id), job);
    // Do not claim the worker slot until persistence succeeds. A write failure
    // remains retryable and cannot permanently block every other queued task.
    runningId_ = id;
    cancelled_ = false;
    running_.setFuture(QtConcurrent::run([job, this] {
        try {
            const auto plan = BackupPlan::fromJson(job["plan"].toObject());
            const auto master =
                crypto::unprotectLocalKey(QByteArray::fromBase64(job["key"].toString().toLatin1()));
            auto repository = Repository::withMasterKey(plan.destination, master);
            Context context;
            context.cancelled = &cancelled_;
            auto result = repository.backup(plan, context);
            // Retention follows a successful commit and clamps count to one.
            // The repository traces live manifests before deleting blocks;
            // a failed backup therefore never triggers destructive pruning.
            repository.prune(qMax(1, job.value("keepCount").toInt(10)),
                             qMax(0, job.value("keepDays").toInt(0)), context);
            return result;
        } catch (const Error &e) {
            // Qt may wrap exceptions crossing a future boundary. Preserve
            // domain error codes explicitly as data for the finished handler
            // and keep worker failures from escaping Engine's destructor.
            TaskResult result;
            result.success = false;
            result.details = {{"code", e.code}, {"error", QString::fromUtf8(e.what())}};
            return result;
        } catch (const std::exception &e) {
            TaskResult result;
            result.success = false;
            result.details = {{"code", "JOB_FAILED"}, {"error", QString::fromUtf8(e.what())}};
            return result;
        }
    }));
}
} // namespace cv::schedule
