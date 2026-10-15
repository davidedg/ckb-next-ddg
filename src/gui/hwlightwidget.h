#ifndef HWLIGHTWIDGET_H
#define HWLIGHTWIDGET_H

#include <QWidget>
#include <functional>
#include "hwslotdraft.h"
#include "keymap.h"

class KeyWidget;
class QListWidget;
class QLabel;
class QCheckBox;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class QDoubleSpinBox;
class QSpinBox;
class QComboBox;
class ColorButton;
class HwLightStrip;
class QToolButton;

// Edits only the local, complete lighting draft. Saving the slot is a separate operation.
class HwLightWidget : public QWidget {
    Q_OBJECT
public:
    explicit HwLightWidget(QWidget* parent = nullptr);
    void setSlot(HwSlotDraft::Draft* draft, const KeyMap& map, const QString& physicalLayout,
                 const QMap<QString, QColor>& indicators, bool editable);

signals:
    void draftChanged();

private:
    HwSlotDraft::Draft* draft = nullptr;
    bool filling = false, editable = false;
    int layout = 0, current = -1;
    unsigned revision = 0;
    KeyWidget* keyboard;
    QListWidget* layers;
    QLabel *quota, *selectionNote, *mutedNote, *emptyNote;
    QCheckBox* allKeys;
    QPushButton *clear, *remove, *up, *down;
    QToolButton* add;
    QStackedWidget* panels;
    ColorButton* staticColour;
    QDoubleSpinBox *opacity, *duration, *tail, *velocity;
    QSpinBox *angle, *after;
    QCheckBox* twoSides;
    QComboBox *start, *stop;
    QTabWidget* customTabs;
    QTableWidget* points;
    HwLightStrip* strip;
    QPushButton *addPoint, *removePoint;
    QComboBox *colourMode, *speed, *direction, *typeMode, *typeDuration;
    ColorButton* presetColours[2];
    QWidget *colourRow, *directionRow, *typeRow, *typeDurationRow, *speedRow;

    cape_profile_layer* selected();
    void mutate(const std::function<void(cape_profile_layer&)>& change, bool refresh = true);
    void refreshList();
    void refreshPanel();
    void refreshKeys();
    void toggle(int row);
    void rename(int row);
    void move(int from, int to);
    void removeLayer();
    void addLayer(int kind);
    void changeType(int row, int kind);
    void contextMenu(const QPoint& at);
    void changeKeys(const QStringList& keys);
    QStringList selectedKeys() const;
    QString profileKeyName(const QString& key) const;
    void fillPoints();
    void pointColour(int row);
    void addTransition();
    void removeTransition();
    void setPointTime(int row, int end, double time);
};

#endif
