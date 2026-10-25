#include "chengvault/archive.hpp"
#include "chengvault/network.hpp"
#include "chengvault/repair.hpp"
#include "chengvault/repository.hpp"
#include "chengvault/scheduler.hpp"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>
#include <windows.h>

namespace {
// CLI output is machine-readable JSON from the same core used by the GUI.
// Dispatch selects an operation; it does not implement a second backup format
// or bypass the repository's authentication, locks and restore validation.
QJsonObject execute(const QString &command, const QCommandLineParser &parser) {
    const auto arg = [&parser](const char *key) { return parser.value(QString::fromLatin1(key)); };
    // Passwords are read from the calling process environment or an explicitly
    // selected file, never echoed in progress or result JSON.
    QString password = qEnvironmentVariable("CHENGVAULT_PASSWORD");
    if (parser.isSet("password-file"))
        password = QString::fromUtf8(cv::readFile(arg("password-file"), 65536)).trimmed();
    cv::BackupPlan plan;
    // An optional JSON plan provides structured filter settings. Explicit
    // command-line options override only their respective fields, preserving
    // the rest of that plan for reproducible scripted runs.
    if (parser.isSet("plan"))
        plan = cv::BackupPlan::fromJson(cv::readJson(arg("plan")));
    if (parser.isSet("source"))
        plan.sources = parser.values("source");
    if (parser.isSet("output"))
        plan.destination = arg("output");
    if (parser.isSet("format"))
        plan.format = arg("format");
    if (parser.isSet("compression"))
        plan.compression = arg("compression");
    if (parser.isSet("cipher"))
        plan.cipher = arg("cipher");
    if (!password.isEmpty())
        plan.password = password;
    // Strict metadata remains the default. The lenient switch is an explicit
    // user choice and preserves warnings in result JSON instead of claiming
    // that owner/ACL restoration succeeded with insufficient privileges.
    if (parser.isSet("lenient-metadata"))
        plan.strictMetadata = false;
    cv::RestoreOptions restore;
    // Overwrite must be explicitly selected. Core still verifies a temporary
    // restored file before replacing a target, even when this option is true.
    // Repeated --select values are relative manifest paths validated by core.
    restore.overwrite = parser.isSet("overwrite");
    restore.strictMetadata = !parser.isSet("lenient-metadata");
    restore.selectedPaths = parser.values("select");
    const cv::Context context{};
    if (command == "preview")
        return {{"entries", cv::fs::preview(plan, context)}};
    if (command == "backup")
        return cv::archive::backup(plan, context).toJson();
    if (command == "inspect")
        return cv::archive::inspect(arg("input"), password, context);
    if (command == "restore")
        return cv::archive::restore(arg("input"), arg("output"), password, restore, context).toJson();
    if (command == "verify")
        return cv::archive::verify(arg("input"), password, context).toJson();
    if (command.startsWith("repo-")) {
        // Repository mode requires an explicit root, preventing an empty path
        // from implicitly creating or pruning data in the working directory.
        // A repository instance is scoped to one command and owns its key.
        const QString repositoryPath = arg("repo");
        cv::require(!repositoryPath.isEmpty(), "MISSING_REPOSITORY", "请指定 --repo");
        cv::require(!password.isEmpty(), "REPOSITORY_PASSWORD_EMPTY", "仓库密码不能为空");
        cv::Repository repository(repositoryPath, password);
        if (command == "repo-backup") {
            plan.destination = repositoryPath;
            return repository.backup(plan, context).toJson();
        }
        if (command == "repo-list")
            return {{"snapshots", repository.list()}};
        if (command == "repo-inspect")
            return repository.manifest(arg("snapshot"));
        if (command == "repo-restore")
            return repository.restore(arg("snapshot"), arg("output"), restore, context).toJson();
        if (command == "repo-verify")
            return repository.verify(context).toJson();
        if (command == "repo-prune")
            return repository.prune(arg("keep-count").toInt(), arg("keep-days").toInt(), context).toJson();
        if (command == "repo-change-password") {
            // Rewrap the persisted master key instead of rewriting all data
            // blocks. The new password is not accepted in process arguments,
            // which are more easily exposed by process-list inspection.
            const auto next = qEnvironmentVariable("CHENGVAULT_NEW_PASSWORD");
            cv::require(!next.isEmpty(), "MISSING_PASSWORD", "请设置 CHENGVAULT_NEW_PASSWORD");
            repository.changePassword(next);
            return {{"success", true}};
        }
    }
    if (command == "repair-create")
        // Repair commands operate on archive bytes without decrypting them.
        // Recovery still refuses existing outputs and authenticates the final
        // byte digest; password-based restore is a separate subsequent step.
        return cv::repair::create(arg("input"), arg("sidecar"), context).toJson();
    if (command == "repair-check")
        return cv::repair::check(arg("input"), arg("sidecar"), context);
    if (command == "repair-recover")
        return cv::repair::recover(arg("input"), arg("sidecar"), arg("output"), context).toJson();
    if (command.startsWith("remote-")) {
        // Connection metadata and device pairing are independent from the
        // repository password. The login password is read separately and only
        // sent inside the authenticated encrypted network session.
        const auto config = cv::readJson(arg("remote-config"));
        cv::network::RemoteOptions options;
        options.host = config.value("host").toString("127.0.0.1");
        options.port = quint16(config.value("port").toInt(48484));
        options.user = config.value("user").toString();
        options.password = qEnvironmentVariable("CHENGVAULT_LOGIN_PASSWORD");
        options.deviceId = config.value("deviceId").toString();
        options.deviceKey = config.contains("deviceKeyHex")
                                ? QByteArray::fromHex(config.value("deviceKeyHex").toString().toLatin1())
                                : QByteArray::fromBase64(config.value("deviceKey").toString().toLatin1());
        cv::network::Client client(options);
        if (command == "remote-list")
            return {{"repositories", client.list()}};
        if (command == "remote-upload")
            return client.upload(arg("repo"), context).toJson();
        if (command == "remote-download")
            return client.download(arg("remote-id"), arg("output"), context).toJson();
    }
    if (command == "jobs")
        // Listing is observational. GUI/agent state remains in per-user files;
        // invoking this command does not start or reschedule a background job.
        return {{"jobs", cv::schedule::listJobs()}};
    throw cv::Error("UNKNOWN_COMMAND", "未知命令: " + command);
}
} // namespace
int main(int argc, char **argv) {
    // UTF-8 console output preserves Chinese filenames and domain errors.
    // Organization/application names intentionally match GUI and agent so
    // app-data paths identify the same profile across all three frontends.
    SetConsoleOutputCP(CP_UTF8);
    QCoreApplication app(argc, argv);
    app.setOrganizationName("ChengVault");
    app.setApplicationName("ChengVault");
    app.setApplicationVersion("1.0.0");
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "澄匣备份 CLI\n密码环境变量 CHENGVAULT_PASSWORD；网络登录使用 CHENGVAULT_LOGIN_PASSWORD。");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("command",
                                 "preview | backup | inspect | restore | verify | "
                                 "repo-backup/list/inspect/restore/verify/prune/change-password | "
                                 "repair-create/check/recover | remote-list/upload/download | jobs");
    for (const QString &name :
         {"plan", "source", "input", "output", "format", "compression", "cipher", "repo", "snapshot",
          "sidecar", "password-file", "select", "remote-config", "remote-id"})
        parser.addOption({name, name, "value"});
    parser.addOption({"keep-count", "保留最新版本数", "n", "10"});
    parser.addOption({"keep-days", "保留天数，0表示不按年龄淘汰", "days", "0"});
    parser.addOption({"overwrite", "允许替换已验证的现有文件"});
    parser.addOption({"lenient-metadata", "元数据权限不足时保留警告，不伪装为完整恢复"});
    parser.process(app);
    if (parser.positionalArguments().isEmpty())
        parser.showHelp(2);
    try {
        // Success JSON goes to stdout for scripting; structured failure goes
        // to stderr with nonzero exit status. A returned unsuccessful result
        // has a distinct status from an exception during command execution.
        const auto result = execute(parser.positionalArguments().first(), parser);
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Indented);
        return result.contains("success") && !result.value("success").toBool() ? 3 : 0;
    } catch (const cv::Error &error) {
        // Preserve the stable domain code for scripts. The localized message
        // remains useful to humans without requiring string parsing for logic.
        QTextStream(stderr) << QJsonDocument(QJsonObject{{"success", false},
                                                         {"code", error.code},
                                                         {"error", QString::fromUtf8(error.what())}})
                                   .toJson();
        return 1;
    } catch (const std::exception &error) {
        QTextStream(stderr) << QJsonDocument(QJsonObject{{"success", false},
                                                         {"code", "UNEXPECTED"},
                                                         {"error", QString::fromUtf8(error.what())}})
                                   .toJson();
        return 1;
    }
}
