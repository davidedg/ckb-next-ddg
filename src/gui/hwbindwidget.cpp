#include "hwbindwidget.h"
#include "ui_hwbindwidget.h"
#include "hwbindwidgetparts.h"
#include "keywidget.h"
#include "kbbind.h"
#include "k95pkeytable.h"
#include "hwpickermap.h"
#include "hwtextshape.h"
#ifdef USE_XKBCOMMON
#include "hwtextxkb.h"
#endif

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStackedWidget>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextCursor>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <functional>

using HwBinding::Action;
using HwBindEdit::Row;

#ifndef USE_XKBCOMMON
class HwTextXkb {};
#endif

// The keyboard in its holder: as wide as the holder's height allows for its proportions, centred (the holder's height is set by
// HwBindWidget::fitKeyboard). One item, owned and deleted here
class HwKeysLayout : public QLayout {
public:
    using QLayout::QLayout;
    ~HwKeysLayout() override { delete item; }
    void addItem(QLayoutItem* i) override { delete item; item = i; }
    QLayoutItem* itemAt(int i) const override { return i == 0 ? item : nullptr; }
    QLayoutItem* takeAt(int i) override {
        if(i != 0) return nullptr;
        QLayoutItem* taken = item;
        item = nullptr;
        return taken;
    }
    int count() const override { return item ? 1 : 0; }
    QSize sizeHint() const override { return QSize(0, 0); }
    QSize minimumSize() const override { return QSize(0, 0); }
    void setGeometry(const QRect& r) override {
        QLayout::setGeometry(r);
        const KeyWidget* keys = item ? qobject_cast<KeyWidget*>(item->widget()) : nullptr;
        if(!keys) return;
        // (all of the width when its height fits: from the height, rounding could take a pixel off)
        const int width = keys->heightForWidth(r.width()) <= r.height() ? r.width() :
                          qMin(r.width(), int(std::lround(r.height() * double(keys->aspectRatio()))));
        const int height = qMin(r.height(), keys->heightForWidth(width));
        item->setGeometry(QRect(r.x() + (r.width() - width) / 2, r.y(), width, height));
    }
private:
    QLayoutItem* item = nullptr;
};

// The picker of a tab of the Remap type and the column of its options: the
// picker at the top left, as large as its proportions allow in the height of the tab and in the width that the column (of a fixed
// width) and the spacing leave, the column on its right. In the Shortcut tab the column is as high as the picker, so that the frame
// of its first control is on the first row of the keys' dark area and the frame of its last one on the last row (inset: the pixels a
// style draws the frame of a push button inside its widget). The picker counts nothing in the tab's minimum: fitKeyboard() leaves
// room for it
class HwPickerLayout : public QLayout {
public:
    enum { SPACING = 12, PICKER_MIN = 120 };
    HwPickerLayout(QWidget* parent, QWidget* holder, QWidget* column, const KeyWidget* keys, bool matchHeight) :
        QLayout(parent), picker(keys), match(matchHeight) {
        addWidget(holder);
        addWidget(column);
    }
    ~HwPickerLayout() override { qDeleteAll(items); }
    void addItem(QLayoutItem* i) override { items.append(i); }
    QLayoutItem* itemAt(int i) const override { return items.value(i); }
    QLayoutItem* takeAt(int i) override { return i >= 0 && i < items.size() ? items.takeAt(i) : nullptr; }
    int count() const override { return int(items.size()); }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override {
        const QMargins m = contentsMargins();
        const QSize c = items.size() > 1 ? items[1]->minimumSize() : QSize(0, 0);
        return QSize(PICKER_MIN + SPACING + c.width() + m.left() + m.right(), c.height() + m.top() + m.bottom());
    }
    void setGeometry(const QRect& r) override {
        QLayout::setGeometry(r);
        if(items.size() < 2) return;
        const QRect c = r.marginsRemoved(contentsMargins());
        const QSize column = items[1]->minimumSize();
        int w = qMax(0, c.width() - SPACING - column.width());
        if(w > c.height() * picker->aspectRatio()) w = int(c.height() * picker->aspectRatio());
        const int h = qMin(c.height(), picker->heightForWidth(w));
        items[0]->setGeometry(QRect(c.x(), c.y(), w, h));
        // (the frame of the last control on the last row of the picker may take a pixel of the margin under a picker as high as the tab)
        items[1]->setGeometry(QRect(c.x() + w + SPACING, c.y(), column.width(), match ? qMax(column.height(), h + inset) : c.height()));
    }
    int inset = 0;
private:
    QList<QLayoutItem*> items;
    const KeyWidget* picker;
    bool match;
};

// The modifiers of a Shortcut: the four buttons of the Shortcut tab, never picked on its keyboard
static const char* const MODIFIER_KEYS[] = {"lwin", "rwin", "lctrl", "rctrl", "lalt", "ralt", "lshift", "rshift"};
// The Language keys iCUE writes on a real key, in the order of its list (hwbindwidget.ui)
static const char* const LANGUAGE_KEYS[] = {"hangul", "hanja", "ro", "katahira", "yen", "henkan", "muhenkan", "hash", "bslash_iso"};

// A line of the editor that takes room only when it has something to say (no empty band above the bottom row)
static void showMessage(QLabel* label, const QString& text){
    label->setText(text);
    label->setVisible(!text.isEmpty());
}

// The dot of Record: dark red while stopped, light red while recording
static QIcon recordDot(bool recording){
    const QColor c = recording ? QColor(0xff, 0x5a, 0x5a) : QColor(0x8b, 0x10, 0x10);
    QPixmap p(16, 16);
    p.fill(Qt::transparent);
    QPainter g(&p);
    g.setRenderHint(QPainter::Antialiasing);
    g.setPen(c.darker(130));
    g.setBrush(c);
    g.drawEllipse(2, 2, 12, 12);
    return QIcon(p);
}

// The data of an entry of the Run selector: start condition and run type of the PROFILE.DAT entry
static unsigned runData(uint8_t start, uint8_t run){
    return unsigned(start) << 8 | run;
}

