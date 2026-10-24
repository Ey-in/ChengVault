#include "chengvault/repository.hpp"
#include "chengvault/scheduler.hpp"
#include "test_support.hpp"
#include <QCoreApplication>
#include <QDirIterator>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

// This is deliberately an executable integration test: an in-process Engine
// test alone cannot prove that closing the real GUI leaves the Agent alive.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName("ChengVault");
    app.setApplicationName("ChengVaultBackgroundTests");
    cv::test::Suite suite;
    QTemporaryDir fixture;
    cv::test::check(fixture.isValid(), "cannot create fixture");
    qputenv("CHENGVAULT_DATA_HOME", fixture.filePath("state").toUtf8());
    cv::BackupPlan plan;
    plan.sources = {fixture.filePath("source")};
    plan.destination = fixture.filePath("repository");
    plan.password = "background-process-test-only";
    plan.cipher = "chacha20";
    QDir().mkpath(plan.sources.first());
    cv::atomicWrite(plan.sources.first() + "/live.txt", "before-gui-close");
    cv::Repository repository(plan.destination, plan.password);
    repository.init();
    const auto job = cv::schedule::saveJob({{"name", "process integration"},
                                            {"mode", "interval"},
                                            {"intervalSeconds", 3},
                                            {"keepCount", 2},
                                            {"keepDays", 0}},
                                           plan, repository.masterKey());
    const auto jobPath = cv::schedule::jobsDirectory() + '/' + job["id"].toString() + ".json";
    const QString binaries = QCoreApplication::applicationDirPath();
    QProcess agent, gui, duplicate;
    QString lastJobReadDiagnostic;
    int transientJobReadRetries = 0;
    int repositoryBusyRetries = 0;
    QString completedSnapshot;
    auto readPolledJob = [&] {
        try {
            return cv::readJson(jobPath);
        } catch (const cv::Error &error) {
            // The separate Agent atomically replaces this file while the
            // observer polls it. Retry only transient open failures;
            // short reads, malformed JSON and invalid sizes still fail.
            if (error.code != "READ_FAILED")
                throw;
            lastJobReadDiagnostic = error.code + ": " + QString::fromUtf8(error.what());
            ++transientJobReadRetries;
            return QJsonObject{};
        }
    };
    auto waitUntil = [&](const std::function<bool()> &ready) {
        lastJobReadDiagnostic.clear();
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 25000) {
            try {
                if (ready())
                    return;
            } catch (const cv::Error &error) {
                // A background writer owns the repository lock until commit
                // or retention finishes. Only the explicit busy result is
                // retryable; missing/corrupt snapshots remain real failures.
                if (error.code != "REPOSITORY_BUSY")
                    throw;
                lastJobReadDiagnostic = error.code + ": " + QString::fromUtf8(error.what());
                ++repositoryBusyRetries;
            }
            QCoreApplication::processEvents();
            QThread::msleep(50);
        }
        cv::test::check(
            false,
            "timed out waiting for independent Agent" +
                (lastJobReadDiagnostic.isEmpty() ? QString{} : "; last job read: " + lastJobReadDiagnostic));
    };
    suite.run("PROC-01 independent Agent executes persisted DPAPI task", "集成", [&] {
        agent.start(binaries + "/ChengVaultAgent.exe", {"--background"});
        cv::test::check(agent.waitForStarted());
        waitUntil([&] {
            const auto state = readPolledJob();
            if (state["state"] != "waiting" || state["lastRun"].toString().isEmpty())
                return false;
            completedSnapshot = state["lastResult"].toObject()["details"].toObject()["snapshotId"].toString();
            return !completedSnapshot.isEmpty() && repository.list().size() >= 1;
        });
    });
    suite.run("PROC-02 per-user lock rejects duplicate Agent", "并发稳定性", [&] {
        duplicate.start(binaries + "/ChengVaultAgent.exe", {"--background"});
        cv::test::check(duplicate.waitForStarted());
        cv::test::check(duplicate.waitForFinished(5000));
        cv::test::check(duplicate.exitCode() == 0);
        cv::test::check(agent.state() == QProcess::Running);
    });
    suite.run("PROC-03 actual GUI exits while Agent remains running", "GUI与部署兼容性", [&] {
        gui.start(binaries + "/ChengVault.exe", {"--screenshot-dir", fixture.filePath("screens")});
        cv::test::check(gui.waitForStarted());
        cv::test::check(gui.waitForFinished(20000));
        cv::test::check(gui.exitStatus() == QProcess::NormalExit && gui.exitCode() == 0);
        cv::test::check(QDir(fixture.filePath("screens")).entryList({"*.png"}, QDir::Files).size() == 8);
        cv::test::check(agent.state() == QProcess::Running);
    });
    suite.run("PROC-04 after GUI exit changed data produces recoverable snapshot", "系统功能", [&] {
        waitUntil([&] {
            const auto state = readPolledJob();
            if (state["state"] != "waiting")
                return false;
            completedSnapshot = state["lastResult"].toObject()["details"].toObject()["snapshotId"].toString();
            return !completedSnapshot.isEmpty();
        });
        const auto prior = completedSnapshot;
        cv::atomicWrite(plan.sources.first() + "/live.txt", "after-gui-close");
        waitUntil([&] {
            const auto state = readPolledJob();
            const auto snapshot =
                state["lastResult"].toObject()["details"].toObject()["snapshotId"].toString();
            if (state["state"] != "waiting" || snapshot.isEmpty() || snapshot == prior)
                return false;
            completedSnapshot = snapshot;
            return true;
        });
        const auto target = fixture.filePath("restored");
        cv::TaskResult result;
        waitUntil([&] {
            result = repository.restore(completedSnapshot, target);
            return true;
        });
        cv::test::check(result.success && result.warnings.isEmpty());
        bool found = false;
        QDirIterator files(target, {"live.txt"}, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext())
            found |= cv::readFile(files.next()) == "after-gui-close";
        cv::test::check(found, "restored snapshot did not contain post-GUI change");
    });
    suite.run("PROC-05 background retention leaves two valid complete versions", "系统功能", [&] {
        waitUntil([&] {
            const auto state = readPolledJob();
            if (state["state"] != "waiting")
                return false;
            completedSnapshot = state["lastResult"].toObject()["details"].toObject()["snapshotId"].toString();
            return !completedSnapshot.isEmpty();
        });
        const auto prior = completedSnapshot;
        cv::atomicWrite(plan.sources.first() + "/live.txt", "third-independent-version");
        waitUntil([&] {
            const auto state = readPolledJob();
            const auto snapshot =
                state["lastResult"].toObject()["details"].toObject()["snapshotId"].toString();
            return state["state"] == "waiting" && !snapshot.isEmpty() && snapshot != prior;
        });
        // Inspect a stable final repository after the actual Agent has reported
        // both backup and retention complete. A later scheduled tick must not
        // race this assertion or delete a version while verify enumerates it.
        agent.kill();
        cv::test::check(agent.waitForFinished(5000));
        cv::test::check(repository.list().size() == 2);
        cv::test::check(repository.verify().success);
    });
    // Only processes started by this test are terminated; no user task or
    // login registry entry is touched. State lives in the temporary fixture.
    if (agent.state() != QProcess::NotRunning) {
        agent.kill();
        agent.waitForFinished(5000);
    }
    if (gui.state() != QProcess::NotRunning) {
        gui.kill();
        gui.waitForFinished(5000);
    }
    if (duplicate.state() != QProcess::NotRunning) {
        duplicate.kill();
        duplicate.waitForFinished(5000);
    }
    const QJsonObject report{{"passed", suite.passed},
                             {"failed", suite.failed},
                             {"skipped", suite.skipped},
                             {"transientJobReadRetries", transientJobReadRetries},
                             {"repositoryBusyRetries", repositoryBusyRetries},
                             {"cases", suite.records}};
    const auto arguments = app.arguments();
    const int output = arguments.indexOf("--output");
    if (output >= 0 && output + 1 < arguments.size())
        cv::writeJson(arguments[output + 1], report);
    return suite.failed ? 1 : 0;
}
