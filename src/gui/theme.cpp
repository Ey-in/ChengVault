#include "theme.hpp"
#include <QApplication>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOption>

namespace cvui {
namespace {
// Drawing helpers are deliberately private to this translation unit. Pages
// request a semantic icon or card and do not depend on its control points.
// None of these widgets own backup state: changing a palette or illustration
// must not alter the selected source, repository or pending operation.
// Every icon shares a 24-unit grid and rounded strokes. The painter's caller
// controls color, size and DPI rather than storing platform-dependent bitmaps.
// Geometry stays inside the grid with room for half a stroke at its edges.
// Saving the incoming painter state is important because the overview reuses
// this function inside a scaled composition and continues drawing afterwards.
void drawIcon(QPainter &p, Icon kind, const QRectF &bounds, const QColor &color) {
    p.save();
    p.translate(bounds.topLeft());
    p.scale(bounds.width() / 24.0, bounds.height() / 24.0);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    if (kind == Icon::Overview) {
        for (const auto &r :
             {QRectF(4, 4, 6, 6), QRectF(14, 4, 6, 6), QRectF(4, 14, 6, 6), QRectF(14, 14, 6, 6)})
            p.drawRoundedRect(r, 1.4, 1.4);
    } else if (kind == Icon::Folder || kind == Icon::Backup) {
        // A shared folder silhouette keeps the picker and backup action related.
        // The plus is an additional stroke rather than a separate raster asset;
        // this preserves the same weight at sidebar and button sizes.
        QPainterPath path;
        path.moveTo(3, 7);
        path.lineTo(3, 19);
        path.lineTo(21, 19);
        path.lineTo(21, 8);
        path.lineTo(12, 8);
        path.lineTo(10, 5);
        path.lineTo(3, 5);
        path.closeSubpath();
        p.drawPath(path);
        if (kind == Icon::Backup) {
            p.drawLine(12, 11, 12, 16);
            p.drawLine(9.5, 13.5, 14.5, 13.5);
        }
    } else if (kind == Icon::Restore) {
        // Qt arc angles use sixteenths of a degree. Leaving a gap gives the
        // counterclockwise return arrow a distinct silhouette from the clock.
        p.drawArc(QRectF(5, 5, 15, 15), 35 * 16, 285 * 16);
        p.drawPolyline(QPolygonF{QPointF(3, 5), QPointF(3, 11), QPointF(9, 11)});
        p.drawLine(12, 9, 12, 15);
        p.drawLine(9, 12, 12, 15);
        p.drawLine(15, 12, 12, 15);
    } else if (kind == Icon::Clock) {
        p.drawEllipse(QRectF(4, 4, 16, 16));
        p.drawLine(12, 7, 12, 12);
        p.drawLine(12, 12, 16, 14);
    } else if (kind == Icon::Cloud) {
        QPainterPath path;
        path.moveTo(7, 19);
        path.cubicTo(1, 19, 1, 11, 6, 11);
        path.cubicTo(5, 3, 17, 2, 18, 10);
        path.cubicTo(24, 10, 24, 19, 18, 19);
        path.closeSubpath();
        p.drawPath(path);
        p.drawLine(12, 10, 12, 16);
        p.drawLine(9.5, 13.5, 12, 16);
        p.drawLine(14.5, 13.5, 12, 16);
    } else if (kind == Icon::Repair || kind == Icon::Shield) {
        // Repair and protection share an outline but use different inner marks.
        // Their text labels remain authoritative; an icon alone never asserts
        // that a particular archive passed an integrity or recoverability test.
        QPainterPath path;
        path.moveTo(12, 3);
        path.lineTo(20, 6);
        path.lineTo(19, 14);
        path.cubicTo(18, 18, 15, 20, 12, 22);
        path.cubicTo(9, 20, 6, 18, 5, 14);
        path.lineTo(4, 6);
        path.closeSubpath();
        p.drawPath(path);
        if (kind == Icon::Repair) {
            p.drawLine(8.5, 12, 15.5, 12);
            p.drawLine(12, 8.5, 12, 15.5);
        } else {
            p.drawPolyline(QPolygonF{QPointF(8, 12), QPointF(11, 15), QPointF(16, 9)});
        }
    } else if (kind == Icon::Log) {
        p.drawRoundedRect(QRectF(5, 3, 14, 18), 2, 2);
        for (int y : {8, 12, 16})
            p.drawLine(9, y, 15, y);
    } else if (kind == Icon::Settings) {
        for (int x : {6, 12, 18})
            p.drawLine(x, 4, x, 20);
        p.setBrush(QColor("#fffefa"));
        p.drawEllipse(QPointF(6, 9), 2, 2);
        p.drawEllipse(QPointF(12, 15), 2, 2);
        p.drawEllipse(QPointF(18, 8), 2, 2);
    } else {
        p.drawLine(5, 12, 19, 12);
        p.drawLine(14, 7, 19, 12);
        p.drawLine(14, 17, 19, 12);
    }
    p.restore();
}

// Fusion supplies predictable metrics while these indicators avoid mixing
// native Windows checkbox art with the application's softer visual language.
// QProxyStyle takes ownership of the newly created base style. QApplication
// later owns this proxy, so no page should delete either style independently.
// Only small primitive artwork is overridden. Focus traversal, accessibility,
// toggle behavior and item-view hit testing remain Qt's normal implementation.
class DesktopStyle final : public QProxyStyle {
  public:
    DesktopStyle() : QProxyStyle(QStyleFactory::create("Fusion")) {}
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *p,
                       const QWidget *widget = nullptr) const override {
        if (element == PE_IndicatorCheckBox || element == PE_IndicatorItemViewItemCheck) {
            // The tree's restoration checkboxes and standalone preferences use
            // the same state rendering. Mixed is a real third state, displayed
            // as a dash instead of being mistaken for a checked child item.
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            const bool checked = option->state & State_On, mixed = option->state & State_NoChange,
                       enabled = option->state & State_Enabled;
            const QRectF box = QRectF(option->rect).adjusted(1.5, 1.5, -1.5, -1.5);
            // Inset the border rather than painting across adjacent item cells.
            // Disabled controls keep their current value visible with reduced
            // contrast; a disabled checked box must not appear unchecked.
            p->setPen(
                QPen(enabled ? QColor(checked || mixed ? "#16765c" : "#b8c5bd") : QColor("#d8dfd9"), 1.2));
            p->setBrush(checked || mixed ? QColor(enabled ? "#16765c" : "#b5c2ba") : QColor("#ffffff"));
            p->drawRoundedRect(box, 4, 4);
            if (checked || mixed) {
                p->setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                if (mixed)
                    p->drawLine(box.left() + 3, box.center().y(), box.right() - 3, box.center().y());
                else
                    p->drawPolyline(QPolygonF{QPointF(box.left() + 3, box.center().y()),
                                              QPointF(box.center().x() - 1, box.bottom() - 4),
                                              QPointF(box.right() - 3, box.top() + 4)});
            }
            p->restore();
            return;
        }
        if (element == PE_IndicatorArrowDown || element == PE_IndicatorArrowUp) {
            // Center against the supplied primitive rectangle, not the parent
            // combo box. Qt already accounts for padding and layout direction.
            // Multiplying the vertical displacement reuses one symmetric path.
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(QPen(QColor("#6f8178"), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QPointF c = option->rect.center();
            const int d = element == PE_IndicatorArrowDown ? 1 : -1;
            p->drawPolyline(
                QPolygonF{c + QPointF(-4, -2 * d), c + QPointF(0, 2 * d), c + QPointF(4, -2 * d)});
            p->restore();
            return;
        }
        // Delegating every unhandled primitive avoids a partial style that
        // silently loses focus frames, menu indicators or window decorations.
        QProxyStyle::drawPrimitive(element, option, p, widget);
    }
};

class ComboBox final : public QComboBox {
  public:
    void paintEvent(QPaintEvent *event) override {
        QComboBox::paintEvent(event);
        // QStyleSheetStyle owns a styled dropdown and may bypass the proxy's
        // PE_IndicatorArrowDown primitive. Draw its chevron after the base control
        // using the actual arrow subcontrol rectangle, including RTL placement.
        // Only painting changes: clicking and keyboard popup handling stay Qt's.
        QStyleOptionComboBox option;
        initStyleOption(&option);
        const auto arrow =
            style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(isEnabled() ? "#61776a" : "#acb7ad"), 1.6, Qt::SolidLine, Qt::RoundCap,
                            Qt::RoundJoin));
        const QPointF center = arrow.center();
        painter.drawPolyline(
            QPolygonF{center + QPointF(-4, -2), center + QPointF(0, 2), center + QPointF(4, -2)});
    }
};

class Brand final : public QWidget {
  public:
    explicit Brand(QWidget *parent) : QWidget(parent) {
        // This size is a logical widget size; QPainter maps it to device pixels.
        // A compact square reserves consistent room beside the two brand lines.
        setFixedSize(38, 38);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#3ca684"));
        p.drawRoundedRect(QRectF(0, 0, 38, 38), 11, 11);
        // The box and check form the original mark. It is constructed from
        // ordinary paths rather than an icon font, whose fallback could turn
        // a missing glyph into a square on another Windows installation.
        p.setPen(QPen(QColor("#f0fff8"), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(10, 10, 18, 19), 4, 4);
        p.drawLine(14, 8, 24, 8);
        p.drawLine(15, 15, 23, 15);
        p.drawPolyline(QPolygonF{QPointF(14, 22), QPointF(18, 25), QPointF(24, 19)});
    }
};

class VaultIllustration final : public QWidget {
  public:
    explicit VaultIllustration(QWidget *parent) : QWidget(parent) {
        // The illustration may grow but has a modest minimum so the hero's
        // real navigation controls still fit in a 1040-pixel-wide window.
        // It has no focus policy or mouse handlers because it is decorative.
        setMinimumSize(180, 140);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    }
    QSize sizeHint() const override {
        return {220, 155};
    }
    void paintEvent(QPaintEvent *) override {
        // This is a decorative archive motif, not a fake storage chart or a
        // claim about the user's files. It has no data-bearing labels.
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        // Fit the complete composition without cropping or stretching circles.
        // A single uniform scale also scales stroke widths with the artwork.
        // Centering absorbs extra height from the neighboring text column.
        const qreal s = qMin(width() / 240.0, height() / 200.0);
        p.translate((width() - 240 * s) / 2, (height() - 200 * s) / 2);
        p.scale(s, s);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#e0eee2"));
        p.drawEllipse(QRectF(32, 17, 176, 176));
        p.setPen(QPen(QColor("#c8ded0"), 1));
        p.setBrush(QColor("#eef5e9"));
        p.drawRoundedRect(QRectF(65, 43, 121, 116), 14, 14);
        p.setBrush(QColor("#f9fbf3"));
        p.drawRoundedRect(QRectF(52, 58, 121, 116), 14, 14);
        p.setPen(QPen(QColor("#92bfa6"), 1));
        p.setBrush(QColor("#ffffff"));
        p.drawRoundedRect(QRectF(38, 73, 121, 112), 14, 14);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#d5e9dc"));
        p.drawRoundedRect(QRectF(53, 90, 34, 10), 5, 5);
        p.setBrush(QColor("#e6eee6"));
        p.drawRoundedRect(QRectF(53, 116, 87, 6), 3, 3);
        p.drawRoundedRect(QRectF(53, 130, 64, 6), 3, 3);
        p.setBrush(QColor("#267e60"));
        p.drawRoundedRect(QRectF(127, 126, 54, 54), 16, 16);
        // The shield reuses the same icon geometry as the repair workflow.
        // The pale background layers separate depth without blurred shadows,
        // keeping repainting cheap even on software-rendered remote desktops.
        drawIcon(p, Icon::Shield, QRectF(140, 137, 28, 28), Qt::white);
        p.setBrush(QColor("#91bca3"));
        p.drawEllipse(QRectF(202, 84, 7, 7));
        p.drawEllipse(QRectF(22, 57, 5, 5));
        p.setPen(QPen(QColor("#a2c5ac"), 1.5));
        p.drawLine(190, 31, 190, 41);
        p.drawLine(185, 36, 195, 36);
    }
};

class ActionCard final : public QPushButton {
    QString description_;
    Icon kind_;