HwBindWidget::HwBindWidget(QWidget* parent) : QWidget(parent), keyWidget(new KeyWidget(this)), pickerWidget(new KeyWidget(this)),
    ui(new Ui::HwBindWidget){
    ui->setupUi(this);
    // What hwbindwidget.ui cannot say: the spacing of the style between the controls of a row, the keyboard in its holder, the
    // items with data, the ranges, the table's header and drops, the menu of the events, the lines that show only when they say
    // something
    const int spacing = qMax(0, style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing));
    rowSpacing = spacing;
    for(QBoxLayout* row : {static_cast<QBoxLayout*>(ui->typeRow), static_cast<QBoxLayout*>(ui->textDelayLayout),
                           static_cast<QBoxLayout*>(ui->macroRightLayout), static_cast<QBoxLayout*>(ui->textRow)})
        row->setSpacing(spacing);
    ui->macroGrid->setSpacing(spacing);
    for(QWidget* line : {static_cast<QWidget*>(ui->banner), static_cast<QWidget*>(ui->recreateButton), ui->textDelayBox,
                         static_cast<QWidget*>(ui->textWhy), static_cast<QWidget*>(ui->flagsLabel),
                         static_cast<QWidget*>(ui->errorLabel)})
        line->hide();
    connect(ui->recreateButton, &QPushButton::clicked, this, [this](){ emit recreateRequested(!recreateActive); });

    keyWidget->setObjectName("deviceKeys");
    keyWidget->rgbMode(false);
    keyWidget->setFocusPolicy(Qt::ClickFocus);
    connect(keyWidget, &KeyWidget::selectionChanged, this, &HwBindWidget::newSelection);
    // The keyboard keeps its proportions in a holder whose height resizeEvent sets to what the width asks (no empty band under it)
    ui->keysHolder->setFixedHeight(KEYS_MIN);
    HwKeysLayout* keys = new HwKeysLayout(ui->keysHolder);
    keys->setContentsMargins(0, 0, 0, 0);
    keys->addWidget(keyWidget);

    // Remap: one picker of iCUE's keyboard, moved between the Keyboard and the Shortcut tab (placePicker()), each with its own
    // layout of the picker and its column; the data of the mouse buttons (the firmware's codes c8..cc) and of the Language keys
    pickerWidget->setObjectName("pickerKeys");
    pickerWidget->rgbMode(false);
    pickerWidget->setPickMode(true);
    pickerWidget->setFocusPolicy(Qt::ClickFocus);
    connect(pickerWidget, &KeyWidget::selectionChanged, this, &HwBindWidget::picked);
    for(QWidget* holder : {ui->keyboardKeys, ui->shortcutKeys}){
        HwKeysLayout* l = new HwKeysLayout(holder);
        l->setContentsMargins(0, 0, 0, 0);
    }
    ui->keyboardKeys->layout()->addWidget(pickerWidget);
    new HwPickerLayout(ui->keyboardTab, ui->keyboardKeys, ui->imitateColumn, pickerWidget, false);
    HwPickerLayout* shortcutRow = new HwPickerLayout(ui->shortcutTab, ui->shortcutKeys, ui->shortcutColumn, pickerWidget, true);
    // (Breeze draws the frame of a push button one pixel inside its widget: Clear one pixel lower, its frame on the last dark row)
    shortcutRow->inset = style()->objectName().compare("breeze", Qt::CaseInsensitive) == 0 ? 1 : 0;
    connect(ui->remapTabs, &QTabWidget::currentChanged, this, &HwBindWidget::remapTabChanged);
    connect(ui->imitateCheck, &QCheckBox::toggled, this, &HwBindWidget::imitateChanged);
    connect(ui->imitateMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &HwBindWidget::imitateChanged);
    for(QPushButton* b : {ui->winButton, ui->ctrlButton, ui->altButton, ui->shiftButton})
        connect(b, &QPushButton::toggled, this, &HwBindWidget::shortcutChanged);
    connect(ui->clearShortcutButton, &QPushButton::clicked, this, &HwBindWidget::clearShortcut);
    const QList<QAbstractButton*> mouse = {ui->mouse1Radio, ui->mouse2Radio, ui->mouse3Radio, ui->mouse4Radio, ui->mouse5Radio};
    for(int m = 0; m < mouse.size(); ++m)
        ui->mouseGroup->setId(mouse[m], int(K95PKeyTable::MOUSE_FIRST) + m);
    for(int i = 0; i < ui->languageBox->count(); ++i){
        languageNames << ui->languageBox->itemText(i);
        ui->languageBox->setItemData(i, K95PKeyTable::indexOf(LANGUAGE_KEYS[i]));
    }
    resetRemap();

    ui->textDelay->setRange(0, int(HwBindEdit::TEXT_DELAY_MAX));
    ui->selectedKeys->setContentsMargins(0, 0, 3, 0);   // the text ends where the frame of Record ends, not on its shadow

    // Macro: Delay is a constant delay and the lower bound of a random
    // one, Max its upper bound, never below Delay; iCUE's four alternatives of when the macro runs in one selector (start condition
    // and run type of the PROFILE.DAT entry), Record's dot, the event table under Release from the row of Max, at least three rows
    ui->delaySpin->setRange(0, int(HwBindEdit::MACRO_DELAY_MAX));
    ui->maxSpin->setRange(0, int(HwBindEdit::MACRO_DELAY_MAX));
    connect(ui->delaySpin, QOverload<int>::of(&QSpinBox::valueChanged), ui->maxSpin, &QSpinBox::setMinimum);
    ui->runBox->addItem(tr("On press"), runData(HwBindEdit::START_PRESS, HwBindEdit::RUN_ONCE));
    ui->runBox->addItem(tr("On release"), runData(HwBindEdit::START_RELEASE, HwBindEdit::RUN_ONCE));
    ui->runBox->addItem(tr("While pressed"), runData(HwBindEdit::START_PRESS, HwBindEdit::RUN_WHILE_PRESSED));
    ui->runBox->addItem(tr("Toggle"), runData(HwBindEdit::START_PRESS, HwBindEdit::RUN_TOGGLE));
    ui->recordButton->setIcon(recordDot(false));
    ui->macroTable->horizontalHeader()->setSectionsClickable(false);
    ui->macroTable->setColumnWidth(0, 130);
    ui->macroTable->setColumnWidth(1, 140);
    ui->macroTable->setColumnWidth(2, 100);
    ui->macroTable->setMinimumHeight(ui->macroTable->horizontalHeader()->sizeHint().height() +
                                     3 * ui->macroTable->verticalHeader()->defaultSectionSize() + 2 * ui->macroTable->frameWidth());
    ui->macroTable->viewport()->setAcceptDrops(true);
    ui->macroTable->dropped = [this](int to){ dropMacroRows(to); };
    // The context menu of the selected rows, also as shortcuts of the table
    macroMenu = new QMenu(this);
    copyAction = macroMenu->addAction(tr("Copy"), this, &HwBindWidget::copyMacroRows);
    pasteAction = macroMenu->addAction(tr("Paste below"), this, &HwBindWidget::pasteMacroRows);
    macroMenu->addSeparator();
    upAction = macroMenu->addAction(tr("Move up"), this, [this](){ moveMacroRows(-1); });
    downAction = macroMenu->addAction(tr("Move down"), this, [this](){ moveMacroRows(1); });
    macroMenu->addSeparator();
    removeAction = macroMenu->addAction(tr("Remove"), this, &HwBindWidget::removeMacroRows);
    copyAction->setShortcut(QKeySequence::Copy);
    pasteAction->setShortcut(QKeySequence::Paste);
    upAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up));
    downAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Down));
    removeAction->setShortcut(QKeySequence::Delete);
    for(QAction* a : {copyAction, pasteAction, upAction, downAction, removeAction}){
        a->setShortcutContext(Qt::WidgetShortcut);
        ui->macroTable->addAction(a);
    }
    connect(ui->macroTable, &QWidget::customContextMenuRequested, this, [this](const QPoint& at){
        updateMacroActions();
        macroMenu->popup(ui->macroTable->viewport()->mapToGlobal(at));
    });
    connect(ui->macroTable, &QTableWidget::itemSelectionChanged, this, &HwBindWidget::updateMacroActions);
    connect(ui->recordButton, &QPushButton::clicked, this, [this](){ emit recordRequested(!isRecording); });
    connect(ui->pressButton, &QPushButton::clicked, this, &HwBindWidget::addPress);
    connect(ui->releaseButton, &QPushButton::clicked, this, &HwBindWidget::addRelease);
    connect(ui->delayButton, &QPushButton::clicked, this, &HwBindWidget::addDelay);
    connect(ui->varDelayButton, &QPushButton::clicked, this, &HwBindWidget::addVarDelay);

    // Text: a field of several lines in the place of the Macro page's table, with its height; Enter starts a new line, Tab types a tab (Ctrl+Tab moves the focus); the
    // delay is on the row of the type, or at the top right of the field when that row has no room
    ui->textEdit->installEventFilter(this);   // the keys that would type past TEXT_CHARS_MAX
    // A text typed in the field is no longer the one rebuilt from the key's events (the rebuild sets the flag again after filling it)
    connect(ui->textEdit, &QPlainTextEdit::textChanged, this, [this](){
        limitText();
        textRebuilt = false;
        updateTextPreview();
    });
    connect(ui->textDelay, QOverload<int>::of(&QSpinBox::valueChanged), this, &HwBindWidget::updateTextPreview);

    connect(ui->typeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &HwBindWidget::typeChanged);
    connect(ui->applyButton, &QPushButton::clicked, this, [this](){ apply(); });
    connect(ui->resetButton, &QPushButton::clicked, this, &HwBindWidget::reset);
    connect(ui->copyButton, &QPushButton::clicked, this, [this](){ emit copyRequested(currentSelection); });

    // The keys a macro can use: every key of the files that has a name
    for(unsigned k = 0; k < K95PKeyTable::KEYS; ++k){
        const std::string label = K95PKeyTable::label(k);
        if(label.empty() || label[0] == '#') continue;
        ui->keyChooser->addItem(QString::fromStdString(label), k);
    }
    uniformControls();
    updateEnabled();
}

