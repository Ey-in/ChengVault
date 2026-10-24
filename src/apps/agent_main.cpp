#include "chengvault/scheduler.hpp"
#include <QCoreApplication>
#include <QLockFile>
#include <QTextStream>

int main(int argc, char **argv) {
    // A QCoreApplication provides timers/future completion without any windows.
    // The agent is a separate process, so closing the GUI cannot stop its loop.
    QCoreApplication app(argc, argv);
    app.setOrganizationName("ChengVault");
    app.setApplicationName("ChengVault");
    // A per-user process lock prevents a second GUI or startup entry from
    // scheduling the same job twice.
    QLockFile lock(cv::appDataDirectory() + "/agent.lock");
    lock.setStaleLockTime(0);
    // Do not expire a live long-running backup merely because it is old.
    // QLockFile still detects dead owners when a prior process has crashed.
    if (!lock.tryLock(0))
        return 0;
    try {
        cv::schedule::Engine engine;
        engine.start();
        // Normal shutdown cooperatively cancels and joins current work before
        // stack destruction releases the engine and the process lock.
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &engine, [&engine] { engine.stop(); });
        return app.exec();
    } catch (const std::exception &error) {
        QTextStream(stderr) << error.what() << Qt::endl;
        return 1;
    }
}