  public:
    ActionCard(QString title, QString description, Icon kind)
        : QPushButton(title), description_(std::move(description)), kind_(kind) {
        // Retain QPushButton's native clicked signal and Space/Enter behavior.
        // The extra description is exposed to assistive software independently
        // from the short action name, which remains the button's actual text.
        setCursor(Qt::PointingHandCursor);
        setMinimumHeight(108);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleDescription(description_);
    }
    QSize sizeHint() const override {
        // Fixed vertical rhythm leaves enough room for two description lines.
        // Width remains flexible; the surrounding grid divides available space.
        return {300, 108};
    }
    void paintEvent(QPaintEvent *) override {
        // Keeping the entire card as one real button provides a generous hit
        // target and keyboard activation without overlapping nested controls.
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF r = rect();
        r.adjust(.7, .7, -.7, -.7);
        // Focus has priority over hover so keyboard location stays visible even
        // when the pointer happens to rest on the same card. A pressed surface
        // is distinct from both states without moving the content under a click.
        const bool hover = underMouse(), focus = hasFocus();
        p.setPen(QPen(QColor(focus ? "#16765c" : hover ? "#b0cdbb" : "#e0e6df"), focus ? 1.8 : 1));
        p.setBrush(QColor(isDown() ? "#e9f2e8" : hover ? "#f9fcf7" : "#ffffff"));
        p.drawRoundedRect(r, 14, 14);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#edf5ee"));
        p.drawRoundedRect(QRectF(18, 18, 34, 34), 10, 10);
        drawIcon(p, kind_, QRectF(25, 25, 20, 20), QColor("#267e60"));
        drawIcon(p, Icon::Arrow, QRectF(width() - 42, 23, 18, 18), QColor(hover ? "#267e60" : "#a2afa6"));
        QFont titleFont = font();
        // Derive both text fonts from the widget to preserve the installed CJK
        // family. Constructing a new generic sans font here would reintroduce
        // different Chinese fallback glyphs inside the custom-painted cards.
        titleFont.setPixelSize(15);
        titleFont.setWeight(QFont::DemiBold);
        p.setFont(titleFont);
        p.setPen(QColor("#24382e"));
        p.drawText(QRect(64, 17, width() - 110, 36), Qt::AlignVCenter, text());
        QFont bodyFont = font();
        bodyFont.setPixelSize(12);
        bodyFont.setWeight(QFont::Normal);
        p.setFont(bodyFont);
        p.setPen(QColor("#657569"));
        // Description wrapping is confined to its own region; it cannot cover
        // the title or arrow. The current short copy is sized for the minimum
        // supported window, rather than relying on an artificially wide hint.
        p.drawText(QRect(18, 60, width() - 36, 38), Qt::TextWordWrap | Qt::AlignTop, description_);
    }
};

class EmptyTableHint final : public QObject {
    QTreeWidget *table_;
    QWidget *hint_;

