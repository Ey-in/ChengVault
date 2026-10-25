#include "main_window.hpp"
#include "chengvault/archive.hpp"
#include "chengvault/crypto.hpp"
#include "chengvault/filesystem.hpp"
#include "chengvault/repair.hpp"
#include "chengvault/repository.hpp"
#include "chengvault/scheduler.hpp"
#include "theme.hpp"
#include <QtConcurrent>
#include <QtWidgets>
#include <windows.h>

namespace {
// The footer preserves the complete diagnostic as accessible label text and
// tooltip. Only painting is elided, so a long path cannot widen the window or
// push the cancel action outside the available width.
class StatusLabel final : public QLabel {
  public:
    explicit StatusLabel(const QString &text) : QLabel(text) {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    QSize minimumSizeHint() const override {
        return {0, 22};
    }
    void paintEvent(QPaintEvent *) override {
        setToolTip(text());
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::WindowText));
        painter.setFont(font());
        painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(text(), Qt::ElideMiddle, contentsRect().width()));
    }
};
// These helpers create widgets but do not own them permanently. Adding a
// widget to a layout establishes Qt parent ownership; destroying the page
// then releases its child controls and their signal connections together.
// Dynamic properties are visual roles, not application state. Core backup
// decisions must never depend on a stylesheet or translated display label.
QLabel *titleLabel(const QString &text, const char *role = "section") {
    auto *label = new QLabel(text);
    label->setProperty("role", role);
    label->setWordWrap(true);
    return label;
}
QPushButton *button(const QString &text, bool primary = false) {
    auto *result = new QPushButton(text);
    result->setCursor(Qt::PointingHandCursor);
    if (primary)
        result->setProperty("primary", true);
    return result;
}
QWidget *pageBody(QVBoxLayout *&layout) {
    auto *widget = new QWidget;
    widget->setProperty("role", "pageBody");
    layout = new QVBoxLayout(widget);
    layout->setContentsMargins(28, 24, 28, 28);
    layout->setSpacing(18);
    return widget;
}
QFormLayout *form(QVBoxLayout *parent) {
    auto *result = new QFormLayout;
    result->setHorizontalSpacing(20);
    result->setVerticalSpacing(12);
    result->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    result->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    parent->addLayout(result);
    return result;
}
void twoColumns(QFormLayout *fields, QVBoxLayout *parent) {
    // Labels above their controls use horizontal space efficiently without
    // forcing a wide fixed label column. Each cell retains the real widget.
    // This is layout-only: tab order, object names and signal wiring survive.
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(22);
    grid->setVerticalSpacing(8);
    int index = 0;
    while (fields->rowCount()) {
        auto row = fields->takeRow(0);
        if (row.labelItem) {
            if (auto *label = qobject_cast<QLabel *>(row.labelItem->widget()))
                label->setProperty("role", "muted");
            grid->addItem(row.labelItem, (index / 2) * 2, index % 2);
        }
        if (row.fieldItem)
            grid->addItem(row.fieldItem, (index / 2) * 2 + 1, index % 2);
        ++index;
    }
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    parent->removeItem(fields);
    delete fields;
    parent->addLayout(grid);
}
QString bytesLabel(qint64 value) {
    // Binary units match the repository's 1 MiB block definition. Formatting
    // rounds only the displayed value; manifests retain exact byte counts.
    // Large files therefore do not lose size precision in backup/restore.
    const QStringList units{"B", "KiB", "MiB", "GiB", "TiB"};
    double number = double(value);
    int i = 0;
    while (number >= 1024 && i < 4) {
        number /= 1024;
        ++i;
    }
    return QString::number(number, 'f', i ? 2 : 0) + ' ' + units[i];
}
QStringList splitList(const QString &text) {
    // The UI accepts newline or semicolon separated lists. Empty tokens are
    // omitted so a trailing separator cannot create an unintended empty path.
    // Quoting separators inside a path is not implemented by this editor;
    // callers needing unusual names can provide a JSON plan through the CLI.
    QStringList result;
    for (auto item : text.split(QRegularExpression("[;,\\n]"), Qt::SkipEmptyParts)) {
        item = item.trimmed();
        if (!item.isEmpty())
            result << item;
    }
    return result;
}
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    cvui::installApplicationStyle();
    // Page factories execute before the footer is constructed. Their slots
    // are connected now but cannot run until the application event loop starts.
    // Navigation IDs are stable and also used by GUI automation screenshots.
    // The stacked pages retain entered values when switching destinations.
    setWindowTitle("澄匣备份 ChengVault");
    const auto available = QGuiApplication::primaryScreen()->availableGeometry().size();
    resize(qMax(1040, qMin(1280, available.width() - 48)), qMax(720, qMin(860, available.height() - 60)));
    setMinimumSize(1040, 720);
    auto *container = new QWidget;
    auto *horizontal = new QHBoxLayout(container);
    horizontal->setContentsMargins(0, 0, 0, 0);
    horizontal->setSpacing(0);
    auto *navigation = new QFrame;
    navigation->setObjectName("navigation");
    navigation->setFixedWidth(208);
    auto *navLayout = new QVBoxLayout(navigation);
    navLayout->setContentsMargins(16, 26, 16, 22);
    navLayout->setSpacing(9);
    auto *brandRow = new QHBoxLayout;
    brandRow->setContentsMargins(8, 0, 0, 0);
    brandRow->setSpacing(11);
    brandRow->addWidget(cvui::brand());
    auto *brandText = new QVBoxLayout;
    brandText->setSpacing(3);
    brandText->addWidget(titleLabel("澄匣备份", "brand"));
    brandText->addWidget(titleLabel("CHENGVAULT", "brandSub"));
    brandRow->addLayout(brandText);
    brandRow->addStretch();
    navLayout->addLayout(brandRow);
    navLayout->addSpacing(32);
    navLayout->addWidget(titleLabel("工作空间", "navGroup"));
    const QStringList names{"总览",     "创建备份",   "浏览与恢复", "自动任务",
                            "远程仓库", "检查与修复", "操作日志",   "设置与帮助"};
    auto *group = new QButtonGroup(this);
    group->setObjectName("navigationGroup");
    group->setExclusive(true);
    for (int index = 0; index < names.size(); ++index) {
        if (index == 6) {
            navLayout->addSpacing(20);
            navLayout->addWidget(titleLabel("偏好与记录", "navGroup"));
        }
        auto *item = button(names[index]);
        item->setIcon(cvui::icon(static_cast<cvui::Icon>(index), QColor("#aabeb0")));
        item->setIconSize(QSize(19, 19));
        item->setProperty("nav", true);
        item->setCheckable(true);
        if (!index)
            item->setChecked(true);
        group->addButton(item, index);
        navLayout->addWidget(item);
    }
    navLayout->addStretch();
    navLayout->addWidget(titleLabel("让每次保存，都有备而来。", "navFooter"));
    navLayout->addSpacing(6);
    navLayout->addWidget(titleLabel("WINDOWS  /  NTFS", "brandSub"));
    horizontal->addWidget(navigation);
    auto *right = new QWidget;
    auto *vertical = new QVBoxLayout(right);
    vertical->setContentsMargins(0, 0, 0, 0);
    vertical->setSpacing(0);
    auto *header = new QWidget;
    header->setObjectName("pageHeader");
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(28, 22, 28, 20);
    auto *heading = new QVBoxLayout;
    heading->setSpacing(6);
    heading->addWidget(titleLabel("WORKSPACE", "eyebrow"));
    pageTitle_ = titleLabel(names[0], "pageTitle");
    heading->addWidget(pageTitle_);
    pageSubtitle_ = titleLabel("从一份可靠的备份开始。", "muted");
    heading->addWidget(pageSubtitle_);
    headerLayout->addLayout(heading, 1);
    headerLayout->addStretch();
    auto *version = titleLabel("本地优先  /  v1.0", "badge");
    headerLayout->addWidget(version, 0, Qt::AlignVCenter);
    vertical->addWidget(header);
    pages_ = new QStackedWidget;
    // Each page scrolls independently. A short screen must not make recovery
    // controls inaccessible or require a second implementation of the page.
    // setWidgetResizable lets layouts adapt while preserving minimum controls.
    for (auto *page : {overviewPage(), backupPage(), restorePage(), automationPage(), networkPage(),
                       repairPage(), logPage(), settingsPage()}) {
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        pages_->addWidget(scroll);
    }
    vertical->addWidget(pages_, 1);
    auto *footer = new QWidget;
    footer->setObjectName("taskFooter");
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(28, 14, 28, 14);
    status_ = new StatusLabel("就绪");
    status_->setProperty("role", "muted");
    progress_ = new QProgressBar;
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->setFixedWidth(180);
    progress_->setTextVisible(false);
    status_->setMinimumWidth(0);
    status_->setMaximumHeight(40);
    auto *cancel = button("取消当前任务");
    cancel_ = cancel;
    cancel->setEnabled(false);
    connect(cancel, &QPushButton::clicked, this, [this] {
        // Cancellation is cooperative: setting the flag does not kill a thread
        // during a file write. Core operations observe it at safe boundaries
        // and discard their uncommitted temporary results on failure.
        if (work_.isRunning()) {
            cancelled_ = true;
            status_->setText("正在安全取消…");
        }
    });
    footerLayout->addWidget(status_, 1);
    // The main backup action stays reachable while a long preview scrolls.
    // It is the same button and slot as the form, not a duplicate command.
    footerLayout->addWidget(backupStart_);
    backupStart_->setVisible(false);
    footerLayout->addWidget(progress_);
    footerLayout->addWidget(cancel);
    vertical->addWidget(footer);
    horizontal->addWidget(right, 1);
    setCentralWidget(container);
    connect(group, &QButtonGroup::idClicked, this, [this, names](int id) {
        selectPage(id);
        pageTitle_->setText(names[id]);
    });
    connect(&work_, &QFutureWatcher<QJsonObject>::finished, this, [this] {
        // The watcher delivers completion on the GUI thread. Workers return
        // error data, so this slot never relies on a cross-thread exception.
        // A completion callback runs only for successful operations; stale
        // selections are not replaced with an incomplete manifest after error.
        cancel_->setEnabled(false);
        const auto result = work_.result();
        if (result.contains("_error")) {
            status_->setText("任务未完成");
            addLog(result.value("_error").toString());
            QMessageBox::warning(this, "操作未完成", result.value("_error").toString());
        } else {
            progress_->setValue(100);
            status_->setText("操作完成");
            addLog(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented)));
            if (completion_)
                completion_(result);
        }
        completion_ = {};
        refreshJobs();
    });
    setStyleSheet(cvui::styleSheet());
    refreshJobs();
}
MainWindow::~MainWindow() {
    // Worker closures reference this window's cancellation flag and queued
    // progress target. Wait before those members are destroyed; deleting the
    // window cannot leave a detached worker writing into freed GUI memory.
    // This waits only for a foreground task, not the independent agent process.
    cancelled_ = true;
    work_.waitForFinished();
}
void MainWindow::selectPage(int index) {
    // One navigation operation updates content, heading and checked button.
    // Tests and overview actions share it with user navigation to avoid a
    // visually selected tab disagreeing with the page actually displayed.
    // Invalid indices are ignored because they have no meaningful destination.
    const QStringList titles{"总览",     "创建备份",   "浏览与恢复", "自动任务",
                             "远程仓库", "检查与修复", "操作日志",   "设置与帮助"};
    if (index < 0 || index >= titles.size())
        return;
    const QStringList descriptions{"从一份可靠的备份开始。",
                                   "选择重要文件，按你的方式妥善保存。",
                                   "查看备份内容，把所需文件带回身边。",
                                   "设定一次，让备份持续发生。",
                                   "将加密仓库同步到你的另一台设备。",
                                   "检查归档完整性，使用冗余数据修复损坏。",
                                   "查看每次操作的结果与详细记录。",
                                   "管理恢复偏好、后台行为与使用帮助。"};
    pageSubtitle_->setText(descriptions[index]);
    pages_->setCurrentIndex(index);
    backupStart_->setVisible(index == 1);
    pageTitle_->setText(titles[index]);
    if (auto *group = findChild<QButtonGroup *>("navigationGroup"))
        if (auto *b = group->button(index))
            b->setChecked(true);
}
QLineEdit *MainWindow::input(const QString &key, const QString &placeholder, bool secret) {
    // Stable object names serve automation and the internal control registry.
    // Secret echo mode masks display only: it is not secure memory or storage.
    // Passwords are copied to a task at launch and are never logged here.
    auto *edit = new QLineEdit;
    edit->setObjectName(key);
    edit->setPlaceholderText(placeholder);
    if (secret)
        edit->setEchoMode(QLineEdit::Password);
    lines_[key] = edit;
    return edit;
}
QComboBox *MainWindow::choice(const QString &key, const QList<QPair<QString, QString>> &values) {
    // Human-readable labels may change language without changing protocol IDs.
    // Always dispatch using currentData(), never a translated visible label.
    // This prevents GUI choices from diverging from the CLI's canonical names.
    auto *box = cvui::comboBox();
    box->setObjectName(key);
    for (const auto &[label, value] : values)
        box->addItem(label, value);
    choices_[key] = box;
    return box;
}
QWidget *MainWindow::pathRow(const QString &key, const QString &placeholder, bool directory, bool save) {
    // A repository is a directory while an independent archive is a file.
    // Reevaluate the mode at click time because users can change it after the
    // widget is created. Cancelling a dialog preserves the previous value.
    // Choosing a path does not grant overwrite permission; core restore checks
    // that separate option again when it commits each verified output.
    auto *widget = new QWidget;
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *edit = input(key, placeholder);
    auto *browse = button("浏览…");
    layout->addWidget(edit, 1);
    layout->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [=, this] {
        const bool isDirectory =
            directory ||
            (key == "backup.output" && choices_["backup.storage"]->currentData() == "repository") ||
            (key == "restore.input" && choices_["restore.mode"]->currentData() == "repository");
        QString path = isDirectory ? QFileDialog::getExistingDirectory(this, "选择目录", edit->text())
                       : save      ? QFileDialog::getSaveFileName(this, "选择保存文件", edit->text())
                                   : QFileDialog::getOpenFileName(this, "选择文件", edit->text());
        if (!path.isEmpty())
            edit->setText(QDir::toNativeSeparators(path));
    });
    return widget;
}
void MainWindow::addLog(const QString &text) {
    // Logs contain operation names, structured results and reported failures.
    // Calling code must not pass a plan/password/options dump into this method.
    // A best-effort log append cannot turn a successful data operation into a
    // failed one; the on-screen entry remains visible if disk logging fails.
    const QString entry = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") + "  " + text;
    if (log_)
        log_->appendPlainText(entry);
    QFile file(cv::appDataDirectory() + "/gui.log");
    if (file.open(QIODevice::Append | QIODevice::WriteOnly))
        file.write(entry.toUtf8() + '\n');
}
void MainWindow::runTask(const QString &title, std::function<QJsonObject(const cv::Context &)> fn,
                         std::function<void(const QJsonObject &)> done) {
    // Only one foreground operation owns the progress bar and completion hook.
    // A second button click cannot overwrite the callback of a running task.
    // Slots capture immutable input values before launch; worker functions must
    // never dereference QLineEdit/QTreeWidget controls from a pool thread.
    // Repository/file locks enforce cross-process exclusion independently.
    // The UI rule does not claim to serialize the independent background agent.
    if (work_.isRunning()) {
        QMessageBox::information(this, "任务执行中", "请等待当前操作完成，或先取消它。");
        return;
    }
    cancelled_ = false;
    cancel_->setEnabled(true);
    completion_ = std::move(done);
    status_->setText(title);
    progress_->setValue(0);
    addLog(title);
    work_.setFuture(QtConcurrent::run([this, fn = std::move(fn)] {
        cv::Context context;
        context.cancelled = &cancelled_;
        context.progress = [this](quint64 done, quint64 total, const QString &text) {
            // A queued invocation establishes the Qt thread-affinity boundary.
            // Numeric progress is optional when a scan has no known total yet.
            // Floating conversion avoids integer overflow on multi-GiB totals.
            // The context object is local to the worker, so no reference escapes.
            QMetaObject::invokeMethod(
                this,
                [this, done, total, text] {
                    status_->setText(text);
                    status_->setToolTip(text);
                    if (total)
                        progress_->setValue(int(qMin(100.0, 100.0 * double(done) / double(total))));
                },
                Qt::QueuedConnection);
        };
        try {
            return fn(context);
        } catch (const cv::Error &e) {
            return QJsonObject{{"_error", e.code + ": " + QString::fromUtf8(e.what())}};
        } catch (const std::exception &e) {
            return QJsonObject{{"_error", QString::fromUtf8(e.what())}};
        }
    }));
}
QWidget *MainWindow::overviewPage() {
    // The overview offers real destinations, never fabricated backup counts.
    // Its illustration is decorative; every action uses the same page state
    // and core workflow as navigation on the left.
    QVBoxLayout *layout;
    auto *page = pageBody(layout);
    auto *hero = new QFrame;
    hero->setProperty("role", "heroPanel");
    auto *heroLayout = new QHBoxLayout(hero);
    heroLayout->setContentsMargins(24, 20, 20, 20);
    auto *heroText = new QVBoxLayout;
    heroText->setSpacing(10);
    heroText->addWidget(titleLabel("A LITTLE CARE. A LASTING COPY.", "eyebrow"));
    heroText->addWidget(titleLabel("重要文件，安心有备。", "hero"));
    heroText->addWidget(
        titleLabel("保存文件、目录与属性。需要时，\n从容找到那一份，验证后完整恢复。", "muted"));
    heroText->addSpacing(6);
    auto *actions = new QHBoxLayout;
    auto *create = button("创建一份备份", true);
    create->setIcon(cvui::icon(cvui::Icon::Backup, Qt::white));
    auto *restore = button("浏览已有备份");
    actions->addWidget(create);
    actions->addWidget(restore);
    actions->addStretch();
    heroText->addLayout(actions);
    heroLayout->addLayout(heroText, 3);
    heroLayout->addWidget(cvui::illustration(), 2);
    layout->addWidget(hero);
    connect(create, &QPushButton::clicked, this, [this] { selectPage(1); });
    connect(restore, &QPushButton::clicked, this, [this] { selectPage(2); });
    auto *section = new QHBoxLayout;
    section->addWidget(titleLabel("你的备份工作台"));
    section->addStretch();
    section->addWidget(titleLabel("选择接下来要做的事", "muted"));
    layout->addLayout(section);
    auto *grid = new QGridLayout;
    grid->setSpacing(14);
    struct Destination {
        QString title;
        QString description;
        cvui::Icon icon = cvui::Icon::Overview;
        int page = 0;
    };
    const QList<Destination> destinations{
        {"保存一份新备份", "单文件归档，或持续保留多个版本。筛选所需内容，选择压缩与密码保护。",
         cvui::Icon::Backup, 1},
        {"找到并恢复文件", "先浏览文件与历史版本，再将所选内容恢复到新的目录。", cvui::Icon::Restore, 2},
        {"安排自动备份", "按时间或文件变化自动执行。关闭窗口后，后台任务仍可继续。", cvui::Icon::Clock, 3},
        {"检查与修复归档", "检查完整性，并在冗余数据允许的范围内修复受损归档。", cvui::Icon::Repair, 5}};
    for (int i = 0; i < destinations.size(); ++i) {
        const auto destination = destinations[i];
        auto *action = cvui::actionCard(destination.title, destination.description, destination.icon);
        connect(action, &QPushButton::clicked, this, [this, destination] { selectPage(destination.page); });
        grid->addWidget(action, i / 2, i % 2);
    }
    layout->addLayout(grid);
    auto *note = new QFrame;
    note->setProperty("role", "note");
    auto *noteLayout = new QHBoxLayout(note);
    noteLayout->setContentsMargins(17, 14, 17, 14);
    auto *noteIcon = new QLabel;
    noteIcon->setPixmap(cvui::icon(cvui::Icon::Shield).pixmap(22, 22));
    noteLayout->addWidget(noteIcon);
    noteLayout->addWidget(
        titleLabel("第一次使用？创建备份后，建议在新目录恢复一次，确认文件完整可用。", "muted"), 1);
    layout->addWidget(note);
    layout->addStretch();
    return page;
}
cv::BackupPlan MainWindow::plan() const {
    // Read all GUI fields on the UI thread and return an owning value snapshot.
    // Edits made after a worker starts apply to the next operation only.
    // filters_ already contains typed JSON produced by the filter dialog.
    // The filesystem module still validates rules and source/destination overlap;
    // a preview does not authorize a different source tree later at execution.
    cv::BackupPlan result;
    result.sources = splitList(sources_->toPlainText());
    result.destination = lines_["backup.output"]->text();
    result.format = choices_["backup.format"]->currentData().toString();
    result.compression = choices_["backup.compression"]->currentData().toString();
    result.cipher = choices_["backup.cipher"]->currentData().toString();
    result.password = lines_["backup.password"]->text();
    result.filters = filters_;
    result.strictMetadata = checks_["strict"]->isChecked();
    return result;
}
QWidget *MainWindow::backupPage() {
    // Independent archives and repositories share capture/filter controls.
    // Storage-specific behavior is dispatched explicitly below; repository
    // chunks and full version manifests are not represented as a fake archive.
    // Algorithm labels refer to local core implementations, not external tools.
    QVBoxLayout *layout;
    auto *page = pageBody(layout);
    auto *root = layout;
    layout = cvui::card(root, "01  选择备份内容", "添加文件或目录，再通过筛选与预览确认本次备份范围。");
    sources_ = new QPlainTextEdit;
    sources_->setObjectName("backup.sources");
    sources_->setPlaceholderText("每行一个文件或目录路径");
    sources_->setFixedHeight(76);
    layout->addWidget(sources_);
    auto *sourceButtons = new QHBoxLayout;
    auto *addDirectory = button("添加目录");
    auto *addFile = button("添加文件");
    auto *filters = button("筛选规则…");
    auto *preview = button("预览文件");
    for (auto *item : {addDirectory, addFile, filters, preview})
        sourceButtons->addWidget(item);
    sourceButtons->addStretch();
    layout->addLayout(sourceButtons);
    connect(addDirectory, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getExistingDirectory(this, "选择备份目录");
        if (!path.isEmpty())
            sources_->appendPlainText(path);
    });
    connect(addFile, &QPushButton::clicked, this, [this] {
        for (const auto &path : QFileDialog::getOpenFileNames(this, "选择备份文件"))
            sources_->appendPlainText(path);
    });
    connect(filters, &QPushButton::clicked, this, &MainWindow::editFilters);
    connect(preview, &QPushButton::clicked, this, [this] {
        // Preview uses the real filesystem matcher and explains exclusions.
        // It runs in the same task boundary as backup, keeping a large scan
        // responsive without mutating an archive or creating a repository.
        const auto p = plan();
        runTask(
            "扫描筛选结果",
            [p](const cv::Context &c) { return QJsonObject{{"entries", cv::fs::preview(p, c)}}; },
            [this](const QJsonObject &o) { showEntries(o["entries"].toArray()); });
    });
    layout = cvui::card(root, "02  保存与保护");
    auto *settings = form(layout);
    settings->addRow("保存方式",
                     choice("backup.storage", {{"单文件归档", "archive"}, {"增量版本仓库", "repository"}}));
    settings->addRow("保存位置", pathRow("backup.output", "归档文件路径，或仓库目录", false, true));
    settings->addRow("归档格式",
                     choice("backup.format", {{"索引归档", "indexed"}, {"PAX / TAR 归档", "pax"}}));
    settings->addRow(
        "压缩", choice("backup.compression", {{"LZ77", "lz77"}, {"Huffman", "huffman"}, {"不压缩", "none"}}));
    settings->addRow("加密", choice("backup.cipher", {{"AES-256 + 认证校验", "aes256"},
                                                      {"ChaCha20 + 认证校验", "chacha20"},
                                                      {"不加密（仅独立归档）", "none"}}));
    settings->addRow("备份密码", input("backup.password", "加密备份及仓库需输入密码", true));
    twoColumns(settings, layout);
    auto *redundancy = new QCheckBox("同时生成 .cvr 纠错文件（独立归档）");
    // Redundancy is opt-in because padded final stripes can cost much more
    // than 20% for small archives. It is generated over final encrypted bytes,
    // so repair never requires or exposes the archive plaintext password.
    checks_["backup.repair"] = redundancy;
    layout->addWidget(redundancy);
    layout->addWidget(titleLabel("纠错文件完整分组增加约20%数据量，小文件和末组比例更高。", "muted"));
    auto *start = button("开始备份", true);
    start->setIcon(cvui::icon(cvui::Icon::Backup, Qt::white));
    backupStart_ = start;
    layout = cvui::card(root, "文件预览", "扫描来源后，这里会显示命中规则的文件与筛选原因。");
    entries_ = new QTreeWidget;
    entries_->setHeaderLabels({"路径", "类型", "大小", "是否备份 / 原因"});
    entries_->setRootIsDecorated(false);
    entries_->setAlternatingRowColors(true);
    entries_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    cvui::styleTable(entries_, "还没有预览文件", "添加来源后，点击“预览文件”查看备份范围。");
    layout->addWidget(entries_, 1);
    connect(start, &QPushButton::clicked, this, [this] {
        // Freeze the storage and repair choices with the plan before launching.
        // The sidecar follows successful archive commit; if sidecar creation
        // fails, the already valid archive remains and the error is displayed.
        // A repository backup ignores the standalone sidecar checkbox because
        // its content consists of many blocks rather than one archive file.
        const auto p = plan();
        const bool repo = choices_["backup.storage"]->currentData().toString() == "repository";
        const bool repair = checks_["backup.repair"]->isChecked();
        if (repo && p.password.isEmpty()) {
            QMessageBox::warning(this, "仓库密码不能为空", "仓库模式需要非空密码。");
            return;
        }
        runTask(
            "正在创建备份",
            [p, repo, repair](const cv::Context &c) {
                cv::TaskResult result;
                if (repo) {
                    cv::Repository repository(p.destination, p.password);
                    result = repository.backup(p, c);
                } else {
                    result = cv::archive::backup(p, c);
                    if (repair)
                        result.details["repair"] =
                            cv::repair::create(p.destination, p.destination + ".cvr", c).toJson();
                }
                return result.toJson();
            },
            [this](const QJsonObject &result) {
                QMessageBox::information(this, "备份已完成",
                                         "输出：" + result["output"].toString() +
                                             "\n请在浏览与恢复页面验证备份。");
            });
    });
    return page;
}
void MainWindow::showEntries(const QJsonArray &entries) {
    // Preview rows include rejected objects and the matcher's explanation.
    // Keeping them visible helps distinguish an empty source from rules that
    // intentionally exclude every file; no second GUI-side filter is applied.
    // The tree is replaced only by a successful completed scan callback.
    entries_->clear();
    for (const auto &value : entries) {
        const auto entry = value.toObject();
        auto *item = new QTreeWidgetItem(
            entries_,
            {entry["path"].toString(), entry["type"].toString(), bytesLabel(entry["size"].toInteger()),
             (entry["selected"].toBool() ? "包含：" : "排除：") + entry["reason"].toString()});
        item->setToolTip(0, entry["metadata"].toObject()["ownerSid"].toString());
    }
}
void MainWindow::editFilters() {
    // Edit a local draft and publish it only when the modal dialog is accepted.
    // Cancel leaves the active plan unchanged, including its comparison manifest.
    // Arrays, numeric bounds and scalar strings retain distinct JSON types;
    // the core matcher owns semantics such as exclusion priority and conjunction.
    // This editor is not a second independent implementation of filtering.
    QDialog dialog(this);
    dialog.setWindowTitle("自定义备份筛选");
    dialog.resize(qMin(760, width() - 60), qMin(650, height() - 60));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(22, 20, 22, 18);
    layout->setSpacing(16);
    layout->addWidget(titleLabel("只备份你需要的内容"));
    layout->addWidget(titleLabel("排除规则优先；不同条件需同时满足。留空表示不限制。", "muted"));
    auto *scroll = new QScrollArea;
    scroll->setObjectName("filterFieldsScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget;
    content->setProperty("role", "pageBody");
    auto *body = new QVBoxLayout(content);
    body->setContentsMargins(2, 2, 12, 12);
    auto *fields = form(body);
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    QHash<QString, QLineEdit *> edits;
    const QList<QPair<QString, QString>> stringFields{{"pathInclude", "包含路径"},
                                                      {"pathExclude", "排除路径"},
                                                      {"nameInclude", "包含文件名"},
                                                      {"nameExclude", "排除文件名"},
                                                      {"extensions", "扩展名"},
                                                      {"types", "对象类型"},
                                                      {"owners", "属主账户或 SID"},
                                                      {"timeFrom", "起始时间 ISO 8601"},
                                                      {"timeTo", "结束时间 ISO 8601"},
                                                      {"minSize", "最小字节数"},
                                                      {"maxSize", "最大字节数"},
                                                      {"maxDepth", "最大递归深度"},
                                                      {"attributesAll", "必须具有属性位掩码"},
                                                      {"attributesNone", "排除属性位掩码"},
                                                      {"contentContains", "文本内容包含"}};
    const QStringList arrays{"pathInclude", "pathExclude", "nameInclude", "nameExclude",
                             "extensions",  "types",       "owners"};
    const QStringList numbers{"minSize", "maxSize", "maxDepth", "attributesAll", "attributesNone"};
    for (const auto &[key, label] : stringFields) {
        auto *edit = new QLineEdit;
        edit->setObjectName("filter." + key);
        if (key == "types")
            edit->setPlaceholderText("file; directory; symlink; junction; hardlink");
        edits[key] = edit;
        if (arrays.contains(key)) {
            QStringList values;
            for (const auto &item : filters_[key].toArray())
                values << item.toString();
            edit->setText(values.join(';'));
        } else if (filters_.contains(key))
            edit->setText(filters_[key].isDouble() ? QString::number(filters_[key].toInteger())
                                                   : filters_[key].toString());
        fields->addRow(label, edit);
    }
    auto *timeField = cvui::comboBox();
    timeField->addItem("修改时间", "modified");
    timeField->addItem("创建时间", "created");
    timeField->addItem("访问时间", "accessed");
    timeField->setCurrentIndex(qMax(0, timeField->findData(filters_["timeField"].toString("modified"))));
    fields->addRow("时间字段", timeField);
    auto *baseline = button("选择变化比较清单 JSON…");
    fields->addRow("变化筛选", baseline);
    twoColumns(fields, body);
    QJsonObject prior = filters_["changedSinceManifest"].toObject();
    // The comparison baseline is copied into the draft, not retained as a path
    // to a file which may change between preview and backup. Reading an invalid
    // JSON object displays an error and keeps the previously valid baseline.
    connect(baseline, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, "选择快照清单", {}, "JSON (*.json)");
        if (!path.isEmpty())
            try {
                prior = cv::readJson(path);
                baseline->setText(QFileInfo(path).fileName());
            } catch (const std::exception &e) {
                QMessageBox::warning(&dialog, "无法读取", e.what());
            }
    });
    layout->addWidget(
        titleLabel("列表用分号分隔，路径和名字支持 * 与 ?。排除优先，各条件同时满足。\n常用属性：只读 "
                   "1，隐藏 2，系统 4，归档 32。留空表示不限制。",
                   "muted"));
    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Reset);
    buttons->button(QDialogButtonBox::Ok)->setText("应用规则");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    buttons->button(QDialogButtonBox::Reset)->setText("重置");
    buttons->button(QDialogButtonBox::Ok)->setProperty("primary", true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, &dialog, [&] {
        for (auto *edit : edits)
            edit->clear();
        prior = {};
    });
    if (dialog.exec() != QDialog::Accepted)
        return;
    // Build the complete replacement first. An invalid integer aborts before
    // assigning filters_, so partially accepted controls never leak into a plan.
    // Empty fields are omitted instead of interpreted as an accidental zero.
    QJsonObject result;
    result["timeField"] = timeField->currentData().toString();
    for (auto it = edits.begin(); it != edits.end(); ++it) {
        const QString text = it.value()->text().trimmed();
        if (text.isEmpty())
            continue;
        if (arrays.contains(it.key()))
            result[it.key()] = QJsonArray::fromStringList(splitList(text));
        else if (numbers.contains(it.key())) {
            bool ok = false;
            const auto number = text.toLongLong(&ok);
            if (!ok) {
                QMessageBox::warning(this, "无效规则", it.key() + " 必须为整数");
                return;
            }
            result[it.key()] = number;
        } else
            result[it.key()] = text;
    }
    if (!prior.isEmpty())
        result["changedSinceManifest"] = prior;
    filters_ = result;
}

