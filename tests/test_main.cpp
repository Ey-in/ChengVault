#include "test_support.hpp"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QSysInfo>

void algorithmTests(cv::test::Suite &);
void filesystemTests(cv::test::Suite &);
void repositoryNetworkTests(cv::test::Suite &);
void schedulerTests(cv::test::Suite &);

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("ChengVaultTests");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"output", "JSON evidence output", "path", "test-results.json"});
    parser.addOption({"module", "Run one module: algorithms, filesystem, repository, scheduler; default all",
                      "module", "all"});
    parser.process(app);
    cv::test::Suite suite;
    const auto module = parser.value("module");
    if (module == "all" || module == "algorithms")
        algorithmTests(suite);
    if (module == "all" || module == "filesystem")
        filesystemTests(suite);
    if (module == "all" || module == "repository")
        repositoryNetworkTests(suite);
    if (module == "all" || module == "scheduler")
        schedulerTests(suite);
    const QJsonObject result{
        {"passed", suite.passed},   {"failed", suite.failed},
        {"skipped", suite.skipped}, {"platform", QSysInfo::prettyProductName()},
        {"qt", QT_VERSION_STR},     {"date", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"cases", suite.records}};
    cv::writeJson(parser.value("output"), result);
    QTextStream(stdout) << "Passed " << suite.passed << ", failed " << suite.failed << ", skipped "
                        << suite.skipped << Qt::endl;
    return suite.failed ? 1 : 0;
}
