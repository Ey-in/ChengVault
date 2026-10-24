#pragma once
#include "common.hpp"
#include <QFutureWatcher>
#include <QObject>
#include <QTimer>
#include <memory>

namespace cv::schedule {
// Time calculation is independent of wall-clock access so missed-run and
// daylight-saving cases can be tested with an injected timestamp.
QDateTime nextRun(const QJsonObject &job, const QDateTime &after);
QString jobsDirectory();
QJsonArray listJobs();
// saveJob commits only after validation and protects masterKey with the
// current user's DPAPI identity. It deliberately excludes plan.password.
// removeJob deletes configuration, not snapshots or an active worker's data.
QJsonObject saveJob(QJsonObject job, const BackupPlan &plan, const QByteArray &masterKey);
void removeJob(const QString &id);
void setEnabled(const QString &id, bool enabled);
void setAutoStart(bool enabled, const QString &agentExecutable);
bool autoStartEnabled();

// The event source reports dirtiness, never performs backup on the notification
// thread. Backup reads a stable snapshot independently of event coalescing.
class DirectoryWatch {
  public:
    explicit DirectoryWatch(const QStringList &paths);
    ~DirectoryWatch();
    // Returns and clears accumulated dirtiness. A rename pair, overflow or
    // notification failure all request a conservative complete rescan.
    bool takeChanged();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One Engine schedules one foreground backup worker at a time. The agent
// additionally uses a process lock so multiple launchers cannot duplicate it.
// Construct/use on one QObject thread; stop joins work before member teardown.
// Tests inject tick timestamps instead of changing the system clock.
class Engine : public QObject {
  public:
    explicit Engine(QObject *parent = nullptr);
    ~Engine() override;
    void start();
    void stop();
    void tick(const QDateTime &now = QDateTime::currentDateTime());

  private:
    QTimer timer_;
    QFutureWatcher<TaskResult> running_;
    QString runningId_;
    std::atomic_bool cancelled_{false};
    QHash<QString, std::shared_ptr<DirectoryWatch>> watches_;
    QHash<QString, QDateTime> dirty_;
    void launch(QJsonObject job, const QDateTime &now);
};
} // namespace cv::schedule