  public:
    EmptyTableHint(QTreeWidget *table, const QString &title, const QString &description)
        : QObject(table), table_(table) {
        // The hint and observer share the viewport's lifetime. In particular,
        // the observer must not outlive the hint when QTreeWidget dismantles its
        // viewport and model in different stages of widget destruction.
        // The hint observes the real model and disappears as soon as rows exist.
        // It is mouse-transparent, so empty-state art cannot swallow selection.
        hint_ = new QWidget(table->viewport());
        setParent(hint_);
        hint_->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto *body = new QVBoxLayout(hint_);
        body->setContentsMargins(12, 8, 12, 8);
        body->setSpacing(5);
        body->addStretch();
        auto *heading = new QLabel(title);
        heading->setProperty("role", "emptyTitle");
        heading->setAlignment(Qt::AlignCenter);
        auto *detail = new QLabel(description);
        detail->setProperty("role", "muted");
        detail->setWordWrap(true);
        detail->setAlignment(Qt::AlignCenter);
        body->addWidget(heading);
        body->addWidget(detail);
        body->addStretch();
        table->viewport()->installEventFilter(this);
        // Read the actual model after its notification. Placeholder rows would
        // pollute selection, item counts and restoration checkboxes, so this
        // helper never inserts anything into the data model itself.
        auto update = [this] {
            hint_->setGeometry(table_->viewport()->rect());
            hint_->setVisible(table_->topLevelItemCount() == 0);
        };
        // QTreeWidget clears its model during destruction. A direct modelReset
        // callback can therefore observe a partially destroyed tree or viewport.
        // Queued delivery waits until the current model operation has completed;
        // if teardown is in progress, QObject removes pending calls when this
        // observer is destroyed with the hint. A pointer validity check alone
        // would not protect a still-addressable, partially destroyed QWidget.
        connect(table->model(), &QAbstractItemModel::rowsInserted, this, update, Qt::QueuedConnection);
        connect(table->model(), &QAbstractItemModel::rowsRemoved, this, update, Qt::QueuedConnection);
        connect(table->model(), &QAbstractItemModel::modelReset, this, update, Qt::QueuedConnection);
        // The initial update matters before any results have ever been loaded;
        // later updates also handle clearing a previous preview after failure.
        update();
    }
    bool eventFilter(QObject *, QEvent *event) override {
        // Viewport size excludes the header and scrollbar. Watching it instead
        // of the table's outer rectangle prevents the hint covering headings.
        // Returning false lets Qt deliver the resize event normally afterwards.
        if (event->type() == QEvent::Resize)
            hint_->setGeometry(table_->viewport()->rect());
        return false;
    }
};
} // namespace

void installApplicationStyle() {
    // Tests construct MainWindow directly, while the application uses main().
    // Installing once here gives both entry paths exactly the same metrics.
    if (qApp->property("chengvaultStyleInstalled").toBool())
        return;
    qApp->setProperty("chengvaultStyleInstalled", true);
    // This function runs on the GUI thread before constructing page controls.
    // Replacing the style for every new test window would repolish existing
    // windows and make their geometry depend on unrelated window creation.
    // Set an actual installed CJK family at application level. Stylesheets
    // alone leave item headers, dialogs and custom painter text inconsistent.
    QString family = "Microsoft YaHei";
    const auto available = QFontDatabase::families();
    // Query the actual font database, not just Windows registry display names.
    // The first installed Chinese family is used for all ordinary controls;
    // Segoe UI remains the final Latin fallback rather than the primary font.
    for (const auto &candidate : QStringList{"Microsoft YaHei UI", "Microsoft YaHei", "Noto Sans SC"})
        if (available.contains(candidate, Qt::CaseInsensitive)) {
            family = candidate;
            break;
        }
    QFont font(family);
    font.setFamilies({family, "Microsoft YaHei", "Segoe UI"});
    font.setPixelSize(13);
    // Pixel sizes here are Qt logical pixels. The backing-store device ratio
    // supplies physical scaling; multiplying by screen DPI here would double
    // scale the interface. Normal weight is the default for body text.
    font.setWeight(QFont::Normal);
    QApplication::setFont(font);
    QApplication::setStyle(new DesktopStyle);
}
QIcon icon(Icon kind, const QColor &color) {
    // A 48-pixel backing image tagged at DPR 2 represents a 24-logical-pixel
    // icon. Qt can downsample it for the 19-pixel sidebar without jagged strokes.
    // The checked state has explicit light artwork for the dark active row.
    QIcon result;
    for (auto state : {QIcon::Off, QIcon::On}) {
        QPixmap pixmap(48, 48);
        pixmap.setDevicePixelRatio(2);
        pixmap.fill(Qt::transparent);
        {
            // End painting before adding the pixmap to QIcon. QIcon may copy or
            // detach image data, which should not happen on an active device.
            QPainter p(&pixmap);
            drawIcon(p, kind, QRectF(0, 0, 24, 24), state == QIcon::On ? QColor("#e8fff2") : color);
        }
        result.addPixmap(pixmap, QIcon::Normal, state);
    }
    return result;
}
QComboBox *comboBox() {
    return new ComboBox;
}
QWidget *brand(QWidget *parent) {
    return new Brand(parent);
}
QWidget *illustration(QWidget *parent) {
    return new VaultIllustration(parent);
}
QPushButton *actionCard(const QString &title, const QString &description, Icon kind) {
    return new ActionCard(title, description, kind);
}
QVBoxLayout *card(QBoxLayout *parent, const QString &title, const QString &description) {
    // Returning the content layout lets each page own its real controls while
    // the helper supplies only common spacing and presentation. Adding the
    // frame to a parent layout transfers widget ownership in the usual Qt way.
    auto *frame = new QFrame;
    frame->setProperty("role", "card");
    auto *body = new QVBoxLayout(frame);
    body->setContentsMargins(22, 20, 22, 20);
    body->setSpacing(13);
    if (!title.isEmpty()) {
        auto *label = new QLabel(title);
        label->setProperty("role", "section");
        body->addWidget(label);
    }
    if (!description.isEmpty()) {
        // Descriptions wrap instead of imposing a long minimum width. This is
        // especially important for Chinese help text at the minimum window size.
        auto *label = new QLabel(description);
        label->setProperty("role", "muted");
        label->setWordWrap(true);
        body->addWidget(label);
    }
    parent->addWidget(frame);
    return body;
}
void styleTable(QTreeWidget *table, const QString &emptyTitle, const QString &emptyDescription) {
    // All current tables are flat lists. Removing unused branch space gives
    // filenames more room; it does not change the checked state of restore rows.
    // Uniform heights are valid because row contents are single-line labels.
    table->setRootIsDecorated(false);
    table->setAlternatingRowColors(false);
    table->setUniformRowHeights(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    // Selection is navigation, not the restore inclusion mechanism. Restore
    // inclusion still uses independent checkboxes on each manifest entry.
    // The dedicated empty hint has no item identity and cannot be selected.
    table->setFrameShape(QFrame::NoFrame);
    table->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->header()->setHighlightSections(false);
    table->setMinimumHeight(154);
    new EmptyTableHint(table, emptyTitle, emptyDescription);
}

QString styleSheet() {
    // Visual roles describe hierarchy rather than individual page names. This
    // keeps newly created dialogs and dynamically populated tables consistent.
    // The dark rail separates navigation from the warm working surface; emerald
    // is reserved for actions, active navigation and focus instead of every box.
    // Text uses darker muted colors than borders so explanatory copy remains
    // readable. Borders may be subtle because they do not carry instructions.
    // Focus borders accompany hover colors: keyboard users need an independent
    // location cue, including on tables after the default dotted outline is off.
    // Disabled controls keep labels visible while reducing emphasis. Selected
    // rows retain a distinct but quieter color after the table loses activation.
    // Scroll handles retain a minimum target even for very large file lists.
    // No font-family rule is set here; the resolved application CJK font also
    // reaches item headers, native dialogs and the custom-painted action cards.
    return QString::fromUtf8(R"(
QWidget { color:#293b31; font-size:13px; }
QMainWindow,QDialog { background:#f4f5f0; }
QLabel { background:transparent; border:0; }
QWidget[role="pageBody"] { background:#f4f5f0; }
QFrame#navigation { background:#202b28; border:0; }
QFrame#navigation QLabel { color:#c4d2ca; }
QLabel[role="brand"] { font-size:20px; font-weight:600; color:#f6faf5; }
QLabel[role="brandSub"] { font-size:10px; color:#8fa69a; }
QLabel[role="navGroup"] { font-size:10px; color:#7f998a; padding-left:12px; }
QLabel[role="navFooter"] { font-size:11px; color:#8fa69a; }
QLabel[role="pageTitle"] { font-size:23px; font-weight:600; }
QLabel[role="section"] { font-size:15px; font-weight:600; }
QLabel[role="hero"] { font-size:31px; font-weight:600; color:#244d3b; }
QLabel[role="eyebrow"] { font-size:10px; font-weight:600; color:#4c705b; }
QLabel[role="muted"] { font-size:12px; color:#657569; }
QLabel[role="emptyTitle"] { font-size:13px; color:#617568; font-weight:600; }
QLabel[role="badge"] { font-size:11px; color:#39745a; background:#e7f0e6; border-radius:10px; padding:5px 10px; }
QWidget#pageHeader { background:#fafbf7; border-bottom:1px solid #e2e7df; }
QWidget#taskFooter { background:#fafbf7; border-top:1px solid #e2e7df; }
QFrame[role="card"] { background:white; border:1px solid #e1e6de; border-radius:14px; }
QFrame[role="heroPanel"] { background:#eaf1e5; border:1px solid #dce6d5; border-radius:18px; }
QFrame[role="note"] { background:#edf3e9; border:0; border-radius:10px; }
QPushButton { background:white; border:1px solid #d6e0d5; border-radius:8px; padding:9px 15px; min-height:18px; }
QPushButton:hover { background:#f1f7ee; border-color:#9ab9a3; }
QPushButton:pressed { background:#e4efdf; }
QPushButton:focus { border:1px solid #238563; }
QPushButton:disabled { color:#a8b2a8; background:#f1f3ee; border-color:#e3e7e0; }
QPushButton[primary="true"] { background:#18775a; color:white; border-color:#18775a; font-weight:600; }
QPushButton[primary="true"]:hover { background:#126a4e; border-color:#126a4e; }
QPushButton[primary="true"]:pressed { background:#0c5a40; }
QPushButton[primary="true"]:disabled { background:#b6cbc0; border-color:#b6cbc0; color:#f3f7f4; }
QPushButton[nav="true"] { text-align:left; padding:11px 13px; min-height:20px; border:1px solid transparent; background:transparent; color:#aabeb0; border-radius:8px; }
QPushButton[nav="true"]:hover { background:#2b3932; color:#f0f6ef; }
QPushButton[nav="true"]:checked { background:#355147; color:#f5fcf4; border-color:#3e5c4e; }
QPushButton[nav="true"]:focus { border-color:#6f9d81; }
QLineEdit,QPlainTextEdit,QComboBox,QSpinBox,QDateTimeEdit { background:#fcfdfb; border:1px solid #dce4d9; border-radius:7px; padding:9px 10px; selection-background-color:#cce7d7; selection-color:#194b35; }
QLineEdit:hover,QComboBox:hover { border-color:#b4c8b6; }
QLineEdit:focus,QPlainTextEdit:focus,QComboBox:focus { background:white; border:1px solid #328b66; }
QLineEdit:disabled,QComboBox:disabled { background:#f0f2ec; color:#a4afa3; }
QComboBox { padding-right:30px; }
QComboBox::drop-down { subcontrol-origin:padding; subcontrol-position:top right; width:30px; border:0; }
QComboBox::down-arrow { image:none; }
QComboBox QAbstractItemView { background:white; border:1px solid #d4e0d2; padding:6px; selection-background-color:#e4f0e1; selection-color:#21583e; outline:0; }
QCheckBox { spacing:9px; padding:5px 0; background:transparent; }
QCheckBox::indicator,QTreeView::indicator { width:19px; height:19px; }
QCheckBox:disabled { color:#a4afa3; }
QTreeWidget { background:white; border:1px solid #e0e7db; border-radius:9px; outline:0; selection-background-color:#e4f0e1; selection-color:#245e40; }
QTreeWidget:focus { border:1px solid #328b66; }
QTreeWidget::item { padding:8px 7px; border-bottom:1px solid #f0f3eb; }
QTreeWidget::item:hover { background:#f3f8ee; }
QTreeWidget::item:selected { background:#e3f0dd; color:#235b3e; }
QTreeWidget::item:selected:!active { background:#edf1e8; color:#465c49; }
QHeaderView::section { background:#f4f7ef; color:#586c5e; border:0; border-bottom:1px solid #e0e7da; padding:10px 8px; font-size:11px; font-weight:600; }
QProgressBar { background:#e6ede2; border:0; border-radius:3px; min-height:6px; max-height:6px; }
QProgressBar::chunk { background:#3a9873; border-radius:3px; }
QScrollArea { background:transparent; border:0; }
QScrollBar:vertical { background:transparent; width:10px; margin:3px 1px; }
QScrollBar::handle:vertical { background:#ccd7c7; border-radius:4px; min-height:35px; }
QScrollBar::handle:vertical:hover { background:#a9bba4; }
QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }
QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical { background:transparent; }
QScrollBar:horizontal { background:transparent; height:10px; margin:1px 3px; }
QScrollBar::handle:horizontal { background:#ccd7c7; border-radius:4px; min-width:35px; }
QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal { width:0; }
QScrollBar::add-page:horizontal,QScrollBar::sub-page:horizontal { background:transparent; }
QToolTip { background:#263e2f; color:#f4faf1; border:0; padding:8px; }
QDialogButtonBox { background:transparent; }
)");
}
} // namespace cvui
