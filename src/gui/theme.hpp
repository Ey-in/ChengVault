#pragma once
#include <QBoxLayout>
#include <QColor>
#include <QComboBox>
#include <QIcon>
#include <QPushButton>
#include <QTreeWidget>

namespace cvui {
// Icons are drawn in logical coordinates so the same geometry remains sharp
// on the navigation rail, buttons and high-DPI screenshots.
// The first eight values follow MainWindow's stable page IDs. Additional
// decorative/action symbols follow them and do not introduce navigation IDs.
enum class Icon { Overview, Backup, Restore, Clock, Cloud, Repair, Log, Settings, Folder, Arrow, Shield };
// Apply this before creating page children. It is safe to call again for a
// second MainWindow because the installation is scoped to QApplication.
void installApplicationStyle();
// The stylesheet contains visual roles only; it must not hide controls based
// on backup state. Workflow visibility is handled explicitly in MainWindow.
QString styleSheet();
QIcon icon(Icon kind, const QColor &color = QColor("#5d7068"));
// A real combo box with a painted chevron; popup behavior and model semantics
// remain Qt's. The custom arrow survives stylesheet-owned dropdown controls.
QComboBox *comboBox();
// Factory widgets are adopted by their caller's layout/parent. They carry no
// persistent state and never access the filesystem, network or account store.
QWidget *brand(QWidget *parent = nullptr);
QWidget *illustration(QWidget *parent = nullptr);
// The returned card is an actual QPushButton; callers connect clicked exactly
// as they would for a normal button. No duplicate hidden action is created.
QPushButton *actionCard(const QString &title, const QString &description, Icon kind);
// The card helper returns the inner layout, allowing page controls to keep
// their original object names and connections while sharing presentation.
QVBoxLayout *card(QBoxLayout *parent, const QString &title = {}, const QString &description = {});
// Model observation only drives the empty illustration. Populating the table,
// selection and checkbox state remain the responsibility of the real page.
void styleTable(QTreeWidget *table, const QString &emptyTitle, const QString &emptyDescription);
} // namespace cvui
