#ifndef HWBINDWIDGETPARTS_H
#define HWBINDWIDGETPARTS_H

// Three widgets of the editor of the bindings of a hardware slot (hwbindwidget.ui), in a header of their own for the .ui

#include <QDrag>
#include <QDropEvent>
#include <QPainter>
#include <QTableWidget>
#include <QTimer>
#include <functional>

// The event table of a Macro: a drag moves the selected rows (one contiguous block) and nothing else. The table never changes
// its own items: the drop says where the block goes, and the editor moves its rows and fills the table again.
class HwMacroTable : public QTableWidget {
public:
    using QTableWidget::QTableWidget;
    std::function<void(int)> dropped;       // the row (of the order before the drag) the block goes before
protected:
    void startDrag(Qt::DropActions) override {
        const QModelIndexList sel = selectedIndexes();
        if(sel.isEmpty()) return;
        QDrag* drag = new QDrag(this);
        drag->setMimeData(model()->mimeData(sel));
        drag->exec(Qt::MoveAction);         // (QAbstractItemView's own would then remove the items: the rows move in dropEvent)
    }
    void dragEnterEvent(QDragEnterEvent* e) override {
        if(e->source() != this){ e->ignore(); return; }
        QTableWidget::dragEnterEvent(e);
        e->setDropAction(Qt::MoveAction);
        e->accept();
    }
    void dragMoveEvent(QDragMoveEvent* e) override {
        if(e->source() != this){ e->ignore(); return; }
        QTableWidget::dragMoveEvent(e);     // the drop indicator and the scrolling at the edges
        e->setDropAction(Qt::MoveAction);
        e->accept();
    }
    void dropEvent(QDropEvent* e) override {
        if(e->source() != this){ e->ignore(); return; }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPoint at = e->position().toPoint();
#else
        const QPoint at = e->pos();
#endif
        const QModelIndex index = indexAt(at);
        int to = rowCount();
        if(index.isValid()){
            if(dropIndicatorPosition() == AboveItem) to = index.row();
            else if(dropIndicatorPosition() == BelowItem) to = index.row() + 1;
            else to = at.y() > visualRect(index).center().y() ? index.row() + 1 : index.row();
        }
        e->setDropAction(Qt::MoveAction);
        e->accept();
        stopAutoScroll();
        setState(NoState);
        viewport()->update();
        // after the drag has finished with the table's items
        if(dropped) QTimer::singleShot(0, this, [this, to](){ if(dropped) dropped(to); });
    }
};

// The preview of a Shortcut of the Remap type, as the chips of iCUE's Keystroke field: the keys in the order of their presses, joined
// by "+", in a field like a line edit that shows the result and is not typed into. Its frame is 1 px inside on the left and the right,
// as Breeze draws the frame of a push button, so it is as wide as the buttons under it
class HwShortcutChips : public QWidget {
public:
    explicit HwShortcutChips(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(34);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setKeys(const QStringList& k){ shown = k; update(); }
    const QStringList& keys() const { return shown; }
    QSize sizeHint() const override { return QSize(100, 34); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(palette().color(QPalette::Mid));
        p.setBrush(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::Base));
        p.drawRoundedRect(QRectF(rect()).adjusted(1.5, 0.5, -1.5, -0.5), 3, 3);
        const QFontMetrics fm(font());
        int x = 9;
        for(int i = 0; i < shown.size(); ++i){
            if(i){
                p.setPen(palette().color(QPalette::PlaceholderText));
                p.drawText(QRect(x, 0, 18, height()), Qt::AlignCenter, "+");
                x += 18;
            }
            const int w = fm.horizontalAdvance(shown[i]) + 16;
            const QRect chip(x, 6, w, height() - 12);
            p.setPen(palette().color(QPalette::Mid));
            p.setBrush(palette().color(QPalette::Button));
            p.drawRoundedRect(chip, 3, 3);
            p.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::ButtonText));
            p.drawText(chip, Qt::AlignCenter, shown[i]);
            x += w;
        }
    }
private:
    QStringList shown;
};

// The editor under the keyboard, for the layout of the whole widget a box of plain minimum and preferred heights: its word-wrapped
// messages made it a box whose height depends on its width, and then the layout took its preferred height (the event table's
// included) as its minimum, and the keyboard's height was computed against the wrong minimum
class HwEditorBox : public QWidget {
public:
    using QWidget::QWidget;
    bool hasHeightForWidth() const override { return false; }
};

#endif // HWBINDWIDGETPARTS_H