void HwBindWidget::uniformControls(){
    // One height for every control, one width for every button (the widest, with every text it shows: Record and Stop, Delay and Set
    // delays, Var. Delay and Set Var. Delays), one width for the selectors and the delays (the controls of a row
    // are of one size). Then the columns: "Type:", "Key:",
    // "Delay:" and "Max:" in one column; the type selector from the left edge of the key selector to the right edge of Press;
    // "Selected:" from the column of "Run:" to the right edge of Record, on every page
    const QList<QPushButton*> buttons = {ui->pressButton, ui->releaseButton, ui->delayButton, ui->varDelayButton, ui->recordButton, ui->copyButton,
                                         ui->resetButton, ui->applyButton};
    const QList<QComboBox*> selectors = {ui->keyChooser, ui->runBox};
    QList<QWidget*> all = {ui->typeBox, ui->delaySpin, ui->maxSpin, ui->textDelay, ui->selectedKeys};
    for(QPushButton* b : buttons) all << b;
    for(QComboBox* c : selectors) all << c;
    int height = 0;
    for(QWidget* w : all) height = qMax(height, w->sizeHint().height());
    for(QWidget* w : all) w->setFixedHeight(height);
    int width = 0;
    for(QPushButton* b : buttons) width = qMax(width, b->sizeHint().width());
    ui->recordButton->setText(tr("Stop"));
    width = qMax(width, ui->recordButton->sizeHint().width());
    ui->recordButton->setText(tr("Record"));
    ui->delayButton->setText(tr("Set delays"));
    width = qMax(width, ui->delayButton->sizeHint().width());
    ui->delayButton->setText(tr("Delay"));
    ui->varDelayButton->setText(tr("Set Var. Delays"));
    width = qMax(width, ui->varDelayButton->sizeHint().width());
    ui->varDelayButton->setText(tr("Var. Delay"));
    width += 8;
    for(QPushButton* b : buttons) b->setFixedWidth(width);
    int selectorWidth = 0;
    for(QComboBox* c : selectors) selectorWidth = qMax(selectorWidth, c->sizeHint().width());
    for(QComboBox* c : selectors) c->setFixedWidth(selectorWidth);
    ui->delaySpin->setFixedWidth(selectorWidth);
    ui->maxSpin->setFixedWidth(selectorWidth);
    ui->textDelay->setFixedWidth(selectorWidth);
    const int spacing = rowSpacing;
    // ("Button:" of the Mouse tab counts, as when it was in this column)
    int labelWidth = ui->mouseLabel->sizeHint().width();
    for(QLabel* l : {ui->typeLabel, ui->macroKeyLabel, ui->delayLabel, ui->maxLabel}) labelWidth = qMax(labelWidth, l->sizeHint().width());
    // (and the height of the controls, so that each label is centred on its selector)
    for(QLabel* l : {ui->typeLabel, ui->macroKeyLabel, ui->delayLabel, ui->maxLabel}){ l->setFixedWidth(labelWidth); l->setFixedHeight(height); }
    ui->typeBox->setFixedWidth(selectorWidth + spacing + width);
    ui->languageLabel->setFixedHeight(ui->languageBox->sizeHint().height());
    // (spacing goes only between two widgets, not around the fixed space before Record)
    ui->selectedBlock->setFixedWidth(ui->runLabel->sizeHint().width() + spacing + selectorWidth + spacing + 12 + width);
    // Breeze draws the frame of a spin box one pixel above those of the buttons and selectors: one pixel shorter, at the bottom
    if(style()->objectName().compare("breeze", Qt::CaseInsensitive) == 0){
        for(QSpinBox* d : {ui->delaySpin, ui->maxSpin}){
            d->setFixedHeight(height - 1);
            ui->macroGrid->setAlignment(d, Qt::AlignBottom);
        }
        ui->textDelay->setFixedHeight(height - 1);
        ui->textDelayBox->layout()->setAlignment(ui->textDelay, Qt::AlignBottom);
    }
}

void HwBindWidget::resizeEvent(QResizeEvent* e){
    QWidget::resizeEvent(e);
    arrange();
    fitKeyboard();
}

void HwBindWidget::showEvent(QShowEvent* e){
    QWidget::showEvent(e);
    arrange();
    fitKeyboard();
}

bool HwBindWidget::event(QEvent* e){
    // The minimum of the editor changes without a change of size when a line of a message appears or goes (the flags of a key, an
    // error): the keyboard is fitted again (the same height again changes nothing, and asks for no new layout)
    const bool done = QWidget::event(e);
    if(e->type() == QEvent::LayoutRequest)
        fitKeyboard();
    return done;
}

void HwBindWidget::arrange(){
    // The Text's delay next to the type selector, or next to the text field when the row of the type has no room for it (the Macro
    // page has one layout at every width)
    const int avail = width();
    const int typeRowWide = ui->typeLabel->width() + ui->typeBox->width() + 12 + ui->textDelayBox->sizeHint().width() + ui->selectedBlock->width() +
                            4 * rowSpacing;
    const bool narrowText = avail < typeRowWide;
    QLayout* now = ui->textDelayBox->parentWidget() == ui->pages->widget(TEXT) ? static_cast<QLayout*>(ui->textRow) : ui->typeRow;
    QBoxLayout* want = narrowText ? static_cast<QBoxLayout*>(ui->textRow) : static_cast<QBoxLayout*>(ui->typeRow);
    if(now != want){
        now->removeWidget(ui->textDelayBox);
        if(narrowText) ui->textRow->addWidget(ui->textDelayBox, 0, Qt::AlignTop);
        else ui->typeRow->insertWidget(ui->typeRow->indexOf(ui->selectedBlock) - 1, ui->textDelayBox);
    }
}

void HwBindWidget::fitKeyboard(){
    // The editor under the keyboard keeps its minimum height; the keyboard gets the height its proportions want at the editor's
    // width, or what is left above that minimum (then it is narrower, centred), less what the picker of the Remap type needs above
    // the minimum of its tab (pickerRoom()); the event table takes the rest. The minimum the editor declares counts the keyboard at
    // KEYS_MIN (minimumSizeHint), so it can always be made smaller again
    // (the rows above the keyboard counted one by one: the layout's own minimum is cached, and right after a change of the keyboard's
    // height it still counts the old one, which put the keyboard over the rows under it)
    const int boxMin = ui->editorBox->minimumSizeHint().height();
    QLayout* top = layout();
    int others = 0, items = 0;
    for(int i = 0; i < top->count(); ++i){
        QLayoutItem* item = top->itemAt(i);
        if(item->isEmpty()) continue;
        ++items;
        if(item->widget() != ui->keysHolder && item->widget() != ui->editorBox) others += item->minimumSize().height();
    }
    others += qMax(0, items - 1) * qMax(0, static_cast<QBoxLayout*>(top)->spacing());
    const int room = height() - others - boxMin;
    // the room of the picker of the Remap type kept on every type, so the device keyboard has one height whatever the type
    const int picker = pickerRoom(room);
    const int h = qMax(int(KEYS_MIN), qMin(keyWidget->heightForWidth(width()), room - picker));
    if(h != ui->keysHolder->height() || ui->keysHolder->minimumHeight() != h){
        ui->keysHolder->setFixedHeight(h);
    }
}

int HwBindWidget::pickerRoom(int room) const {
    // The height the picker needs above the height of its tab with the editor at its minimum, when the device keyboard and it share
    // room above that minimum: as wide as the tab leaves next to the column of options, and no larger than with keys of the size of
    // the device keyboard's (at 730x520 the picker 440 px wide next to the column of 240, the device keyboard what is left)
    if(keyWidget->map().width() <= 0 || pickerWidget->map().width() <= 0)
        return 0;
    const QSize around = pickerAround();
    const int wide = width() - around.width() - HwPickerLayout::SPACING - ui->imitateColumn->minimumWidth();
    const int widest = wide > 0 ? pickerWidget->heightForWidth(wide) : 0;
    const int tabMin = ui->pages->minimumSizeHint().height() - around.height();
    // with keys of the same size the picker is r times as wide as the device keyboard: the device's part of the height they share
    const double r = double(pickerWidget->map().width()) / keyWidget->map().width();
    const double device = 1. / (1. + keyWidget->aspectRatio() * r / pickerWidget->aspectRatio());
    const int same = int((room + tabMin) * (1. - device));
    return qMax(0, qMin(widest, same) - tabMin);
}

QSize HwBindWidget::pickerAround() const {
    // The room the tabs of the Remap type take around the picker and its column (the tab bar, the frames, the margins of the tab),
    // from the size hints: the height 1 or 2 pixels more than as laid out (Breeze, Fusion), which leaves the picker its width
    const QStackedWidget* inner = ui->remapTabs->findChild<QStackedWidget*>();
    const QMargins m = ui->keyboardTab->layout()->contentsMargins();
    return ui->remapTabs->minimumSizeHint() - (inner ? inner->minimumSizeHint() : QSize(0, 0)) +
           QSize(m.left() + m.right(), m.top() + m.bottom());
}

QSize HwBindWidget::minimumSizeHint() const {
    // The minimum with the keyboard at its smallest and the Text's delay by its field (arrange(): the layout's own minimum counts it
    // where it is, next to the type selector when the editor is wide)
    QSize s = QWidget::minimumSizeHint();
    s.setHeight(s.height() - ui->keysHolder->minimumHeight() + KEYS_MIN);
    const int typeRow = ui->typeLabel->width() + ui->typeBox->width() + ui->selectedBlock->width() + 3 * rowSpacing;
    s.setWidth(qMin(s.width(), qMax(qMax(ui->pages->minimumSizeHint().width(), ui->buttonsRow->minimumSize().width()), typeRow)));
    return s;
}