QWidget *MainWindow::restorePage() {
    // Browsing and full verification are separate actions: a listed snapshot
    // does not imply every payload block has already been checked. Restore
    // performs its own authentication and integrity checks before publishing.
    // The destination is selected independently from the archive/repository.
    QVBoxLayout *layout;
    auto *page = pageBody(layout);
    auto *root = layout;
    layout = cvui::card(root, "01  打开一份备份", "归档与版本仓库都可以在这里读取、校验和恢复。");
    auto *fields = form(layout);
    fields->addRow("备份类型",
                   choice("restore.mode", {{"单文件归档", "archive"}, {"增量版本仓库", "repository"}}));
    fields->addRow("备份位置", pathRow("restore.input", "归档文件或仓库目录", false));
    fields->addRow("备份密码", input("restore.password", "加密时需要正确密码", true));
    auto *actions = new QHBoxLayout;
    auto *browse = button("读取备份", true);
    auto *verify = button("完整校验");
    auto *exportManifest = button("导出清单 JSON");
    auto *compare = button("比较文件哈希");
    actions->addWidget(browse);
    actions->addWidget(verify);
    actions->addWidget(exportManifest);
    actions->addWidget(compare);
    actions->addStretch();
    layout->addLayout(actions);
    connect(compare, &QPushButton::clicked, this, [this] {
        const auto source = QFileDialog::getOpenFileName(this, "选择源文件");
        if (source.isEmpty())
            return;
        const auto restored = QFileDialog::getOpenFileName(this, "选择恢复文件");
        if (!restored.isEmpty())
            compareFiles(source, restored);
    });
    layout = cvui::card(root, "02  选择需要恢复的文件");
    snapshots_ = new QTreeWidget;
    snapshots_->setHeaderLabels({"快照编号", "时间", "文件数"});
    snapshots_->setFixedHeight(108);
    snapshots_->setVisible(false);
    connect(choices_["restore.mode"], &QComboBox::currentIndexChanged, this,
            [this] { snapshots_->setVisible(choices_["restore.mode"]->currentData() == "repository"); });
    snapshots_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(snapshots_);
    restoreEntries_ = new QTreeWidget;
    restoreEntries_->setHeaderLabels({"选择 / 路径", "类型", "逻辑大小"});
    restoreEntries_->setRootIsDecorated(false);
    restoreEntries_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    restoreEntries_->setMinimumHeight(140);
    cvui::styleTable(restoreEntries_, "等待打开备份", "读取备份后，可在这里勾选需要恢复的文件。");
    restoreEntries_->setMinimumHeight(185);
    layout->addWidget(restoreEntries_, 1);
    auto display = [this](const QJsonObject &manifest) {
        // Only an authenticated/parsed manifest returned by core reaches here.
        // Checkboxes describe relative manifest paths, not absolute disk paths.
        // Core restore revalidates every selected path against traversal and
        // link escape; a widget row is not treated as a filesystem capability.
        inspected_ = manifest;
        restoreEntries_->clear();
        for (const auto &value : manifest["entries"].toArray()) {
            const auto entry = value.toObject();
            auto *item =
                new QTreeWidgetItem(restoreEntries_, {entry["path"].toString(), entry["type"].toString(),
                                                      bytesLabel(entry["size"].toInteger())});
            item->setCheckState(0, Qt::Checked);
        }
    };
    connect(browse, &QPushButton::clicked, this, [this, display] {
        const auto path = lines_["restore.input"]->text(), password = lines_["restore.password"]->text();
        const bool repo = choices_["restore.mode"]->currentData() == "repository";
        runTask(
            "读取备份清单",
            [path, password, repo](const cv::Context &context) {
                if (repo) {
                    cv::Repository r(path, password);
                    return QJsonObject{{"snapshots", r.list()}};
                }
                return QJsonObject{{"manifest", cv::archive::inspect(path, password, context)}};
            },
            [this, display](const QJsonObject &result) {
                snapshots_->clear();
                if (result.contains("manifest"))
                    display(result["manifest"].toObject());
                for (const auto &value : result["snapshots"].toArray()) {
                    const auto row = value.toObject();
                    auto *item =
                        new QTreeWidgetItem(snapshots_, {row["id"].toString(), row["createdUtc"].toString(),
                                                         QString::number(row["fileCount"].toInteger())});
                    item->setData(0, Qt::UserRole, row);
                }
                if (snapshots_->topLevelItemCount())
                    snapshots_->setCurrentItem(snapshots_->topLevelItem(0));
            });
    });
    connect(snapshots_, &QTreeWidget::currentItemChanged, this, [this, display](QTreeWidgetItem *item) {
        // A list completion selects its first row while still in the watcher
        // callback. Defer the next task to the event loop so the completed
        // operation can release its completion hook before the manifest load.
        if (!item)
            return;
        const auto id = item->text(0), path = lines_["restore.input"]->text(),
                   password = lines_["restore.password"]->text();
        QTimer::singleShot(0, this, [this, display, id, path, password] {
            runTask(
                "读取版本文件清单",
                [=](const cv::Context &) { return cv::Repository(path, password).manifest(id); }, display);
        });
    });
    connect(verify, &QPushButton::clicked, this, [this] {
        const auto path = lines_["restore.input"]->text(), password = lines_["restore.password"]->text();
        const bool repo = choices_["restore.mode"]->currentData() == "repository";
        runTask("逐块校验备份", [=](const cv::Context &c) {
            return repo ? cv::Repository(path, password).verify(c).toJson()
                        : cv::archive::verify(path, password, c).toJson();
        });
    });
    connect(exportManifest, &QPushButton::clicked, this, [this] {
        // Export is explicitly requested plaintext metadata. The dialog avoids
        // silently placing names/ACLs beside an encrypted archive. It exports
        // only the last inspected manifest and never includes the master key.
        if (inspected_.isEmpty())
            return;
        const auto file = QFileDialog::getSaveFileName(this, "保存清单", {}, "JSON (*.json)");
        if (!file.isEmpty())
            try {
                cv::writeJson(file, inspected_);
                addLog("已导出清单：" + file);
            } catch (const std::exception &e) {
                QMessageBox::warning(this, "导出失败", e.what());
            }
    });
    layout = cvui::card(root, "03  选择恢复位置");
    auto *target = form(layout);
    target->addRow("恢复目录", pathRow("restore.output", "选择新的恢复目录", true));
    auto *overwrite = new QCheckBox("允许替换同名文件（验证临时文件后才替换）");
    checks_["restore.overwrite"] = overwrite;
    layout->addWidget(overwrite);
    auto *restore = button("恢复已勾选文件", true);
    layout->addWidget(restore, 0, Qt::AlignLeft);
    connect(restore, &QPushButton::clicked, this, [this] {
        // Empty selectedPaths means all entries to the core API. When rows are
        // visible and the user unchecked every row, reject instead of turning
        // that deliberate empty selection into an unintended full restoration.
        // Overwrite and metadata strictness are captured with the selection.
        const auto path = lines_["restore.input"]->text(), password = lines_["restore.password"]->text(),
                   target = lines_["restore.output"]->text();
        const bool repo = choices_["restore.mode"]->currentData() == "repository";
        const auto id = snapshots_->currentItem() ? snapshots_->currentItem()->text(0) : QString();
        cv::RestoreOptions options;
        options.overwrite = checks_["restore.overwrite"]->isChecked();
        options.strictMetadata = checks_["strict"]->isChecked();
        for (int i = 0; i < restoreEntries_->topLevelItemCount(); ++i)
            if (restoreEntries_->topLevelItem(i)->checkState(0) == Qt::Checked)
                options.selectedPaths << restoreEntries_->topLevelItem(i)->text(0);
        if (restoreEntries_->topLevelItemCount() && !options.selectedPaths.size()) {
            QMessageBox::information(this, "未选择文件", "请至少勾选一个文件或目录。");
            return;
        }
        runTask("正在恢复数据", [=](const cv::Context &c) {
            return repo ? cv::Repository(path, password).restore(id, target, options, c).toJson()
                        : cv::archive::restore(path, target, password, options, c).toJson();
        });
    });
    return page;
}

