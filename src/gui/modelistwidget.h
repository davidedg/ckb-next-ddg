#ifndef MODELISTWIDGET_H
#define MODELISTWIDGET_H

#include <QTableView>
#include <QResizeEvent>
#include <QTimer>
#include "kb.h"
#include "modelisttablemodel.h"

class ModeListWidget : public QTableView
{
    Q_OBJECT
public:
    explicit ModeListWidget(QWidget* parent = nullptr);
    void setDevice(Kb* dev);
    inline void setIgnoreFocusLoss(const bool e) { ignoreFocusLoss = e; }
    // The right edge of the part of the viewport that the floating column of the event icons leaves visible
    int visibleRight() const;
    ~ModeListWidget();
signals:
    void orderChanged();

private:
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    Kb* device;
    QTableView* floatingView;
    void updateFloatingGeometry();
    ModeListTableModel* modelptr;
    bool ignoreFocusLoss;
};

// The editor of a mode name: it ends where the floating column of the event icons begins, so the end of the text and the cursor
// stay in sight
class ModeNameDelegate : public QStyledItemDelegate
{
public:
    explicit ModeNameDelegate(ModeListWidget* view) : QStyledItemDelegate(view), list(view) {}
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
private:
    ModeListWidget* list;
};

class FloatingDelegate : public QStyledItemDelegate
{
public:
    explicit FloatingDelegate(QObject* parent) : QStyledItemDelegate(parent) {}
protected:
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const;
};

#endif // MODELISTWIDGET_H
