#include "chengvault/crypto.hpp"
#include "main_window.hpp"
#include "test_support.hpp"
#include <QTemporaryDir>
#include <QtTest>
#include <QtWidgets>

// Exercise actual widget actions and the asynchronous production worker. A
// timer dismisses completion dialogs so this executable can run unattended.
// The assertions compare real output files rather than button labels alone.
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("ChengVault");
    app.setApplicationName("ChengVaultGuiTests");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"output", "Evidence JSON", "path", "gui-test-results.json"});
    parser.addOption({"demo-dir", "Record real workflow frames with readable pauses", "directory"});
    parser.addOption(
        {"review-dir", "Optional keyboard/dialog review, separate from course test counts", "directory"});
    parser.process(app);
    const auto demo = parser.value("demo-dir");
    if (!demo.isEmpty())
        QDir().mkpath(demo);
    QTemporaryDir state;
    QTemporaryDir data;
    qputenv("CHENGVAULT_DATA_HOME", state.path().toUtf8());
    cv::test::Suite suite;
    MainWindow window;
    window.show();
    QTest::qWait(100);
    if (!demo.isEmpty()) {
        window.setWindowTitle("ChengVault Demonstration");
        window.resize(1100, 760);
        QTest::qWait(5000);
    }
    QStringList dialogs;
    QTimer closer;
    closer.setInterval(25);
    QObject::connect(&closer, &QTimer::timeout, [&] {
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *box = qobject_cast<QMessageBox *>(widget)) {
                dialogs << box->text();
                box->accept();
            }
    });
    closer.start();
    auto edit = [&](const QString &name, const QString &value) {
        auto *field = window.findChild<QLineEdit *>(name);
        cv::test::check(field != nullptr, "missing input " + name);
        field->setText(value);
    };
    auto click = [&](const QString &label) {
        for (auto *b : window.findChildren<QPushButton *>())
            if (b->text() == label) {
                b->click();
                return;
            }
        throw std::runtime_error(("missing button " + label).toStdString());
    };
    // A committed file can appear before the worker returns. Wait for the
    // production footer's terminal state as well as the operation's evidence;
    // this observes delivery of the finished callback on the GUI thread.
    // Fixed sleeps hide this race on fast Release builds and fail under gcov.
    auto settled = [&] {
        auto *footer = window.findChild<QWidget *>("taskFooter");
        cv::test::check(footer != nullptr, "missing task footer");
        for (auto *label : footer->findChildren<QLabel *>())
            if (label->text() == "操作完成" || label->text() == "任务未完成")
                return true;
        return false;
    };
    auto wait = [&](const std::function<bool()> &ready, int timeout = 60000) {
        QElapsedTimer timer;
        timer.start();
        while ((!ready() || !settled()) && timer.elapsed() < timeout)
            QTest::qWait(25);
        cv::test::check(ready() && settled(), "GUI worker did not complete: " + dialogs.join(" | "));
    };
    auto frame = [&](const QString &name, int page) {
        if (demo.isEmpty())
            return;
        window.selectPage(page);
        QApplication::processEvents();
        window.grab().save(demo + '/' + name + ".png");
        QTest::qWait(4500);
    };
    for (int i = 0; i < 8; ++i)
        suite.run(QString("GUI-Page-%1 renders at minimum size").arg(i + 1), "GUI与部署兼容性", [&, i] {
            if (demo.isEmpty())
                window.resize(1040, 720);
            window.selectPage(i);
            QApplication::processEvents();
            auto shot = window.grab();
            cv::test::check(!shot.isNull());
            cv::test::check(window.minimumWidth() <= 1040);
        });
    suite.run("GUI-passwords use masked controls", "安全", [&] {
        for (const auto &name : {"backup.password", "restore.password", "remote.password", "remote.key"})
            cv::test::check(window.findChild<QLineEdit *>(name)->echoMode() == QLineEdit::Password);
    });
    const auto dataRoot = demo.isEmpty() ? data.path() : demo + "/data";
    QDir().mkpath(dataRoot);
    const auto source = dataRoot + "/source", archive = dataRoot + "/demo.cva",
               restored = dataRoot + "/restored";
    QDir().mkpath(source);
    cv::atomicWrite(source + "/中文.txt", QByteArray("ChengVault GUI integration\n").repeated(10000));
    QDir().mkpath(source + "/空目录");
    cv::atomicWrite(source + "/empty", {});
    suite.run("GUI-create encrypted archive through real button", "系统功能", [&] {
        window.selectPage(1);
        window.findChild<QPlainTextEdit *>("backup.sources")->setPlainText(source);
        edit("backup.output", archive);
        edit("backup.password", "gui-test-password");
        frame("01-backup-configuration", 1);
        click("开始备份");
        wait([&] { return QFileInfo::exists(archive) && !dialogs.isEmpty(); });
        cv::test::check(dialogs.last().contains("输出"), dialogs.last());
        frame("02-encrypted-archive-created", 6);
    });
    suite.run("GUI-inspect decrypts and displays manifest", "集成", [&] {
        window.selectPage(2);
        edit("restore.input", archive);
        edit("restore.password", "gui-test-password");
        click("读取备份");
        wait([&] {
            for (auto *tree : window.findChildren<QTreeWidget *>())
                if (tree->columnCount() == 3 && tree->topLevelItemCount() >= 4)
                    return true;
            return false;
        });
    });
    suite.run("GUI-restore unpack decompress decrypt roundtrip", "系统功能", [&] {
        edit("restore.output", restored);
        click("恢复已勾选文件");
        wait([&] { return QFileInfo::exists(restored + "/source/中文.txt"); });
        cv::test::check(cv::crypto::hashFile(restored + "/source/中文.txt") ==
                        cv::crypto::hashFile(source + "/中文.txt"));
        cv::test::check(QFileInfo(restored + "/source/空目录").isDir());
    });
    suite.run("GUI-existing-file conflict reports failure", "异常健壮性", [&] {
        const auto count = dialogs.size();
        click("恢复已勾选文件");
        wait([&] { return dialogs.size() > count; });
        cv::test::check(cv::crypto::hashFile(restored + "/source/中文.txt") ==
                        cv::crypto::hashFile(source + "/中文.txt"));
    });
    suite.run("GUI-wrong-password reports authentication failure", "安全", [&] {
        const auto count = dialogs.size();
        edit("restore.password", "incorrect");
        click("读取备份");
        wait([&] { return dialogs.size() > count; });
        cv::test::check(!dialogs.last().contains("输出"));
    });
    const auto sidecar = archive + ".cvr", damaged = dataRoot + "/damaged.cva",
               repaired = dataRoot + "/repaired.cva", repairedFiles = dataRoot + "/repaired-files";
    suite.run("GUI-generate repair sidecar using real button", "集成", [&] {
        window.selectPage(5);
        edit("repair.archive", archive);
        edit("repair.sidecar", sidecar);
        click("生成纠错文件");
        wait([&] { return QFileInfo::exists(sidecar); });
        frame("03-redundancy-generated", 5);
    });
    suite.run("GUI-damaged encrypted archive is rejected", "安全", [&] {
        auto bytes = cv::readFile(archive);
        bytes[bytes.size() / 2] ^= char(0x55);
        cv::atomicWrite(damaged, bytes);
        window.findChild<QPlainTextEdit *>("operationLog")
            ->appendPlainText("实际故障注入：复制归档并修改中间一个字节；原始归档保留。");
        edit("restore.input", damaged);
        edit("restore.password", "gui-test-password");
        const auto count = dialogs.size();
        window.selectPage(2);
        click("读取备份");
        wait([&] { return dialogs.size() > count; });
        frame("04-damage-rejected", 6);
    });
    suite.run("GUI-repair damaged archive to new file", "集成", [&] {
        window.selectPage(5);
        edit("repair.archive", damaged);
        edit("repair.output", repaired);
        const auto count = dialogs.size();
        click("检查损坏");
        wait([&] { return dialogs.size() > count; });
        frame("05-damaged-shards-located", 6);
        window.selectPage(5);
        click("修复到新文件");
        wait([&] { return QFileInfo::exists(repaired); });
        cv::test::check(cv::crypto::hashFile(repaired) == cv::crypto::hashFile(archive));
        frame("06-archive-repaired", 6);
    });
    suite.run("GUI-repaired encrypted archive restores with original password", "集成", [&] {
        window.selectPage(2);
        edit("restore.input", repaired);
        edit("restore.password", "gui-test-password");
        edit("restore.output", repairedFiles);
        click("读取备份");
        wait([] { return true; });
        frame("07-password-restore", 2);
        click("恢复已勾选文件");
        wait([&] { return QFileInfo::exists(repairedFiles + "/source/中文.txt"); });
        cv::test::check(cv::crypto::hashFile(repairedFiles + "/source/中文.txt") ==
                        cv::crypto::hashFile(source + "/中文.txt"));
    });
    suite.run("GUI-compare restored file hashes in production workflow", "系统功能", [&] {
        const auto count = dialogs.size();
        window.compareFiles(source + "/中文.txt", repairedFiles + "/source/中文.txt");
        wait([&] { return dialogs.size() > count; });
        cv::test::check(dialogs.last().contains("完全一致"));
        frame("08-hashes-match", 6);
    });
    suite.run("GUI-real pages screenshot export", "GUI与部署兼容性", [&] {
        if (demo.isEmpty())
            window.resize(1260, 880);
        window.saveScreenshots(data.filePath("screenshots"));
        cv::test::check(QDir(data.filePath("screenshots")).entryList({"*.png"}, QDir::Files).size() == 8);
    });
    bool reviewFailed = false;
    if (parser.isSet("review-dir")) {
        // These opt-in observations are deliberately outside Suite: repeated
        // visual checks must not inflate the existing twenty functional cases.
        // No review action accepts an administrative dialog or starts a task.
        const auto directory = parser.value("review-dir");
        QJsonArray records;
        auto record = [&](const QString &name, bool passed, const QString &detail = QString()) {
            records.append(QJsonObject{{"name", name}, {"passed", passed}, {"detail", detail}});
            reviewFailed |= !passed;
        };
        auto capture = [&](QWidget *widget, const QString &name) {
            QApplication::processEvents();
            record(name + " saved", widget->grab().save(directory + '/' + name + ".png"));
        };
        try {
            cv::require(!directory.isEmpty() && QDir().mkpath(directory), "REVIEW_DIRECTORY",
                        "无法创建独立界面审阅目录");
            window.resize(1040, 720);
            window.selectPage(1);
            auto *field = window.findChild<QLineEdit *>("backup.output");
            cv::require(field != nullptr, "REVIEW_INPUT", "缺少备份输出输入框");
            field->setText("D:/仅供界面排版验收/" + QString("长中文目录与空格 示例/").repeated(12) +
                           "完整备份归档.cva");
            field->setCursorPosition(field->text().size());
            field->setFocus(Qt::OtherFocusReason);
            QApplication::processEvents();
            record("minimum window retained", window.size() == QSize(1040, 720));
            record("long path retained", field->text().endsWith("完整备份归档.cva"));
            record("input focus", window.focusWidget() == field,
                   "resolved font: " + QFontInfo(field->font()).family());
            capture(&window, "01-long-path-focus");
            QTest::keyClick(field, Qt::Key_Tab);
            auto *next = window.focusWidget();
            record("Tab moves focus", next && next != field);
            capture(&window, "01-tab-focus");
            if (next)
                QTest::keyClick(next, Qt::Key_Tab, Qt::ShiftModifier);
            record("ShiftTab returns focus", window.focusWidget() == field);
            for (const auto &name :
                 {"backup.password", "restore.password", "remote.password", "remote.key"}) {
                const auto *secret = window.findChild<QLineEdit *>(name);
                record(QString(name) + " masked", secret && secret->echoMode() == QLineEdit::Password);
            }
            // This is explicitly a layout sample, not a fabricated operation
            // failure. It changes only the label and never writes to the log.
            bool sampledError = false;
            if (auto *footer = window.findChild<QWidget *>("taskFooter"))
                for (auto *label : footer->findChildren<QLabel *>())
                    if (label->text() == "操作完成" || label->text() == "任务未完成") {
                        const auto previous = label->text();
                        label->setText("排版样例（未执行操作）：无法访问长中文路径。" + field->text() +
                                       " 请检查目标位置与访问权限，原有文件保持不变。");
                        capture(&window, "02-long-error-layout-sample");
                        label->setText(previous);
                        sampledError = true;
                        break;
                    }
            record("long error layout sample captured", sampledError);
            auto reviewDialog = [&](const QString &buttonText, const QString &title, const QString &name,
                                    bool needsScroll) {
                bool observed = false;
                QTimer inspect;
                inspect.setSingleShot(true);
                QObject::connect(&inspect, &QTimer::timeout, [&] {
                    // The button below enters QDialog::exec. Inspect and reject
                    // inside that nested loop; never let an assertion escape it.
                    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    if (!dialog)
                        for (auto *widget : QApplication::topLevelWidgets())
                            if (auto *candidate = qobject_cast<QDialog *>(widget);
                                candidate && candidate->isVisible() && candidate->windowTitle() == title)
                                dialog = candidate;
                    if (!dialog) {
                        record(name + " modal visible", false);
                        return;
                    }
                    observed = true;
                    record(name + " title", dialog->windowTitle() == title);
                    record(name + " fits review viewport",
                           dialog->width() <= window.width() && dialog->height() <= window.height(),
                           QString("dialog %1x%2; review window %3x%4")
                               .arg(dialog->width())
                               .arg(dialog->height())
                               .arg(window.width())
                               .arg(window.height()));
                    auto *buttons = dialog->findChild<QDialogButtonBox *>();
                    for (auto role : {QDialogButtonBox::Ok, QDialogButtonBox::Cancel}) {
                        auto *b = buttons ? buttons->button(role) : nullptr;
                        const bool visible =
                            b && b->isVisible() &&
                            dialog->rect().contains(QRect(b->mapTo(dialog, QPoint()), b->size())) &&
                            b->visibleRegion().boundingRect().contains(b->rect());
                        record(name + (role == QDialogButtonBox::Ok ? " OK visible" : " Cancel visible"),
                               visible);
                    }
                    capture(dialog, name);
                    if (needsScroll) {
                        auto *scroll = dialog->findChild<QScrollArea *>();
                        record(name + " scroll container", scroll && scroll->viewport()->isVisible());
                        if (scroll) {
                            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
                            capture(dialog, name + "-scrolled");
                        }
                    } else {
                        int masked = 0;
                        for (const auto *input : dialog->findChildren<QLineEdit *>())
                            masked += input->echoMode() == QLineEdit::Password;
                        record(name + " administrator secrets masked", masked >= 2);
                    }
                    QTest::keyClick(dialog, Qt::Key_Escape);
                    record(name + " Escape closes", !dialog->isVisible());
                    if (dialog->isVisible())
                        dialog->reject();
                    record(name + " rejected without request", dialog->result() == QDialog::Rejected);
                });
                inspect.start(80);
                click(buttonText);
                inspect.stop();
                record(name + " inspected without acceptance", observed);
            };
            reviewDialog("筛选规则…", "自定义备份筛选", "03-filter-dialog", true);
            window.selectPage(4);
            reviewDialog("账户与设备管理", "账户与设备管理", "04-administration-dialog", false);
        } catch (const std::exception &error) {
            record("review exception", false, QString::fromUtf8(error.what()));
        }
        if (!directory.isEmpty() && QFileInfo(directory).isDir())
            cv::writeJson(directory + "/review.json",
                          {{"scope", "Optional UI observations; excluded from course functional test counts"},
                           {"status", reviewFailed ? "FAIL" : "PASS"},
                           {"visualReview", "PENDING"},
                           {"scaleFactor", qEnvironmentVariable("QT_SCALE_FACTOR", "default")},
                           {"date", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                           {"records", records}});
    }
    window.close();
    closer.stop();
    cv::writeJson(parser.value("output"), {{"passed", suite.passed},
                                           {"failed", suite.failed},
                                           {"skipped", suite.skipped},
                                           {"date", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                                           {"cases", suite.records}});
    return suite.failed || reviewFailed ? 1 : 0;
}