QWidget *MainWindow::automationPage() {
    // A saved job references a version repository and receives a DPAPI-wrapped
    // repository key. Its JSON does not persist the user's plaintext password.
    // This page configures the independent agent; a QTimer in the GUI is not
    // used to claim that scheduling continues after the window has closed.
    QVBoxLayout *layout;
    auto *page = pageBody(layout);
    auto *root = layout;
    layout = cvui::card(root, "新建自动任务");
    layout->addWidget(titleLabel(
        "沿用“创建备份”页的来源、筛选和密码，保存位置必须为版本仓库目录。后台进程可在关闭界面后继续运行。",
        "muted"));
    auto *fields = form(layout);
    fields->addRow("任务名称", input("job.name", "例如：课程资料每日备份"));
    fields->addRow("运行方式", choice("job.mode", {{"固定间隔", "interval"},
                                                   {"每天", "daily"},
                                                   {"每周", "weekly"},
                                                   {"实时感知变化", "realtime"}}));
    fields->addRow("间隔秒数", input("job.interval", "3600"));
    lines_["job.interval"]->setText("3600");
    fields->addRow("每日 / 每周时间", input("job.time", "HH:mm"));
    lines_["job.time"]->setText("02:00");
    fields->addRow("星期", choice("job.weekday", {{"星期一", "1"},
                                                  {"星期二", "2"},
                                                  {"星期三", "3"},
                                                  {"星期四", "4"},
                                                  {"星期五", "5"},
                                                  {"星期六", "6"},
                                                  {"星期日", "7"}}));
    fields->addRow("保留版本数", input("job.keepCount", "10"));
    lines_["job.keepCount"]->setText("10");
    fields->addRow("保留天数", input("job.keepDays", "0 表示不限"));
    lines_["job.keepDays"]->setText("0");
    twoColumns(fields, layout);
    auto *actions = new QHBoxLayout;
    auto *save = button("创建并启动任务", true);
    auto *refresh = button("刷新任务");
    auto *enable = button("启用 / 暂停");
    auto *remove = button("删除任务");
    for (auto *b : {save, refresh, enable, remove})
        actions->addWidget(b);
    layout->addLayout(actions);
    layout = cvui::card(root, "已保存的任务", "任务状态来自后台进程，刷新即可查看最新结果。");
    jobs_ = new QTreeWidget;
    jobs_->setHeaderLabels({"任务", "模式", "状态", "下次运行", "最近错误"});
    jobs_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    jobs_->setMinimumHeight(190);
    cvui::styleTable(jobs_, "还没有自动任务", "先完成上方设置，创建你的第一个自动备份任务。");
    layout->addWidget(jobs_, 1);
    connect(save, &QPushButton::clicked, this, [this] {
        // Snapshot settings before requesting password-derived repository access.
        // At least one successful version must survive retention. Schedule
        // validation happens in saveJob before the configuration is committed.
        const auto p = plan();
        QJsonObject job{{"name", lines_["job.name"]->text()},
                        {"mode", choices_["job.mode"]->currentData().toString()},
                        {"intervalSeconds", lines_["job.interval"]->text().toInt()},
                        {"time", lines_["job.time"]->text()},
                        {"weekday", choices_["job.weekday"]->currentData().toString().toInt()},
                        {"keepCount", lines_["job.keepCount"]->text().toInt()},
                        {"keepDays", lines_["job.keepDays"]->text().toInt()}};
        runTask(
            "保存后台任务",
            [p, job](const cv::Context &) {
                cv::require(job["keepCount"].toInt() > 0, "BAD_RETENTION", "至少保留一个成功版本");
                cv::require(!p.password.isEmpty(), "REPOSITORY_PASSWORD_EMPTY", "仓库密码不能为空");
                cv::Repository repo(p.destination, p.password);
                if (repo.masterKey().isEmpty())
                    repo.init(p.cipher == "none" ? "chacha20" : p.cipher);
                return cv::schedule::saveJob(job, p, repo.masterKey());
            },
            [this](const QJsonObject &) {
                startAgent();
                refreshJobs();
            });
    });
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshJobs);
    connect(enable, &QPushButton::clicked, this, [this] {
        if (auto *item = jobs_->currentItem())
            try {
                auto job = item->data(0, Qt::UserRole).toJsonObject();
                cv::schedule::setEnabled(job["id"].toString(), !job["enabled"].toBool());
                refreshJobs();
            } catch (const std::exception &e) {
                QMessageBox::warning(this, "任务设置失败", e.what());
            }
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (auto *item = jobs_->currentItem())
            try {
                cv::schedule::removeJob(item->data(0, Qt::UserRole).toJsonObject()["id"].toString());
                refreshJobs();
            } catch (const std::exception &e) {
                QMessageBox::warning(this, "任务删除失败", e.what());
            }
    });
    return page;
}
void MainWindow::refreshJobs() {
    // The agent owns persisted run state. Refresh reads its current JSON files
    // instead of inferring success from elapsed time or the previous GUI state.
    // Keep the full job object in UserRole for enable/remove ID lookup; only
    // the nonsecret summary columns are displayed in the task table.
    if (!jobs_)
        return;
    jobs_->clear();
    for (const auto &value : cv::schedule::listJobs()) {
        const auto job = value.toObject();
        auto *item =
            new QTreeWidgetItem(jobs_, {job["name"].toString(job["id"].toString()), job["mode"].toString(),
                                        job["enabled"].toBool() ? job["state"].toString() : "已暂停",
                                        job["nextRun"].toString(), job["error"].toString()});
        item->setData(0, Qt::UserRole, job);
    }
}
void MainWindow::startAgent() {
    // A detached process outlives this window. CREATE_NO_WINDOW prevents a
    // console flash; it does not hide failures, which appear in agent logs.
    // The agent's per-user lock handles duplicate launches from other windows
    // or login startup. This function never starts a second scheduler thread.
    // A launch failure leaves the saved task intact for a later retry.
    QProcess process;
    process.setProgram(QCoreApplication::applicationDirPath() + "/ChengVaultAgent.exe");
    process.setArguments({"--background"});
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
    if (!process.startDetached())
        QMessageBox::warning(this, "后台启动失败",
                             "请检查 ChengVaultAgent.exe 是否存在。此次任务配置已经保存。");
}

