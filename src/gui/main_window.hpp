#pragma once
#include "chengvault/common.hpp"
#include "chengvault/network.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QFutureWatcher>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QTreeWidget>

// The window is an adapter over the shared core. It owns widget state and
// presentation only; no alternate compression/crypto/restore implementation
// is permitted here. All child widgets follow Qt's parent ownership model.
// Public automation hooks exercise the same pages and real hash operation
// available to the user, rather than a separate demonstration-only workflow.
class MainWindow : public QMainWindow {
  public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    void selectPage(int index);
    void saveScreenshots(const QString &directory);
    void compareFiles(const QString &source, const QString &restored);

  private:
    // Registries are non-owning indices into widgets owned by page layouts.
    // A key is stable across display translations and GUI automation tests.
    // Only the GUI thread may access these objects, including their text/data.
    QHash<QString, QLineEdit *> lines_;
    QHash<QString, QComboBox *> choices_;
    QHash<QString, QCheckBox *> checks_;
    QStackedWidget *pages_ = nullptr;
    QPlainTextEdit *sources_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
    QTreeWidget *entries_ = nullptr;
    QTreeWidget *snapshots_ = nullptr;
    QTreeWidget *restoreEntries_ = nullptr;
    QTreeWidget *jobs_ = nullptr;
    QTreeWidget *remote_ = nullptr;
    QProgressBar *progress_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *pageTitle_ = nullptr;
    QLabel *pageSubtitle_ = nullptr;
    QPushButton *cancel_ = nullptr;
    QPushButton *backupStart_ = nullptr;
    QJsonObject filters_;
    // This is the last successfully inspected plaintext manifest. It is kept
    // separately from form values so exporting cannot dump connection secrets.
    QJsonObject inspected_;
    // One watcher, one cancellation flag and one completion callback describe
    // the active foreground operation. Destruction cancels and joins it.
    // Detached background scheduling belongs to ChengVaultAgent, not this state.
    QFutureWatcher<QJsonObject> work_;
    std::atomic_bool cancelled_{false};
    std::function<void(const QJsonObject &)> completion_;
    QWidget *overviewPage();
    QWidget *backupPage();
    QWidget *restorePage();
    QWidget *automationPage();
    QWidget *networkPage();
    QWidget *repairPage();
    QWidget *logPage();
    QWidget *settingsPage();
    QLineEdit *input(const QString &key, const QString &placeholder = {}, bool secret = false);
    QComboBox *choice(const QString &key, const QList<QPair<QString, QString>> &values);
    QWidget *pathRow(const QString &key, const QString &placeholder, bool directory, bool save = false);
    cv::BackupPlan plan() const;
    cv::network::RemoteOptions remoteOptions() const;
    // fn runs on a Qt pool thread and returns data; done runs on the UI thread.
    // Capture input values before calling. Never capture pointers to controls
    // for use inside fn, and never retain the supplied stack-local Context.
    void runTask(const QString &title, std::function<QJsonObject(const cv::Context &)> fn,
                 std::function<void(const QJsonObject &)> done = {});
    void editFilters();
    void showEntries(const QJsonArray &entries);
    void refreshJobs();
    void startAgent();
    void addLog(const QString &text);
};
