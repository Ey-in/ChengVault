#include "../gui/main_window.hpp"
#include <QApplication>
#include <QCommandLineParser>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>

int main(int argc, char **argv) {
    // Match CLI/agent identity before constructing widgets that read settings.
    // QApplication owns the event loop; worker completion reaches widgets only
    // through queued signals, never direct writes from the backup thread.
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("ChengVault");
    QCoreApplication::setApplicationName("ChengVault");
    QCoreApplication::setApplicationVersion("1.0.0");
    QCommandLineParser parser;
    parser.setApplicationDescription("澄匣备份 ChengVault");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"screenshot-dir", "Capture real application pages and exit", "directory"});
    parser.addOption({"screenshot-size", "Logical window size for screenshot capture only", "widthxheight"});
    parser.process(application);
    MainWindow window;
    // Size overrides are restricted to evidence capture. Normal launches keep
    // the screen-aware default instead of silently changing user preferences.
    if (parser.isSet("screenshot-dir") && parser.isSet("screenshot-size")) {
        const auto match =
            QRegularExpression("^([0-9]{3,4})x([0-9]{3,4})$").match(parser.value("screenshot-size"));
        const int width = match.captured(1).toInt(), height = match.captured(2).toInt();
        if (!match.hasMatch() || width < 1040 || height < 720 || width > 4096 || height > 4096) {
            QTextStream(stderr) << "Screenshot size must be between 1040x720 and 4096x4096.\n";
            return 2;
        }
        window.resize(width, height);
    }
    window.show();
    if (parser.isSet("screenshot-dir"))
        // Delay until the window is actually shown and layouts have settled.
        // This captures real pages for evidence and exits through normal Qt
        // teardown; it does not replace functional or independent-machine tests.
        QTimer::singleShot(500, &window, [&] {
            window.saveScreenshots(parser.value("screenshot-dir"));
            application.quit();
        });
    return application.exec();
}