cv::network::RemoteOptions MainWindow::remoteOptions() const {
    // Login credentials authenticate a server account; the archive password is
    // a different secret and never belongs in these connection parameters.
    // The device key is a prepaired 32-byte secret decoded from visible hex.
    // Core network validation rejects malformed options before session setup.
    cv::network::RemoteOptions result;
    result.host = lines_["remote.host"]->text();
    result.port = quint16(lines_["remote.port"]->text().toUInt());
    result.user = lines_["remote.user"]->text();
    result.password = lines_["remote.password"]->text();
    result.deviceId = lines_["remote.device"]->text();
    result.deviceKey = QByteArray::fromHex(lines_["remote.key"]->text().toLatin1());
    return result;
}
QWidget *MainWindow::networkPage() {
    // Each action creates its own authenticated client inside the task worker.
    // No QTcpSocket created on the GUI thread is handed across thread affinity.
    // The server lists ciphertext repository metadata; downloading does not
    // decrypt content. The user restores locally using the repository password.
    QVBoxLayout *layout;
    auto *page = pageBody(layout);
    auto *root = layout;
    layout =
        cvui::card(root, "连接你的服务器", "填写服务器账户与设备配对信息，备份密码用于下载后的本地恢复。");
    auto *fields = form(layout);
    fields->addRow("服务器", input("remote.host", "127.0.0.1"));
    lines_["remote.host"]->setText("127.0.0.1");
    fields->addRow("端口", input("remote.port", "48484"));
    lines_["remote.port"]->setText("48484");
    fields->addRow("账户", input("remote.user", "服务器账户"));
    fields->addRow("登录密码", input("remote.password", "与备份密码分别设置", true));
    fields->addRow("设备编号", input("remote.device", "desktop"));
    fields->addRow("设备配对密钥", input("remote.key", "64 个十六进制字符", true));
    twoColumns(fields, layout);
    auto *actions = new QHBoxLayout;
    auto *list = button("连接并列出仓库", true);
    auto *upload = button("上传本地仓库");
    auto *download = button("下载选中仓库");
    auto *admin = button("账户与设备管理");
    for (auto *b : {list, upload, download, admin})
        actions->addWidget(b);
    layout->addLayout(actions);
    layout = cvui::card(root, "远端仓库", "选择一个仓库下载，或上传本地仓库的新增内容。");
    remote_ = new QTreeWidget;
    remote_->setHeaderLabels({"仓库编号", "更新时间", "存储大小", "版本数"});
    remote_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    remote_->setMinimumHeight(170);
    cvui::styleTable(remote_, "尚未连接远端仓库", "完成连接后，这里会显示此账户可访问的仓库。");
    layout->addWidget(remote_, 1);
    auto *local = form(layout);
    local->addRow("本地仓库", pathRow("remote.local", "上传来源 / 下载保存目录", true));
    layout->addWidget(titleLabel(
        "上传自动复用远端已有数据块。下载完成后，在“浏览与恢复”页面输入仓库密码恢复文件。服务器仅保存密文。",
        "muted"));
    connect(list, &QPushButton::clicked, this, [this] {
        auto options = remoteOptions();
        runTask(
            "连接远程服务器",
            [options](const cv::Context &) {
                return QJsonObject{{"repositories", cv::network::Client(options).list()}};
            },
            [this](const QJsonObject &result) {
                remote_->clear();
                for (const auto &value : result["repositories"].toArray()) {
                    auto row = value.toObject();
                    new QTreeWidgetItem(remote_, {row["id"].toString(), row["updatedUtc"].toString(),
                                                  bytesLabel(row["size"].toVariant().toLongLong()),
                                                  QString::number(row["snapshots"].toInt())});
                }
            });
    });
    connect(upload, &QPushButton::clicked, this, [this] {
        // Upload operates on already encrypted repository objects. Missing-block
        // negotiation and ownership/quota checks are implemented by the network
        // layer, so the UI cannot report skipped bytes without a real response.
        auto options = remoteOptions();
        auto path = lines_["remote.local"]->text();
        runTask("上传加密数据块", [options, path](const cv::Context &c) {
            return cv::network::Client(options).upload(path, c).toJson();
        });
    });
    connect(download, &QPushButton::clicked, this, [this] {
        if (!remote_->currentItem())
            return;
        auto options = remoteOptions();
        auto path = lines_["remote.local"]->text(), id = remote_->currentItem()->text(0);
        runTask("下载远程仓库", [options, path, id](const cv::Context &c) {
            return cv::network::Client(options).download(id, path, c).toJson();
        });
    });
    connect(admin, &QPushButton::clicked, this, [this] {
        // Showing administrative controls grants no server privilege. Every
        // command is authorized again against the authenticated account role.
        // Gather edits locally; cancelling does not create or modify a user.
        // Password/key controls are masked and arguments are not written to logs.
        QDialog dialog(this);
        dialog.setWindowTitle("账户与设备管理");
        dialog.resize(qMin(640, width() - 60), qMin(620, height() - 60));
        auto *body = new QVBoxLayout(&dialog);
        body->setContentsMargins(22, 20, 22, 18);
        body->setSpacing(16);
        body->addWidget(titleLabel("账户与设备管理"));
        body->addWidget(titleLabel("操作将在确认后提交，由服务器校验当前账户的管理权限。", "muted"));
        auto *scroll = new QScrollArea;
        scroll->setObjectName("adminFieldsScroll");
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *content = new QWidget;
        auto *fieldsLayout = new QVBoxLayout(content);
        fieldsLayout->setContentsMargins(2, 2, 12, 8);
        auto *f = form(fieldsLayout);
        scroll->setWidget(content);
        body->addWidget(scroll, 1);
        auto *action = cvui::comboBox();
        const QList<QPair<QString, QString>> actions{
            {"列出账户", "list-users"},      {"创建账户", "create-user"},    {"禁用账户", "disable-user"},
            {"启用账户", "enable-user"},     {"重置密码", "reset-password"}, {"设置配额", "set-quota"},
            {"授权设备", "register-device"}, {"撤销设备", "revoke-device"}};
        for (auto pair : actions)
            action->addItem(pair.first, pair.second);
        f->addRow("操作", action);
        QHash<QString, QLineEdit *> edits;
        for (auto pair : QList<QPair<QString, QString>>{{"user", "目标账户"},
                                                        {"password", "新登录密码"},
                                                        {"quota", "配额（字节）"},
                                                        {"device", "设备编号"},
                                                        {"key", "设备密钥（十六进制）"}}) {
            auto *edit = new QLineEdit;
            if (pair.first == "password" || pair.first == "key")
                edit->setEchoMode(QLineEdit::Password);
            edits[pair.first] = edit;
            f->addRow(pair.second, edit);
        }
        edits["quota"]->setText("10737418240");
        auto *administrator = new QCheckBox("管理员账户（创建时使用）");
        fieldsLayout->addWidget(administrator);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText("确认操作");
        buttons->button(QDialogButtonBox::Cancel)->setText("取消");
        buttons->button(QDialogButtonBox::Ok)->setProperty("primary", true);
        body->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
            return;
        QString command = action->currentData().toString();
        QJsonObject args;
        for (auto it = edits.begin(); it != edits.end(); ++it)
            args[it.key()] = it.value()->text();
        args["administrator"] = administrator->isChecked();
        args["disabled"] = command != "enable-user";
        if (command == "enable-user")
            command = "disable-user";
        auto options = remoteOptions();
        runTask(
            "执行账户管理",
            [options, command, args](const cv::Context &) {
                return cv::network::Client(options).admin(command, args);
            },
            [this](const QJsonObject &result) {
                QMessageBox::information(
                    this, "管理结果",
                    QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented)));
            });
    });
    return page;
}