bool HwBindWidget::eventFilter(QObject* watched, QEvent* e){
    // While recording, the keys typed for the macro do nothing in the GUI (a Space would press Stop, which had the focus)
    if(isRecording){
        if(e->type() == QEvent::ShortcutOverride){ e->accept(); return true; }
        if(e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease) return true;
    }
    // A key that would type a character into a full Text field (nothing selected) does nothing, rather than being typed and cut:
    // an undo step that changes nothing is not left behind. Pastes and input methods are cut by limitText()
    if(watched == ui->textEdit && e->type() == QEvent::KeyPress){
        const QKeyEvent* k = static_cast<const QKeyEvent*>(e);
        const bool typing = !(k->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) &&
                            (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter || k->key() == Qt::Key_Tab ||
                             (!k->text().isEmpty() && k->text().at(0).isPrint()));
        const int allowed = std::max(int(HwBindEdit::TEXT_CHARS_MAX), textAccepted);
        if(typing && !ui->textEdit->textCursor().hasSelection() && int(ui->textEdit->toPlainText().size()) >= allowed)
            return true;
    }
    return QWidget::eventFilter(watched, e);
}

HwBindWidget::~HwBindWidget(){
    delete ui;
}

QString HwBindWidget::keyName(unsigned index) const {
    const QString label = QString::fromStdString(K95PKeyTable::label(index));
    const Key key = keyMap.key(label);
    return key ? key.friendlyName(false).split("\n")[0] : label;
}

int HwBindWidget::indexOf(const QString& key) const {
    const int i = K95PKeyTable::indexOf(key.toStdString());
    return i >= 0 && unsigned(i) < K95PKeyTable::KEYS ? i : -1;
}

const Action* HwBindWidget::baseAction(int index) const {
    if(!base || index < 0 || (base->state != HwBinding::Record::OK && base->state != HwBinding::Record::EMPTY))
        return nullptr;
    return &base->keys[size_t(index)];
}

void HwBindWidget::setSlot(const HwBinding::Record* newBase, HwSlotDraft::Draft* newDraft, const KeyMap& map, const QString& layout,
                           bool isEditable, const QString& why){
    base = newBase;
    draft = newDraft;
    keyMap = map;
    editable = isEditable && draft && draft->hasBindings;
    keyWidget->map(map);
    // iCUE's keyboard with the labels of the device layout; its keys in the colours of what they do on their own (media green)
    const KeyMap picker = HwPickerMap::k95p(map);
    pickerWidget->map(picker);
    KeyWidget::BindMap defaults;
    for(const QString& key : picker.keys())
        defaults[key] = KbBind::defaultAction(key, picker.model());
    pickerWidget->bindMap(defaults);
    // the two Non-US Language keys with what they type on the device layout, if it has them
    for(int i = 0; i < languageNames.size(); ++i){
        const Key k = map.key(LANGUAGE_KEYS[i]);
        const bool label = (!strcmp(LANGUAGE_KEYS[i], "hash") || !strcmp(LANGUAGE_KEYS[i], "bslash_iso")) && k;
        ui->languageBox->setItemText(i, label ? QString("%1 (%2)").arg(languageNames[i], k.friendlyName(false)) : languageNames[i]);
    }
    ui->banner->setText(why);
    ui->banner->setVisible(!why.isEmpty());
    // A new Text is typed with the layout of the device
#ifdef USE_XKBCOMMON
    if(layout != xkbLayout || !xkb){
        xkbLayout = layout;
        std::string name, variant;
        xkb.reset();
        // Right Alt only on the keys of the Windows layout with the same name (none when there is no table for it)
        std::vector<std::pair<int, bool>> altgr;
        std::vector<int> skip;
        windowsTable = HwBindEdit::windowsAltGr(layout.toStdString(), altgr, &skip);
        if(HwBindEdit::xkbNames(layout.toStdString(), name, variant))
            xkb = HwTextXkb::fromNames(name, variant, KeyMap::isISO(map.layout()), &altgr, &skip);
    }
#else
    Q_UNUSED(layout);
#endif
    ui->textWhy->setText(xkb ? QString() :
#ifdef USE_XKBCOMMON
                     tr("A new Text needs the XKB layout of the keyboard, which could not be loaded.")
#else
                     tr("A new Text is not available in this build.")
#endif
                     );
    ui->textWhy->setVisible(!xkb);   // empty, it would take a row under the field
    const KeyMap::Layout l = KeyMap::getLayout(layout);
    layoutName = l >= 0 && int(l) < KeyMap::layoutList.count() ? KeyMap::layoutList.at(int(l)) : layout;
    keyWidget->clearSelection();
    currentSelection.clear();
    showMessage(ui->errorLabel, QString());
    ui->noticeLabel->clear();
    refreshKeys();
    loadPanel();
    // The map sets the keyboard's proportions: its height again (the editor can be shown before the first slot comes)
    fitKeyboard();
}

void HwBindWidget::setRecreate(bool offered, bool active){
    recreateActive = active;
    ui->recreateButton->setText(active ? tr("Keep the bindings the slot has") : tr("Recreate the bindings from scratch..."));
    ui->recreateButton->setVisible(offered);
    updateEnabled();
}

bool HwBindWidget::recreateOffered() const {
    return !ui->recreateButton->isHidden();
}

void HwBindWidget::setControlsEnabled(bool e){
    controls = e;
    updateEnabled();
}

void HwBindWidget::select(const QStringList& keys){
    keyWidget->setSelection(keys);
    newSelection(keys);
}

void HwBindWidget::newSelection(const QStringList& keys){
    currentSelection = keys;
    showMessage(ui->errorLabel, QString());
    ui->noticeLabel->clear();
    loadPanel();
}

void HwBindWidget::refreshKeys(){
    // The keyboard shows the draft: a key that does what it does on its own is white, a remapped one yellow, a macro blue
    KeyWidget::BindMap actions;
    for(const QString& key : keyMap.keys()){
        const QString def = KbBind::defaultAction(key, keyMap.model());
        const int i = indexOf(key);
        if(!draft || !draft->hasBindings || i < 0){
            actions[key] = def;
            continue;
        }
        const Action& a = draft->keys[size_t(i)];
        if(a.kind == Action::REMAP)
            actions[key] = QString::fromStdString(K95PKeyTable::label(a.dst));
        else if(a.kind == Action::MACRO)
            actions[key] = "$hwmacro";
        else
            actions[key] = def;
    }
    keyWidget->bindMap(actions);
}

HwBindWidget::Type HwBindWidget::type() const {
    return Type(ui->typeBox->currentIndex());
}

void HwBindWidget::setType(Type t){
    ui->typeBox->setCurrentIndex(int(t));
}

void HwBindWidget::typeChanged(int index){
    ui->pages->setCurrentIndex(index);
    ui->textDelayBox->setVisible(index == TEXT);
    ui->noticeLabel->clear();
    if(index == TEXT)
        updateTextPreview();
    updateEnabled();
}

