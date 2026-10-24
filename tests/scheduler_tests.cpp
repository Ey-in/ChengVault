#include "chengvault/crypto.hpp"
#include "chengvault/repository.hpp"
#include "chengvault/scheduler.hpp"
#include "test_support.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QThread>

void schedulerTests(cv::test::Suite &suite) {
    using namespace cv::test;
    const auto reference = QDateTime::fromString("2026-10-17T12:30:00+08:00", Qt::ISODate);
    suite.run("SCH-01 interval advances injected clock", "白盒单元", [&] {
        check(cv::schedule::nextRun({{"mode", "interval"}, {"intervalSeconds", 60}}, reference) ==
              reference.addSecs(60));
    });
    suite.run("SCH-02 zero interval clamps to one second", "边界健壮性", [&] {
        check(cv::schedule::nextRun({{"mode", "interval"}, {"intervalSeconds", 0}}, reference) ==
              reference.addSecs(1));
    });
    suite.run("SCH-03 negative interval cannot loop immediately", "边界健壮性",
              [&] { check(cv::schedule::nextRun({{"intervalSeconds", -60}}, reference) > reference); });
    suite.run("SCH-04 daily future time stays same date", "功能", [&] {
        const auto next = cv::schedule::nextRun({{"mode", "daily"}, {"time", "13:00"}}, reference);
        check(next.date() == reference.date() && next.time() == QTime(13, 0));
    });
    suite.run("SCH-05 daily elapsed time moves next date", "白盒单元", [&] {
        check(cv::schedule::nextRun({{"mode", "daily"}, {"time", "09:00"}}, reference).date() ==
              reference.date().addDays(1));
    });
    suite.run("SCH-06 exact boundary schedules next day", "边界健壮性", [&] {
        check(cv::schedule::nextRun({{"mode", "daily"}, {"time", "12:30"}}, reference).date() ==
              reference.date().addDays(1));
    });
    suite.run("SCH-07 weekly weekday is correct", "功能", [&] {
        const auto next =
            cv::schedule::nextRun({{"mode", "weekly"}, {"time", "09:00"}, {"weekday", 1}}, reference);
        check(next.date().dayOfWeek() == 1 && next > reference);
    });
    suite.run("SCH-08 invalid weekday rejected", "安全", [&] {
        throws([&] { cv::schedule::nextRun({{"mode", "weekly"}, {"weekday", 8}}, reference); });
    });
    suite.run("SCH-09 invalid clock rejected", "安全", [&] {
        throws([&] { cv::schedule::nextRun({{"mode", "daily"}, {"time", "25:00"}}, reference); });
    });
    suite.run("SCH-10 unknown schedule mode rejected", "安全",
              [&] { throws([&] { cv::schedule::nextRun({{"mode", "invalid"}}, reference); }); });
    suite.run("SCH-11 task serialization omits password", "安全", [] {
        cv::BackupPlan plan;
        plan.password = "must-not-be-persisted";
        check(!plan.toJson().contains("password"));
    });
    suite.run("SCH-12 plan roundtrip retains filters", "集成", [] {
        cv::BackupPlan plan;
        plan.sources = {"C:/one", "D:/two"};
        plan.filters = {{"maxDepth", 3}, {"contentContains", "text"}};
        const auto decoded = cv::BackupPlan::fromJson(plan.toJson());
        check(decoded.sources == plan.sources && decoded.filters == plan.filters);
    });
    suite.run("SCH-13 cancellation checkpoint aborts work", "并发稳定性", [] {
        std::atomic_bool cancelled{true};
        cv::Context context;
        context.cancelled = &cancelled;
        throws([&] { context.check(); });
    });
    suite.run("SCH-14 real Windows write event is detected", "集成", [] {
        QTemporaryDir directory;
        check(directory.isValid());
        cv::schedule::DirectoryWatch watch({directory.path()});
        QThread::msleep(150);
        cv::atomicWrite(directory.filePath("change.txt"), "changed");
        bool seen = false;
        for (int attempt = 0; attempt < 40 && !seen; ++attempt) {
            QThread::msleep(50);
            seen = watch.takeChanged();
        }
        check(seen, "ReadDirectoryChangesW did not observe a real write");
    });
    suite.run("SCH-15 monitor shutdown cancels pending I/O", "并发稳定性", [] {
        QTemporaryDir directory;
        QElapsedTimer timer;
        timer.start();
        {
            cv::schedule::DirectoryWatch watch({directory.path()});
            QThread::msleep(50);
        }
        check(timer.elapsed() < 3000, "watch cancellation blocked");
    });
    suite.run("SCH-16 atomic JSON I/O and malformed JSON failure", "集成", [] {
        QTemporaryDir directory;
        cv::writeJson(directory.filePath("valid.json"),
                      {{"version", 1}, {"name", QString::fromUtf8("中文任务")}});
        check(cv::readJson(directory.filePath("valid.json"))["version"].toInt() == 1);
        cv::atomicWrite(directory.filePath("bad.json"), "not json");
        throws([&] { cv::readJson(directory.filePath("bad.json")); });
    });

    // Integration fixtures live in an isolated task directory; no user's real
    // jobs or startup registry are changed by these tests.
    QTemporaryDir state, files;
    const auto previous = qgetenv("CHENGVAULT_DATA_HOME");
    qputenv("CHENGVAULT_DATA_HOME", state.path().toUtf8());
    cv::BackupPlan plan;
    plan.sources = {files.filePath("source")};
    plan.destination = files.filePath("repository");
    plan.password = "scheduler-test";
    plan.cipher = "chacha20";
    QDir().mkpath(plan.sources.first());
    cv::atomicWrite(plan.sources.first() + "/file.txt", "version-one");
    cv::Repository repository(plan.destination, plan.password);
    repository.init();
    QJsonObject job;
    suite.run("SCH-17 task key protected by current-user DPAPI", "安全", [&] {
        job = cv::schedule::saveJob({{"name", "test task"},
                                     {"mode", "interval"},
                                     {"intervalSeconds", 3600},
                                     {"keepCount", 2},
                                     {"keepDays", 0}},
                                    plan, repository.masterKey());
        const auto stored =
            cv::readFile(cv::schedule::jobsDirectory() + "/" + job["id"].toString() + ".json");
        check(!stored.contains(plan.password.toUtf8()));
        check(!stored.contains(repository.masterKey().toBase64()));
        check(cv::crypto::unprotectLocalKey(QByteArray::fromBase64(job["key"].toString().toLatin1())) ==
              repository.masterKey());
    });
    auto waitFor = [&](const std::function<bool()> &condition) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < 12000) {
            QCoreApplication::processEvents();
            QThread::msleep(20);
        }
        check(condition(), "background engine did not reach expected state");
    };
    auto current = [&] {
        return cv::readJson(cv::schedule::jobsDirectory() + "/" + job["id"].toString() + ".json");
    };
    suite.run("SCH-18 persisted overdue job runs without GUI", "集成", [&] {
        cv::schedule::Engine engine;
        engine.start();
        waitFor([&] { return !current()["lastRun"].toString().isEmpty(); });
        engine.stop();
        check(repository.list().size() == 1);
        check(current()["state"] == "waiting");
    });
    suite.run("SCH-19 missed execution catches up and retention keeps two", "系统功能", [&] {
        for (int i = 0; i < 3; ++i) {
            cv::atomicWrite(plan.sources.first() + "/file.txt", QByteArray::number(i));
            auto next = current();
            next["nextRun"] = "2020-01-01T00:00:00.000Z";
            next.remove("lastRun");
            cv::writeJson(cv::schedule::jobsDirectory() + "/" + job["id"].toString() + ".json", next);
            cv::schedule::Engine engine;
            engine.start();
            waitFor([&] { return !current()["lastRun"].toString().isEmpty(); });
            engine.stop();
        }
        check(repository.list().size() == 2);
        check(repository.verify().success);
    });
    suite.run("SCH-20 disabled job produces no snapshot", "系统功能", [&] {
        cv::schedule::setEnabled(job["id"].toString(), false);
        const auto count = repository.list().size();
        cv::schedule::Engine engine;
        engine.start();
        QCoreApplication::processEvents();
        engine.stop();
        check(repository.list().size() == count);
    });
    suite.run("SCH-21 realtime write creates a new complete version", "集成", [&] {
        auto realtime = current();
        realtime["enabled"] = true;
        realtime["mode"] = "realtime";
        realtime["nextRun"] = "2020-01-01T00:00:00.000Z";
        realtime["keepCount"] = 5;
        cv::writeJson(cv::schedule::jobsDirectory() + "/" + job["id"].toString() + ".json", realtime);
        const auto count = repository.list().size();
        const auto priorRun = current()["lastRun"].toString();
        cv::schedule::Engine engine;
        engine.start();
        QThread::msleep(150);
        cv::atomicWrite(plan.sources.first() + "/file.txt", "realtime-new-data");
        // Completion is published only after backup and retention release the
        // repository lock. Polling the directory mid-task is not completion.
        waitFor([&] {
            const auto state = current();
            return state["state"] == "waiting" && state["lastRun"].toString() != priorRun;
        });
        engine.stop();
        QCoreApplication::processEvents();
        check(repository.list().size() > count);
        check(repository.verify().success);
    });
    suite.run("SCH-22 missing source schedules bounded retry", "异常健壮性", [&] {
        auto failed = current();
        failed["mode"] = "interval";
        failed["nextRun"] = "2020-01-01T00:00:00.000Z";
        auto badPlan = plan;
        badPlan.sources = {files.filePath("missing-source")};
        failed["plan"] = badPlan.toJson();
        cv::writeJson(cv::schedule::jobsDirectory() + "/" + job["id"].toString() + ".json", failed);
        cv::schedule::Engine engine;
        engine.start();
        waitFor([&] { return current()["state"] == "retrying"; });
        engine.stop();
        check(current()["failures"].toInt() >= 1);
        check(!current()["error"].toString().isEmpty());
    });
    suite.run("SCH-23 task removal persists", "系统功能", [&] {
        cv::schedule::removeJob(job["id"].toString());
        check(cv::schedule::listJobs().isEmpty());
    });
    suite.run("SCH-24 job path traversal rejected", "安全",
              [&] { throws([&] { cv::schedule::removeJob("../outside"); }); });
    suite.run("SCH-25 malformed job remains visible and disabled", "异常健壮性", [&] {
        const auto path = cv::schedule::jobsDirectory() + "/broken.json";
        cv::atomicWrite(path, "{broken");
        const auto jobs = cv::schedule::listJobs();
        check(jobs.size() == 1 && jobs.first().toObject()["state"] == "invalid");
        check(!jobs.first().toObject()["enabled"].toBool());
        check(jobs.first().toObject()["id"] == "broken");
        cv::schedule::removeJob(jobs.first().toObject()["id"].toString());
        check(!QFileInfo::exists(path));
    });
    suite.run("SCH-26 mismatched persisted ID cannot overwrite another task", "安全", [&] {
        const auto path = cv::schedule::jobsDirectory() + "/wrong-name.json";
        cv::writeJson(path, {{"id", "other"}, {"enabled", true}});
        const auto jobs = cv::schedule::listJobs();
        check(jobs.size() == 1 && jobs.first().toObject()["state"] == "invalid");
        cv::schedule::Engine engine;
        engine.tick();
        check(!QFileInfo::exists(cv::schedule::jobsDirectory() + "/other.json"));
        check(QFile::remove(path));
    });
    suite.run("SCH-27 empty colon and excessive IDs rejected", "安全", [&] {
        for (const auto &id : QStringList{"", "job:stream", QString(129, 'a'), "dir\\job"})
            throws([&] { cv::schedule::removeJob(id); });
    });
    suite.run("SCH-28 invalid plan cannot persist a job", "异常健壮性", [&] {
        auto empty = plan;
        empty.sources.clear();
        throws([&] { cv::schedule::saveJob({}, empty, repository.masterKey()); });
        empty = plan;
        empty.destination.clear();
        throws([&] { cv::schedule::saveJob({}, empty, repository.masterKey()); });
        check(cv::schedule::listJobs().isEmpty());
    });
    suite.run("SCH-29 invalid reference time and lower weekday rejected", "边界健壮性", [&] {
        throws([&] { cv::schedule::nextRun({}, {}); });
        throws([&] { cv::schedule::nextRun({{"mode", "weekly"}, {"weekday", 0}}, reference); });
        check(cv::schedule::nextRun({{"mode", "realtime"}}, reference) == reference.addSecs(1));
        check(cv::schedule::nextRun({{"mode", "weekly"}, {"weekday", 6}, {"time", "13:00"}}, reference)
                  .date() == reference.date());
    });
    auto makeJob = [&](const QString &id) {
        return cv::schedule::saveJob({{"id", id}, {"mode", "interval"}, {"intervalSeconds", 3600}}, plan,
                                     repository.masterKey());
    };
    auto pathFor = [](const QString &id) { return cv::schedule::jobsDirectory() + '/' + id + ".json"; };
    suite.run("SCH-30 future task is not admitted early", "系统功能", [&] {
        auto future = makeJob("future");
        future["nextRun"] = reference.addDays(1).toString(Qt::ISODateWithMs);
        cv::writeJson(pathFor("future"), future);
        const auto before = repository.list().size();
        cv::schedule::Engine engine;
        engine.tick(reference);
        check(repository.list().size() == before);
        check(cv::readJson(pathFor("future"))["state"] == "waiting");
        cv::schedule::removeJob("future");
    });
    suite.run("SCH-31 invalid due timestamp gets one catch-up run", "系统功能", [&] {
        auto due = makeJob("invalid-due");
        due["nextRun"] = "not a timestamp";
        cv::writeJson(pathFor("invalid-due"), due);
        cv::schedule::Engine engine;
        engine.tick();
        engine.tick(); // A second admission attempt must not duplicate active work.
        waitFor([&] { return cv::readJson(pathFor("invalid-due"))["state"] == "waiting"; });
        check(!cv::readJson(pathFor("invalid-due"))["lastRun"].toString().isEmpty());
        engine.stop();
        cv::schedule::removeJob("invalid-due");
    });
    suite.run("SCH-32 invalid schedule does not starve a valid job", "异常健壮性", [&] {
        auto bad = makeJob("a-invalid");
        bad["mode"] = "unsupported";
        cv::writeJson(pathFor("a-invalid"), bad);
        makeJob("z-valid");
        cv::schedule::Engine engine;
        engine.tick();
        waitFor([&] { return !cv::readJson(pathFor("z-valid"))["lastRun"].toString().isEmpty(); });
        engine.stop();
        check(cv::readJson(pathFor("a-invalid"))["state"] == "waiting");
        check(cv::readFile(cv::appDataDirectory() + "/agent-events.jsonl").contains("a-invalid"));
        cv::schedule::removeJob("a-invalid");
        cv::schedule::removeJob("z-valid");
    });
    suite.run("SCH-33 corrupt protected key produces retry not success", "安全", [&] {
        auto bad = makeJob("bad-key");
        bad["key"] = "AAAA";
        cv::writeJson(pathFor("bad-key"), bad);
        const auto before = repository.list().size();
        cv::schedule::Engine engine;
        engine.tick();
        waitFor([&] { return cv::readJson(pathFor("bad-key"))["state"] == "retrying"; });
        engine.stop();
        check(repository.list().size() == before);
        check(cv::readJson(pathFor("bad-key"))["failures"].toInt() == 1);
        cv::schedule::removeJob("bad-key");
    });
    suite.run("SCH-34 excessive retry count saturates without overflow", "边界健壮性", [&] {
        auto bad = makeJob("retry-limit");
        bad["key"] = "AAAA";
        bad["failures"] = 2147483647;
        cv::writeJson(pathFor("retry-limit"), bad);
        cv::schedule::Engine engine;
        engine.tick();
        waitFor([&] { return cv::readJson(pathFor("retry-limit"))["state"] == "retrying"; });
        const auto stored = cv::readJson(pathFor("retry-limit"));
        const auto delay = QDateTime::currentDateTime().secsTo(
            QDateTime::fromString(stored["nextRun"].toString(), Qt::ISODateWithMs));
        check(stored["failures"].toInt() == 60 && delay >= 295 && delay <= 300);
        engine.stop();
        cv::schedule::removeJob("retry-limit");
    });
    suite.run("SCH-35 completion does not recreate a deleted task", "并发稳定性", [&] {
        makeJob("removed-in-flight");
        cv::schedule::Engine engine;
        engine.tick();
        cv::schedule::removeJob("removed-in-flight");
        engine.stop();
        QCoreApplication::processEvents();
        check(!QFileInfo::exists(pathFor("removed-in-flight")));
    });
    suite.run("SCH-36 corrupt in-flight job is preserved and logged", "并发稳定性", [&] {
        makeJob("corrupt-in-flight");
        cv::schedule::Engine engine;
        engine.tick();
        cv::atomicWrite(pathFor("corrupt-in-flight"), "{broken");
        engine.stop();
        QCoreApplication::processEvents();
        check(cv::readFile(pathFor("corrupt-in-flight")) == "{broken");
        check(cv::readFile(cv::appDataDirectory() + "/agent-events.jsonl").contains("corrupt-in-flight"));
        cv::schedule::removeJob("corrupt-in-flight");
    });
    suite.run("SCH-37 pause releases real-time watch without snapshot", "并发稳定性", [&] {
        auto realtime = makeJob("paused-watch");
        realtime["mode"] = "realtime";
        realtime["lastRun"] = reference.toString(Qt::ISODateWithMs);
        cv::writeJson(pathFor("paused-watch"), realtime);
        const auto count = repository.list().size();
        cv::schedule::Engine engine;
        engine.tick();
        engine.tick(); // Existing clean watcher is reused.
        cv::schedule::setEnabled("paused-watch", false);
        engine.tick();
        check(repository.list().size() == count);
        cv::schedule::removeJob("paused-watch");
    });
    suite.run("SCH-38 selected file watches parent and consumes change flag", "集成", [&] {
        QTemporaryDir directory;
        const auto file = directory.filePath("selected.txt");
        cv::atomicWrite(file, "before");
        cv::schedule::DirectoryWatch watch({file});
        QThread::msleep(150);
        cv::atomicWrite(file, "after");
        bool observed = false;
        waitFor([&] { return observed = observed || watch.takeChanged(); });
        QThread::msleep(150);
        watch.takeChanged();
        check(!watch.takeChanged());
    });
    suite.run("SCH-39 missing watched directory requests failure rescan", "异常健壮性", [&] {
        QTemporaryDir directory;
        cv::schedule::DirectoryWatch watch({directory.filePath("missing/child/file")});
        bool observed = false;
        waitFor([&] { return observed = observed || watch.takeChanged(); });
    });
    suite.run("SCH-40 rename and delete both request real-time rescan", "系统功能", [&] {
        QTemporaryDir directory;
        const auto original = directory.filePath("before.txt");
        const auto renamed = directory.filePath("after.txt");
        cv::atomicWrite(original, "data");
        cv::schedule::DirectoryWatch watch({directory.path()});
        QThread::msleep(150);
        check(QFile::rename(original, renamed));
        bool observed = false;
        waitFor([&] { return observed = observed || watch.takeChanged(); });
        QThread::msleep(150);
        watch.takeChanged();
        check(QFile::remove(renamed));
        observed = false;
        waitFor([&] { return observed = observed || watch.takeChanged(); });
    });
    suite.run("SCH-41 missing autostart binary leaves registry unchanged", "部署兼容性", [&] {
        const bool before = cv::schedule::autoStartEnabled();
        throws([&] { cv::schedule::setAutoStart(true, files.filePath("missing-agent.exe")); });
        check(cv::schedule::autoStartEnabled() == before);
    });
    suite.run("SCH-42 inaccessible profile does not escape timer callback", "异常健壮性", [&] {
        const auto blocked = files.filePath("blocked-profile");
        cv::atomicWrite(blocked, "existing file");
        qputenv("CHENGVAULT_DATA_HOME", blocked.toUtf8());
        cv::schedule::Engine engine;
        engine.tick();
        engine.stop();
        qputenv("CHENGVAULT_DATA_HOME", state.path().toUtf8());
        check(cv::readFile(blocked) == "existing file");
    });
    suite.run("SCH-43 profile lost during completion does not crash agent", "异常健壮性", [&] {
        makeJob("profile-lost");
        cv::schedule::Engine engine;
        engine.tick();
        const auto blocked = files.filePath("blocked-profile");
        qputenv("CHENGVAULT_DATA_HOME", blocked.toUtf8());
        engine.stop();
        QCoreApplication::processEvents();
        qputenv("CHENGVAULT_DATA_HOME", state.path().toUtf8());
        check(cv::readJson(pathFor("profile-lost"))["state"] == "running");
        cv::schedule::removeJob("profile-lost");
    });
    suite.run("SCH-44 negative retry counter cannot schedule a busy loop", "边界健壮性", [&] {
        auto bad = makeJob("negative-retry");
        bad["key"] = "AAAA";
        bad["failures"] = -2147483647;
        cv::writeJson(pathFor("negative-retry"), bad);
        cv::schedule::Engine engine;
        engine.tick();
        waitFor([&] { return cv::readJson(pathFor("negative-retry"))["state"] == "retrying"; });
        const auto stored = cv::readJson(pathFor("negative-retry"));
        check(stored["failures"].toInt() == 1);
        check(QDateTime::fromString(stored["nextRun"].toString(), Qt::ISODateWithMs) >
              QDateTime::currentDateTime());
        engine.stop();
        cv::schedule::removeJob("negative-retry");
    });
    if (previous.isNull())
        qunsetenv("CHENGVAULT_DATA_HOME");
    else
        qputenv("CHENGVAULT_DATA_HOME", previous);
}