QWidget *MainWindow::repairPage() {
    // Repair restores archive bytes, not the files inside an archive. A later
    // restore still authenticates the encrypted envelope with its password.
    // The UI states erasure capacity per stripe rather than implying arbitrary
    // 20% corruption can be recovered. Core rejects an insufficient sidecar.
    // create/check/recover remain distinct actions so checking never writes.
    QVBoxLayout *layout;
    auto *page = pageBody(layout);
    auto *root = layout;
    layout = cvui::card(root, "归档与纠错文件");
    layout->addWidget(titleLabel(
        "每组 10 个数据分片 + 2 个校验分片。根据摘要定位损坏后，每组最多恢复 2 个分片；原归档保持不变。",
        "muted"));
    auto *fields = form(layout);
    fields->addRow("归档文件", pathRow("repair.archive", "原始或损坏归档", false));
    fields->addRow("纠错文件", pathRow("repair.sidecar", "与该归档匹配的 .cvr 文件", false, true));
    fields->addRow("修复输出", pathRow("repair.output", "新的修复归档路径", false, true));
    auto *actions = new QHBoxLayout;
    auto *create = button("生成纠错文件");
    auto *check = button("检查损坏");
    auto *recover = button("修复到新文件", true);
    for (auto *b : {create, check, recover})
        actions->addWidget(b);
    actions->addStretch();
    layout->addLayout(actions);
    connect(create, &QPushButton::clicked, this, [this] {
        auto archive = lines_["repair.archive"]->text(), sidecar = lines_["repair.sidecar"]->text();
        runTask("生成 RS 冗余数据",
                [=](const cv::Context &c) { return cv::repair::create(archive, sidecar, c).toJson(); });
    });
    connect(check, &QPushButton::clicked, this, [this] {
        auto archive = lines_["repair.archive"]->text(), sidecar = lines_["repair.sidecar"]->text();
        runTask(
            "定位损坏分片", [=](const cv::Context &c) { return cv::repair::check(archive, sidecar, c); },
            [this](const QJsonObject &result) {
                QMessageBox::information(
                    this, "检查结果",
                    QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented)));
            });
    });
    connect(recover, &QPushButton::clicked, this, [this] {
        // Recovery always targets a new file. Even an explicitly selected path
        // is rejected by core if it already exists or aliases either input.
        // The original damaged archive is evidence and remains untouched.
        auto archive = lines_["repair.archive"]->text(), sidecar = lines_["repair.sidecar"]->text(),
             output = lines_["repair.output"]->text();
        runTask("修复并验证完整摘要", [=](const cv::Context &c) {
            return cv::repair::recover(archive, sidecar, output, c).toJson();
        });
    });
    layout = cvui::card(root, "从检查到修复", "保留原始归档，将修复结果写入新的文件。");
    layout->addWidget(
        titleLabel("1  在归档完好时生成 .cvr，并分开妥善保存\n2  发生损坏后，选择归档与对应 .cvr 进行检查\n3 "
                   " 修复到一个新文件，确认整包摘要一致\n4  在“浏览与恢复”中输入原密码恢复文件",
                   "muted"));
    layout->addStretch();
    return page;
}
QWidget *MainWindow::logPage() {
    // The text view is observational: editing displayed history must not alter
    // task state. Export writes a separate file through atomic commit.
    // Background logs are loaded explicitly because their lifetime is separate
    // from this GUI process; a fresh window does not invent prior events.
    QVBoxLayout *layout;
    auto *page = pageBody(layout);
    auto *root = layout;
    layout = cvui::card(root, "操作记录", "保留每次操作的真实结果，便于检查与排查问题。");
    auto *actions = new QHBoxLayout;
    auto *refresh = button("读取后台日志");
    auto *save = button("导出日志…");
    actions->addWidget(refresh);
    actions->addWidget(save);
    actions->addStretch();
    layout->addLayout(actions);
    log_ = new QPlainTextEdit;
    log_->setObjectName("operationLog");
    log_->setReadOnly(true);
    log_->setMinimumHeight(390);
    log_->setPlaceholderText("暂无操作记录。完成一次备份、恢复或校验后，详细结果会显示在这里。");
    layout->addWidget(log_, 1);
    connect(refresh, &QPushButton::clicked, this, [this] {
        QFile file(cv::appDataDirectory() + "/agent-events.jsonl");
        if (file.open(QIODevice::ReadOnly))
            log_->appendPlainText(QString::fromUtf8(file.readAll()));
    });
    connect(save, &QPushButton::clicked, this, [this] {
        auto path = QFileDialog::getSaveFileName(this, "导出日志", {}, "文本 (*.txt)");
        if (!path.isEmpty())
            try {
                cv::atomicWrite(path, log_->toPlainText().toUtf8());
            } catch (const std::exception &e) {
                QMessageBox::warning(this, "导出失败", e.what());
            }
    });
    return page;
}
QWidget *MainWindow::settingsPage() {
    // Strict metadata is the default for course verification. Relaxing it
    // preserves warnings and does not relabel a failed owner/ACL restoration
    // as an exact match. The option is copied into each foreground operation.
    // Startup affects only HKCU for the current user, matching DPAPI scope.
    QVBoxLayout *layout;
    auto *page = pageBody(layout);
    auto *root = layout;
    layout = cvui::card(root, "恢复偏好与后台运行");
    auto *strict = new QCheckBox("严格验证元数据：无法保存或恢复权限、属主等信息时报告失败");
    strict->setChecked(true);
    checks_["strict"] = strict;
    layout->addWidget(strict);
    auto *startup = new QCheckBox("当前用户登录后自动启动后台任务进程");
    startup->setChecked(cv::schedule::autoStartEnabled());
    layout->addWidget(startup);
    connect(startup, &QCheckBox::toggled, this, [this, startup](bool enabled) {
        try {
            cv::schedule::setAutoStart(enabled,
                                       QCoreApplication::applicationDirPath() + "/ChengVaultAgent.exe");
            if (enabled)
                startAgent();
        } catch (const std::exception &e) {
            QSignalBlocker blocker(startup);
            // Revert a failed registry update without recursively triggering
            // the toggled handler and attempting a second registry operation.
            startup->setChecked(!enabled);
            QMessageBox::warning(this, "无法设置自启动", e.what());
        }
    });
    auto *runAgent = button("启动后台任务进程");
    connect(runAgent, &QPushButton::clicked, this, &MainWindow::startAgent);
    layout->addWidget(runAgent, 0, Qt::AlignLeft);
    layout = cvui::card(root, "平台与文件支持");
    layout->addWidget(titleLabel("Windows 10 / 11 与 "
                                 "NTFS。保留硬链接、符号链接、目录联接、稀疏范围和备用数据流。符号链接创建、"
                                 "部分属主或权限恢复需要相应系统权限。界面会显示失败原因。",
                                 "muted"));
    layout = cvui::card(root, "备份密码与设备密钥");
    layout->addWidget(
        titleLabel("请保存好备份密码。后台任务使用当前 Windows 用户的 DPAPI "
                   "保护仓库主密钥。网络登录密码、设备配对密钥和备份密码各司其职，服务器不能代为解密备份。",
                   "muted"));
    layout = cvui::card(root, "帮助文档", "查看使用说明、文件格式与常见问题。");
    auto *docs = button("打开项目文档目录");
    connect(docs, &QPushButton::clicked, this, [] {
        QString path = QCoreApplication::applicationDirPath() + "/docs";
        if (!QFileInfo::exists(path))
            path = QCoreApplication::applicationDirPath() + "/../docs";
        QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(path).absolutePath()));
    });
    layout->addWidget(docs, 0, Qt::AlignLeft);
    layout->addStretch();
    return page;
}
void MainWindow::saveScreenshots(const QString &directory) {
    // Capture rendered widgets for deployment/report evidence. Process pending
    // paint/layout events before each grab; these are not hand-drawn mockups.
    // Navigation returns to overview so an evidence run has deterministic state.
    // Screenshot capture performs no backup, upload or account mutation.
    QDir().mkpath(directory);
    // Requested font names are not proof of the font Qt actually selected.
    // Record resolved families alongside logical size and device pixel ratio
    // so screenshots at different Windows scaling levels can be interpreted.
    QJsonObject environment{{"applicationFont", QApplication::font().family()},
                            {"resolvedApplicationFont", QFontInfo(QApplication::font()).family()},
                            {"resolvedHeadingFont", QFontInfo(pageTitle_->font()).family()},
                            {"resolvedInputFont", QFontInfo(lines_["backup.output"]->font()).family()},
                            {"resolvedTableHeaderFont", QFontInfo(entries_->header()->font()).family()},
                            {"width", width()},
                            {"height", height()},
                            {"devicePixelRatio", devicePixelRatioF()},
                            {"logicalDpi", screen()->logicalDotsPerInch()}};
    cv::writeJson(directory + "/ui-environment.json", environment);
    const QStringList names{"overview", "backup", "restore", "automation",
                            "network",  "repair", "logs",    "settings"};
    const QStringList titles{"总览",     "创建备份",   "浏览与恢复", "自动任务",
                             "远程仓库", "检查与修复", "操作日志",   "设置与帮助"};
    for (int i = 0; i < names.size(); ++i) {
        selectPage(i);
        pageTitle_->setText(titles[i]);
        QApplication::processEvents();
        grab().save(directory + '/' + names[i] + ".png");
    }
    selectPage(0);
    pageTitle_->setText(titles[0]);
}
void MainWindow::compareFiles(const QString &source, const QString &restored) {
    // Hash comparison reads both real files through the same streaming SHA-256
    // implementation used by verification. It reports both digests in the log,
    // so the user can distinguish a comparison result from a completed copy.
    // Hash equality describes file bytes only, not ACLs, owner identity, ADS,
    // sparse ranges or link relationships. Those properties are verified by
    // the dedicated restore checks and filesystem tests rather than this UI.
    // A read failure produces an error, not a misleading "different" result.
    // Both paths are captured by value so browsing elsewhere cannot change an
    // in-flight comparison. The streaming implementation bounds memory use.
    runTask(
        "比较源文件与恢复文件 SHA-256",
        [source, restored](const cv::Context &c) {
            const auto first = cv::crypto::hashFile(source, c), second = cv::crypto::hashFile(restored, c);
            return QJsonObject{{"source", source},
                               {"restored", restored},
                               {"sourceSha256", QString::fromLatin1(first.toHex())},
                               {"restoredSha256", QString::fromLatin1(second.toHex())},
                               {"hashesMatch", cv::crypto::equal(first, second)}};
        },
        [this](const QJsonObject &result) {
            selectPage(6);
            QMessageBox::information(this, "哈希比较",
                                     result["hashesMatch"].toBool() ? "两份文件的 SHA-256 完全一致。"
                                                                    : "两份文件内容不同，SHA-256 不一致。");
        });
}