void HwBindWidget::loadPanel(){
    // The panel shows the action of the first selected key (Default when there is none)
    showMessage(ui->flagsLabel, QString());
    int index = -1;
    for(const QString& key : currentSelection)
        if((index = indexOf(key)) >= 0) break;
    const int count = currentSelection.count();
    if(count == 0)
        ui->selectedKeys->setText(tr("none"));
    else if(count == 1)
        ui->selectedKeys->setText(index >= 0 ? keyName(unsigned(index)) : tr("%1 (not in the files)").arg(currentSelection[0]));
    else
        ui->selectedKeys->setText(tr("%1 keys selected").arg(count));
    Action a;
    if(draft && draft->hasBindings && index >= 0)
        a = draft->keys[size_t(index)];
    rows.clear();
    resetRemap();
    RemapTab tab = KEYBOARD_TAB;
    fillText(QString());
    ui->textDelay->setValue(0);
    textRebuilt = false;
    Type t = DEFAULT;
    // The event table always holds the events of a macro of any kind: a Text or a Shortcut can be turned into a Macro
    if(a.kind == Action::MACRO){
        HwBindEdit::rows(a.events, rows);
        // a combination iCUE does not offer shows no entry (the macro is kept as read: it can be replaced, not edited)
        ui->runBox->setCurrentIndex(ui->runBox->findData(runData(a.start, a.run)));
    }
    // The Remap type opens on the tab of the action (iCUE's: the Keyboard tab for a key of its keyboard or a media key, also with
    // 'Imitate holding key'; a mouse button; its Keystroke; a Language key)
    switch(HwBindEdit::kindOf(a)){
    case HwBindEdit::NATIVE:
        break;
    case HwBindEdit::REMAP_KEYBOARD:
        t = REMAP;
        keyboardPick = a.dst;
        break;
    case HwBindEdit::REMAP_MOUSE:
        t = REMAP;
        tab = MOUSE_TAB;
        if(QAbstractButton* b = ui->mouseGroup->button(a.dst)) b->setChecked(true);
        break;
    case HwBindEdit::REMAP_LANGUAGE:
        t = REMAP;
        tab = LANGUAGE_TAB;
        ui->languageBox->setCurrentIndex(ui->languageBox->findData(int(a.dst)));
        break;
    case HwBindEdit::IMITATE_PRESS:
    case HwBindEdit::IMITATE_TOGGLE: {
        t = REMAP;
        uint8_t key = 0;
        unsigned ms = 0;
        bool toggle = false;
        HwBindEdit::recognizeImitate(a, key, ms, toggle);
        keyboardPick = key;
        const QSignalBlocker b1(ui->imitateCheck), b2(ui->imitateMode);
        ui->imitateCheck->setChecked(true);
        ui->imitateMode->setCurrentIndex(toggle ? 1 : 0);
        if(!toggle) ui->terminateSpin->setValue(ms / 1000.);
        break;
    }
    case HwBindEdit::MACRO:
        t = MACRO;
        break;
    case HwBindEdit::SHORTCUT: {
        t = REMAP;
        tab = SHORTCUT_TAB;
        std::vector<uint8_t> keys;
        HwBindEdit::recognizeShortcut(a.events, keys);
        QStringList names;
        for(uint8_t k : keys) names << QString::fromStdString(K95PKeyTable::label(k));
        fillShortcut(names);
        break;
    }
    case HwBindEdit::TEXT: {
        t = TEXT;
        // The text the events type with the layout of the device, rebuilt from them
        std::vector<HwTextShape::Stroke> strokes;
        unsigned ms = 0;
        HwTextShape::recognize(a.events, strokes, ms);
        QString text;
#ifdef USE_XKBCOMMON
        for(const HwTextShape::Stroke& s : strokes){
            const char32_t c = xkb ? xkb->character(s) : 0;
            text += c ? QString::fromUcs4(&c, 1) : QString("?");
        }
#else
        text = QString(int(strokes.size()), QChar('?'));
#endif
        fillText(text);
        ui->textDelay->setValue(int(ms));
        textRebuilt = true;
        break;
    }
    case HwBindEdit::KEPT:
        t = KEPT;
        break;
    }
    // A macro whose values iCUE does not write is kept as it was read: it can be replaced, not edited
    if(a.kind == Action::MACRO && !HwBindEdit::icueValues(a.subtype, a.start, a.run, a.repeat))
        t = KEPT;
    ui->keptLabel->setText(a.kind == Action::MACRO ?
        tr("A macro kept as the slot has it (subtype %1, start %2, run %3, repeat %4, %5 events): it can be replaced, not edited.")
            .arg(a.subtype).arg(a.start, 2, 16, QChar('0')).arg(a.run).arg(a.repeat).arg(a.events.size()) :
        a.kind == Action::REMAP ?
        tr("A remap to %1, a key iCUE does not offer, kept as the slot has it: it can be replaced, not edited.")
            .arg(QString::fromStdString(K95PKeyTable::label(a.dst))) : QString());
    const Action* b = baseAction(index);
    if(b && count == 1){
        QStringList flags;
        if(b->flags & HwBinding::NON_ICUE) flags << tr("not in the form iCUE writes");
        if(b->flags & HwBinding::SHARED) flags << tr("its file is shared with other keys");
        if(b->flags & HwBinding::LOCKED) flags << tr("values iCUE does not offer: kept as read");
        if(!flags.isEmpty()) showMessage(ui->flagsLabel, tr("In the slot: %1.").arg(flags.join(", ")));
    }
    refreshMacroList();
    {
        const QSignalBlocker block(ui->remapTabs);
        ui->remapTabs->setCurrentIndex(tab);
    }
    placePicker();
    refreshShortcut();
    const bool block = ui->typeBox->blockSignals(true);
    ui->typeBox->setCurrentIndex(int(t));
    ui->typeBox->blockSignals(block);
    ui->pages->setCurrentIndex(int(t));
    ui->textDelayBox->setVisible(t == TEXT);
    if(t == TEXT)
        updateTextPreview();
    updateEnabled();
}

void HwBindWidget::updateEnabled(){
    if(isRecording){
        // Only Stop
        for(QWidget* w : {static_cast<QWidget*>(ui->typeBox), static_cast<QWidget*>(ui->applyButton), static_cast<QWidget*>(ui->resetButton),
                          static_cast<QWidget*>(ui->copyButton), static_cast<QWidget*>(keyWidget)})
            w->setEnabled(false);
        for(QWidget* child : ui->pages->widget(MACRO)->findChildren<QWidget*>())
            child->setEnabled(child == ui->recordButton || child->isAncestorOf(ui->recordButton) || child == ui->macroTable ||
                              ui->macroTable->isAncestorOf(child));
        ui->recordButton->setText(tr("Stop"));
        ui->recordButton->setIcon(recordDot(true));
        updateMacroActions();
        return;
    }
    ui->recordButton->setText(tr("Record"));
    ui->recordButton->setIcon(recordDot(false));
    keyWidget->setEnabled(true);
    for(QWidget* child : ui->pages->widget(MACRO)->findChildren<QWidget*>())
        child->setEnabled(true);
    const bool on = editable && controls;
    const bool selected = on && !currentSelection.isEmpty();
    ui->typeBox->setEnabled(selected);
    ui->pages->setEnabled(selected);
    // "Kept as read" is only what a key already has
    if(QStandardItemModel* model = qobject_cast<QStandardItemModel*>(ui->typeBox->model()))
        if(QStandardItem* item = model->item(KEPT))
            item->setEnabled(false);
    ui->pages->widget(TEXT)->setEnabled(selected && xkb);
    ui->textDelayBox->setEnabled(selected && xkb);
    ui->applyButton->setEnabled(selected && type() != KEPT);
    ui->recreateButton->setEnabled(controls);
    ui->resetButton->setEnabled(selected && base);
    ui->copyButton->setEnabled(on);
    updateMacroActions();
}

void HwBindWidget::refreshMacroList(){
    ui->macroTable->clearContents();
    ui->macroTable->setRowCount(int(rows.size()));
    for(int i = 0; i < int(rows.size()); ++i){
        const Row& r = rows[size_t(i)];
        const bool isDelay = r.type == Row::DELAY || r.type == Row::RANDOM;
        const QString what = r.type == Row::PRESS ? tr("\u2193  Press") : r.type == Row::RELEASE ? tr("\u2191  Release") :
                             r.type == Row::RANDOM ? tr("\u23f1  Var. delay") : tr("\u23f1  Delay");
        ui->macroTable->setItem(i, 0, new QTableWidgetItem(what));
        ui->macroTable->setItem(i, 1, new QTableWidgetItem(isDelay ? QString() : keyName(r.value)));
        QTableWidgetItem* ms = new QTableWidgetItem(r.type == Row::DELAY ? tr("%1 ms").arg(r.value) :
                                                    r.type == Row::RANDOM ? tr("%1 - %2 ms").arg(r.value).arg(r.max) : QString());
        ms->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        ui->macroTable->setItem(i, 2, ms);
    }
    updateMacroActions();
}

std::pair<int, int> HwBindWidget::macroSelection() const {
    // The selected rows are one block (ContiguousSelection): its first row and its size, (-1, 0) for none
    int first = -1, last = -1;
    for(const QModelIndex& index : ui->macroTable->selectionModel()->selectedRows()){
        if(first < 0 || index.row() < first) first = index.row();
        if(index.row() > last) last = index.row();
    }
    return first < 0 ? std::make_pair(-1, 0) : std::make_pair(first, last - first + 1);
}

void HwBindWidget::selectMacroRows(int first, int count){
    ui->macroTable->clearSelection();
    if(first < 0 || count <= 0 || first >= int(rows.size()))
        return;
    const int last = qMin(first + count, int(rows.size())) - 1;
    ui->macroTable->setRangeSelected(QTableWidgetSelectionRange(first, 0, last, 2), true);
    ui->macroTable->setCurrentCell(last, 0, QItemSelectionModel::NoUpdate);
    ui->macroTable->scrollToItem(ui->macroTable->item(last, 0));
}

void HwBindWidget::updateMacroActions(){
    const bool on = editable && controls && !isRecording && !currentSelection.isEmpty();
    const std::pair<int, int> sel = macroSelection();
    const bool selected = on && sel.first >= 0;
    copyAction->setEnabled(selected);
    pasteAction->setEnabled(on && !macroClipboard.empty());
    upAction->setEnabled(selected && sel.first > 0);
    downAction->setEnabled(selected && sel.first + sel.second < int(rows.size()));
    removeAction->setEnabled(selected);
    // With delays among the selected rows, Delay sets them all to the value of its box (as iCUE after a recording); otherwise it inserts a new delay. Two delays in a row: insert one elsewhere and move it, or copy and paste
    const bool setting = selectedDelays() > 0;
    ui->delayButton->setText(setting ? tr("Set delays") : tr("Delay"));
    ui->delayButton->setToolTip(setting ? tr("Sets every selected delay to the value of Delay") : tr("Inserts a delay of the value of Delay below the selected rows"));
    // and Var. Delay a random delay from Delay to Max (iCUE's random delay), or every selected delay to that
    ui->varDelayButton->setText(setting ? tr("Set Var. Delays") : tr("Var. Delay"));
    ui->varDelayButton->setToolTip(setting ? tr("Sets every selected delay to a random delay from Delay to Max")
                                           : tr("Inserts a random delay from Delay to Max below the selected rows"));
}

int HwBindWidget::selectedDelays() const {
    const std::pair<int, int> sel = macroSelection();
    int n = 0;
    for(int i = sel.first; i >= 0 && i < sel.first + sel.second && i < int(rows.size()); ++i)
        n += rows[size_t(i)].type == Row::DELAY || rows[size_t(i)].type == Row::RANDOM;
    return n;
}

void HwBindWidget::copyMacroRows(){
    const std::pair<int, int> sel = macroSelection();
    if(sel.first < 0) return;
    macroClipboard.assign(rows.begin() + sel.first, rows.begin() + sel.first + sel.second);
    updateMacroActions();
}

void HwBindWidget::pasteMacroRows(){
    // Below the selected rows, or at the end with none
    if(macroClipboard.empty() || isRecording) return;
    const std::pair<int, int> sel = macroSelection();
    const size_t at = HwBindEdit::insertRows(rows, sel.first < 0 ? rows.size() : size_t(sel.first + sel.second), macroClipboard);
    refreshMacroList();
    selectMacroRows(int(at), int(macroClipboard.size()));
}

void HwBindWidget::removeMacroRows(){
    const std::pair<int, int> sel = macroSelection();
    if(sel.first < 0 || isRecording) return;
    rows.erase(rows.begin() + sel.first, rows.begin() + sel.first + sel.second);
    refreshMacroList();
    selectMacroRows(qMin(sel.first, int(rows.size()) - 1), 1);
}

void HwBindWidget::moveMacroRows(int by){
    const std::pair<int, int> sel = macroSelection();
    if(sel.first < 0 || isRecording || by == 0) return;
    // up: before the row above the block; down: before the row after the one below it
    const int to = by < 0 ? sel.first + by : sel.first + sel.second + by;
    if(to < 0 || to > int(rows.size())) return;
    const size_t at = HwBindEdit::moveRows(rows, size_t(sel.first), size_t(sel.second), size_t(to));
    refreshMacroList();
    selectMacroRows(int(at), sel.second);
}

void HwBindWidget::dropMacroRows(int to){
    const std::pair<int, int> sel = macroSelection();
    if(sel.first < 0 || isRecording || !editable || !controls || to < 0) return;
    const size_t at = HwBindEdit::moveRows(rows, size_t(sel.first), size_t(sel.second), size_t(to));
    refreshMacroList();
    selectMacroRows(int(at), sel.second);
}

int HwBindWidget::insertAt() const {
    // Below the selected rows, or at the end with none
    const std::pair<int, int> sel = macroSelection();
    return sel.first < 0 ? int(rows.size()) : sel.first + sel.second;
}

void HwBindWidget::addRow(const Row& row){
    const size_t at = HwBindEdit::insertRows(rows, size_t(insertAt()), {row});
    refreshMacroList();
    selectMacroRows(int(at), 1);
}

void HwBindWidget::addPress(){
    addRow(Row(Row::PRESS, ui->keyChooser->currentData().toUInt()));
}

void HwBindWidget::addRelease(){
    addRow(Row(Row::RELEASE, ui->keyChooser->currentData().toUInt()));
}

void HwBindWidget::addDelay(){
    const std::pair<int, int> sel = macroSelection();
    if(selectedDelays() == 0){
        addRow(Row(Row::DELAY, unsigned(ui->delaySpin->value())));
        return;
    }
    // Every selected delay to the value of the box; the selection stays, for another value
    for(int i = sel.first; i < sel.first + sel.second; ++i)
        if(rows[size_t(i)].type == Row::DELAY || rows[size_t(i)].type == Row::RANDOM)   // a random one becomes constant
            rows[size_t(i)] = Row(Row::DELAY, unsigned(ui->delaySpin->value()));
    refreshMacroList();
    selectMacroRows(sel.first, sel.second);
}

void HwBindWidget::addVarDelay(){
    const Row r(Row::RANDOM, unsigned(ui->delaySpin->value()), unsigned(ui->maxSpin->value()));
    const std::pair<int, int> sel = macroSelection();
    if(selectedDelays() == 0){
        addRow(r);
        return;
    }
    // Every selected delay, constant or random, to a random one from Delay to Max; the selection stays
    for(int i = sel.first; i < sel.first + sel.second; ++i)
        if(rows[size_t(i)].type == Row::DELAY || rows[size_t(i)].type == Row::RANDOM)
            rows[size_t(i)] = r;
    refreshMacroList();
    selectMacroRows(sel.first, sel.second);
}

void HwBindWidget::resetRemap(){
    // The Remap tabs with nothing chosen: no key picked, 'Imitate holding key' off (On press, 0.1 s), no mouse button, no Shortcut,
    // no Language key
    keyboardPick = shortcutPick = -1;
    {
        const QSignalBlocker b1(ui->imitateCheck), b2(ui->imitateMode);
        ui->imitateCheck->setChecked(false);
        ui->imitateMode->setCurrentIndex(0);
        ui->terminateSpin->setValue(ui->terminateSpin->minimum());
    }
    ui->mouseGroup->setExclusive(false);
    for(QAbstractButton* b : ui->mouseGroup->buttons()) b->setChecked(false);
    ui->mouseGroup->setExclusive(true);
    fillShortcut(QStringList());
    ui->languageBox->setCurrentIndex(-1);
}

HwBindWidget::RemapTab HwBindWidget::remapTab() const {
    return RemapTab(ui->remapTabs->currentIndex());
}

void HwBindWidget::setRemapTab(RemapTab t){
    ui->remapTabs->setCurrentIndex(int(t));
}

void HwBindWidget::remapTabChanged(int){
    showMessage(ui->errorLabel, QString());
    placePicker();
}

void HwBindWidget::placePicker(){
    // The one picker goes to the tab shown, Keyboard or Shortcut, with what that tab has picked and the keys it does not offer:
    // the media keys while 'Imitate holding key' is on (iCUE offers it on the keys of its keyboard only), the media keys and the
    // modifiers in a Shortcut (the modifiers are its buttons; iCUE's Keystroke has no media key). A second click takes back the key
    // of a Shortcut (it may have none)
    const bool shortcut = remapTab() == SHORTCUT_TAB;
    QWidget* holder = shortcut ? ui->shortcutKeys : ui->keyboardKeys;
    if(pickerWidget->parentWidget() != holder){
        holder->layout()->addWidget(pickerWidget);
        pickerWidget->show();
    }
    pickerWidget->setPickMode(true, shortcut);
    QStringList dimmed;
    for(unsigned k = 0; k < K95PKeyTable::KEYS; ++k)
        if(K95PKeyTable::icueMedia(k) && (shortcut || ui->imitateCheck->isChecked()))
            dimmed << QString::fromStdString(K95PKeyTable::label(k));
    if(shortcut)
        for(const char* m : MODIFIER_KEYS) dimmed << m;
    pickerWidget->setDimmedKeys(dimmed);
    const int pick = shortcut ? shortcutPick : keyboardPick;
    pickerFilling = true;
    pickerWidget->setSelection(pick >= 0 ? QStringList(QString::fromStdString(K95PKeyTable::label(unsigned(pick)))) : QStringList());
    pickerFilling = false;
    imitateChanged();
}

void HwBindWidget::picked(const QStringList& keys){
    // A click on the picker (not the program filling it)
    if(pickerFilling) return;
    const int i = keys.isEmpty() ? -1 : K95PKeyTable::indexOf(keys[0].toStdString());
    showMessage(ui->errorLabel, QString());
    if(remapTab() == SHORTCUT_TAB){
        shortcutPick = i;
        refreshShortcut();
    } else {
        keyboardPick = i;
        imitateChanged();
    }
}

bool HwBindWidget::pickKey(const QString& key){
    // As a click on the key of the picker of the tab shown
    const RemapTab tab = remapTab();
    const Key k = pickerWidget->map().key(key);
    if((tab != KEYBOARD_TAB && tab != SHORTCUT_TAB) || !k || !k.hasScan || pickerWidget->dimmedKeys().contains(key))
        return false;
    const int i = K95PKeyTable::indexOf(key.toStdString());
    const bool off = tab == SHORTCUT_TAB && shortcutPick == i;
    pickerFilling = true;
    pickerWidget->setSelection(off ? QStringList() : QStringList(key));
    pickerFilling = false;
    picked(off ? QStringList() : QStringList(key));
    return true;
}

void HwBindWidget::imitateChanged(){
    // 'Imitate holding key' is not offered on a media key; its time only On press
    const bool media = keyboardPick >= 0 && K95PKeyTable::icueMedia(unsigned(keyboardPick));
    if(media && ui->imitateCheck->isChecked()){
        const QSignalBlocker block(ui->imitateCheck);
        ui->imitateCheck->setChecked(false);
    }
    ui->imitateCheck->setEnabled(!media);
    const bool on = ui->imitateCheck->isChecked();
    ui->imitateMode->setEnabled(on);
    ui->terminateLabel->setEnabled(on);
    ui->terminateSpin->setEnabled(on);
    ui->terminateRow->setVisible(ui->imitateMode->currentIndex() == 0);
    // the media keys of the picker follow
    if(remapTab() == KEYBOARD_TAB){
        QStringList dimmed;
        if(on)
            for(unsigned k = 0; k < K95PKeyTable::KEYS; ++k)
                if(K95PKeyTable::icueMedia(k)) dimmed << QString::fromStdString(K95PKeyTable::label(k));
        if(dimmed != pickerWidget->dimmedKeys()) pickerWidget->setDimmedKeys(dimmed);
    }
}

void HwBindWidget::setImitate(bool on, bool toggle, double seconds){
    ui->imitateMode->setCurrentIndex(toggle ? 1 : 0);
    ui->terminateSpin->setValue(seconds);
    ui->imitateCheck->setChecked(on);
    imitateChanged();
}

std::vector<uint8_t> HwBindWidget::shortcutKeys() const {
    // iCUE's order: Win, Ctrl, Alt, Shift, then the key
    std::vector<uint8_t> keys;
    const QPushButton* const mods[] = {ui->winButton, ui->ctrlButton, ui->altButton, ui->shiftButton};
    const char* const names[] = {"lwin", "lctrl", "lalt", "lshift"};
    for(int m = 0; m < 4; ++m)
        if(mods[m]->isChecked()) keys.push_back(uint8_t(K95PKeyTable::indexOf(names[m])));
    if(shortcutPick >= 0) keys.push_back(uint8_t(shortcutPick));
    return keys;
}

void HwBindWidget::refreshShortcut(){
    // The chips of the keys; at most three modifiers: with three on, the fourth is off and says why
    QStringList names;
    for(uint8_t k : shortcutKeys()){
        const std::string l = K95PKeyTable::label(k);
        names << (l == "lwin" ? tr("Win") : l == "lctrl" ? tr("Ctrl") : l == "lalt" ? tr("Alt") : l == "lshift" ? tr("Shift") : keyName(k));
    }
    ui->shortcutChips->setKeys(names);
    QPushButton* const mods[] = {ui->winButton, ui->ctrlButton, ui->altButton, ui->shiftButton};
    int on = 0;
    for(QPushButton* b : mods) on += b->isChecked();
    for(QPushButton* b : mods){
        const bool can = b->isChecked() || on < 3;
        b->setEnabled(can);
        b->setToolTip(can ? QString() : tr("A shortcut has at most three of Win, Ctrl, Alt and Shift."));
    }
}

void HwBindWidget::shortcutChanged(){
    showMessage(ui->errorLabel, QString());
    refreshShortcut();
}

void HwBindWidget::clearShortcut(){
    fillShortcut(QStringList());
    placePicker();
}

void HwBindWidget::fillShortcut(const QStringList& keys){
    // The buttons of the modifiers (a right one is its button too) and the key picked, with no other effect
    QPushButton* const mods[] = {ui->winButton, ui->ctrlButton, ui->altButton, ui->shiftButton};
    const char* const names[4][2] = {{"lwin", "rwin"}, {"lctrl", "rctrl"}, {"lalt", "ralt"}, {"lshift", "rshift"}};
    for(QPushButton* b : mods){
        const QSignalBlocker block(b);
        b->setChecked(false);
    }
    shortcutPick = -1;
    for(const QString& key : keys){
        bool modifier = false;
        for(int m = 0; m < 4; ++m)
            if(key == names[m][0] || key == names[m][1]){
                const QSignalBlocker block(mods[m]);
                mods[m]->setChecked(true);
                modifier = true;
            }
        if(!modifier) shortcutPick = K95PKeyTable::indexOf(key.toStdString());
    }
    refreshShortcut();
}

void HwBindWidget::setRemap(const QString& key){
    // A key, a mouse button ("mouse1".."mouse5") or a Language key: the type and the tab follow
    const int i = K95PKeyTable::indexOf(key.toStdString());
    setType(REMAP);
    if(i >= int(K95PKeyTable::MOUSE_FIRST) && i <= int(K95PKeyTable::MOUSE_LAST)){
        setRemapTab(MOUSE_TAB);
        ui->mouseGroup->button(i)->setChecked(true);
        return;
    }
    if(i >= 0 && unsigned(i) < K95PKeyTable::KEYS && K95PKeyTable::icueLanguage(unsigned(i))){
        setRemapTab(LANGUAGE_TAB);
        ui->languageBox->setCurrentIndex(ui->languageBox->findData(i));
        return;
    }
    setRemapTab(KEYBOARD_TAB);
    keyboardPick = i >= 0 && unsigned(i) < K95PKeyTable::KEYS ? i : -1;
    placePicker();
}

void HwBindWidget::setMacroRows(const std::vector<Row>& newRows, uint8_t start, uint8_t run){
    rows = newRows;
    ui->runBox->setCurrentIndex(ui->runBox->findData(runData(start, run)));
    refreshMacroList();
}

void HwBindWidget::setShortcut(const QStringList& keys){
    // The Shortcut tab of the Remap type with these keys
    setType(REMAP);
    setRemapTab(SHORTCUT_TAB);
    fillShortcut(keys);
    placePicker();
}

void HwBindWidget::setText(const QString& text, unsigned delayMs){
    textRebuilt = false;
    fillText(text);
    ui->textDelay->setValue(int(delayMs));
    updateTextPreview();
}

void HwBindWidget::fillText(const QString& text){
    textAccepted = int(text.size());   // before the change: limitText() then keeps all of it
    ui->textEdit->setPlainText(text);
}

void HwBindWidget::limitText(){
    const int len = int(ui->textEdit->toPlainText().size());
    const int allowed = std::max(int(HwBindEdit::TEXT_CHARS_MAX), textAccepted);
    if(len > allowed){
        // What was just added ends at the cursor (typing, pasting, also over a selection): the characters past the limit are the
        // last ones of it. The cut joins the edit that made them, so undo and redo never bring back a text over the limit
        const int excess = len - allowed;
        QSignalBlocker block(ui->textEdit);
        QTextCursor c = ui->textEdit->textCursor();
        const int end = c.position() >= excess ? c.position() : len;
        c.joinPreviousEditBlock();
        c.setPosition(end - excess);
        c.setPosition(end, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        c.endEditBlock();
        ui->textEdit->setTextCursor(c);
    }
    textAccepted = int(ui->textEdit->toPlainText().size());
}

QString HwBindWidget::textPreview() const {
    return textPreviewText;
}

QString HwBindWidget::error() const {
    return ui->errorLabel->text();
}

QString HwBindWidget::notice() const {
    return ui->noticeLabel->text();
}

QString HwBindWidget::textRefusal(const std::u32string& rejected, const std::u32string& notOnWindows) const {
    // What a Text cannot type: no key of the layout makes it, or only a key that Windows does not give it to
    QStringList why;
    if(!rejected.empty())
        why << tr("No key of the layout makes: %1").arg(QString::fromUcs4(rejected.data(), int(rejected.size())));
    if(!notOnWindows.empty())
        why << (windowsTable ? tr("Not on the Windows %1 layout (it would type something else there): %2")
                               : tr("AltGr characters are refused: no table of the Windows %1 layout yet: %2"))
                   .arg(layoutName, QString::fromUcs4(notOnWindows.data(), int(notOnWindows.size())));
    return why.join(" ");
}

void HwBindWidget::updateTextPreview(){
    // The count of events and the layout, next to Apply like the notices of the Macro page
    textPreviewText.clear();
#ifdef USE_XKBCOMMON
    if(xkb){
        std::u32string text, rejected, notOnWindows;
        HwTextShape::utf32(ui->textEdit->toPlainText().toStdString(), text);
        const std::vector<HwTextShape::Stroke> strokes = xkb->strokes(text, &rejected, &notOnWindows);
        textPreviewText = tr("%1 events (layout: %2).").arg(HwTextShape::events(strokes, unsigned(ui->textDelay->value())).size())
                                                       .arg(layoutName);
        const QString refusal = textRefusal(rejected, notOnWindows);
        if(!refusal.isEmpty()) textPreviewText += " " + refusal;
    }
#endif
    if(type() != TEXT)
        return;
    ui->noticeLabel->setText(textRebuilt && !textPreviewText.isEmpty() ?
                         textPreviewText + " " + tr("Rebuilt from the events of the key.") : textPreviewText);
}

void HwBindWidget::setRecording(bool on){
    if(on == isRecording)
        return;
    isRecording = on;
    if(on) qApp->installEventFilter(this);
    else qApp->removeEventFilter(this);
    if(on){
        rows.clear();
        recordDown.assign(HwBinding::KEYS, false);
        recordFull = false;
        refreshMacroList();
        ui->noticeLabel->setText(tr("Recording: type the macro, then Stop. The keyboard is grey while recording."));
    } else {
        // A key still held when recording stops is released at the end, after a delay of 0 (a row of the table, as every event
        // recorded has one before it: Apply adds none)
        for(unsigned k = 0; k < recordDown.size(); ++k)
            if(recordDown[k]){
                rows.push_back(Row(Row::DELAY, 0));
                rows.push_back(Row(Row::RELEASE, k));
            }
        recordDown.clear();
        refreshMacroList();
        ui->noticeLabel->setText(recordFull ?
            tr("Recording stopped: a Macro has at most %1 rows (iCUE's editor). Apply puts the %2 recorded on the selected keys.")
                .arg(HwBindEdit::MACRO_ROWS_MAX).arg(rows.size()) :
            tr("Recorded %n event(s): Apply puts them on the selected keys.", nullptr, int(rows.size())));
    }
    updateEnabled();
}

void HwBindWidget::recordedEvent(const QString& key, qint64 us, bool down){
    if(!isRecording || recordFull)
        return;
    const int i = indexOf(key);
    if(i < 0 || recordDown.size() != HwBinding::KEYS)
        return;
    // A release of a key held before recording started, or a second press, is not an event of the macro
    if(recordDown[size_t(i)] == down)
        return;
    // A Macro has at most MACRO_ROWS_MAX rows (as iCUE's editor): this event with the delay before it, and the release of
    // every key still held after it with its delay of 0 (setRecording(false)), must fit. Else the recorder stops by the way of Stop,
    // once the reader that sent this event has returned (stopping deletes it)
    const size_t held = size_t(std::count(recordDown.begin(), recordDown.end(), true)) + (down ? 1 : 0) - (down ? 0 : 1);
    if(rows.size() + (rows.empty() ? 1 : 2) + 2 * held > HwBindEdit::MACRO_ROWS_MAX){
        recordFull = true;
        QTimer::singleShot(0, this, [this](){ if(isRecording) emit recordRequested(false); });
        return;
    }
    recordDown[size_t(i)] = down;
    if(!rows.empty()){
        const qint64 ms = us / 1000;
        rows.push_back(Row(Row::DELAY, unsigned(ms > qint64(HwBindEdit::MACRO_DELAY_MAX) ? qint64(HwBindEdit::MACRO_DELAY_MAX) : ms < 0 ? 0 : ms)));
    }
    rows.push_back(Row(down ? Row::PRESS : Row::RELEASE, unsigned(i)));
    refreshMacroList();
    ui->macroTable->scrollToBottom();
}

void HwBindWidget::fail(const QString& why){
    showMessage(ui->errorLabel, why);
}

bool HwBindWidget::build(Action& out){
    std::string why;
    switch(type()){
    case DEFAULT:
        out = HwBindEdit::native();
        return true;
    case REMAP:
        switch(remapTab()){
        case KEYBOARD_TAB: {
            if(keyboardPick < 0){ fail(tr("Choose the key to remap to.")); return false; }
            const uint8_t key = uint8_t(keyboardPick);
            if(!ui->imitateCheck->isChecked())
                out = HwBindEdit::remap(key);
            else if(ui->imitateMode->currentIndex() == 1)
                out = HwBindEdit::imitateToggle(key);
            else   // tenths of a second, as the field shows them
                out = HwBindEdit::imitatePress(key, unsigned(std::lround(ui->terminateSpin->value() * 10.)) * 100u);
            return true;
        }
        case MOUSE_TAB:
            if(ui->mouseGroup->checkedId() < 0){ fail(tr("Choose the mouse button to remap to.")); return false; }
            out = HwBindEdit::remap(uint8_t(ui->mouseGroup->checkedId()));
            return true;
        case SHORTCUT_TAB: {
            const std::vector<uint8_t> keys = shortcutKeys();
            if(keys.empty()){ fail(tr("A shortcut needs at least one key.")); return false; }
            out = HwBindEdit::shortcut(keys);   // in iCUE's order; the check of the draft refuses one that breaks its rules
            return true;
        }
        case LANGUAGE_TAB:
            if(ui->languageBox->currentIndex() < 0){ fail(tr("Choose the Language key to remap to.")); return false; }
            out = HwBindEdit::remap(uint8_t(ui->languageBox->currentData().toInt()));
            return true;
        }
        return false;
    case MACRO: {
        std::vector<HwBinding::Event> ev;
        if(!HwBindEdit::events(rows, ev, why)){ fail(QString::fromStdString(why)); return false; }
        if(ui->runBox->currentIndex() < 0){ fail(tr("Choose when the macro runs.")); return false; }
        const unsigned when = ui->runBox->currentData().toUInt();
        out = HwBindEdit::macro(ev, uint8_t(when >> 8), uint8_t(when & 0xff));
        return true;
    }
    case TEXT: {
#ifdef USE_XKBCOMMON
        if(!xkb){ fail(ui->textWhy->text()); return false; }
        std::u32string text, rejected, notOnWindows;
        if(!HwTextShape::utf32(ui->textEdit->toPlainText().toStdString(), text) || text.empty()){ fail(tr("Type the text.")); return false; }
        const std::vector<HwTextShape::Stroke> strokes = xkb->strokes(text, &rejected, &notOnWindows);
        if(!rejected.empty() || !notOnWindows.empty()){
            fail(textRefusal(rejected, notOnWindows));
            return false;
        }
        const std::vector<HwBinding::Event> ev = HwTextShape::events(strokes, unsigned(ui->textDelay->value()));
        if(ev.size() > HwBindEdit::EVENTS_MAX){
            fail(tr("The text needs %1 key events, at most %2 fit in one hardware action: shorten it.").arg(ev.size()).arg(HwBindEdit::EVENTS_MAX));
            return false;
        }
        out = HwBindEdit::text(ev);
        return true;
#else
        fail(ui->textWhy->text());
        return false;
#endif
    }
    case KEPT:
        return false;
    }
    return false;
}

bool HwBindWidget::apply(){
    showMessage(ui->errorLabel, QString());
    ui->noticeLabel->clear();
    if(!editable || !controls || isRecording || currentSelection.isEmpty())
        return false;
    Action a;
    if(!build(a))
        return false;
    // What was a Text and is now a Macro made by hand: it is saved as a Macro
    bool wasText = false;
    const HwBinding::Keys before = draft->keys;
    for(const QString& key : currentSelection){
        const int i = indexOf(key);
        if(i < 0) continue;
        wasText |= type() == MACRO && HwBindEdit::kindOf(draft->keys[size_t(i)]) == HwBindEdit::TEXT;
        // An action equal to the slot's goes back to the slot's own (its file name and flags)
        const Action* b = baseAction(i);
        draft->keys[size_t(i)] = b && b->sameAs(a) ? *b : a;
    }
    std::string why;
    const HwBinding::Keys* baseKeys = base && (base->state == HwBinding::Record::OK || base->state == HwBinding::Record::EMPTY) ?
                                      &base->keys : nullptr;
    if(!HwBindEdit::check(draft->keys, baseKeys, why)){
        draft->keys = before;
        fail(QString::fromStdString(why));
        return false;
    }
    refreshKeys();
    loadPanel();
    if(wasText)
        ui->noticeLabel->setText(tr("A Text whose events were changed by hand is saved as a Macro."));
    emit draftChanged();
    return true;
}

void HwBindWidget::reset(){
    showMessage(ui->errorLabel, QString());
    if(!editable || !controls || isRecording || currentSelection.isEmpty())
        return;
    for(const QString& key : currentSelection){
        const int i = indexOf(key);
        if(i < 0) continue;
        const Action* b = baseAction(i);
        draft->keys[size_t(i)] = b ? *b : Action();
    }
    refreshKeys();
    loadPanel();
    emit draftChanged();
}
